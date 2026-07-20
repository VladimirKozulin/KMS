#include "winksm.h"

/*
 * Page scanning engine - core of WinKSM.
 *
 * Iterates over all processes, reads their memory, hashes pages,
 * and feeds them to the merge logic via stable/unstable trees.
 *
 * Ported from Linux KSM's ksm_do_scan() and scan_get_next_rmap_item().
 */

#define WINKSM_MAX_PROCESSES_ENUM 4096

typedef struct _SYSTEM_PROCESS_INFORMATION {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER WorkingSetSize;
    KUSER_MODE_TIME CreationTime;
    KUSER_MODE_TIME UserTime;
    KUSER_MODE_TIME KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
} SYSTEM_PROCESS_INFORMATION;

#define SystemProcessInformation 5
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)

/* Internal scan state */
static HANDLE g_ScanThreadHandle = NULL;
static KEVENT g_ScanStopEvent;
static KEVENT g_ScanCompleteEvent;
static BOOLEAN g_ScanRunning = FALSE;

static WINKSM_RMAP_ITEM* Scan_AllocRmapItem(void) {
    return (WINKSM_RMAP_ITEM *)WinKSM_AllocNonPaged(sizeof(WINKSM_RMAP_ITEM), WINKSM_TAG);
}

static void Scan_FreeRmapItem(WINKSM_RMAP_ITEM *Item) {
    WinKSM_FreeNonPaged(Item, WINKSM_TAG);
}

static BOOLEAN Scan_IsProcessEligible(uint32_t Pid) {
    /* If no trust filter, all processes are eligible */
    if (g_Winksm.TrustedPidCount == 0) {
        return TRUE;
    }

    KLOCK_QUEUE_HANDLE lockHandle;
    KeAcquireInStackQueuedSpinLock(&g_Winksm.TrustedLock, &lockHandle);
    for (uint32_t i = 0; i < g_Winksm.TrustedPidCount; i++) {
        if (g_Winksm.TrustedPids[i] == Pid) {
            KeReleaseInStackQueuedSpinLock(&lockHandle);
            return TRUE;
        }
    }
    KeReleaseInStackQueuedSpinLock(&lockHandle);
    return FALSE;
}

static NTSTATUS Scan_EnumProcesses(SYSTEM_PROCESS_INFORMATION **OutBuffer, ULONG *OutCount) {
    ULONG bufferSize = 64 * 1024;
    SYSTEM_PROCESS_INFORMATION *buffer = NULL;
    NTSTATUS status;

    for (int retry = 0; retry < 4; retry++) {
        buffer = (SYSTEM_PROCESS_INFORMATION *)WinKSM_AllocNonPaged(bufferSize, WINKSM_TAG);
        if (!buffer) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        status = ZwQuerySystemInformation(
            SystemProcessInformation,
            buffer,
            bufferSize,
            &bufferSize
        );

        if (NT_SUCCESS(status)) {
            break;
        }

        WinKSM_FreeNonPaged(buffer, WINKSM_TAG);
        buffer = NULL;
    }

    if (!NT_SUCCESS(status)) {
        return status;
    }

    *OutBuffer = buffer;

    /* Count processes */
    ULONG count = 0;
    SYSTEM_PROCESS_INFORMATION *current = buffer;
    while (current) {
        count++;
        if (current->NextEntryOffset == 0) break;
        current = (SYSTEM_PROCESS_INFORMATION *)((PUCHAR)current + current->NextEntryOffset);
    }
    *OutCount = count;

    return STATUS_SUCCESS;
}

