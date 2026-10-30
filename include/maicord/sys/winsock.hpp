#pragma once

// Winsock, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <MSWSock.h>
#endif

// <inaddr.h>
#define s_addr S_un.S_addr

// <ws2def.h>
#define AF_UNSPEC 0
#define AF_INET 2
#define AF_INET6 23
#define TCP_NODELAY 0x0001
#define IOC_WS2 0x08000000
#define _WSAIORW(x,y) (IOC_INOUT|(x)|(y))
#define SIO_GET_EXTENSION_FUNCTION_POINTER _WSAIORW(IOC_WS2,6)
#define INADDR_ANY (ULONG)0x00000000
#define INADDR_LOOPBACK 0x7f000001
#define IOCPARM_MASK 0x7f
#define IOC_OUT 0x40000000
#define IOC_IN 0x80000000
#define IOC_INOUT (IOC_IN|IOC_OUT)
#define _IOW(x,y,t) (IOC_IN|(((long)sizeof(t)&IOCPARM_MASK)<<16)|((x)<<8)|(y))

// <WinSock2.h>
#define FIONBIO _IOW('f', 126, u_long)
#define INVALID_SOCKET (SOCKET)(~0)
#define SOCKET_ERROR (-1)

// <ws2def.h>
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SO_REUSEADDR 0x0004
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#define SO_RCVBUF 0x1002
#define SO_SNDTIMEO 0x1005
#define SO_RCVTIMEO 0x1006
#define SOL_SOCKET 0xffff

// <WinSock2.h>
#define FD_READ_BIT 0
#define FD_READ (1 << FD_READ_BIT)
#define FD_WRITE_BIT 1
#define FD_WRITE (1 << FD_WRITE_BIT)
#define FD_ACCEPT_BIT 3
#define FD_ACCEPT (1 << FD_ACCEPT_BIT)
#define FD_CLOSE_BIT 5
#define FD_CLOSE (1 << FD_CLOSE_BIT)
#define WSAEVENT HANDLE
#define WSA_IO_PENDING (ERROR_IO_PENDING)
#define WSA_INVALID_HANDLE (ERROR_INVALID_HANDLE)
#define WSA_INVALID_EVENT ((WSAEVENT)NULL)
#define SD_SEND 0x01
#define WSA_FLAG_OVERLAPPED 0x01
#define WSA_FLAG_NO_HANDLE_INHERIT 0x80

// <ws2def.h>
#define NS_DNS (12)

// <WS2tcpip.h>
#define InetPtonA inet_pton

// <MSWSock.h>
#define SO_UPDATE_CONNECT_CONTEXT 0x7010
#define WSAID_CONNECTEX {0x25a207b9,0xddf3,0x4660,{0x8e,0xe9,0x76,0xe5,0x8c,0x74,0x06,0x3e}}

typedef unsigned short u_short;
typedef unsigned long u_long;
typedef USHORT ADDRESS_FAMILY;
typedef unsigned int GROUP;

MAICORD_SYS_TYPES_BEGIN

