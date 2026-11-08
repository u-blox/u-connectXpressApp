/* The module over TCP: "tcp:HOST:PORT" opens a socket instead of a serial port, so a simulated module (the vucx
 * harness, `ucxapp --virtual`) works the same on Windows and Linux. Every other name goes to the platform serial
 * port, whose functions CMake renames to uPortSerial* for this file to wrap. */

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sockClose closesocket
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sockClose close
#endif

#include <stdlib.h>
#include <string.h>

#include "u_port_uart.h"

uPortUartHandle_t uPortSerialOpen(const char *pDevName, int32_t baudRate, bool useFlowControl);
void uPortSerialClose(uPortUartHandle_t handle);
int32_t uPortSerialWrite(uPortUartHandle_t handle, const void *pData, size_t length);
int32_t uPortSerialRead(uPortUartHandle_t handle, void *pData, size_t length, int32_t timeoutMs);
int32_t uPortSerialWaitForData(uPortUartHandle_t handle, int32_t timeoutMs);
void uPortSerialWake(uPortUartHandle_t handle);
int32_t uPortSerialSetDtr(uPortUartHandle_t handle, bool asserted);

#define MAX_TCP 4

typedef struct {
    sock_t s;
} tcpPort_t;

/* The serial handles are opaque, so ours are told apart by address, never by reading through a handle. */
static tcpPort_t *gTcp[MAX_TCP];

static tcpPort_t *tcp(uPortUartHandle_t h)
{
    for (int i = 0; h != NULL && i < MAX_TCP; i++) {
        if (gTcp[i] == h) {
            return gTcp[i];
        }
    }
    return NULL;
}

/* "tcp:HOST:PORT"; the host may be a name, an IPv4 address or [IPv6]. */
static uPortUartHandle_t tcpOpen(const char *spec)
{
    char host[128];
    const char *colon = strrchr(spec, ':');
    struct addrinfo hints, *res = NULL;
    sock_t s = SOCK_BAD;

    if (colon == NULL || colon == spec || (size_t)(colon - spec) >= sizeof(host) || colon[1] == '\0') {
        return NULL;
    }
    memcpy(host, spec, (size_t)(colon - spec));
    host[colon - spec] = '\0';
    if (host[0] == '[' && host[strlen(host) - 1] == ']') {
        memmove(host, host + 1, strlen(host));
        host[strlen(host) - 1] = '\0';
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return NULL;
    }
#endif
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0) {
        return NULL;
    }
    for (struct addrinfo *a = res; a != NULL && s == SOCK_BAD; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s != SOCK_BAD && connect(s, a->ai_addr, (int)a->ai_addrlen) != 0) {
            sockClose(s);
            s = SOCK_BAD;
        }
    }
    freeaddrinfo(res);
    if (s == SOCK_BAD) {
        return NULL;
    }
    int one = 1;
    (void)setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));   /* AT is many small writes */
    for (int i = 0; i < MAX_TCP; i++) {
        if (gTcp[i] == NULL) {
            gTcp[i] = calloc(1, sizeof(tcpPort_t));
            if (gTcp[i] != NULL) {
                gTcp[i]->s = s;
                return gTcp[i];
            }
            break;
        }
    }
    sockClose(s);
    return NULL;
}

/* 1 when the socket has data, 0 on timeout, -1 on error; timeoutMs < 0 waits for ever. */
static int tcpWait(tcpPort_t *t, int32_t timeoutMs)
{
    fd_set set;
    struct timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };

    FD_ZERO(&set);
    FD_SET(t->s, &set);
    int n = select((int)t->s + 1, &set, NULL, NULL, timeoutMs < 0 ? NULL : &tv);
    return n > 0 ? 1 : n == 0 ? 0 : -1;
}

uPortUartHandle_t uPortUartOpen(const char *pDevName, int32_t baudRate, bool useFlowControl)
{
    if (pDevName != NULL && strncmp(pDevName, "tcp:", 4) == 0) {
        return tcpOpen(pDevName + 4);
    }
    return uPortSerialOpen(pDevName, baudRate, useFlowControl);
}

void uPortUartClose(uPortUartHandle_t handle)
{
    tcpPort_t *t = tcp(handle);

    if (t == NULL) {
        uPortSerialClose(handle);
        return;
    }
    for (int i = 0; i < MAX_TCP; i++) {
        if (gTcp[i] == t) {
            gTcp[i] = NULL;
        }
    }
    sockClose(t->s);
    free(t);
}

int32_t uPortUartWrite(uPortUartHandle_t handle, const void *pData, size_t length)
{
    tcpPort_t *t = tcp(handle);
    size_t done = 0;

    if (t == NULL) {
        return uPortSerialWrite(handle, pData, length);
    }
    while (done < length) {
        int n = send(t->s, (const char *)pData + done, (int)(length - done), 0);
        if (n <= 0) {
            return done > 0 ? (int32_t)done : -1;
        }
        done += (size_t)n;
    }
    return (int32_t)done;
}

int32_t uPortUartRead(uPortUartHandle_t handle, void *pData, size_t length, int32_t timeoutMs)
{
    tcpPort_t *t = tcp(handle);

    if (t == NULL) {
        return uPortSerialRead(handle, pData, length, timeoutMs);
    }
    int w = tcpWait(t, timeoutMs);
    if (w <= 0) {
        return w;
    }
    int n = recv(t->s, pData, (int)length, 0);
    return n > 0 ? n : -1;   /* 0: the simulator went away */
}

int32_t uPortUartWaitForData(uPortUartHandle_t handle, int32_t timeoutMs)
{
    tcpPort_t *t = tcp(handle);

    return t == NULL ? uPortSerialWaitForData(handle, timeoutMs) : tcpWait(t, timeoutMs);
}

void uPortUartWake(uPortUartHandle_t handle)
{
    if (tcp(handle) == NULL) {
        uPortSerialWake(handle);
    }
}

int32_t uPortUartSetDtr(uPortUartHandle_t handle, bool asserted)
{
    return tcp(handle) == NULL ? uPortSerialSetDtr(handle, asserted) : -1;   /* no DTR line over TCP */
}
