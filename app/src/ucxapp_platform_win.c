/* Windows backend for ucxapp_platform.h */

#define U_CX_LOG_DEBUG 1
#define U_CX_LOG_USE_ANSI_COLOR 1

#include "ucxapp_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <conio.h>
#include <winhttp.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <psapi.h>

#include "u_cx_log.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

/* DEVPKEY_Device_BusReportedDeviceDesc, defined here to avoid INITGUID issues */
static const DEVPROPKEY MY_DEVPKEY_BusReportedDeviceDesc = {
    {0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4
};

/* FT_HANDLE in FTD2XX is a pointer despite being typedef'd as ULONG */
typedef PVOID FT_HANDLE;
typedef ULONG FT_STATUS;

#define FT_OK 0
#define FT_LIST_NUMBER_ONLY     0x80000000
#define FT_LIST_BY_INDEX        0x40000000
#define FT_OPEN_BY_DESCRIPTION   2

typedef FT_STATUS (WINAPI *PFN_FT_ListDevices)(PVOID, PVOID, DWORD);
typedef FT_STATUS (WINAPI *PFN_FT_Open)(int, FT_HANDLE*);
typedef FT_STATUS (WINAPI *PFN_FT_OpenEx)(PVOID, DWORD, FT_HANDLE*);
typedef FT_STATUS (WINAPI *PFN_FT_GetComPortNumber)(FT_HANDLE, LPLONG);
typedef FT_STATUS (WINAPI *PFN_FT_Close)(FT_HANDLE);

static HMODULE gFtd2xxModule = NULL;
static PFN_FT_ListDevices gpFT_ListDevices = NULL;
static PFN_FT_Open gpFT_Open = NULL;
static PFN_FT_OpenEx gpFT_OpenEx = NULL;
static PFN_FT_GetComPortNumber gpFT_GetComPortNumber = NULL;
static PFN_FT_Close gpFT_Close = NULL;

void ucxapp_platform_init(void)
{
    SetConsoleOutputCP(CP_UTF8);
}

void ucxapp_platform_sleep_ms(uint32_t ms)
{
    Sleep(ms);
}

uint32_t ucxapp_platform_tick_ms(void)
{
    return (uint32_t)GetTickCount64();
}

bool ucxapp_platform_clock_adjust(int64_t deltaSeconds)
{
    (void)deltaSeconds;
    return false;
}

bool ucxapp_platform_key_pressed(void)
{
    return _kbhit() != 0;
}

int ucxapp_platform_get_key(void)
{
    return _getch();
}

bool ucxapp_platform_local_ip(char *out, size_t size)
{
    static const wchar_t *const skip[] = { L"Virtual", L"vEthernet", L"Hyper-V", L"VPN", L"VMware", L"VirtualBox", L"AnyConnect" };
    DWORD flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG len = 15000;
    IP_ADAPTER_ADDRESSES *all = (IP_ADAPTER_ADDRESSES *)malloc(len);
    bool found = false;

    out[0] = '\0';
    if (all != NULL && GetAdaptersAddresses(AF_INET, flags, NULL, all, &len) == ERROR_BUFFER_OVERFLOW) {
        free(all);
        all = (IP_ADAPTER_ADDRESSES *)malloc(len);
    }
    if (all == NULL || GetAdaptersAddresses(AF_INET, flags, NULL, all, &len) != NO_ERROR) {
        free(all);
        return false;
    }
    for (IP_ADAPTER_ADDRESSES *a = all; a != NULL && !found; a = a->Next) {
        bool virt = false;

        for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
            virt = virt || wcsstr(a->FriendlyName, skip[i]) != NULL;
        }
        /* a real LAN or Wi-Fi link has a default gateway; virtual switches do not */
        if (virt || a->OperStatus != IfOperStatusUp || a->FirstGatewayAddress == NULL) {
            continue;
        }
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u != NULL && !found; u = u->Next) {
            const struct sockaddr_in *sa = (const struct sockaddr_in *)u->Address.lpSockaddr;
            char ip[INET_ADDRSTRLEN];

            if (sa->sin_family == AF_INET && inet_ntop(AF_INET, (void *)&sa->sin_addr, ip, sizeof(ip)) != NULL &&
                strncmp(ip, "127.", 4) != 0 && strncmp(ip, "169.254.", 8) != 0) {
                snprintf(out, size, "%s", ip);
                found = true;
            }
        }
    }
    free(all);
    return found;
}