static NTSTATUS Scan_ReadProcessMemory(uint32_t ProcessId, uint64_t Address,
                                        void *Buffer, size_t Size, size_t *BytesRead) {
    HANDLE processHandle;
    OBJECT_ATTRIBUTES objAttr;
    CLIENT_ID clientId;

    clientId.UniqueProcess = (HANDLE)(ULONG_PTR)ProcessId;
    clientId.UniqueThread = NULL;
    InitializeObjectAttributes(&objAttr, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    NTSTATUS status = ZwOpenProcess(&processHandle, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                     &objAttr, &clientId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    SIZE_T readSize = 0;
    status = ZwReadVirtualMemory(processHandle, (PVOID)(ULONG_PTR)Address,
                                  Buffer, Size, &readSize);
    ZwClose(processHandle);

    if (BytesRead) {
        *BytesRead = readSize;
    }
    return status;
}

static NTSTATUS Scan_EnumProcessMemory(uint32_t ProcessId, uint64_t *Regions,
                                        uint32_t *RegionCount, uint32_t MaxRegions) {
    HANDLE processHandle;
    OBJECT_ATTRIBUTES objAttr;
    CLIENT_ID clientId;

    clientId.UniqueProcess = (HANDLE)(ULONG_PTR)ProcessId;
    clientId.UniqueThread = NULL;
    InitializeObjectAttributes(&objAttr, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    NTSTATUS status = ZwOpenProcess(&processHandle, PROCESS_QUERY_INFORMATION,
                                     &objAttr, &clientId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    MEMORY_BASIC_INFORMATION mbi;
    uint64_t address = 0;
    uint32_t count = 0;

    while (count < MaxRegions) {
        SIZE_T retLen = 0;
        status = ZwQueryVirtualMemory(processHandle, (PVOID)(ULONG_PTR)address,
                                       MemoryBasicInformation, &mbi, sizeof(mbi), &retLen);
        if (!NT_SUCCESS(status)) {
            break;
        }

        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & PAGE_GUARD) &&
            mbi.Type == MEM_PRIVATE) {

            uint64_t regionStart = (uint64_t)mbi.BaseAddress;
            uint64_t regionSize = mbi.RegionSize;

            /* Align to page boundaries */
            uint64_t alignedStart = regionStart & ~(WINKSM_PAGE_SIZE - 1);
            uint64_t alignedEnd = (regionStart + regionSize + WINKSM_PAGE_SIZE - 1)
                                  & ~(WINKSM_PAGE_SIZE - 1);

            /* Store as virtual address entries */
            for (uint64_t addr = alignedStart; addr < alignedEnd && count < MaxRegions; addr += WINKSM_PAGE_SIZE) {
                Regions[count++] = addr;
            }
        }

        address = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
        if (address <= (uint64_t)mbi.BaseAddress) break; /* overflow */
    }

    ZwClose(processHandle);
    *RegionCount = count;
    return STATUS_SUCCESS;
}

static void Scan_ProcessPage(uint32_t ProcessId, uint64_t VirtualAddress) {
    uint8_t pageBuffer[WINKSM_PAGE_SIZE];
    size_t bytesRead = 0;

    /* Read the page content */
    NTSTATUS status = Scan_ReadProcessMemory(ProcessId, VirtualAddress,
                                              pageBuffer, WINKSM_PAGE_SIZE, &bytesRead);
    if (!NT_SUCCESS(status) || bytesRead != WINKSM_PAGE_SIZE) {
        return;
    }

    /* Hash the page */
    uint32_t checksum = WinKSM_Hash_xxhash32(pageBuffer, WINKSM_PAGE_SIZE);

    /* Increment scanned counter */
    KLOCK_QUEUE_HANDLE lockHandle;
    KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
    g_Winksm.Stats.PagesScanned++;
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    /* Check zero page optimization */
    BOOLEAN isZeroPage = TRUE;
    for (int i = 0; i < WINKSM_PAGE_SIZE; i++) {
        if (pageBuffer[i] != 0) {
            isZeroPage = FALSE;
            break;
        }
    }

    if (isZeroPage && g_Winksm.Params.UseZeroPages) {
        /* Zero pages can be shared via special zero page */
        KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
        g_Winksm.Stats.ZeroPagesShared++;
        g_Winksm.Stats.PagesSharing++;
        g_Winksm.Stats.SavedMemoryBytes += WINKSM_PAGE_SIZE;
        KeReleaseInStackQueuedSpinLock(&lockHandle);
        return;
    }

    /* Check if already in stable tree (already merged) */
    WINKSM_STABLE_NODE *stableNode = WinKSM_Tree_SearchStable(
        g_Winksm.StableTreeRoot, checksum);

    if (stableNode) {
        /* Page is already merged. Check if this process already has a mapping */
        BOOLEAN alreadyMapped = FALSE;
        PLIST_ENTRY entry = stableNode->RmapItems.Flink;
        while (entry != &stableNode->RmapItems) {
            WINKSM_RMAP_ITEM *item = CONTAINING_RECORD(entry, WINKSM_RMAP_ITEM, RmapListEntry);
            if (item->ProcessId == ProcessId && item->VirtualAddress == VirtualAddress) {
                alreadyMapped = TRUE;
                break;
            }
            entry = entry->Flink;
        }

        if (!alreadyMapped && stableNode->SharingCount < g_Winksm.Params.MaxPageSharing) {
            /* Try to merge this process's page into the existing stable node */
            NTSTATUS mergeStatus = WinKSM_Sharer_MergePage(
                ProcessId, VirtualAddress, pageBuffer, WINKSM_PAGE_SIZE, &stableNode);

            if (NT_SUCCESS(mergeStatus)) {
                KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
                g_Winksm.Stats.PagesSharing++;
                g_Winksm.Stats.SavedMemoryBytes += WINKSM_PAGE_SIZE;
                KeReleaseInStackQueuedSpinLock(&lockHandle);
            }
        }
        return;
    }

    /* Check if already in unstable tree */
    WINKSM_RMAP_ITEM *existingItem = WinKSM_Tree_SearchUnstable(
        g_Winksm.UnstableTreeRoot, checksum);

    if (existingItem) {
        /* Found a match in unstable tree - merge both into stable tree */
        NTSTATUS mergeStatus = WinKSM_Sharer_MergePage(
            ProcessId, VirtualAddress, pageBuffer, WINKSM_PAGE_SIZE, &stableNode);

        if (NT_SUCCESS(mergeStatus) && stableNode) {
            /* Remove the existing item from unstable tree */
            WinKSM_Tree_RemoveUnstable(&g_Winksm.UnstableTreeRoot,
                                        &existingItem->TreeNode);

            /* Remove old rmap entry */
            WINKSM_RMAP_ENTRY *rmapEntry = WinKSM_Rmap_FindByProcess(
                existingItem->ProcessId, existingItem->VirtualAddress);
            if (rmapEntry) {
                WinKSM_Rmap_Remove(rmapEntry);
            }

            KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
            g_Winksm.Stats.PagesShared++;
            g_Winksm.Stats.PagesSharing++;
            g_Winksm.Stats.SavedMemoryBytes += WINKSM_PAGE_SIZE * 2;
            KeReleaseInStackQueuedSpinLock(&lockHandle);

            Scan_FreeRmapItem(existingItem);
        }
        return;
    }

    /* Not found anywhere - insert into unstable tree */
    WINKSM_RMAP_ITEM *newItem = Scan_AllocRmapItem();
    if (!newItem) return;

    RtlZeroMemory(newItem, sizeof(WINKSM_RMAP_ITEM));
    newItem->Checksum = checksum;
    newItem->ProcessId = ProcessId;
    newItem->VirtualAddress = VirtualAddress;
    newItem->Age = 0;
    newItem->RemainingSkips = 0;
    newItem->InStableTree = FALSE;
    newItem->StableNode = NULL;
    InitializeListHead(&newItem->RmapListEntry);

    WinKSM_Tree_InsertUnstable(&g_Winksm.UnstableTreeRoot, &newItem->TreeNode);

    /* Track in rmap */
    WinKSM_Rmap_Insert(ProcessId, VirtualAddress, checksum, NULL);

    KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
    g_Winksm.Stats.PagesUnshared++;
    KeReleaseInStackQueuedSpinLock(&lockHandle);
}

static void Scan_ProcessPid(uint32_t ProcessId) {
    if (!Scan_IsProcessEligible(ProcessId)) {
        return;
    }

    /* Enumerate memory regions */
    uint32_t maxRegions = 4096;
    uint64_t *regions = (uint64_t *)WinKSM_AllocNonPaged(maxRegions * sizeof(uint64_t), WINKSM_TAG);
    if (!regions) return;

    uint32_t regionCount = 0;
    NTSTATUS status = Scan_EnumProcessMemory(ProcessId, regions, &regionCount, maxRegions);

    if (NT_SUCCESS(status)) {
        for (uint32_t i = 0; i < regionCount; i++) {
            /* Check if we should stop */
            if (KeReadStateEvent(&g_ScanStopEvent)) {
                break;
            }

            /* Smart scan: skip volatile pages */
            if (g_Winksm.Params.SmartScan) {
                /* Check if this address has been scanned before */
                WINKSM_RMAP_ENTRY *existing = WinKSM_Rmap_FindByProcess(
                    ProcessId, regions[i]);
                if (existing && existing->IsActive) {
                    /* Already tracked, skip */
                    continue;
                }
            }

            Scan_ProcessPage(ProcessId, regions[i]);
            g_Winksm.Cursor.PagesScannedThisBatch++;
        }
    }

    WinKSM_FreeNonPaged(regions, WINKSM_TAG);
}

static void Scan_FlushUnstableTree(void) {
    /* Called at the end of a full scan - flush all unstable entries */
    WINKSM_RB_NODE *root = g_Winksm.UnstableTreeRoot;
    if (!root || root == &g_Sentinel) return;

    /* Simple iterative cleanup - walk the tree and free all items */
    /* In production, we'd use a stack-based traversal, but for simplicity
       we rebuild by clearing the root */
    g_Winksm.UnstableTreeRoot = NULL;

    /* Note: items are not freed here - they remain in the rmap for tracking.
       In a full implementation, we'd iterate and age them. */
}

NTSTATUS WinKSM_Scan_Start(void) {
    KLOCK_QUEUE_HANDLE lockHandle;

    KeAcquireInStackQueuedSpinLock(&g_Winksm.StateLock, &lockHandle);
    if (g_Winksm.RunState == WINKSM_RUN_MERGE) {
        KeReleaseInStackQueuedSpinLock(&lockHandle);
        return STATUS_SUCCESS; /* Already running */
    }
    g_Winksm.RunState = WINKSM_RUN_MERGE;
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    KeClearEvent(&g_ScanStopEvent);
    KeSetEvent(&g_Winksm.WorkerWakeEvent, 0, FALSE);

    return STATUS_SUCCESS;
}

void WinKSM_Scan_Stop(void) {
    KLOCK_QUEUE_HANDLE lockHandle;

    KeAcquireInStackQueuedSpinLock(&g_Winksm.StateLock, &lockHandle);
    if (g_Winksm.RunState == WINKSM_RUN_STOPPED) {
        KeReleaseInStackQueuedSpinLock(&lockHandle);
        return;
    }
    g_Winksm.RunState = WINKSM_RUN_STOPPED;
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    KeSetEvent(&g_ScanStopEvent, 0, FALSE);

    if (g_ScanThreadHandle) {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -50000000LL; /* 5 seconds */
        KeWaitForSingleObject(g_ScanThreadHandle, Executive, KernelMode, FALSE, &timeout);
        ZwClose(g_ScanThreadHandle);
        g_ScanThreadHandle = NULL;
    }
}

static NTSTATUS Scan_ThreadRoutine(void *Context) {
    UNREFERENCED_PARAMETER(Context);

    LARGE_INTEGER timeout;

    while (TRUE) {
        /* Wait for start signal or stop */
        timeout.QuadPart = -10000000LL; /* 1 second */
        KeWaitForSingleObject(&g_Winksm.WorkerWakeEvent, Executive, KernelMode,
                              FALSE, &timeout);

        /* Check for stop */
        if (KeReadStateEvent(&g_ScanStopEvent)) {
            break;
        }

        /* Only scan if running */
        if (g_Winksm.RunState != WINKSM_RUN_MERGE) {
            continue;
        }

        /* Enumerate all processes */
        SYSTEM_PROCESS_INFORMATION *procInfo = NULL;
        ULONG procCount = 0;
        NTSTATUS status = Scan_EnumProcesses(&procInfo, &procCount);

        if (NT_SUCCESS(status) && procInfo) {
            /* Update process count */
            KLOCK_QUEUE_HANDLE lockHandle;
            KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
            g_Winksm.Stats.ProcessCount = procCount;
            KeReleaseInStackQueuedSpinLock(&lockHandle);

            /* Scan each process */
            SYSTEM_PROCESS_INFORMATION *current = procInfo;
            while (current) {
                if (KeReadStateEvent(&g_ScanStopEvent)) break;

                uint32_t pid = (uint32_t)(ULONG_PTR)current->UniqueProcessId;
                if (pid > 0 && pid != 4) { /* Skip System and Idle */
                    Scan_ProcessPid(pid);
                }

                if (current->NextEntryOffset == 0) break;
                current = (SYSTEM_PROCESS_INFORMATION *)((PUCHAR)current + current->NextEntryOffset);
            }

            WinKSM_FreeNonPaged(procInfo, WINKSM_TAG);
        }

        /* Check if full scan completed */
        if (!KeReadStateEvent(&g_ScanStopEvent)) {
            g_Winksm.Cursor.SequenceNumber++;
            g_Winksm.Cursor.FullScanComplete = TRUE;

            /* Flush unstable tree at end of full scan */
            Scan_FlushUnstableTree();

            KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
            g_Winksm.Stats.FullScans++;
            KeReleaseInStackQueuedSpinLock(&lockHandle);
        }

        /* Sleep between batches */
        timeout.QuadPart = -(LONGLONG)g_Winksm.Params.SleepMilliseconds * 10000;
        KeWaitForSingleObject(&g_ScanStopEvent, Executive, KernelMode,
                              FALSE, &timeout);
    }

    g_Winksm.WorkerRunning = FALSE;
    return STATUS_SUCCESS;
}

void WinKSM_Scan_Init(void) {
    KeInitializeEvent(&g_ScanStopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&g_ScanCompleteEvent, NotificationEvent, FALSE);

    RtlZeroMemory(&g_Winksm.Cursor, sizeof(WINKSM_SCAN_CURSOR));
}
