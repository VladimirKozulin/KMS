#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <fcntl.h>

#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif

#ifndef __NR_process_madvise
#define __NR_process_madvise 440
#endif

#define MADV_MERGEABLE 12

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <pid>\n", argv[0]);
        return 1;
    }

    int pid = atoi(argv[1]);
    
    // Open pidfd of the target process
    int pidfd = syscall(__NR_pidfd_open, pid, 0);
    if (pidfd < 0) {
        perror("[injector] pidfd_open failed");
        return 1;
    }

    // Open /proc/<pid>/maps to read memory mappings
    char maps_path[256];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *maps = fopen(maps_path, "r");
    if (!maps) {
        perror("[injector] Failed to open maps");
        close(pidfd);
        return 1;
    }

    char line[512];
    struct iovec iovs[100];
    int iov_count = 0;
    long long total_marked_bytes = 0;

    while (fgets(line, sizeof(line), maps)) {
        unsigned long start, end;
        char prot[5];
        
        // Parse start-end address and protection (e.g., "00400000-00452000 r-xp")
        if (sscanf(line, "%lx-%lx %4s", &start, &end, prot) == 3) {
            // We want to mark readable/writable private (p) regions.
            // prot[0] == 'r' (readable), prot[3] == 'p' (private, i.e., not shared 's')
            if (prot[0] == 'r' && prot[3] == 'p') {
                // Ignore file-backed mappings (we only want anonymous allocations and heap)
                // File-backed maps have absolute paths starting with '/'
                char *path = strchr(line, '/');
                if (!path || strstr(line, "[heap]")) {
                    iovs[iov_count].iov_base = (void *)start;
                    iovs[iov_count].iov_len = end - start;
                    total_marked_bytes += (end - start);
                    iov_count++;

                    // Batch system calls to process_madvise
                    if (iov_count >= 100) {
                        syscall(__NR_process_madvise, pidfd, iovs, iov_count, MADV_MERGEABLE, 0);
                        iov_count = 0;
                    }
                }
            }
        }
    }

    if (iov_count > 0) {
        syscall(__NR_process_madvise, pidfd, iovs, iov_count, MADV_MERGEABLE, 0);
    }

    fclose(maps);
    close(pidfd);
    printf("[injector] Successfully marked %lld KB of memory for PID %d as MERGEABLE\n", total_marked_bytes / 1024, pid);
    return 0;
}