// <inaddr.h>
struct in_addr {
    union {
        struct {
            UCHAR s_b1, s_b2, s_b3, s_b4;
        } S_un_b;
        struct {
            USHORT s_w1, s_w2;
        } S_un_w;
        ULONG S_addr;
    } S_un;
};
typedef in_addr IN_ADDR;
// <ws2def.h>
struct sockaddr {
    ADDRESS_FAMILY sa_family;
    CHAR sa_data[14];
};
struct sockaddr_storage {
    ADDRESS_FAMILY ss_family;
    CHAR __ss_pad1[((sizeof(__int64)) - sizeof(USHORT))];
    __int64 __ss_align;
    CHAR __ss_pad2[(128 - (sizeof(USHORT) + ((sizeof(__int64)) - sizeof(USHORT)) + (sizeof(__int64))))];
};
typedef enum {
    IPPROTO_TCP = 6,
    IPPROTO_UDP = 17,
} IPPROTO;
typedef struct {
    union {
        struct {
            ULONG Zone : 28;
            ULONG Level : 4;
        };
        ULONG Value;
    };
} SCOPE_ID;
struct sockaddr_in {
    ADDRESS_FAMILY sin_family;
    USHORT sin_port;
    IN_ADDR sin_addr;
    CHAR sin_zero[8];
};
struct _WSABUF {
    ULONG len;
    CHAR* buf;
};
typedef _WSABUF WSABUF;
typedef _WSABUF* LPWSABUF;
struct addrinfoW {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    PWSTR ai_canonname;
    struct sockaddr* ai_addr;
    struct addrinfoW* ai_next;
};
typedef addrinfoW ADDRINFOW;
typedef addrinfoW* PADDRINFOW;
struct addrinfoexW {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    PWSTR ai_canonname;
    struct sockaddr* ai_addr;
    void* ai_blob;
    size_t ai_bloblen;
    LPGUID ai_provider;
    struct addrinfoexW* ai_next;
};
typedef addrinfoexW ADDRINFOEXW;
typedef addrinfoexW* PADDRINFOEXW;
// <WinSock2.h>
struct WSAData {
    WORD wVersion;
    WORD wHighVersion;
    unsigned short iMaxSockets;
    unsigned short iMaxUdpDg;
    char* lpVendorInfo;
    char szDescription[256 + 1];
    char szSystemStatus[128 + 1];
};
typedef WSAData WSADATA;
typedef WSAData* LPWSADATA;
typedef struct _OVERLAPPED* LPWSAOVERLAPPED;
struct _WSANETWORKEVENTS {
    long lNetworkEvents;
    int iErrorCode[10];
};
typedef _WSANETWORKEVENTS WSANETWORKEVENTS;
typedef _WSANETWORKEVENTS* LPWSANETWORKEVENTS;
typedef struct _WSAPROTOCOL_INFOW* LPWSAPROTOCOL_INFOW;
typedef void(__stdcall* LPWSAOVERLAPPED_COMPLETION_ROUTINE)(DWORD dwError, DWORD cbTransferred,
    LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags);
// <in6addr.h>
struct in6_addr {
    union {
        UCHAR Byte[16];
        USHORT Word[8];
    } u;
};
typedef in6_addr IN6_ADDR;
// <ws2ipdef.h>
struct sockaddr_in6 {
    ADDRESS_FAMILY sin6_family;
    USHORT sin6_port;
    ULONG sin6_flowinfo;
    IN6_ADDR sin6_addr;
    union {
        ULONG sin6_scope_id;
        SCOPE_ID sin6_scope_struct;
    };
};
// <WS2tcpip.h>
typedef void(__stdcall* LPLOOKUPSERVICE_COMPLETION_ROUTINE)(DWORD dwError, DWORD dwBytes, LPWSAOVERLAPPED lpOverlapped);
// <MSWSock.h>
typedef BOOL(__stdcall* LPFN_CONNECTEX)(SOCKET s, const struct sockaddr* name, int namelen, PVOID lpSendBuffer,
    DWORD dwSendDataLength, LPDWORD lpdwBytesSent, LPOVERLAPPED lpOverlapped);
MAICORD_SYS_TYPES_END

// <WinSock2.h>
extern "C" __declspec(dllimport) SOCKET WINAPI accept(SOCKET s, struct sockaddr* addr, int* addrlen);
extern "C" __declspec(dllimport) int WINAPI bind(SOCKET s, const struct sockaddr* name, int namelen);
extern "C" __declspec(dllimport) int WINAPI closesocket(SOCKET s);
extern "C" __declspec(dllimport) int WINAPI connect(SOCKET s, const struct sockaddr* name, int namelen);
extern "C" __declspec(dllimport) int WINAPI ioctlsocket(SOCKET s, long cmd, u_long* argp);
extern "C" __declspec(dllimport) int WINAPI getsockname(SOCKET s, struct sockaddr* name, int* namelen);
extern "C" __declspec(dllimport) u_long WINAPI htonl(u_long hostlong);
extern "C" __declspec(dllimport) u_short WINAPI htons(u_short hostshort);
extern "C" __declspec(dllimport) int WINAPI listen(SOCKET s, int backlog);
extern "C" __declspec(dllimport) u_short WINAPI ntohs(u_short netshort);
extern "C" __declspec(dllimport) int WINAPI recv(SOCKET s, char* buf, int len, int flags);
extern "C" __declspec(dllimport) int WINAPI recvfrom(SOCKET s, char* buf, int len, int flags, struct sockaddr* from,
    int* fromlen);
