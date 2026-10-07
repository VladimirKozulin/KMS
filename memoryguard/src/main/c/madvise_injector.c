#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define MADV_MERGEABLE   12
#define MADV_UNMERGEABLE 13
#define SYS_MADVISE_X86_64 28

// Usage: madvise_injector [--unmerge] [--dry-run] [--container <id>] <pid>
//
//   --unmerge    issue MADV_UNMERGEABLE on regions already marked mergeable
//                (per-process rollback, instead of the global run=2 unmerge)
//   --dry-run    print the region classification, do not touch the process
//   --container  expected Docker container ID; the PID must belong to that
//                container's cgroup (checked before AND after ptrace attach,
//                which closes the PID-reuse race between `docker top` and attach)

// ============================================================
// KSM Side-Channel Mitigation: Sensitive Region Filter
// ============================================================
// File-backed mappings are never marked (only anonymous memory is), so a
// library blocklist by itself filters nothing. What it does protect is the
// library's *anonymous* .bss segment, which the kernel maps right after the
// library's last file-backed segment and which holds global crypto state
// (RNG pools, cached keys, session caches).
//
// Secrets that live in the general heap cannot be attributed to a library
// from /proc/PID/maps. For those we rely on VmFlags (see SKIP_VMFLAGS):
// well-behaved crypto code already locks (mlock) or excludes from core dumps
// (MADV_DONTDUMP) the memory that holds key material — OpenSSL secure heap,
// libsodium sodium_malloc, libgcrypt/GnuPG secmem, HashiCorp Vault mlock, and
// glibc/OpenSSL RNG state with MADV_WIPEONFORK.
//
// Extra libraries can be added at runtime: MEMORYGUARD_EXCLUDE_LIBS=libfoo,libbar
// ============================================================
static const char *SENSITIVE_LIBS[] = {
    "libcrypto", "libssl", "libgnutls", "libgcrypt",
    "libsodium", "libgpg-error", "libsasl",
    "libkrb5", "libk5crypto", "libpam",
    "libssh", "libnss", "libnspr",
    "libp11-kit", "libkeyutils", "libtasn1",
    "libmbedcrypto", "libmbedtls", "libwolfssl",
    "libnettle", "libhogweed", "libbotan",
    "libSystem.Security.Cryptography.Native",   // .NET on Linux
    NULL
};

// VmFlags (from /proc/PID/smaps) that exclude a region from merging:
//   lo  VM_LOCKED       — mlock()'ed, the standard "this holds secrets" signal
//   dd  VM_DONTDUMP     — MADV_DONTDUMP, used by secure allocators
//   wf  VM_WIPEONFORK   — RNG / DRBG state
//   io  VM_IO, pf VM_PFNMAP — device memory, never mergeable anyway
//   um, uw              — userfaultfd-managed, owned by another agent
static const char *SKIP_VMFLAGS[] = { "lo", "dd", "wf", "io", "pf", "um", "uw", NULL };

// Minimum region size for MADV_MERGEABLE marking (bytes).
// Small anonymous allocations (< 8 KB) are more likely to hold
// secrets (keys, tokens, passwords) and offer negligible merge
// savings. Skipping them shrinks the side-channel attack surface.
#define MIN_REGION_SIZE 8192

#define MAX_EXTRA_LIBS 64
static char *extra_libs[MAX_EXTRA_LIBS];
static int extra_lib_count = 0;

enum verdict {
    V_MARK = 0,
    V_SKIP_NOT_ANON,
    V_SKIP_PSEUDO,
    V_SKIP_LIB_BSS,
    V_SKIP_VMFLAG,
    V_SKIP_SMALL,
    V_SKIP_ALREADY,
    V_SKIP_NOT_MERGED,
    V_SKIP_THREAD_STACK,
};

static const char *VERDICT_NAMES[] = {
    "mark", "file-backed", "pseudo", "sensitive-lib-bss",
    "secret-vmflag", "small", "already-mergeable", "not-mergeable",
    "thread-stack",
};

