/* Win32 net slice: the 22 WS2_32 imports (notes/slice_owners.md, slice "net"),
 * all imported by ordinal. Contract: runtime/include/win32_thunk.h and
 * docs/WIN32_THUNKS.md.
 *
 * The game stays offline (notes/net_usage.md): WSAStartup succeeds, the byte
 * order and address helpers are real, and every socket call fails as if the
 * network were down. The Winsock error is the thread's Win32 last error, as
 * on Windows, where WSAGetLastError and GetLastError share one value. */
#include <arpa/inet.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "recomp_types.h"
#include "guest_mem.h"
#include "guest_heap.h"
#include "win32_thunk.h"

#define WS_SOCKET_ERROR 0xFFFFFFFFu
#define WS_INVALID_SOCKET 0xFFFFFFFFu
#define WS_INADDR_NONE 0xFFFFFFFFu
#define WSAEFAULT 10014u
#define WSAENOTSOCK 10038u
#define WSAENETDOWN 10050u
#define WSAHOST_NOT_FOUND 11001u

/* The name gethostname gives; gethostbyname resolves it to 127.0.0.1. */
#define WS_HOST_NAME "MAC"

/* Every call that takes a SOCKET: no socket is ever issued. */
#define WS_FAIL_NOTSOCK(nargs, value)          \
    do {                                       \
        w32_set_last_error(WSAENOTSOCK);       \
        W32_RETURN((nargs), (value));          \
    } while (0)

/* ---- Start-up ---------------------------------------------------------- */

/* int WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData). 32-bit
 * WSADATA: +0 wVersion, +2 wHighVersion, +4 szDescription[257],
 * +261 szSystemStatus[129], +390 iMaxSockets, +392 iMaxUdpDg,
 * +396 lpVendorInfo; 400 bytes. */
static void imp_WSAStartup(void)
{
    uint8_t *d = guest_ptr(W32_ARG(1));
    memset(d, 0, 400);
    memcpy(d + 0, &(uint16_t){0x0202u}, 2);
    memcpy(d + 2, &(uint16_t){0x0202u}, 2);
    strcpy((char *)d + 4, "WinSock 2.0");
    strcpy((char *)d + 261, "Running");
    W32_RETURN(2, 0);
}

static void imp_WSACleanup(void)
{
    W32_RETURN(0, 0);
}

static void imp_WSAGetLastError(void)
{
    W32_RETURN(0, w32_last_error());
}

/* ---- Byte order -------------------------------------------------------- */

static void imp_htonl(void)
{
    W32_RETURN(1, __builtin_bswap32(W32_ARG(0)));
}

static void imp_htons(void)
{
    W32_RETURN(1, __builtin_bswap16((uint16_t)W32_ARG(0)));
}

static void imp_ntohs(void)
{
    W32_RETURN(1, __builtin_bswap16((uint16_t)W32_ARG(0)));
}

/* ---- Addresses and names ----------------------------------------------- */

/* unsigned long inet_addr(const char *cp). The host's inet_addr has the same
 * BSD syntax (a.b.c.d, a.b.c, a.b, a; octal and hex parts) and returns the
 * address in network order, which is the guest's in-memory order too. */
static void imp_inet_addr(void)
{
    uint32_t cp = W32_ARG(0);
    W32_RETURN(1, cp != 0 ? inet_addr((const char *)guest_ptr(cp)) : WS_INADDR_NONE);
}

/* char *inet_ntoa(struct in_addr in). Windows keeps one buffer per thread,
 * valid until the thread's next call. */
static _Thread_local uint32_t t_ntoa_buf;

static void imp_inet_ntoa(void)
{
    uint32_t in = W32_ARG(0);
    if (t_ntoa_buf == 0)
        t_ntoa_buf = guest_malloc(16);
    if (t_ntoa_buf != 0)
        snprintf((char *)guest_ptr(t_ntoa_buf), 16, "%u.%u.%u.%u", in & 0xFFu,
                 (in >> 8) & 0xFFu, (in >> 16) & 0xFFu, in >> 24);
    W32_RETURN(1, t_ntoa_buf);
}

/* int gethostname(char *name, int namelen). */
static void imp_gethostname(void)
{
    uint32_t name = W32_ARG(0);
    int32_t len = (int32_t)W32_ARG(1);
    uint32_t ret = 0;
    if (len < (int32_t)sizeof WS_HOST_NAME) {
        w32_set_last_error(WSAEFAULT);
        ret = WS_SOCKET_ERROR;
    } else {
        strcpy((char *)guest_ptr(name), WS_HOST_NAME);
    }
    W32_RETURN(2, ret);
}

/* struct hostent *gethostbyname(const char *name). Offline, only this
 * machine's names resolve, to 127.0.0.1: the gethostname name, "localhost",
 * and NULL or "" (the local host, as on Windows). Windows keeps one hostent
 * per thread, valid until the thread's next call. The block holds the
 * 32-bit hostent (+0 h_name, +4 h_aliases, +8 h_addrtype, +10 h_length,
 * +12 h_addr_list), then the empty alias list at +16, the address list at
 * +20, the address at +28 and the name at +32. */
static _Thread_local uint32_t t_hostent;