extern "C" __declspec(dllimport) int WINAPI send(SOCKET s, const char* buf, int len, int flags);
extern "C" __declspec(dllimport) int WINAPI sendto(SOCKET s, const char* buf, int len, int flags,
    const struct sockaddr* to, int tolen);
extern "C" __declspec(dllimport) int WINAPI setsockopt(SOCKET s, int level, int optname, const char* optval,
    int optlen);
extern "C" __declspec(dllimport) int WINAPI shutdown(SOCKET s, int how);
extern "C" __declspec(dllimport) SOCKET WINAPI socket(int af, int type, int protocol);
extern "C" __declspec(dllimport) int WINAPI WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData);
extern "C" __declspec(dllimport) int WINAPI WSACleanup();
extern "C" __declspec(dllimport) void WINAPI WSASetLastError(int iError);
extern "C" __declspec(dllimport) int WINAPI WSAGetLastError();
extern "C" __declspec(dllimport) BOOL WINAPI WSACloseEvent(HANDLE hEvent);
extern "C" __declspec(dllimport) HANDLE WINAPI WSACreateEvent();
extern "C" __declspec(dllimport) int WINAPI WSAEnumNetworkEvents(SOCKET s, HANDLE hEventObject,
    LPWSANETWORKEVENTS lpNetworkEvents);
extern "C" __declspec(dllimport) int WINAPI WSAEventSelect(SOCKET s, HANDLE hEventObject, long lNetworkEvents);
extern "C" __declspec(dllimport) int WINAPI WSAIoctl(SOCKET s, DWORD dwIoControlCode, LPVOID lpvInBuffer,
    DWORD cbInBuffer, LPVOID lpvOutBuffer, DWORD cbOutBuffer, LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
extern "C" __declspec(dllimport) int WINAPI WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
extern "C" __declspec(dllimport) BOOL WINAPI WSAResetEvent(HANDLE hEvent);
extern "C" __declspec(dllimport) int WINAPI WSASend(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
extern "C" __declspec(dllimport) SOCKET WINAPI WSASocketW(int af, int type, int protocol,
    LPWSAPROTOCOL_INFOW lpProtocolInfo, GROUP g, DWORD dwFlags);

// <ws2ipdef.h>
extern "C" extern const IN6_ADDR in6addr_loopback;

// <WS2tcpip.h>
extern "C" __declspec(dllimport) INT WINAPI GetAddrInfoW(PCWSTR pNodeName, PCWSTR pServiceName,
    const ADDRINFOW* pHints, PADDRINFOW* ppResult);
extern "C" __declspec(dllimport) INT WINAPI GetAddrInfoExW(PCWSTR pName, PCWSTR pServiceName, DWORD dwNameSpace,
    LPGUID lpNspId, const ADDRINFOEXW* hints, PADDRINFOEXW* ppResult, struct timeval* timeout,
    LPOVERLAPPED lpOverlapped, LPLOOKUPSERVICE_COMPLETION_ROUTINE lpCompletionRoutine, LPHANDLE lpHandle);
extern "C" __declspec(dllimport) INT WINAPI GetAddrInfoExCancel(LPHANDLE lpHandle);
extern "C" __declspec(dllimport) void WINAPI FreeAddrInfoW(PADDRINFOW pAddrInfo);
extern "C" __declspec(dllimport) void WINAPI FreeAddrInfoExW(PADDRINFOEXW pAddrInfoEx);
extern "C" __declspec(dllimport) INT WINAPI inet_pton(INT Family, PCSTR pszAddrString, PVOID pAddrBuf);
extern "C" INT WINAPI InetPtonW(INT Family, PCWSTR pszAddrString, PVOID pAddrBuf);