struct region {
    unsigned long start, end;
    char prot[5];
    char path[512];
    char vmflags[256];
    enum verdict verdict;
};

static void load_extra_libs(void) {
    const char *env = getenv("MEMORYGUARD_EXCLUDE_LIBS");
    if (!env || !*env) return;
    char *copy = strdup(env);
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save);
         tok && extra_lib_count < MAX_EXTRA_LIBS;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;
        if (*tok) extra_libs[extra_lib_count++] = tok;
    }
}

static int is_sensitive_lib(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; SENSITIVE_LIBS[i] != NULL; i++) {
        if (strstr(base, SENSITIVE_LIBS[i])) return 1;
    }
    for (int i = 0; i < extra_lib_count; i++) {
        if (strstr(base, extra_libs[i])) return 1;
    }
    return 0;
}

static int has_vmflag(const char *flags, const char *flag) {
    // VmFlags is a space-separated list of 2-letter tokens
    for (const char *p = flags; (p = strstr(p, flag)) != NULL; p += 2) {
        int start_ok = (p == flags || p[-1] == ' ');
        int end_ok = (p[2] == ' ' || p[2] == '\0' || p[2] == '\n');
        if (start_ok && end_ok) return 1;
    }
    return 0;
}

static enum verdict classify(const struct region *r, const struct region *prev, int unmerge) {
    unsigned long len = r->end - r->start;

    if (unmerge) {
        return has_vmflag(r->vmflags, "mg") ? V_MARK : V_SKIP_NOT_MERGED;
    }
    if (!(r->prot[0] == 'r' && r->prot[3] == 'p')) return V_SKIP_NOT_ANON;
    if (r->path[0] == '/') return V_SKIP_NOT_ANON;
    // [stack], [vvar], [vdso], [vsyscall], [stack:tid] ... — everything in
    // brackets except the heap and named anonymous VMAs (PR_SET_VMA_ANON_NAME)
    if (r->path[0] == '[' && strcmp(r->path, "[heap]") != 0
            && strncmp(r->path, "[anon:", 6) != 0) {
        return V_SKIP_PSEUDO;
    }
    // Anonymous region glued to the end of a sensitive library = its .bss
    if (r->path[0] == '\0' && prev && prev->end == r->start
            && prev->path[0] == '/' && is_sensitive_lib(prev->path)) {
        return V_SKIP_LIB_BSS;
    }
    for (int i = 0; SKIP_VMFLAGS[i] != NULL; i++) {
        if (has_vmflag(r->vmflags, SKIP_VMFLAGS[i])) return V_SKIP_VMFLAG;
    }
    if (has_vmflag(r->vmflags, "mg")) return V_SKIP_ALREADY;
    if (len < MIN_REGION_SIZE) return V_SKIP_SMALL;
    return V_MARK;
}

// Parses /proc/PID/smaps into a heap-allocated region array.
static struct region *read_regions(int pid, int *count) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/smaps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    int cap = 256, n = 0;
    struct region *regions = calloc(cap, sizeof(struct region));
    char line[4096];
    struct region *cur = NULL;

    while (fgets(line, sizeof(line), f)) {
        unsigned long start, end;
        char prot[5];
        int path_off = 0;
        // Header line: "start-end perms offset dev inode [path]"
        if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %n", &start, &end, prot, &path_off) == 3
                && path_off > 0) {
            if (n == cap) {
                cap *= 2;
                regions = realloc(regions, cap * sizeof(struct region));
            }
            cur = &regions[n++];
            memset(cur, 0, sizeof(*cur));
            cur->start = start;
            cur->end = end;
            memcpy(cur->prot, prot, sizeof(cur->prot));
            snprintf(cur->path, sizeof(cur->path), "%.511s", line + path_off);
            cur->path[strcspn(cur->path, "\n")] = '\0';
        } else if (cur && strncmp(line, "VmFlags:", 8) == 0) {
            snprintf(cur->vmflags, sizeof(cur->vmflags), "%.255s", line + 8);
            cur->vmflags[strcspn(cur->vmflags, "\n")] = '\0';
        }
    }
    fclose(f);
    *count = n;
    return regions;
}