static DWORD gSavedInMode, gSavedOutMode;
static bool gTermActive;

bool ucxapp_platform_terminal_enter(void)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);

    if (!GetConsoleMode(in, &gSavedInMode) || !GetConsoleMode(out, &gSavedOutMode)) {
        return false;
    }
    if (!SetConsoleMode(out, gSavedOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT)) {
        return false;
    }
    SetConsoleMode(in, gSavedInMode & ~(DWORD)ENABLE_PROCESSED_INPUT);
    gTermActive = true;
    return true;
}

void ucxapp_platform_terminal_leave(void)
{
    if (gTermActive) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), gSavedInMode);
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), gSavedOutMode);
        gTermActive = false;
    }
}

void ucxapp_platform_terminal_size(int *cols, int *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO info;

    *cols = 80;
    *rows = 24;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        *cols = info.srWindow.Right - info.srWindow.Left + 1;
        *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
}

int ucxapp_platform_read_key(void)
{
    if (!_kbhit()) {
        return -1;
    }
    int c = _getch();
    if (c == 0 || c == 0xE0) {
        switch (_getch()) {
        case 72: return UCXAPP_KEY_UP;
        case 80: return UCXAPP_KEY_DOWN;
        case 75: return UCXAPP_KEY_LEFT;
        case 77: return UCXAPP_KEY_RIGHT;
        case 71: return UCXAPP_KEY_HOME;
        case 79: return UCXAPP_KEY_END;
        case 73: return UCXAPP_KEY_PGUP;
        case 81: return UCXAPP_KEY_PGDN;
        case 83: return UCXAPP_KEY_DEL;
        case 59: return UCXAPP_KEY_F1;
        case 68: return UCXAPP_KEY_F10;
        default: return -1;
        }
    }
    return c;
}

bool ucxapp_platform_extract_zip(const char *zipPath, const char *destDir)
{
    if (strchr(zipPath, '"') != NULL || strchr(destDir, '"') != NULL) {
        return false;
    }
    CreateDirectoryA(destDir, NULL);
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "tar -xf \"%s\" -C \"%s\"", zipPath, destDir);
    return system(cmd) == 0;
}

bool ucxapp_platform_find_file(const char *dir, const char *ext, char *out, size_t outSize)
{
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool found = false;
    size_t extLen = strlen(ext);
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
            continue;
        }
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        size_t n = strlen(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            found = ucxapp_platform_find_file(path, ext, out, outSize);
        } else if (n >= extLen && _stricmp(fd.cFileName + n - extLen, ext) == 0) {
            snprintf(out, outSize, "%s", path);
            found = true;
        }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

int ucxapp_platform_list_files(const char *dir, void (*cb)(const char *name, uint64_t size, void *ctx), void *ctx)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    int count = 0;

    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            cb(fd.cFileName, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, ctx);
            count++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return count;
}

static int memRow(ucxapp_mem_t *rows, int n, int max, const char *name, uint64_t size, uint64_t used)
{
    if (n < max && size > 0) {
        snprintf(rows[n].name, sizeof(rows[n].name), "%s", name);
        rows[n].size = size;
        rows[n].used = used;
        n++;
    }
    return n;
}

