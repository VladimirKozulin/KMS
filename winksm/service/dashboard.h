#ifndef WINKSM_DASHBOARD_H
#define WINKSM_DASHBOARD_H

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>

#include "../shared/winksm_types.h"
#include "../shared/winksm_protocol.h"

#pragma comment(lib, "ws2_32.lib")

#define WINKSM_DASH_BUF_SIZE 65536

typedef struct _DASHBOARD_CONTEXT {
    SOCKET ListenSocket;
    HANDLE DeviceHandle;
    BOOL Running;
    HANDLE ThreadHandle;
    int Port;
} DASHBOARD_CONTEXT;

static DASHBOARD_CONTEXT g_DashCtx = {0};

/* Forward declarations */
static DWORD WINAPI Dashboard_AcceptThread(LPVOID param);
static void Dashboard_HandleClient(SOCKET clientSocket, HANDLE device);
static void Dashboard_SendResponse(SOCKET sock, int statusCode,
                                    const char *contentType, const char *body);
static void Dashboard_SendStaticFile(SOCKET sock, const char *path);
static BOOL Dashboard_HandleApiStatus(SOCKET sock, HANDLE device);
static BOOL Dashboard_HandleApiToggle(SOCKET sock, HANDLE device, const char *body);
static BOOL Dashboard_HandleApiTune(SOCKET sock, HANDLE device, const char *body);

static const char* Dashboard_GetHttpStatus(int code) {
    switch (code) {
    case 200: return "OK";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "Unknown";
    }
}

static void Dashboard_SendResponse(SOCKET sock, int statusCode,
                                    const char *contentType, const char *body) {
    char header[512];
    int bodyLen = (int)strlen(body);
    _snprintf_s(header, sizeof(header), _TRUNCATE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        statusCode, Dashboard_GetHttpStatus(statusCode),
        contentType, bodyLen);

    send(sock, header, (int)strlen(header), 0);
    send(sock, body, bodyLen, 0);
}

static BOOL Dashboard_HandleApiStatus(SOCKET sock, HANDLE device) {
    WINKSM_STATS stats = {0};
    WINKSM_STATUS_RESPONSE status = {0};

    if (device != INVALID_HANDLE_VALUE) {
        DWORD bytesReturned;
        DeviceIoControl(device, IOCTL_WINKSM_GET_STATS,
                        NULL, 0, &stats, sizeof(stats), &bytesReturned, NULL);
        DeviceIoControl(device, IOCTL_WINKSM_GET_STATUS,
                        NULL, 0, &status, sizeof(status), &bytesReturned, NULL);
    }

    char json[2048];
    _snprintf_s(json, sizeof(json), _TRUNCATE,
        "{"
        "\"ksmSupported\":true,"
        "\"ksmRunning\":%s,"
        "\"pagesShared\":%llu,"
        "\"pagesSharing\":%llu,"
        "\"pagesUnshared\":%llu,"
        "\"pagesVolatile\":%llu,"
        "\"pagesScanned\":%llu,"
        "\"fullScans\":%llu,"
        "\"savedMemoryMb\":%.2f,"
        "\"monthlySavingsUsd\":%.2f,"
        "\"systemTotalMemory\":0,"
        "\"systemAvailableMemory\":0,"
        "\"systemMemoryUsagePercent\":0,"
        "\"processCount\":%u,"
        "\"zeroPagesShared\":%llu"
        "}",
        status.IsRunning ? "true" : "false",
        (unsigned long long)stats.PagesShared,
        (unsigned long long)stats.PagesSharing,
        (unsigned long long)stats.PagesUnshared,
        (unsigned long long)stats.PagesVolatile,
        (unsigned long long)stats.PagesScanned,
        (unsigned long long)stats.FullScans,
        stats.SavedMemoryBytes / (1024.0 * 1024.0),
        stats.MonthlySavingsUsd,
        status.ProcessCount,
        (unsigned long long)stats.ZeroPagesShared
    );

    Dashboard_SendResponse(sock, 200, "application/json", json);
    return TRUE;
}

static BOOL Dashboard_HandleApiToggle(SOCKET sock, HANDLE device, const char *body) {
    /* Parse JSON: {"run": true/false} */
    const char *runStr = strstr(body, "\"run\"");
    if (!runStr) {
        Dashboard_SendResponse(sock, 400, "application/json", "{\"error\":\"Missing 'run' field\"}");
        return FALSE;
    }

    BOOL run = (strstr(runStr, "true") != NULL);

    if (device != INVALID_HANDLE_VALUE) {
        DWORD bytesReturned;
        if (run) {
            DeviceIoControl(device, IOCTL_WINKSM_START_SCAN,
                            NULL, 0, NULL, 0, &bytesReturned, NULL);
        } else {
            DeviceIoControl(device, IOCTL_WINKSM_STOP_SCAN,
                            NULL, 0, NULL, 0, &bytesReturned, NULL);
        }
    }

    Dashboard_SendResponse(sock, 200, "application/json", "{\"success\":true}");
    return TRUE;
}

static BOOL Dashboard_HandleApiTune(SOCKET sock, HANDLE device, const char *body) {
    WINKSM_PARAMETERS params = {0};

    /* Simple JSON parsing */
    const char *val;

    val = strstr(body, "\"pagesToScan\"");
    if (val) {
        val = strchr(val, ':');
        if (val) params.PagesToScan = (uint32_t)atoi(val + 1);
    }

    val = strstr(body, "\"sleepMillisecs\"");
    if (val) {
        val = strchr(val, ':');
        if (val) params.SleepMilliseconds = (uint32_t)atoi(val + 1);
    }

    val = strstr(body, "\"useZeroPages\"");
    if (val) {
        params.UseZeroPages = (strstr(val, "true") != NULL) ? 1 : 0;
    }

    val = strstr(body, "\"autoTune\"");
    if (val) {
        /* Auto-tune handled by service logic */
    }

    if (device != INVALID_HANDLE_VALUE) {
        DWORD bytesReturned;
        DeviceIoControl(device, IOCTL_WINKSM_SET_PARAMETERS,
                        &params, sizeof(params), NULL, 0, &bytesReturned, NULL);
    }

    Dashboard_SendResponse(sock, 200, "application/json", "{\"success\":true}");
    return TRUE;
}

