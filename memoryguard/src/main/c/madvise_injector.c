#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define MADV_MERGEABLE 12
#define SYS_MADVISE_X86_64 28

// Reads 8 bytes from target process memory
long poke_peek_text(int pid, unsigned long addr, long *new_data, int write) {
    errno = 0;
    if (write) {
        long ret = ptrace(PTRACE_POKETEXT, pid, (void *)addr, (void *)*new_data);
        return ret;
    } else {
        long ret = ptrace(PTRACE_PEEKTEXT, pid, (void *)addr, NULL);
        return ret;
    }
}

// Injects madvise syscall into the target process using ptrace
int inject_madvise_ptrace(int pid, unsigned long start, unsigned long len) {
    int status;
    struct user_regs_struct regs, orig_regs;
    
    // Attach to the target process
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0) {
        perror("[injector-ptrace] PTRACE_ATTACH failed");
        return -1;
    }

    // Wait for the process to stop
    if (waitpid(pid, &status, 0) < 0) {
        perror("[injector-ptrace] waitpid failed");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }

    // Get original register values
    if (ptrace(PTRACE_GETREGS, pid, NULL, &orig_regs) < 0) {
        perror("[injector-ptrace] PTRACE_GETREGS failed");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }

    regs = orig_regs;

    // Read the instruction at current instruction pointer (RIP)
    unsigned long rip = regs.rip;
    long original_instr = poke_peek_text(pid, rip, NULL, 0);
    if (errno != 0) {
        perror("[injector-ptrace] PEEKTEXT failed");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }

    // Inject 'syscall' instruction (opcode 0x050F in x86_64)
    long syscall_instr = (original_instr & ~0xFFFFL) | 0x050FL; 
    if (poke_peek_text(pid, rip, &syscall_instr, 1) < 0) {
        perror("[injector-ptrace] POKETEXT failed to inject syscall instruction");
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }

    // Set up registers for syscall: madvise(start, len, MADV_MERGEABLE)
    regs.rax = SYS_MADVISE_X86_64; // syscall number
    regs.rdi = start;              // 1st arg: addr
    regs.rsi = len;                // 2nd arg: length
    regs.rdx = MADV_MERGEABLE;     // 3rd arg: advice

    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) < 0) {
        perror("[injector-ptrace] PTRACE_SETREGS failed");
        poke_peek_text(pid, rip, &original_instr, 1); // restore text
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }

    // Execute the injected syscall instruction
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) < 0) {
        perror("[injector-ptrace] PTRACE_SINGLESTEP failed");
        poke_peek_text(pid, rip, &original_instr, 1); // restore text
        ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs); // restore regs
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }

    // Wait for the single step to complete
    waitpid(pid, &status, 0);

    // Get register state to read syscall return value (in RAX)
    struct user_regs_struct post_regs;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &post_regs) == 0) {
        long long result = (long long)post_regs.rax;
        if (result < 0) {
            fprintf(stderr, "[injector-ptrace] Warning: madvise failed for region 0x%lx-0x%lx with code %lld\n", start, start+len, result);
        }
    }

    // Restore the original instruction text and register values
    poke_peek_text(pid, rip, &original_instr, 1);
    ptrace(PTRACE_SETREGS, pid, NULL, &orig_regs);

    // Detach and let the process continue execution
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pid>\n", argv[0]);
        return 1;
    }

    int pid = atoi(argv[1]);

    // Open /proc/<pid>/maps to read memory mappings
    char maps_path[256];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *maps = fopen(maps_path, "r");
    if (!maps) {
        perror("[injector] Failed to open maps");
        return 1;
    }

    char line[512];
    long long total_marked_bytes = 0;
    long long total_failed_bytes = 0;
    int success_calls = 0;
    int failed_calls = 0;

    while (fgets(line, sizeof(line), maps)) {
        unsigned long start, end;
        char prot[5];
        
        // Parse start-end address and protection (e.g., "00400000-00452000 r-xp")
        if (sscanf(line, "%lx-%lx %4s", &start, &end, prot) == 3) {
            // We want to mark readable/writable private (p) regions.
            // prot[0] == 'r' (readable), prot[3] == 'p' (private, i.e., not shared 's')
            if (prot[0] == 'r' && prot[3] == 'p') {
                // Ignore file-backed mappings (we only want anonymous allocations and heap)
                char *path = strchr(line, '/');
                if (!path || strstr(line, "[heap]")) {
                    unsigned long len = end - start;
                    int ret = inject_madvise_ptrace(pid, start, len);
                    if (ret < 0) {
                        total_failed_bytes += len;
                        failed_calls++;
                    } else {
                        total_marked_bytes += len;
                        success_calls++;
                    }
                }
            }
        }
    }

    fclose(maps);
    
    printf("[injector] PID %d (ptrace): marked=%lld KB, failed=%lld KB, ok_calls=%d, err_calls=%d\n", 
           pid, total_marked_bytes / 1024, total_failed_bytes / 1024, success_calls, failed_calls);
    
    if (failed_calls > 0 && success_calls == 0) {
        return 2;
    }
    return 0;
}
