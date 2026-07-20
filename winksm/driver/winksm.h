#ifndef WINKSM_INTERNAL_H
#define WINKSM_INTERNAL_H

#include <ntddk.h>
#include <wdm.h>
#include <ntstrsafe.h>

#include "../shared/winksm_types.h"
#include "../shared/winksm_protocol.h"

#define WINKSM_TAG 'KSMw'
#define WINKSM_RMAP_TAG 'RMSw'
#define WINKSM_TREE_TAG 'TRSw'

typedef enum _WINKSM_RUN_STATE {
    WINKSM_RUN_STOPPED = 0,
    WINKSM_RUN_MERGE = 1,
    WINKSM_RUN_UNMERGE = 2,
} WINKSM_RUN_STATE;

/* Forward declarations */
typedef struct _WINKSM_STABLE_NODE WINKSM_STABLE_NODE;
typedef struct _WINKSM_RMAP_ITEM WINKSM_RMAP_ITEM;
typedef struct _WINKSM_RMAP_ENTRY WINKSM_RMAP_ENTRY;
typedef struct _WINKSM_PROCESS_ENTRY WINKSM_PROCESS_ENTRY;
typedef struct _WINKSM_GLOBALS WINKSM_GLOBALS;

/* Red-black tree node */
typedef struct _WINKSM_RB_NODE {
    struct _WINKSM_RB_NODE *Left;
    struct _WINKSM_RB_NODE *Right;
    struct _WINKSM_RB_NODE *Parent;
    BOOLEAN IsRed;
} WINKSM_RB_NODE;

/* Rmap item - tracks a single page in a process */
struct _WINKSM_RMAP_ITEM {
    WINKSM_RB_NODE      TreeNode;
    uint64_t            Checksum;
    uint32_t            PreviousChecksum;
    uint32_t            ProcessId;
    uint64_t            VirtualAddress;
    uint8_t             Age;
    uint8_t             RemainingSkips;
    BOOLEAN             InStableTree;
    WINKSM_STABLE_NODE  *StableNode;
    LIST_ENTRY          RmapListEntry;
};

/* Stable node - represents a merged page */
struct _WINKSM_STABLE_NODE {
    WINKSM_RB_NODE      TreeNode;
    HANDLE              SectionHandle;
    PVOID               SectionBase;
    uint32_t            SharingCount;
    uint64_t            ContentHash;
    uint32_t            NumDups;
    LIST_ENTRY          RmapItems;
};

/* Rmap entry - reverse mapping from page to process */
struct _WINKSM_RMAP_ENTRY {
    LIST_ENTRY          ListEntry;
    uint64_t            PhysicalPageId;
    HANDLE              ProcessHandle;
    uint64_t            VirtualAddress;
    uint32_t            ProcessId;
    BOOLEAN             IsActive;
};

/* Per-process tracking entry */
struct _WINKSM_PROCESS_ENTRY {
    uint32_t            ProcessId;
    HANDLE              ProcessHandle;
    uint64_t            WorkingSetSize;
    uint64_t            MergingPages;
    uint32_t            RmapCount;
    LIST_ENTRY          ProcessListEntry;
    LIST_ENTRY          RmapEntries;
};

/* Scan cursor */
typedef struct _WINKSM_SCAN_CURSOR {
    uint32_t            CurrentPid;
    uint64_t            CurrentAddress;
    uint64_t            SequenceNumber;
    BOOLEAN             FullScanComplete;
    uint32_t            PagesScannedThisBatch;
} WINKSM_SCAN_CURSOR;

/* Global state */
struct _WINKSM_GLOBALS {
    PDEVICE_OBJECT      DeviceObject;
    UNICODE_STRING      DeviceName;
    UNICODE_STRING      SymlinkName;

    /* Run state */
    WINKSM_RUN_STATE    RunState;
    KSPIN_LOCK          StateLock;

    /* Worker thread */
    HANDLE              WorkerThreadHandle;
    KEVENT              WorkerStopEvent;
    KEVENT              WorkerWakeEvent;
    BOOLEAN             WorkerRunning;

    /* Parameters */
    WINKSM_PARAMETERS   Params;
    KSPIN_LOCK          ParamsLock;