static void Dashboard_HandleClient(SOCKET clientSocket, HANDLE device) {
    char buffer[WINKSM_DASH_BUF_SIZE];
    int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

    if (bytesReceived <= 0) {
        closesocket(clientSocket);
        return;
    }

    buffer[bytesReceived] = '\0';

    /* Parse request line */
    char method[16] = {0};
    char path[256] = {0};
    sscanf_s(buffer, "%15s %255s", method, (unsigned)sizeof(method), path, (unsigned)sizeof(path));

    /* Check for API routes */
    if (strcmp(path, "/api/status") == 0 && strcmp(method, "GET") == 0) {
        Dashboard_HandleApiStatus(clientSocket, device);
    } else if (strcmp(path, "/api/ksm/toggle") == 0 && strcmp(method, "POST") == 0) {
        /* Extract body */
        const char *body = strstr(buffer, "\r\n\r\n");
        if (body) body += 4;
        Dashboard_HandleApiToggle(clientSocket, device, body ? body : "{}");
    } else if (strcmp(path, "/api/ksm/tune") == 0 && strcmp(method, "POST") == 0) {
        const char *body = strstr(buffer, "\r\n\r\n");
        if (body) body += 4;
        Dashboard_HandleApiTune(clientSocket, device, body ? body : "{}");
    } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        Dashboard_SendStaticFile(clientSocket, "web/index.html");
    } else if (strcmp(path, "/style.css") == 0) {
        Dashboard_SendStaticFile(clientSocket, "web/style.css");
    } else if (strcmp(path, "/app.js") == 0) {
        Dashboard_SendStaticFile(clientSocket, "web/app.js");
    } else {
        Dashboard_SendResponse(clientSocket, 404, "text/plain", "Not Found");
    }

    closesocket(clientSocket);
}

static void Dashboard_SendStaticFile(SOCKET sock, const char *path) {
    FILE *f = NULL;
    fopen_s(&f, path, "rb");
    if (!f) {
        /* Try relative to executable */
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        char *lastSlash = strrchr(exePath, '\\');
        if (lastSlash) {
            *(lastSlash + 1) = '\0';
            strcat_s(exePath, path);
            fopen_s(&f, exePath, "rb");
        }
    }

    if (!f) {
        Dashboard_SendResponse(sock, 404, "text/plain", "File not found");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = (char *)malloc(size + 1);
    if (content) {
        fread(content, 1, size, f);
        content[size] = '\0';

        const char *contentType = "text/plain";
        if (strstr(path, ".html")) contentType = "text/html; charset=utf-8";
        else if (strstr(path, ".css")) contentType = "text/css; charset=utf-8";
        else if (strstr(path, ".js")) contentType = "application/javascript; charset=utf-8";

        Dashboard_SendResponse(sock, 200, contentType, content);
        free(content);
    }

    fclose(f);
}

static DWORD WINAPI Dashboard_AcceptThread(LPVOID param) {
    UNREFERENCED_PARAMETER(param);

    while (g_DashCtx.Running) {
        SOCKET clientSocket = accept(g_DashCtx.ListenSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) {
            if (g_DashCtx.Running) {
                continue;
            }
            break;
        }

        Dashboard_HandleClient(clientSocket, g_DashCtx.DeviceHandle);
    }

    return 0;
}

static int Dashboard_Start(int port, HANDLE device) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return -1;
    }

    g_DashCtx.ListenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_DashCtx.ListenSocket == INVALID_SOCKET) {
        WSACleanup();
        return -1;
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(g_DashCtx.ListenSocket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_LOOPBACK; /* localhost only */
    addr.sin_port = htons((u_short)port);

    if (bind(g_DashCtx.ListenSocket, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(g_DashCtx.ListenSocket);
        WSACleanup();
        return -1;
    }

    if (listen(g_DashCtx.ListenSocket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(g_DashCtx.ListenSocket);
        WSACleanup();
        return -1;
    }

    g_DashCtx.DeviceHandle = device;
    g_DashCtx.Port = port;
    g_DashCtx.Running = TRUE;

    g_DashCtx.ThreadHandle = CreateThread(NULL, 0, Dashboard_AcceptThread,
                                           NULL, 0, NULL);
    if (!g_DashCtx.ThreadHandle) {
        closesocket(g_DashCtx.ListenSocket);
        WSACleanup();
        return -1;
    }

    return 0;
}

static void Dashboard_Stop(void) {
    g_DashCtx.Running = FALSE;

    /* Close listen socket to unblock accept() */
    if (g_DashCtx.ListenSocket != INVALID_SOCKET) {
        closesocket(g_DashCtx.ListenSocket);
        g_DashCtx.ListenSocket = INVALID_SOCKET;
    }

    if (g_DashCtx.ThreadHandle) {
        WaitForSingleObject(g_DashCtx.ThreadHandle, 5000);
        CloseHandle(g_DashCtx.ThreadHandle);
        g_DashCtx.ThreadHandle = NULL;
    }

    WSACleanup();
}

#endif /* WINKSM_DASHBOARD_H */