static void imp_gethostbyname(void)
{
    uint32_t arg = W32_ARG(0), h = 0;
    const char *name = arg != 0 ? (const char *)guest_ptr(arg) : "";
    int local = strcasecmp(name, "localhost") == 0;
    if (!local && name[0] != 0 && strcasecmp(name, WS_HOST_NAME) != 0) {
        w32_set_last_error(WSAHOST_NOT_FOUND);
    } else {
        if (t_hostent == 0)
            t_hostent = guest_malloc(48);
        h = t_hostent;
    }
    if (h != 0) {
        MEM32(h + 0) = h + 32;
        MEM32(h + 4) = h + 16;
        MEM16(h + 8) = 2; /* AF_INET */
        MEM16(h + 10) = 4;
        MEM32(h + 12) = h + 20;
        MEM32(h + 16) = 0;
        MEM32(h + 20) = h + 28;
        MEM32(h + 24) = 0;
        MEM32(h + 28) = 0x0100007Fu; /* 127.0.0.1, network order */
        strcpy((char *)guest_ptr(h + 32), local ? "localhost" : WS_HOST_NAME);
    }
    W32_RETURN(1, h);
}

/* ---- Sockets ----------------------------------------------------------- */

/* SOCKET socket(int af, int type, int protocol). */
static void imp_socket(void)
{
    static atomic_flag reported = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&reported))
        fprintf(stderr,
                "UNSUPPORTED WS2_32.dll!socket: networking disabled "
                "(af %d type %d proto %d, guest return 0x%08X)\n",
                (int32_t)W32_ARG(0), (int32_t)W32_ARG(1), (int32_t)W32_ARG(2), MEM32(g_esp));
    w32_set_last_error(WSAENETDOWN);
    W32_RETURN(3, WS_INVALID_SOCKET);
}

/* SOCKET accept(SOCKET s, struct sockaddr *addr, int *addrlen). */
static void imp_accept(void)
{
    WS_FAIL_NOTSOCK(3, WS_INVALID_SOCKET);
}

/* int bind(SOCKET s, const struct sockaddr *name, int namelen). */
static void imp_bind(void)
{
    WS_FAIL_NOTSOCK(3, WS_SOCKET_ERROR);
}

/* int closesocket(SOCKET s). */
static void imp_closesocket(void)
{
    WS_FAIL_NOTSOCK(1, WS_SOCKET_ERROR);
}

/* int connect(SOCKET s, const struct sockaddr *name, int namelen). */
static void imp_connect(void)
{
    WS_FAIL_NOTSOCK(3, WS_SOCKET_ERROR);
}

/* int ioctlsocket(SOCKET s, long cmd, u_long *argp). */
static void imp_ioctlsocket(void)
{
    WS_FAIL_NOTSOCK(3, WS_SOCKET_ERROR);
}

/* int listen(SOCKET s, int backlog). */
static void imp_listen(void)
{
    WS_FAIL_NOTSOCK(2, WS_SOCKET_ERROR);
}

/* int recv(SOCKET s, char *buf, int len, int flags). */
static void imp_recv(void)
{
    WS_FAIL_NOTSOCK(4, WS_SOCKET_ERROR);
}

/* int recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from,
 * int *fromlen). */
static void imp_recvfrom(void)
{
    WS_FAIL_NOTSOCK(6, WS_SOCKET_ERROR);
}

/* int send(SOCKET s, const char *buf, int len, int flags). */
static void imp_send(void)
{
    WS_FAIL_NOTSOCK(4, WS_SOCKET_ERROR);
}

/* int sendto(SOCKET s, const char *buf, int len, int flags,
 * const struct sockaddr *to, int tolen). */
static void imp_sendto(void)
{
    WS_FAIL_NOTSOCK(6, WS_SOCKET_ERROR);
}

/* int setsockopt(SOCKET s, int level, int optname, const char *optval,
 * int optlen). */
static void imp_setsockopt(void)
{
    WS_FAIL_NOTSOCK(5, WS_SOCKET_ERROR);
}

/* ---- Export table ------------------------------------------------------ */

/* By ordinal: the game imports every WS2_32 entry by ordinal. */
const win32_export_t win32_exports_net[] = {
    {"WS2_32.dll", NULL, 1, imp_accept},
    {"WS2_32.dll", NULL, 2, imp_bind},
    {"WS2_32.dll", NULL, 3, imp_closesocket},
    {"WS2_32.dll", NULL, 4, imp_connect},
    {"WS2_32.dll", NULL, 8, imp_htonl},
    {"WS2_32.dll", NULL, 9, imp_htons},
    {"WS2_32.dll", NULL, 10, imp_ioctlsocket},
    {"WS2_32.dll", NULL, 11, imp_inet_addr},
    {"WS2_32.dll", NULL, 12, imp_inet_ntoa},
    {"WS2_32.dll", NULL, 13, imp_listen},
    {"WS2_32.dll", NULL, 15, imp_ntohs},
    {"WS2_32.dll", NULL, 16, imp_recv},
    {"WS2_32.dll", NULL, 17, imp_recvfrom},
    {"WS2_32.dll", NULL, 19, imp_send},
    {"WS2_32.dll", NULL, 20, imp_sendto},
    {"WS2_32.dll", NULL, 21, imp_setsockopt},
    {"WS2_32.dll", NULL, 23, imp_socket},
    {"WS2_32.dll", NULL, 52, imp_gethostbyname},
    {"WS2_32.dll", NULL, 57, imp_gethostname},
    {"WS2_32.dll", NULL, 111, imp_WSAGetLastError},
    {"WS2_32.dll", NULL, 115, imp_WSAStartup},
    {"WS2_32.dll", NULL, 116, imp_WSACleanup},
    {0},
};