int ucxapp_platform_memory(ucxapp_mem_t *areas, int max)
{
    MEMORYSTATUSEX sys = { sizeof(sys) };
    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
    ULARGE_INTEGER freeBytes, totalBytes;
    int n = 0;

    GlobalMemoryStatusEx(&sys);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        n = memRow(areas, n, max, "App RAM", sys.ullTotalPhys, pmc.WorkingSetSize);
        n = memRow(areas, n, max, "App RAM peak", sys.ullTotalPhys, pmc.PeakWorkingSetSize);
        n = memRow(areas, n, max, "App committed", sys.ullTotalPageFile, pmc.PagefileUsage);
    }
    n = memRow(areas, n, max, "System RAM", sys.ullTotalPhys, sys.ullTotalPhys - sys.ullAvailPhys);
    if (GetDiskFreeSpaceExA(NULL, &freeBytes, &totalBytes, NULL)) {
        n = memRow(areas, n, max, "Disk", totalBytes.QuadPart, totalBytes.QuadPart - freeBytes.QuadPart);
    }
    return n;
}

int ucxapp_platform_tasks(ucxapp_mem_t *tasks, int max)
{
    (void)tasks;
    (void)max;
    return 0;
}

void ucxapp_platform_exe_dir(char *buffer, size_t bufferSize)
{
    GetModuleFileNameA(NULL, buffer, (DWORD)bufferSize);
    char *lastSlash = strrchr(buffer, '\\');
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    }
}

static char *winHttpGet(const char *host, const char *path, bool binary, size_t *outSize)
{
    wchar_t wHost[256];
    wchar_t wPath[512];
    HINTERNET hSession = NULL;
    HINTERNET hConnect = NULL;
    HINTERNET hRequest = NULL;
    char *buffer = NULL;
    char *result = NULL;
    size_t totalSize = 0;

    if (outSize) *outSize = 0;
    if (mbstowcs(wHost, host, 256) == (size_t)-1 || mbstowcs(wPath, path, 512) == (size_t)-1) {
        return NULL;
    }

    hSession = WinHttpOpen(L"u-connectXpressApp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto cleanup;

    hConnect = WinHttpConnect(hSession, wHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) goto cleanup;

    hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath, NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) goto cleanup;

    /* GitHub release downloads redirect to a CDN */
    DWORD dwRedirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &dwRedirectPolicy, sizeof(dwRedirectPolicy));

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto cleanup;
    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;
    {
        DWORD status = 0, len = sizeof(status);
        if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX) ||
            status != 200) {
            goto cleanup;
        }
    }

    for (;;) {
        DWORD dwSize = 0;
        DWORD dwDownloaded = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;

        char *newBuffer = (char *)realloc(buffer, totalSize + dwSize + 1);
        if (!newBuffer) goto cleanup;
        buffer = newBuffer;

        if (!WinHttpReadData(hRequest, buffer + totalSize, dwSize, &dwDownloaded)) break;
        totalSize += dwDownloaded;

        if (binary && totalSize % 10240 == 0) {
            printf("\rDownloaded: %zu KB", totalSize / 1024);
            fflush(stdout);
        }
    }

    if (totalSize > 0) {
        buffer[totalSize] = '\0';
        result = buffer;
        buffer = NULL;
        if (outSize) *outSize = totalSize;
        if (binary) {
            printf("\rDownloaded: %zu KB - Complete!\n", totalSize / 1024);
        }
    }

cleanup:
    free(buffer);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return result;
}

char *ucxapp_platform_https_get_text(const char *host, const char *path)
{
    return winHttpGet(host, path, false, NULL);
}

char *ucxapp_platform_https_get_binary(const char *host, const char *path, size_t *outSize)
{
    return winHttpGet(host, path, true, outSize);
}

// Initialize FTD2XX library (dynamic loading)
// Resource ID for embedded FTDI DLL (must match ucxapp.rc)
#define IDR_FTD2XX_DLL 101

