#include "winksm.h"

WINKSM_GLOBALS g_Winksm = {0};

void* WinKSM_AllocNonPaged(size_t Size, ULONG Tag) {
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, Tag);
}

void WinKSM_FreeNonPaged(void *Ptr, ULONG Tag) {
    if (Ptr) {
        ExFreePoolWithTag(Ptr, Tag);
    }
}

static NTSTATUS WinKSM_CreateDevice(void) {
    NTSTATUS status;
    UNICODE_STRING devName;
    UNICODE_STRING symLink;

    RtlInitUnicodeString(&devName, WINKSM_DEVICE_NAME);
    RtlInitUnicodeString(&symLink, WINKSM_SYMLINK_NAME);

    status = IoCreateDevice(
        NULL,
        0,
        &devName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_Winksm.DeviceObject
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = IoCreateSymbolicLink(&symLink, &devName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_Winksm.DeviceObject);
        g_Winksm.DeviceObject = NULL;
        return status;
    }

    g_Winksm.DeviceObject->Flags |= DO_BUFFERED_IO;
    g_Winksm.DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    g_Winksm.DeviceName = devName;
    g_Winksm.SymlinkName = symLink;

    return STATUS_SUCCESS;
}

static void WinKSM_DeleteDevice(void) {
    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, WINKSM_SYMLINK_NAME);

    if (g_Winksm.DeviceObject) {
        IoDeleteSymbolicLink(&symLink);
        IoDeleteDevice(g_Winksm.DeviceObject);
        g_Winksm.DeviceObject = NULL;
    }
}