// Field 22 of /proc/PID/stat: process start time in clock ticks.
// (PID, starttime) uniquely identifies a process across PID reuse.
static unsigned long long read_start_time(int pid) {
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    // comm (field 2) may contain spaces/parens — skip to the last ')'
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    unsigned long long start = 0;
    // after ')' : field 3 (state) ... field 22 (starttime) = 20th token
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
               &start) != 1) {
        return 0;
    }
    return start;
}

static int pid_in_container(int pid, const char *container_id) {
    if (!container_id) return 1;
    char path[64], line[1024];
    snprintf(path, sizeof(path), "/proc/%d/cgroup", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, container_id)) { found = 1; break; }
    }
    fclose(f);
    return found;
}

// ============================================================
// ptrace: stop the WHOLE thread group, not just the leader.
// ============================================================
// The previous version attached only to the main thread and then patched
// the instruction at its RIP. Every other thread kept running the same
// (shared) code page and could execute the patched bytes → random SIGSEGV /
// corrupted state in multi-threaded targets (JVM, Go, .NET, Node all are).
#define MAX_THREADS 4096
struct tracee { int tid; int pending_sig; unsigned long rsp; };
static struct tracee tracees[MAX_THREADS];
static int tracee_count = 0;

static int is_attached(int tid) {
    for (int i = 0; i < tracee_count; i++) if (tracees[i].tid == tid) return 1;
    return 0;
}

static void detach_all(void) {
    for (int i = 0; i < tracee_count; i++) {
        ptrace(PTRACE_DETACH, tracees[i].tid, NULL, (void *)(long)tracees[i].pending_sig);
    }
    tracee_count = 0;
}

static int attach_thread(int tid) {
    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) < 0) {
        return errno == ESRCH ? 0 : -1;   // thread exited meanwhile — fine
    }
    int status;
    if (waitpid(tid, &status, __WALL) < 0 || !WIFSTOPPED(status)) {
        return 0;
    }
    if (tracee_count == MAX_THREADS) {
        ptrace(PTRACE_DETACH, tid, NULL, NULL);
        return -1;
    }
    int sig = WSTOPSIG(status);
    // Remember any real signal we swallowed, re-deliver it on detach
    tracees[tracee_count].tid = tid;
    tracees[tracee_count].pending_sig = (sig == SIGSTOP) ? 0 : sig;
    tracee_count++;
    return 0;
}

static int attach_all_threads(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    // Threads may be spawned while we attach — repeat until no new TIDs show up
    for (int pass = 0; pass < 5; pass++) {
        DIR *d = opendir(path);
        if (!d) return -1;
        int added = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int tid = atoi(e->d_name);
            if (tid <= 0 || is_attached(tid)) continue;
            if (attach_thread(tid) < 0) { closedir(d); return -1; }
            added++;
        }
        closedir(d);
        if (added == 0) return is_attached(pid) ? 0 : -1;
    }
    return -1;
}

// Only the main thread's stack is labelled [stack] in maps; pthread / JVM /
// Go thread stacks are plain anonymous regions. Once every thread is stopped
// we know each one's RSP exactly — skip any region that contains one.
static void mark_thread_stacks(struct region *regions, int count) {
    for (int i = 0; i < tracee_count; i++) {
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, tracees[i].tid, NULL, &regs) == 0) {
            tracees[i].rsp = regs.rsp;
        }
    }
    for (int r = 0; r < count; r++) {
        if (regions[r].verdict != V_MARK) continue;
        for (int i = 0; i < tracee_count; i++) {
            if (tracees[i].rsp >= regions[r].start && tracees[i].rsp < regions[r].end) {
                regions[r].verdict = V_SKIP_THREAD_STACK;
                break;
            }
        }
    }
}