static bool initFtd2xxLibrary(void)
{
    if (gFtd2xxModule != NULL) {
        return true;  // Already loaded
    }
    
    char dllPath[MAX_PATH];
    bool dllExtracted = false;
    
    // Step 1: Try to extract embedded DLL from resources
    HRSRC hResource = FindResource(NULL, MAKEINTRESOURCE(IDR_FTD2XX_DLL), RT_RCDATA);
    if (hResource) {
        HGLOBAL hLoadedResource = LoadResource(NULL, hResource);
        if (hLoadedResource) {
            LPVOID pResourceData = LockResource(hLoadedResource);
            DWORD dwResourceSize = SizeofResource(NULL, hResource);
            
            if (pResourceData && dwResourceSize > 0) {
                // Extract to temp directory
                DWORD tmpLen = GetTempPath(MAX_PATH, dllPath);
                if (tmpLen == 0 || tmpLen + sizeof("ftd2xx64_embedded.dll") > MAX_PATH) {
                    dllPath[0] = '\0';
                    goto loadExternal;
                }
                snprintf(dllPath + tmpLen, sizeof(dllPath) - tmpLen, "ftd2xx64_embedded.dll");
                
                // Check if already extracted and correct size
                WIN32_FILE_ATTRIBUTE_DATA fileAttr;
                bool needsExtract = true;
                if (GetFileAttributesEx(dllPath, GetFileExInfoStandard, &fileAttr)) {
                    if (fileAttr.nFileSizeLow == dwResourceSize && fileAttr.nFileSizeHigh == 0) {
                        needsExtract = false;
                        dllExtracted = true;
                    }
                }
                
                if (needsExtract) {
                    // Write DLL to temp file
                    HANDLE hFile = CreateFile(dllPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        DWORD dwBytesWritten;
                        if (WriteFile(hFile, pResourceData, dwResourceSize, &dwBytesWritten, NULL)) {
                            if (dwBytesWritten == dwResourceSize) {
                                dllExtracted = true;
                                U_CX_LOG_LINE(U_CX_LOG_CH_DBG, "Extracted embedded FTDI DLL (%lu bytes) to: %s",
                                             dwResourceSize, dllPath);
                            } else {
                                U_CX_LOG_LINE(U_CX_LOG_CH_ERROR, "FTDI DLL extraction incomplete: wrote %lu of %lu bytes",
                                             dwBytesWritten, dwResourceSize);
                            }
                        } else {
                            U_CX_LOG_LINE(U_CX_LOG_CH_ERROR, "FTDI DLL WriteFile failed, error: %lu", GetLastError());
                        }
                        CloseHandle(hFile);
                    } else {
                        U_CX_LOG_LINE(U_CX_LOG_CH_ERROR, "Failed to create FTDI DLL temp file: %s, error: %lu",
                                     dllPath, GetLastError());
                    }
                }
            } else {
                U_CX_LOG_LINE(U_CX_LOG_CH_WARN, "FTDI DLL resource found but data invalid (size=%lu)",
                             SizeofResource(NULL, hResource));
            }
        } else {
            U_CX_LOG_LINE(U_CX_LOG_CH_WARN, "LoadResource failed for embedded FTDI DLL");
        }
    } else {
        U_CX_LOG_LINE(U_CX_LOG_CH_DBG, "No embedded FTDI DLL resource found in executable");
    }
    
    // Step 2: Try to load the extracted DLL
    if (dllExtracted) {
        gFtd2xxModule = LoadLibrary(dllPath);
        if (gFtd2xxModule == NULL) {
            U_CX_LOG_LINE(U_CX_LOG_CH_WARN, "LoadLibrary failed for %s, error: %lu", dllPath, GetLastError());
        }
    }
    
loadExternal:
    // Step 3: Fallback - try external DLL file
    if (gFtd2xxModule == NULL) {
        // Try to load ftd2xx64.dll from executable directory
        DWORD modLen = GetModuleFileName(NULL, dllPath, sizeof(dllPath));
        char *lastSlash = (modLen > 0 && modLen < sizeof(dllPath)) ? strrchr(dllPath, '\\') : NULL;
        if (lastSlash && (size_t)(lastSlash + 1 - dllPath) + sizeof("ftd2xx64.dll") <= sizeof(dllPath)) {
            *(lastSlash + 1) = '\0';
            snprintf(lastSlash + 1, sizeof(dllPath) - (size_t)(lastSlash + 1 - dllPath), "ftd2xx64.dll");
        }
        
        gFtd2xxModule = LoadLibrary(dllPath);
        if (gFtd2xxModule == NULL) {
            // Try current directory
            gFtd2xxModule = LoadLibrary("ftd2xx64.dll");
        }
        
        if (gFtd2xxModule != NULL) {
            U_CX_LOG_LINE(U_CX_LOG_CH_DBG, "Loaded external FTDI DLL");
        } else {
            U_CX_LOG_LINE(U_CX_LOG_CH_WARN, "Could not load FTDI D2XX library (not critical - using SetupAPI fallback)");
        }
    }
    
    if (gFtd2xxModule == NULL) {
        return false;
    }
    
    // Load function pointers
    gpFT_ListDevices = (PFN_FT_ListDevices)GetProcAddress(gFtd2xxModule, "FT_ListDevices");
    gpFT_Open = (PFN_FT_Open)GetProcAddress(gFtd2xxModule, "FT_Open");
    gpFT_OpenEx = (PFN_FT_OpenEx)GetProcAddress(gFtd2xxModule, "FT_OpenEx");
    gpFT_GetComPortNumber = (PFN_FT_GetComPortNumber)GetProcAddress(gFtd2xxModule, "FT_GetComPortNumber");
    gpFT_Close = (PFN_FT_Close)GetProcAddress(gFtd2xxModule, "FT_Close");
    
    if (!gpFT_ListDevices || !gpFT_Open || !gpFT_GetComPortNumber || !gpFT_Close) {
        U_CX_LOG_LINE(U_CX_LOG_CH_ERROR, "FTDI DLL loaded but missing required functions");
        FreeLibrary(gFtd2xxModule);
        gFtd2xxModule = NULL;
        return false;
    }
    
    return true;
}

