#include "winksm.h"

/*
 * Reverse mapping (rmap) tracking.
 *
 * Windows has no kernel-level rmap like Linux's anon_vma tree.
 * We maintain our own rmap to track which virtual addresses in which
 * processes map to a given page.
 *
 * This is used for:
 * 1. Finding all processes sharing a page (for unmerge)
 * 2. Detecting stale entries (process exited or page changed)
 * 3. Statistics collection
 */

#define WINKSM_RMAP_HASH_SIZE 1024

typedef struct _WINKSM_RMAP_HASH_BUCKET {
    LIST_ENTRY List;
    KSPIN_LOCK Lock;
} WINKSM_RMAP_HASH_BUCKET;

static WINKSM_RMAP_HASH_BUCKET g_RmapHash[WINKSM_RMAP_HASH_SIZE];

static uint32_t Rmap_HashPid(uint32_t Pid) {
    return Pid % WINKSM_RMAP_HASH_SIZE;
}

static uint32_t Rmap_HashPage(uint64_t PageId) {
    return (uint32_t)(PageId ^ (PageId >> 32)) % WINKSM_RMAP_HASH_SIZE;
}

void WinKSM_Rmap_Init(void) {
    for (int i = 0; i < WINKSM_RMAP_HASH_SIZE; i++) {
        InitializeListHead(&g_RmapHash[i].List);
        KeInitializeSpinLock(&g_RmapHash[i].Lock);
    }
}

WINKSM_RMAP_ENTRY* WinKSM_Rmap_Insert(uint32_t ProcessId, uint64_t VirtualAddress,
                                       uint64_t PageId, HANDLE ProcessHandle) {
    WINKSM_RMAP_ENTRY *entry = (WINKSM_RMAP_ENTRY *)WinKSM_AllocNonPaged(
        sizeof(WINKSM_RMAP_ENTRY), WINKSM_RMAP_TAG);

    if (!entry) {
        return NULL;
    }

    RtlZeroMemory(entry, sizeof(WINKSM_RMAP_ENTRY));
    entry->ProcessId = ProcessId;
    entry->VirtualAddress = VirtualAddress;
    entry->PhysicalPageId = PageId;
    entry->ProcessHandle = ProcessHandle;
    entry->IsActive = TRUE;
    InitializeListHead(&entry->ListEntry);

    /* Insert into global list */
    KLOCK_QUEUE_HANDLE lockHandle;
    KeAcquireInStackQueuedSpinLock(&g_Winksm.RmapLock, &lockHandle);
    InsertTailList(&g_Winksm.RmapGlobalList, &entry->ListEntry);
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    /* Insert into hash table by PID */
    uint32_t bucket = Rmap_HashPid(ProcessId);
    KeAcquireInStackQueuedSpinLock(&g_RmapHash[bucket].Lock, &lockHandle);
    InsertTailList(&g_RmapHash[bucket].List, &entry->ListEntry);
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    return entry;
}

void WinKSM_Rmap_Remove(WINKSM_RMAP_ENTRY *Entry) {
    if (!Entry) return;

    KLOCK_QUEUE_HANDLE lockHandle;

    /* Remove from global list */
    KeAcquireInStackQueuedSpinLock(&g_Winksm.RmapLock, &lockHandle);
    if (!IsListEmpty(&Entry->ListEntry)) {
        RemoveEntryList(&Entry->ListEntry);
    }
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    /* Free the entry */
    WinKSM_FreeNonPaged(Entry, WINKSM_RMAP_TAG);
}

WINKSM_RMAP_ENTRY* WinKSM_Rmap_FindByPage(uint64_t PageId) {
    KLOCK_QUEUE_HANDLE lockHandle;
    WINKSM_RMAP_ENTRY *found = NULL;

    KeAcquireInStackQueuedSpinLock(&g_Winksm.RmapLock, &lockHandle);
    PLIST_ENTRY entry = g_Winksm.RmapGlobalList.Flink;
    while (entry != &g_Winksm.RmapGlobalList) {
        WINKSM_RMAP_ENTRY *rmap = CONTAINING_RECORD(entry, WINKSM_RMAP_ENTRY, ListEntry);
        if (rmap->PhysicalPageId == PageId && rmap->IsActive) {
            found = rmap;
            break;
        }
        entry = entry->Flink;
    }
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    return found;
}

WINKSM_RMAP_ENTRY* WinKSM_Rmap_FindByProcess(uint32_t ProcessId, uint64_t VirtualAddress) {
    uint32_t bucket = Rmap_HashPid(ProcessId);
    KLOCK_QUEUE_HANDLE lockHandle;
    WINKSM_RMAP_ENTRY *found = NULL;

    KeAcquireInStackQueuedSpinLock(&g_RmapHash[bucket].Lock, &lockHandle);
    PLIST_ENTRY entry = g_RmapHash[bucket].List.Flink;
    while (entry != &g_RmapHash[bucket].List) {
        WINKSM_RMAP_ENTRY *rmap = CONTAINING_RECORD(entry, WINKSM_RMAP_ENTRY, ListEntry);
        if (rmap->ProcessId == ProcessId &&
            rmap->VirtualAddress == VirtualAddress &&
            rmap->IsActive) {
            found = rmap;
            break;
        }
        entry = entry->Flink;
    }
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    return found;
}

void WinKSM_Rmap_Cleanup(void) {
    KLOCK_QUEUE_HANDLE lockHandle;

    KeAcquireInStackQueuedSpinLock(&g_Winksm.RmapLock, &lockHandle);
    PLIST_ENTRY entry = g_Winksm.RmapGlobalList.Flink;
    while (entry != &g_Winksm.RmapGlobalList) {
        WINKSM_RMAP_ENTRY *rmap = CONTAINING_RECORD(entry, WINKSM_RMAP_ENTRY, ListEntry);
        entry = entry->Flink;
        WinKSM_FreeNonPaged(rmap, WINKSM_RMAP_TAG);
    }
    InitializeListHead(&g_Winksm.RmapGlobalList);
    KeReleaseInStackQueuedSpinLock(&lockHandle);
}