    /* Statistics */
    WINKSM_STATS        Stats;
    KSPIN_LOCK          StatsLock;

    /* Stable tree (one per NUMA node, or single) */
    WINKSM_RB_NODE      *StableTreeRoot;

    /* Unstable tree */
    WINKSM_RB_NODE      *UnstableTreeRoot;

    /* Process list */
    LIST_ENTRY          ProcessList;
    KSPIN_LOCK          ProcessListLock;

    /* Rmap entries (global) */
    LIST_ENTRY          RmapGlobalList;
    KSPIN_LOCK          RmapLock;

    /* Scan cursor */
    WINKSM_SCAN_CURSOR  Cursor;

    /* Trusted PIDs (optional filter) */
    uint32_t            TrustedPids[WINKSM_MAX_PROCESSES];
    uint32_t            TrustedPidCount;
    KSPIN_LOCK          TrustedLock;
};

extern WINKSM_GLOBALS g_Winksm;

/* Driver entry (winksm_driver.c) */
DRIVER_UNLOAD WinKSM_Unload;
NTSTATUS WinKSM_CreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS WinKSM_IoControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/* Red-black tree (winksm_trees.c) */
void WinKSM_Tree_Init(void);
void WinKSM_Tree_InsertStable(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *Node);
void WinKSM_Tree_InsertUnstable(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *Node);
void WinKSM_Tree_RemoveStable(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *Node);
void WinKSM_Tree_RemoveUnstable(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *Node);
WINKSM_STABLE_NODE* WinKSM_Tree_SearchStable(WINKSM_RB_NODE *Root, uint64_t Hash);
WINKSM_RMAP_ITEM* WinKSM_Tree_SearchUnstable(WINKSM_RB_NODE *Root, uint64_t Hash);

/* Scanning engine (winksm_scanner.c) */
void WinKSM_Scan_Init(void);
NTSTATUS WinKSM_Scan_Start(void);
void WinKSM_Scan_Stop(void);
void WinKSM_Scan_WorkerRoutine(PVOID Context);

/* Hashing (winksm_hasher.c) */
uint32_t WinKSM_Hash_xxhash32(const void *Data, size_t Length);
int WinKSM_ComparePages(const void *Page1, const void *Page2);

/* Page sharing (winksm_sharer.c) */
NTSTATUS WinKSM_Sharer_MergePage(uint32_t ProcessId, uint64_t VirtualAddress,
                                  const void *PageData, uint32_t DataSize,
                                  WINKSM_STABLE_NODE **OutStableNode);
NTSTATUS WinKSM_Sharer_UnmergePage(uint32_t ProcessId, uint64_t VirtualAddress);
void WinKSM_Sharer_Cleanup(void);

/* Reverse mapping (winksm_rmap.c) */
void WinKSM_Rmap_Init(void);
WINKSM_RMAP_ENTRY* WinKSM_Rmap_Insert(uint32_t ProcessId, uint64_t VirtualAddress,
                                       uint64_t PageId, HANDLE ProcessHandle);
void WinKSM_Rmap_Remove(WINKSM_RMAP_ENTRY *Entry);
WINKSM_RMAP_ENTRY* WinKSM_Rmap_FindByPage(uint64_t PageId);
WINKSM_RMAP_ENTRY* WinKSM_Rmap_FindByProcess(uint32_t ProcessId, uint64_t VirtualAddress);
void WinKSM_Rmap_Cleanup(void);

/* Process tracking (winksm_scanner.c) */
WINKSM_PROCESS_ENTRY* WinKSM_Process_FindOrCreate(uint32_t ProcessId);
void WinKSM_Process_Remove(uint32_t ProcessId);

/* Statistics (winksm_stats.c) */
void WinKSM_Stats_Init(void);
void WinKSM_Stats_Update(void);
void WinKSM_Stats_Get(WINKSM_STATS *OutStats);

/* Utilities */
void* WinKSM_AllocNonPaged(size_t Size, ULONG Tag);
void WinKSM_FreeNonPaged(void *Ptr, ULONG Tag);

#endif /* WINKSM_INTERNAL_H */