#define FTDI_TABLE_MAX 32
static struct {
    LONG comPort;
    char description[64];
} gFtdiTable[FTDI_TABLE_MAX];
static int gFtdiCount;

// Open each FTDI device once per listing; repeated FT_Open/FT_Close storms can crash flaky USB drivers
static void loadFtdiTable(void)
{
    DWORD numDevs = 0;

    gFtdiCount = 0;
    // FT_Open on a busy VCP channel has bugchecked PCs (0x139); SetupAPI gives the same names, so D2XX is opt-in
    const char *optIn = getenv("UCX_FTD2XX");
    if (optIn == NULL || strcmp(optIn, "1") != 0 || !initFtd2xxLibrary() || gpFT_ListDevices(&numDevs, NULL, FT_LIST_NUMBER_ONLY) != FT_OK) {
        return;
    }
    for (DWORD i = 0; i < numDevs && gFtdiCount < FTDI_TABLE_MAX; i++) {
        char description[256];
        FT_HANDLE ftHandle;
        LONG comPortNumber = -1;

        if (gpFT_ListDevices((PVOID)(DWORD_PTR)i, description, FT_LIST_BY_INDEX | FT_OPEN_BY_DESCRIPTION) != FT_OK ||
            gpFT_Open((int)i, &ftHandle) != FT_OK) {
            continue;
        }
        FT_STATUS status = gpFT_GetComPortNumber(ftHandle, &comPortNumber);
        gpFT_Close(ftHandle);
        if (status == FT_OK && comPortNumber > 0) {
            gFtdiTable[gFtdiCount].comPort = comPortNumber;
            snprintf(gFtdiTable[gFtdiCount].description, sizeof(gFtdiTable[0].description), "%s", description);
            gFtdiCount++;
        }
    }
}

