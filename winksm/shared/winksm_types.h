#ifndef WINKSM_TYPES_H
#define WINKSM_TYPES_H

#include <stdint.h>

#define WINKSM_VERSION_MAJOR 1
#define WINKSM_VERSION_MINOR 0
#define WINKSM_DEVICE_NAME L"\\Device\\WinKSM"
#define WINKSM_SYMLINK_NAME L"\\DosDevices\\WinKSM"
#define WINKSM_DEVICE_PATH L"\\\\.\\WinKSM"

#define WINKSM_PAGE_SIZE 4096
#define WINKSM_MAX_PROCESSES 4096
#define WINKSM_MAX_PAGES_PER_SCAN 100000
#define WINKSM_DEFAULT_PAGES_TO_SCAN 100
#define WINKSM_DEFAULT_SLEEP_MS 20
#define WINKSM_DEFAULT_MAX_SHARING 256

typedef struct _WINKSM_PARAMETERS {
    uint32_t PagesToScan;
    uint32_t SleepMilliseconds;
    uint32_t UseZeroPages;
    uint32_t SmartScan;
    uint32_t MaxPageSharing;
    uint32_t TrustClassId;
} WINKSM_PARAMETERS;

typedef struct _WINKSM_STATS {
    uint64_t PagesScanned;
    uint64_t PagesShared;
    uint64_t PagesSharing;
    uint64_t PagesUnshared;
    uint64_t PagesVolatile;
    uint64_t PagesSkipped;
    uint64_t FullScans;
    uint64_t SavedMemoryBytes;
    uint64_t ZeroPagesShared;
    double   MonthlySavingsUsd;
    uint32_t ProcessCount;
    uint32_t IsRunning;
} WINKSM_STATS;

typedef struct _WINKSM_PROCESS_INFO {
    uint32_t ProcessId;
    char     ImageName[256];
    uint64_t WorkingSetSize;
    uint64_t MergingPages;
    uint32_t RmapCount;
} WINKSM_PROCESS_INFO;

typedef struct _WINKSM_CONTAINER_INFO {
    char     Id[64];
    char     Name[128];
    char     Image[256];
    uint64_t MemoryUsageBytes;
} WINKSM_CONTAINER_INFO;

#endif /* WINKSM_TYPES_H */
