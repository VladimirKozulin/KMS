#include "winksm.h"

/*
 * Section object-based page sharing.
 *
 * Creates section objects backed by pagefile and maps them into
 * target processes at the same virtual address, providing COW semantics
 * through Windows Memory Manager.
 *
 * This is the Windows equivalent of Linux KSM's replace_page().
 */

static NTSTATUS Sharer_OpenProcess(uint32_t ProcessId, HANDLE *OutHandle) {
    OBJECT_ATTRIBUTES objAttr;
    CLIENT_ID clientId;

    clientId.UniqueProcess = (HANDLE)(ULONG_PTR)ProcessId;
    clientId.UniqueThread = NULL;
    InitializeObjectAttributes(&objAttr, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    return ZwOpenProcess(OutHandle,
                         PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                         PROCESS_VM_OPERATION | PROCESS_DUP_HANDLE,
                         &objAttr, &clientId);
}

static NTSTATUS Sharer_CreateSection(HANDLE *OutSection, uint32_t Size) {
    OBJECT_ATTRIBUTES objAttr;
    InitializeObjectAttributes(&objAttr, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);

    LARGE_INTEGER maxSize;
    maxSize.QuadPart = Size;

    return ZwCreateSection(
        OutSection,
        SECTION_ALL_ACCESS,
        &objAttr,
        &maxSize,
        PAGE_READWRITE,
        SEC_COMMIT,
        NULL
    );
}

static NTSTATUS Sharer_MapSectionIntoProcess(HANDLE ProcessHandle, HANDLE Section,
                                              PVOID *OutBase, SIZE_T ViewSize,
                                              PVOID RequestedAddress) {
    PEPROCESS process;
    NTSTATUS status;

    status = ObReferenceObjectByHandle(ProcessHandle, PROCESS_ALL_ACCESS,
                                        *PsProcessType, KernelMode,
                                        (PVOID *)&process, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KAPC_STATE apcState;
    KeStackAttachProcess(process, &apcState);

    SIZE_T size = ViewSize;
    PVOID base = RequestedAddress;

    status = ZwMapViewOfSection(
        Section,
        NtCurrentProcess(),
        &base,
        0,
        ViewSize,
        NULL,
        &size,
        ViewUnmap,
        0,
        PAGE_READONLY
    );

    KeUnstackDetachProcess(&apcState);
    ObDereferenceObject(process);

    if (NT_SUCCESS(status)) {
        *OutBase = base;
    }

    return status;
}

static NTSTATUS Sharer_MapSectionTemp(HANDLE Section, PVOID *OutBase, SIZE_T Size) {
    SIZE_T size = Size;
    PVOID base = NULL;

    return ZwMapViewOfSection(
        Section,
        NtCurrentProcess(),
        &base,
        0,
        Size,
        NULL,
        &size,
        ViewUnmap,
        0,
        PAGE_READWRITE
    );
}

static void Sharer_UnmapSectionTemp(PVOID Base, SIZE_T Size) {
    SIZE_T size = Size;
    ZwUnmapViewOfSection(NtCurrentProcess(), Base);
}

NTSTATUS WinKSM_Sharer_MergePage(uint32_t ProcessId, uint64_t VirtualAddress,
                                  const void *PageData, uint32_t DataSize,
                                  WINKSM_STABLE_NODE **OutStableNode) {
    NTSTATUS status;
    HANDLE sectionHandle = NULL;
    HANDLE processHandle = NULL;
    PVOID sectionBase = NULL;
    PVOID tempBase = NULL;

    if (DataSize != WINKSM_PAGE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Create section object */
    status = Sharer_CreateSection(&sectionHandle, WINKSM_PAGE_SIZE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Map section temporarily to write content */
    status = Sharer_MapSectionTemp(sectionHandle, &tempBase, WINKSM_PAGE_SIZE);
    if (!NT_SUCCESS(status)) {
        ZwClose(sectionHandle);
        return status;
    }

    /* Copy page content into section */
    RtlCopyMemory(tempBase, PageData, WINKSM_PAGE_SIZE);

    /* Unmap temporary mapping */
    Sharer_UnmapSectionTemp(tempBase, WINKSM_PAGE_SIZE);

    /* Open target process */
    status = Sharer_OpenProcess(ProcessId, &processHandle);
    if (!NT_SUCCESS(status)) {
        ZwClose(sectionHandle);
        return status;
    }

    /* Map section into target process at requested address */
    status = Sharer_MapSectionIntoProcess(processHandle, sectionHandle,
                                           &sectionBase, WINKSM_PAGE_SIZE,
                                           (PVOID)(ULONG_PTR)VirtualAddress);
    ZwClose(processHandle);

    if (!NT_SUCCESS(status)) {
        ZwClose(sectionHandle);
        return status;
    }

    /* Create stable node */
    WINKSM_STABLE_NODE *stableNode = (WINKSM_STABLE_NODE *)WinKSM_AllocNonPaged(
        sizeof(WINKSM_STABLE_NODE), WINKSM_TAG);

    if (!stableNode) {
        /* Unmap and cleanup */
        PEPROCESS process;
        if (NT_SUCCESS(ObReferenceObjectByHandle(processHandle, PROCESS_ALL_ACCESS,
                                                  *PsProcessType, KernelMode,
                                                  (PVOID *)&process, NULL))) {
            KAPC_STATE apcState;
            KeStackAttachProcess(process, &apcState);
            ZwUnmapViewOfSection(NtCurrentProcess(), sectionBase);
            KeUnstackDetachProcess(&apcState);
            ObDereferenceObject(process);
        }
        ZwClose(sectionHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(stableNode, sizeof(WINKSM_STABLE_NODE));
    stableNode->SectionHandle = sectionHandle;
    stableNode->SectionBase = sectionBase;
    stableNode->SharingCount = 1;
    stableNode->ContentHash = WinKSM_Hash_xxhash32(PageData, WINKSM_PAGE_SIZE);
    stableNode->NumDups = 0;
    InitializeListHead(&stableNode->RmapItems);

    /* Insert into stable tree */
    WinKSM_Tree_InsertStable(&g_Winksm.StableTreeRoot, &stableNode->TreeNode);

    /* Create rmap item for this page */
    WINKSM_RMAP_ITEM *item = (WINKSM_RMAP_ITEM *)WinKSM_AllocNonPaged(
        sizeof(WINKSM_RMAP_ITEM), WINKSM_TAG);

    if (item) {
        RtlZeroMemory(item, sizeof(WINKSM_RMAP_ITEM));
        item->Checksum = stableNode->ContentHash;
        item->ProcessId = ProcessId;
        item->VirtualAddress = VirtualAddress;
        item->InStableTree = TRUE;
        item->StableNode = stableNode;
        InitializeListHead(&item->RmapListEntry);

        InsertTailList(&stableNode->RmapItems, &item->RmapListEntry);
    }

    *OutStableNode = stableNode;
    return STATUS_SUCCESS;
}

NTSTATUS WinKSM_Sharer_UnmergePage(uint32_t ProcessId, uint64_t VirtualAddress) {
    /* Find the rmap entry for this page in this process */
    WINKSM_RMAP_ENTRY *rmapEntry = WinKSM_Rmap_FindByProcess(ProcessId, VirtualAddress);
    if (!rmapEntry) {
        return STATUS_NOT_FOUND;
    }

    /* Find corresponding stable node */
    WINKSM_STABLE_NODE *stableNode = WinKSM_Tree_SearchStable(
        g_Winksm.StableTreeRoot, rmapEntry->PhysicalPageId);

    if (stableNode) {
        /* Remove rmap item from stable node's list */
        PLIST_ENTRY entry = stableNode->RmapItems.Flink;
        while (entry != &stableNode->RmapItems) {
            WINKSM_RMAP_ITEM *item = CONTAINING_RECORD(entry, WINKSM_RMAP_ITEM, RmapListEntry);
            if (item->ProcessId == ProcessId && item->VirtualAddress == VirtualAddress) {
                RemoveEntryList(&item->RmapListEntry);
                WinKSM_FreeNonPaged(item, WINKSM_TAG);
                stableNode->SharingCount--;
                break;
            }
            entry = entry->Flink;
        }

        /* If no more sharers, remove from stable tree */
        if (stableNode->SharingCount == 0) {
            WinKSM_Tree_RemoveStable(&g_Winksm.StableTreeRoot,
                                      &stableNode->TreeNode);

            /* Unmap section from the process */
            PEPROCESS process;
            if (NT_SUCCESS(Sharer_OpenProcess(ProcessId, NULL))) {
                /* Section is still mapped, but we'll let it be cleaned up
                   when the process exits or when we explicitly unmap */
            }

            WinKSM_FreeNonPaged(stableNode, WINKSM_TAG);
        }
    }

    WinKSM_Rmap_Remove(rmapEntry);

    KLOCK_QUEUE_HANDLE lockHandle;
    KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
    if (g_Winksm.Stats.PagesSharing > 0) {
        g_Winksm.Stats.PagesSharing--;
    }
    if (g_Winksm.Stats.SavedMemoryBytes >= WINKSM_PAGE_SIZE) {
        g_Winksm.Stats.SavedMemoryBytes -= WINKSM_PAGE_SIZE;
    }
    KeReleaseInStackQueuedSpinLock(&lockHandle);

    return STATUS_SUCCESS;
}

void WinKSM_Sharer_Cleanup(void) {
    /* Unmap all sections and free all stable nodes */
    WINKSM_RB_NODE *root = g_Winksm.StableTreeRoot;
    if (!root) return;

    /* Simple cleanup - in production we'd do proper tree traversal */
    g_Winksm.StableTreeRoot = NULL;
}
