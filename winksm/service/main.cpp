#include <windows.h>
#include <winsvc.h>
#include <cstdio>
#include <cstring>

#include "../shared/winksm_types.h"
#include "../shared/winksm_protocol.h"

#pragma comment(lib, "advapi32.lib")

#define SERVICE_NAME L"WinKSM"
#define SERVICE_DISPLAY_NAME L"WinKSM Memory Deduplication Service"
#define SERVICE_DESCRIPTION L"Kernel Samepage Merging service for Windows"

static SERVICE_STATUS g_ServiceStatus = {0};
static SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
static HANDLE g_StopEvent = NULL;
static HANDLE g_Device = INVALID_HANDLE_VALUE;

/* HTTP Server (simple embedded HTTP) */
#include "dashboard.h"

static BOOL WinKSM_OpenDevice(void) {
    g_Device = CreateFileW(
        WINKSM_DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    return (g_Device != INVALID_HANDLE_VALUE);
}

static BOOL WinKSM_CloseDevice(void) {
    if (g_Device != INVALID_HANDLE_VALUE) {
        CloseHandle(g_Device);
        g_Device = INVALID_HANDLE_VALUE;
    }
    return TRUE;
}

static BOOL WinKSM_SendIoctl(DWORD ioctl, void *inBuf, DWORD inSize,
                              void *outBuf, DWORD outSize, DWORD *bytesReturned) {
    if (g_Device == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    DWORD returned = 0;
    BOOL result = DeviceIoControl(
        g_Device,
        ioctl,
        inBuf, inSize,
        outBuf, outSize,
        &returned,
        NULL
    );

    if (bytesReturned) {
        *bytesReturned = returned;
    }

    return result;
}

/* Service control handler */
static DWORD WINAPI ServiceCtrlHandler(DWORD ctrlCode, DWORD eventType,
                                        LPVOID eventData, LPVOID context) {
    UNREFERENCED_PARAMETER(eventType);
    UNREFERENCED_PARAMETER(eventData);
    UNREFERENCED_PARAMETER(context);

    switch (ctrlCode) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        g_ServiceStatus.dwWaitHint = 5000;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

        /* Stop the KSM scan */
        DWORD bytesReturned;
        WinKSM_SendIoctl(IOCTL_WINKSM_STOP_SCAN, NULL, 0, NULL, 0, &bytesReturned);

        /* Signal stop event */
        SetEvent(g_StopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

/* Main service thread */
static DWORD WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_StatusHandle) {
        return GetLastError();
    }

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwWin32ExitCode = NO_ERROR;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 10000;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    /* Create stop event */
    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return GetLastError();
    }

    /* Open driver device */
    if (!WinKSM_OpenDevice()) {
        /* Driver not loaded - continue anyway for dashboard-only mode */
        wprintf(L"[WARNING] WinKSM driver not found. Dashboard-only mode.\n");
    } else {
        wprintf(L"[OK] Connected to WinKSM driver.\n");

        /* Set default parameters */
        WINKSM_PARAMETERS params = {0};
        params.PagesToScan = WINKSM_DEFAULT_PAGES_TO_SCAN;
        params.SleepMilliseconds = WINKSM_DEFAULT_SLEEP_MS;
        params.UseZeroPages = 0;
        params.SmartScan = 1;
        params.MaxPageSharing = WINKSM_DEFAULT_MAX_SHARING;

        DWORD bytesReturned;
        WinKSM_SendIoctl(IOCTL_WINKSM_SET_PARAMETERS, &params, sizeof(params),
                          NULL, 0, &bytesReturned);

        /* Start scanning */
        WinKSM_SendIoctl(IOCTL_WINKSM_START_SCAN, NULL, 0, NULL, 0, &bytesReturned);
        wprintf(L"[OK] KSM scanning started.\n");
    }

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    /* Start HTTP dashboard on port 8787 */
    wprintf(L"[OK] Dashboard starting on http://localhost:8787\n");
    Dashboard_Start(8787, g_Device);

    /* Main loop - wait for stop */
    WaitForSingleObject(g_StopEvent, INFINITE);

    /* Cleanup */
    Dashboard_Stop();
    WinKSM_CloseDevice();

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    wprintf(L"[OK] Service stopped.\n");
    return NO_ERROR;
}

int wmain(int argc, wchar_t *argv[]) {
    /* Check if running as service or console */
    BOOL asService = TRUE;

    if (argc > 1) {
        if (wcscmp(argv[1], L"--console") == 0 || wcscmp(argv[1], L"-c") == 0) {
            asService = FALSE;
        }
    }

    if (asService) {
        SERVICE_TABLE_ENTRYW serviceTable[] = {
            { (LPWSTR)SERVICE_NAME, (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
            { NULL, NULL }
        };

        if (!StartServiceCtrlDispatcherW(serviceTable)) {
            wprintf(L"[ERROR] Failed to start service dispatcher. Error: %lu\n", GetLastError());
            wprintf(L"[INFO] Run with --console for console mode.\n");
            return 1;
        }
    } else {
        /* Console mode */
        wprintf(L"=== WinKSM Memory Deduplication Service ===\n");
        wprintf(L"Running in console mode. Press Ctrl+C to stop.\n\n");

        /* Initialize */
        g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

        if (!WinKSM_OpenDevice()) {
            wprintf(L"[WARNING] WinKSM driver not found. Dashboard-only mode.\n");
        } else {
            wprintf(L"[OK] Connected to WinKSM driver.\n");

            /* Set parameters and start */
            WINKSM_PARAMETERS params = {0};
            params.PagesToScan = WINKSM_DEFAULT_PAGES_TO_SCAN;
            params.SleepMilliseconds = WINKSM_DEFAULT_SLEEP_MS;
            params.UseZeroPages = 0;
            params.SmartScan = 1;
            params.MaxPageSharing = WINKSM_DEFAULT_MAX_SHARING;

            DWORD bytesReturned;
            WinKSM_SendIoctl(IOCTL_WINKSM_SET_PARAMETERS, &params, sizeof(params),
                              NULL, 0, &bytesReturned);
            WinKSM_SendIoctl(IOCTL_WINKSM_START_SCAN, NULL, 0, NULL, 0, &bytesReturned);
            wprintf(L"[OK] KSM scanning started.\n");
        }

        wprintf(L"[OK] Dashboard starting on http://localhost:8787\n\n");
        Dashboard_Start(8787, g_Device);

        /* Wait for Ctrl+C */
        SetConsoleCtrlHandler([](DWORD ctrlType) -> BOOL {
            if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
                wprintf(L"\nStopping...\n");
                SetEvent(g_StopEvent);
                return TRUE;
            }
            return FALSE;
        }, TRUE);

        WaitForSingleObject(g_StopEvent, INFINITE);

        /* Cleanup */
        DWORD bytesReturned;
        WinKSM_SendIoctl(IOCTL_WINKSM_STOP_SCAN, NULL, 0, NULL, 0, &bytesReturned);
        Dashboard_Stop();
        WinKSM_CloseDevice();

        wprintf(L"[OK] Service stopped.\n");
    }

    return 0;
}