NTSTATUS WinKSM_CreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;

    if (irpSp->MajorFunction == IRP_MJ_CREATE) {
        /* Allow access - check could be added for access control */
    } else if (irpSp->MajorFunction == IRP_MJ_CLOSE) {
        /* Cleanup if needed */
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS WinKSM_IoControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bytesReturned = 0;
    void *buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG inLen = irpSp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = irpSp->Parameters.DeviceIoControl.OutputBufferLength;

    switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {

    case IOCTL_WINKSM_GET_VERSION: {
        if (outLen >= sizeof(WINKSM_VERSION)) {
            WINKSM_VERSION *ver = (WINKSM_VERSION *)buffer;
            ver->Major = WINKSM_VERSION_MAJOR;
            ver->Minor = WINKSM_VERSION_MINOR;
            ver->Build = 0;
            bytesReturned = sizeof(WINKSM_VERSION);
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }

    case IOCTL_WINKSM_GET_STATUS: {
        if (outLen >= sizeof(WINKSM_STATUS_RESPONSE)) {
            WINKSM_STATUS_RESPONSE *resp = (WINKSM_STATUS_RESPONSE *)buffer;
            resp->IsRunning = (g_Winksm.RunState == WINKSM_RUN_MERGE) ? 1 : 0;
            resp->ProcessCount = g_Winksm.Stats.ProcessCount;
            resp->TotalSavedBytes = g_Winksm.Stats.SavedMemoryBytes;
            bytesReturned = sizeof(WINKSM_STATUS_RESPONSE);
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }

    case IOCTL_WINKSM_SET_PARAMETERS: {
        if (inLen >= sizeof(WINKSM_PARAMETERS)) {
            WINKSM_PARAMETERS *params = (WINKSM_PARAMETERS *)buffer;
            KLOCK_QUEUE_HANDLE lockHandle;
            KeAcquireInStackQueuedSpinLock(&g_Winksm.ParamsLock, &lockHandle);
            g_Winksm.Params.PagesToScan = params->PagesToScan;
            g_Winksm.Params.SleepMilliseconds = params->SleepMilliseconds;
            g_Winksm.Params.UseZeroPages = params->UseZeroPages;
            g_Winksm.Params.SmartScan = params->SmartScan;
            g_Winksm.Params.MaxPageSharing = params->MaxPageSharing;
            KeReleaseInStackQueuedSpinLock(&lockHandle);

            /* Wake worker if sleep changed */
            KeSetEvent(&g_Winksm.WorkerWakeEvent, 0, FALSE);
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }

    case IOCTL_WINKSM_START_SCAN: {
        status = WinKSM_Scan_Start();
        break;
    }

    case IOCTL_WINKSM_STOP_SCAN: {
        WinKSM_Scan_Stop();
        break;
    }

    case IOCTL_WINKSM_GET_STATS: {
        if (outLen >= sizeof(WINKSM_STATS)) {
            WinKSM_Stats_Update();
            KLOCK_QUEUE_HANDLE lockHandle;
            KeAcquireInStackQueuedSpinLock(&g_Winksm.StatsLock, &lockHandle);
            RtlCopyMemory(buffer, &g_Winksm.Stats, sizeof(WINKSM_STATS));
            KeReleaseInStackQueuedSpinLock(&lockHandle);
            bytesReturned = sizeof(WINKSM_STATS);
        } else {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    }

    case IOCTL_WINKSM_ADD_TRUSTED_PID: {
        if (inLen >= sizeof(WINKSM_PID_REQUEST)) {
            WINKSM_PID_REQUEST *req = (WINKSM_PID_REQUEST *)buffer;
            KLOCK_QUEUE_HANDLE lockHandle;
            KeAcquireInStackQueuedSpinLock(&g_Winksm.TrustedLock, &lockHandle);
            if (g_Winksm.TrustedPidCount < WINKSM_MAX_PROCESSES) {
                g_Winksm.TrustedPids[g_Winksm.TrustedPidCount++] = req->ProcessId;
            }
            KeReleaseInStackQueuedSpinLock(&lockHandle);
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }

    case IOCTL_WINKSM_REMOVE_TRUSTED_PID: {
        if (inLen >= sizeof(WINKSM_PID_REQUEST)) {
            WINKSM_PID_REQUEST *req = (WINKSM_PID_REQUEST *)buffer;
            KLOCK_QUEUE_HANDLE lockHandle;
            KeAcquireInStackQueuedSpinLock(&g_Winksm.TrustedLock, &lockHandle);
            for (uint32_t i = 0; i < g_Winksm.TrustedPidCount; i++) {
                if (g_Winksm.TrustedPids[i] == req->ProcessId) {
                    g_Winksm.TrustedPids[i] = g_Winksm.TrustedPids[--g_Winksm.TrustedPidCount];
                    break;
                }
            }
            KeReleaseInStackQueuedSpinLock(&lockHandle);
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesReturned;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

void WinKSM_Unload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);

    WinKSM_Scan_Stop();
    WinKSM_Sharer_Cleanup();
    WinKSM_Rmap_Cleanup();
    WinKSM_DeleteDevice();
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS status;

    /* Initialize globals */
    KeInitializeSpinLock(&g_Winksm.StateLock);
    KeInitializeSpinLock(&g_Winksm.ParamsLock);
    KeInitializeSpinLock(&g_Winksm.StatsLock);
    KeInitializeSpinLock(&g_Winksm.ProcessListLock);
    KeInitializeSpinLock(&g_Winksm.RmapLock);
    KeInitializeSpinLock(&g_Winksm.TrustedLock);

    KeInitializeEvent(&g_Winksm.WorkerStopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&g_Winksm.WorkerWakeEvent, NotificationEvent, FALSE);

    InitializeListHead(&g_Winksm.ProcessList);
    InitializeListHead(&g_Winksm.RmapGlobalList);

    /* Default parameters */
    g_Winksm.Params.PagesToScan = WINKSM_DEFAULT_PAGES_TO_SCAN;
    g_Winksm.Params.SleepMilliseconds = WINKSM_DEFAULT_SLEEP_MS;
    g_Winksm.Params.UseZeroPages = 0;
    g_Winksm.Params.SmartScan = 1;
    g_Winksm.Params.MaxPageSharing = WINKSM_DEFAULT_MAX_SHARING;
    g_Winksm.Params.TrustClassId = 0;

    /* Initialize subsystems */
    WinKSM_Tree_Init();
    WinKSM_Scan_Init();
    WinKSM_Rmap_Init();
    WinKSM_Stats_Init();

    /* Create device */
    status = WinKSM_CreateDevice();
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Set driver dispatch */
    DriverObject->MajorFunction[IRP_MJ_CREATE] = WinKSM_CreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = WinKSM_CreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WinKSM_IoControl;
    DriverObject->DriverUnload = WinKSM_Unload;

    return STATUS_SUCCESS;
}