// Helper function to get FTDI device info from the table built by loadFtdiTable()
static bool getFtdiDeviceInfo(const char *portName, char *deviceDesc, size_t deviceDescSize, char *portLabel, size_t portLabelSize)
{
    // Initialize outputs
    deviceDesc[0] = '\0';
    portLabel[0] = '\0';

    for (int i = 0; i < gFtdiCount; i++) {
        const char *description = gFtdiTable[i].description;
        LONG comPortNumber = gFtdiTable[i].comPort;
        {
            {
                char comPortStr[16];
                snprintf(comPortStr, sizeof(comPortStr), "COM%ld", comPortNumber);
                
                if (strcmp(comPortStr, portName) == 0) {
                    // Found it!
                    strncpy(deviceDesc, description, deviceDescSize - 1);
                    deviceDesc[deviceDescSize - 1] = '\0';
                    
                    // Extract label from description
                    // Common formats: 
                    // "EVK-NORA-W36 A" -> A = AT command port
                    // "EVK-NORA-W36 B" -> B = (not used)
                    // "EVK-NORA-W36 C" -> C = LOG port
                    // "EVK-NORA-W36 D" -> D = (not used)
                    
                    // Check for single letter suffix (A, B, C, D)
                    size_t len = strlen(description);
                    if (len >= 2 && description[len-2] == ' ') {
                        char suffix = description[len-1];
                        if (suffix == 'A') {
                            strncpy(portLabel, "AT", portLabelSize - 1);
                            portLabel[portLabelSize - 1] = '\0';
                        } else if (suffix == 'C') {
                            strncpy(portLabel, "LOG", portLabelSize - 1);
                            portLabel[portLabelSize - 1] = '\0';
                        }
                    } else {
                        // Try to extract from " - XXX" format
                        char *dashPos = strrchr(description, '-');
                        if (dashPos) {
                            dashPos++;
                            while (*dashPos == ' ') dashPos++;
                            
                            if (strncmp(dashPos, "AT", 2) == 0 || strncmp(dashPos, "LOG", 3) == 0) {
                                strncpy(portLabel, dashPos, portLabelSize - 1);
                                portLabel[portLabelSize - 1] = '\0';
                                
                                // Remove any trailing spaces
                                char *end = portLabel + strlen(portLabel) - 1;
                                while (end > portLabel && *end == ' ') {
                                    *end = '\0';
                                    end--;
                                }
                            }
                        }
                    }
                    
                    return true;
                }
            }
        }
    }
    
    return false;
}