// Executes madvise(start, len, advice) inside the stopped leader thread.
// Returns the syscall result (negative errno on failure), or -1000 on a
// ptrace failure.
static long remote_madvise(int pid, unsigned long syscall_addr,
                           const struct user_regs_struct *orig_regs,
                           unsigned long start, unsigned long len, int advice) {
    struct user_regs_struct regs = *orig_regs;
    regs.rip = syscall_addr;
    regs.rax = SYS_MADVISE_X86_64;
    regs.orig_rax = -1;            // not inside a syscall → no restart fixups
    regs.rdi = start;
    regs.rsi = len;
    regs.rdx = advice;

    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) < 0) return -1000;
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) < 0) return -1000;

    int status;
    if (waitpid(pid, &status, __WALL) < 0 || !WIFSTOPPED(status)) return -1000;
    if (WSTOPSIG(status) != SIGTRAP) {
        // A signal arrived instead of the single-step trap — keep it for detach
        for (int i = 0; i < tracee_count; i++) {
            if (tracees[i].tid == pid) tracees[i].pending_sig = WSTOPSIG(status);
        }
        return -1000;
    }

    struct user_regs_struct post;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &post) < 0) return -1000;
    return (long)post.rax;
}

int main(int argc, char *argv[]) {
    int unmerge = 0, dry_run = 0;
    const char *container_id = NULL;
    const char *pid_arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--unmerge") == 0) unmerge = 1;
        else if (strcmp(argv[i], "--dry-run") == 0) dry_run = 1;
        else if (strcmp(argv[i], "--container") == 0 && i + 1 < argc) container_id = argv[++i];
        else pid_arg = argv[i];
    }
    if (!pid_arg) {
        fprintf(stderr, "Usage: %s [--unmerge] [--dry-run] [--container <id>] <pid>\n", argv[0]);
        return 1;
    }

    // Safe PID parsing with strtol (replaces atoi which silently returns 0 on error)
    char *endptr;
    errno = 0;
    long pid_long = strtol(pid_arg, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || pid_long <= 1 || pid_long > 4194304) {
        fprintf(stderr, "[injector] Invalid PID: %s (must be > 1 and <= 4194304)\n", pid_arg);
        return 1;
    }
    int pid = (int)pid_long;

    if (container_id) {
        for (const char *c = container_id; *c; c++) {
            if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) {
                fprintf(stderr, "[injector] Invalid container ID\n");
                return 1;
            }
        }
    }

    load_extra_libs();

    unsigned long long start_time = read_start_time(pid);
    if (start_time == 0) {
        fprintf(stderr, "[injector] Process %d does not exist\n", pid);
        return 1;
    }
    if (!pid_in_container(pid, container_id)) {
        fprintf(stderr, "[injector] PID %d is not in container %.12s — refusing\n", pid, container_id);
        return 3;
    }

    int region_count = 0;
    struct region *regions = read_regions(pid, &region_count);
    if (!regions) {
        perror("[injector] Failed to read smaps");
        return 1;
    }

    int to_mark = 0;
    for (int i = 0; i < region_count; i++) {
        struct region *r = &regions[i];
        r->verdict = classify(r, i > 0 ? &regions[i - 1] : NULL, unmerge);
        if (r->verdict == V_MARK) to_mark++;
        if (dry_run) {
            printf("%012lx-%012lx %s %-18s %s [%s]\n", r->start, r->end, r->prot,
                   VERDICT_NAMES[r->verdict], r->path, r->vmflags);
        }
    }

    long long done_bytes = 0, failed_bytes = 0;
    int ok_calls = 0, err_calls = 0;

    if (!dry_run && to_mark > 0) {
        if (attach_all_threads(pid) < 0) {
            perror("[injector] Failed to stop all threads");
            detach_all();
            free(regions);
            return 1;
        }

        // Re-verify identity now that the process is frozen (PID reuse race)
        if (read_start_time(pid) != start_time || !pid_in_container(pid, container_id)) {
            fprintf(stderr, "[injector] PID %d changed identity during attach — aborting\n", pid);
            detach_all();
            free(regions);
            return 3;
        }

        if (!unmerge) {
            mark_thread_stacks(regions, region_count);
        }

        struct user_regs_struct orig_regs;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &orig_regs) < 0) {
            perror("[injector] PTRACE_GETREGS failed");
            detach_all();
            free(regions);
            return 1;
        }

        // Prefer an existing `syscall` instruction: a thread stopped inside a
        // syscall has RIP right after one (0F 05 at RIP-2). Then we never
        // write to the target's code at all.
        unsigned long syscall_addr = orig_regs.rip;
        long saved_word = 0;
        int patched = 0;
        errno = 0;
        long before = ptrace(PTRACE_PEEKTEXT, pid, (void *)(orig_regs.rip - 2), NULL);
        if (errno == 0 && (before & 0xFFFF) == 0x050F) {
            syscall_addr = orig_regs.rip - 2;
        } else {
            errno = 0;
            saved_word = ptrace(PTRACE_PEEKTEXT, pid, (void *)orig_regs.rip, NULL);
            long patched_word = (saved_word & ~0xFFFFL) | 0x050FL;
            if (errno != 0 || ptrace(PTRACE_POKETEXT, pid, (void *)orig_regs.rip, (void *)patched_word) < 0) {
                perror("[injector] Failed to place syscall instruction");
                detach_all();
                free(regions);
                return 1;
            }
            patched = 1;   // safe: every thread of the process is stopped
        }

        int advice = unmerge ? MADV_UNMERGEABLE : MADV_MERGEABLE;
        for (int i = 0; i < region_count; i++) {
            struct region *r = &regions[i];
            if (r->verdict != V_MARK) continue;
            long ret = remote_madvise(pid, syscall_addr, &orig_regs, r->start, r->end - r->start, advice);
            if (ret == -1000) {
                fprintf(stderr, "[injector] ptrace failure at region 0x%lx — stopping\n", r->start);
                err_calls++;
                failed_bytes += r->end - r->start;
                break;
            } else if (ret < 0) {
                fprintf(stderr, "[injector] madvise failed for region 0x%lx-0x%lx: %s\n",
                        r->start, r->end, strerror((int)-ret));
                err_calls++;
                failed_bytes += r->end - r->start;
            } else {
                ok_calls++;
                done_bytes += r->end - r->start;
            }
        }

        if (patched) {
            ptrace(PTRACE_POKETEXT, pid, (void *)orig_regs.rip, (void *)saved_word);
        }
        ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs);
        detach_all();
    }

    long long counts_bytes[V_SKIP_THREAD_STACK + 1] = {0};
    int counts[V_SKIP_THREAD_STACK + 1] = {0};
    for (int i = 0; i < region_count; i++) {
        counts[regions[i].verdict]++;
        counts_bytes[regions[i].verdict] += regions[i].end - regions[i].start;
    }
    free(regions);

    printf("[injector] PID %d (%s): %s=%lld KB, failed=%lld KB, ok_calls=%d, err_calls=%d | "
           "skipped: lib-bss=%d (%lld KB), secret-vmflag=%d (%lld KB), thread-stack=%d (%lld KB), "
           "small=%d, pseudo=%d, already=%d\n",
           pid, unmerge ? "unmerge" : "merge", dry_run ? "would-mark" : "marked",
           (dry_run ? counts_bytes[V_MARK] : done_bytes) / 1024, failed_bytes / 1024,
           ok_calls, err_calls,
           counts[V_SKIP_LIB_BSS], counts_bytes[V_SKIP_LIB_BSS] / 1024,
           counts[V_SKIP_VMFLAG], counts_bytes[V_SKIP_VMFLAG] / 1024,
           counts[V_SKIP_THREAD_STACK], counts_bytes[V_SKIP_THREAD_STACK] / 1024,
           counts[V_SKIP_SMALL], counts[V_SKIP_PSEUDO], counts[V_SKIP_ALREADY]);

    if (err_calls > 0 && ok_calls == 0) {
        return 2;
    }
    return 0;
}