// Helper function to get device info using Windows SetupAPI
// This works without admin rights and for all COM port devices
static bool getDeviceInfoFromSetupAPI(const char *portName, char *deviceDesc, size_t deviceDescSize, char *portLabel, size_t portLabelSize)
{
    HDEVINFO deviceInfoSet;
    SP_DEVINFO_DATA deviceInfoData;
    DWORD i;
    bool found = false;
    
    // Initialize outputs
    deviceDesc[0] = '\0';
    portLabel[0] = '\0';
    
    // Get the device information set for all COM ports
    deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        return false;
    }
    
    // Enumerate through all devices
    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
    for (i = 0; SetupDiEnumDeviceInfo(deviceInfoSet, i, &deviceInfoData); i++) {
        HKEY hDeviceRegistryKey;
        char portNameFromReg[256];
        DWORD dataType;
        DWORD dataSize;
        
        // Open the device registry key
        hDeviceRegistryKey = SetupDiOpenDevRegKey(deviceInfoSet, &deviceInfoData,
                                                   DICS_FLAG_GLOBAL, 0,
                                                   DIREG_DEV, KEY_QUERY_VALUE);
        
        if (hDeviceRegistryKey == INVALID_HANDLE_VALUE) {
            continue;
        }
        
        // Get the port name from registry
        dataSize = sizeof(portNameFromReg);
        if (RegQueryValueEx(hDeviceRegistryKey, "PortName", NULL, &dataType,
                           (LPBYTE)portNameFromReg, &dataSize) == ERROR_SUCCESS) {
            
            // Check if this is the port we're looking for
            if (strcmp(portNameFromReg, portName) == 0) {
                // Found it! Get the friendly name
                char friendlyName[256];
                if (SetupDiGetDeviceRegistryProperty(deviceInfoSet, &deviceInfoData,
                                                     SPDRP_FRIENDLYNAME,
                                                     &dataType,
                                                     (PBYTE)friendlyName,
                                                     sizeof(friendlyName),
                                                     &dataSize)) {
                    strncpy(deviceDesc, friendlyName, deviceDescSize - 1);
                    deviceDesc[deviceDescSize - 1] = '\0';
                    
                    // Try to extract port label from friendly name
                    // Format can be: "USB Serial Port (COM25) - AT" or "EVK NORA-W36 - AT (COM25)".
                    // Only a spaced " - " starts a label; "USB-SERIAL CH340" has none.
                    char *dashPos = NULL;
                    for (char *p = strstr(friendlyName, " - "); p != NULL; p = strstr(p + 1, " - ")) {
                        dashPos = p + 1;
                    }
                    if (dashPos) {
                        // Skip whitespace after dash
                        dashPos++;
                        while (*dashPos == ' ') dashPos++;
                        
                        // Extract label
                        char *comPos = strstr(dashPos, " (COM");
                        if (comPos) {
                            size_t len = comPos - dashPos;
                            if (len >= portLabelSize) len = portLabelSize - 1;
                            strncpy(portLabel, dashPos, len);
                            portLabel[len] = '\0';
                        } else {
                            // Check for space or end of string
                            char *spacePos = strchr(dashPos, ' ');
                            if (spacePos) {
                                size_t len = spacePos - dashPos;
                                if (len >= portLabelSize) len = portLabelSize - 1;
                                strncpy(portLabel, dashPos, len);
                                portLabel[len] = '\0';
                            } else {
                                strncpy(portLabel, dashPos, portLabelSize - 1);
                                portLabel[portLabelSize - 1] = '\0';
                            }
                        }
                        
                    }
                    
                    // If the friendly name is generic (e.g. "USB Serial Port"),
                    // walk up the USB device tree to find the real device name.
                    // This is needed for FTDI-based EVK boards where the VCP driver
                    // doesn't expose the EEPROM description in the FriendlyName.
                    if (strstr(friendlyName, "USB Serial Port") && 
                        !strstr(friendlyName, "NORA") && !strstr(friendlyName, "EVK")) {
                        DEVINST parentInst, currentInst;
                        currentInst = deviceInfoData.DevInst;
                        
                        // Walk up to 3 levels of parent devices
                        for (int level = 0; level < 3; level++) {
                            if (CM_Get_Parent(&parentInst, currentInst, 0) != CR_SUCCESS) {
                                break;
                            }
                            
                            // Try to get BusReportedDeviceDesc from parent
                            WCHAR busDesc[256];
                            ULONG busDescSize = sizeof(busDesc);
                            DEVPROPTYPE propType;
                            
                            if (CM_Get_DevNode_PropertyW(parentInst,
                                                         &MY_DEVPKEY_BusReportedDeviceDesc,
                                                         &propType, (PBYTE)busDesc, &busDescSize,
                                                         0) == CR_SUCCESS &&
                                propType == DEVPROP_TYPE_STRING) {
                                // Convert wide string to narrow
                                char busDescA[256];
                                WideCharToMultiByte(CP_UTF8, 0, busDesc, -1,
                                                   busDescA, sizeof(busDescA), NULL, NULL);
                                
                                // Check if this contains a known device name
                                if (strstr(busDescA, "NORA") || strstr(busDescA, "EVK")) {
                                    // Use the bus-reported description as device name
                                    snprintf(deviceDesc, deviceDescSize, "%s (%s)",
                                            busDescA, portName);
                                    
                                    // Determine port function from device instance ID
                                    // FTDI FT4232H channels: A=AT, B=unused, C=LOG, D=unused
                                    char instanceId[256];
                                    DWORD instanceIdSize = sizeof(instanceId);
                                    if (SetupDiGetDeviceInstanceIdA(deviceInfoSet, &deviceInfoData,
                                                                     instanceId, instanceIdSize,
                                                                     &instanceIdSize)) {
                                        // Instance ID format: FTDIBUS\VID_0403+PID_6011+UBX...A\0000
                                        // The channel letter is the last char before \0000
                                        char *backslash = strrchr(instanceId, '\\');
                                        if (backslash && backslash > instanceId) {
                                            char channel = *(backslash - 1);
                                            if (channel == 'A') {
                                                strncpy(portLabel, "AT", portLabelSize - 1);
                                                portLabel[portLabelSize - 1] = '\0';
                                            } else if (channel == 'C') {
                                                strncpy(portLabel, "LOG", portLabelSize - 1);
                                                portLabel[portLabelSize - 1] = '\0';
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                            
                            currentInst = parentInst;
                        }
                    }
                    
                    found = true;
                }
            }
        }
        
        RegCloseKey(hDeviceRegistryKey);
        
        if (found) break;
    }
    
    SetupDiDestroyDeviceInfoList(deviceInfoSet);
    return found;
}


static void readUsbIds(const char *portName, uint16_t *vid, uint16_t *pid)
{
    HDEVINFO set = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
    SP_DEVINFO_DATA data;

    if (set == INVALID_HANDLE_VALUE) {
        return;
    }
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); i++) {
        HKEY key = SetupDiOpenDevRegKey(set, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
        char name[64] = "";
        DWORD size = sizeof(name);
        bool match;

        if (key == INVALID_HANDLE_VALUE) {
            continue;
        }
        match = RegQueryValueEx(key, "PortName", NULL, NULL, (LPBYTE)name, &size) == ERROR_SUCCESS &&
                _stricmp(name, portName) == 0;
        RegCloseKey(key);
        if (match) {
            char hwId[512] = "";
            unsigned v = 0, p = 0;
            const char *s;
            (void)SetupDiGetDeviceRegistryPropertyA(set, &data, SPDRP_HARDWAREID, NULL, (PBYTE)hwId,
                                                    sizeof(hwId) - 1, NULL);
            if ((s = strstr(hwId, "VID_")) != NULL && sscanf(s + 4, "%4x", &v) == 1) {
                *vid = (uint16_t)v;
            }
            if ((s = strstr(hwId, "PID_")) != NULL && sscanf(s + 4, "%4x", &p) == 1) {
                *pid = (uint16_t)p;
            }
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}

int ucxapp_platform_list_serial_ports(ucxapp_serial_port_t *ports, int maxPorts)
{
    HKEY hKey;
    int count = 0;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        loadFtdiTable();
        for (DWORD index = 0; count < maxPorts; index++) {
            char valueName[256];
            DWORD valueNameSize = sizeof(valueName);
            BYTE data[256];
            DWORD dataSize = sizeof(data);
            DWORD type;

            LONG result = RegEnumValue(hKey, index, valueName, &valueNameSize, NULL, &type, data, &dataSize);
            if (result == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (result != ERROR_SUCCESS || type != REG_SZ) {
                continue;
            }

            ucxapp_serial_port_t *port = &ports[count++];
            memset(port, 0, sizeof(*port));
            data[sizeof(data) - 1] = '\0';
            snprintf(port->name, sizeof(port->name), "%s", (char *)data);

            char fullName[48];
            snprintf(fullName, sizeof(fullName), "\\\\.\\%s", port->name);
            // Opening a Bluetooth SPP port starts a radio connection attempt; never probe those
            if (strstr(valueName, "BthModem") == NULL) {
                HANDLE hPort = CreateFile(fullName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (hPort != INVALID_HANDLE_VALUE) {
                    CloseHandle(hPort);
                    port->available = true;
                }
            }

            if (!getFtdiDeviceInfo(port->name, port->desc, sizeof(port->desc), port->label, sizeof(port->label)) &&
                !getDeviceInfoFromSetupAPI(port->name, port->desc, sizeof(port->desc), port->label, sizeof(port->label))) {
                snprintf(port->desc, sizeof(port->desc), "%s", valueName);
            }
            readUsbIds(port->name, &port->vid, &port->pid);
        }
        RegCloseKey(hKey);
        return count;
    }

    /* Registry unavailable: probe the common port numbers */
    for (int i = 1; i <= 256 && count < maxPorts; i++) {
        char fullName[48];
        snprintf(fullName, sizeof(fullName), "\\\\.\\COM%d", i);
        HANDLE hPort = CreateFile(fullName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hPort != INVALID_HANDLE_VALUE) {
            CloseHandle(hPort);
            ucxapp_serial_port_t *port = &ports[count++];
            memset(port, 0, sizeof(*port));
            snprintf(port->name, sizeof(port->name), "COM%d", i);
            port->available = true;
        }
    }
    return count;
}
