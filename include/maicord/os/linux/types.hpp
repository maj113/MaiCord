#pragma once

// The Win32 value vocabulary on Linux (D17): the scalar types, handles and small records that shared code names
// through maicord/core/win32_types.hpp, with the widths they have on Windows (LONG and DWORD are 32 bits wide
// although long is 64 here). Only value types live here; the calls that act on them are in
// maicord/os/linux/kernel.hpp, and the operations with an operating-system meaning (events, files, threads) have
// neutral names in maicord/os.

// <minwindef.h>
#define MAX_PATH 260
#define FALSE 0
#define TRUE 1
#define CALLBACK
#define WINAPI

typedef unsigned int ULONG;
typedef ULONG* PULONG;
typedef unsigned short USHORT;
typedef unsigned char UCHAR;
typedef UCHAR* PUCHAR;
typedef unsigned int DWORD;
typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef float FLOAT;
typedef BOOL* PBOOL;
typedef BOOL* LPBOOL;
typedef BYTE* PBYTE;
typedef BYTE* LPBYTE;
typedef WORD* LPWORD;
typedef DWORD* PDWORD;
typedef DWORD* LPDWORD;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef int INT;
typedef unsigned int UINT;
typedef signed short INT16;
typedef signed int INT32;
typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef long long INT_PTR;
typedef unsigned long long UINT_PTR;
typedef long long LONG_PTR;
typedef unsigned long long ULONG_PTR;
typedef unsigned long long* PULONG_PTR;
typedef ULONG_PTR SIZE_T;
typedef ULONG_PTR* PSIZE_T;
typedef ULONG_PTR DWORD_PTR;
typedef ULONG_PTR* PDWORD_PTR;
typedef long long LONG64;
typedef unsigned long long ULONG64;
typedef unsigned long long* PULONG64;
typedef unsigned long long DWORD64;
typedef unsigned long long* PDWORD64;
typedef void* PVOID;
typedef char CHAR;
typedef short SHORT;
typedef int LONG;
typedef wchar_t WCHAR;
typedef const WCHAR* LPCWCH;
typedef WCHAR* LPWSTR;
typedef WCHAR* PWSTR;
typedef const WCHAR* LPCWSTR;
typedef const WCHAR* PCWSTR;
typedef const CHAR* LPCCH;
typedef CHAR* LPSTR;
typedef const CHAR* LPCSTR;
typedef const CHAR* PCSTR;
typedef void* HANDLE;
typedef HANDLE* PHANDLE;
typedef int HRESULT;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef BYTE BOOLEAN;
typedef DWORD ACCESS_MASK;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef HANDLE* LPHANDLE;
typedef LONG SCODE;
typedef LONG SECURITY_STATUS;

// <winnt.h>
struct _FILE_ID_128 {
    BYTE Identifier[16];
};
typedef _FILE_ID_128 FILE_ID_128;
struct _RTL_SRWLOCK {
    PVOID Ptr;
};
typedef _RTL_SRWLOCK RTL_SRWLOCK;
// <minwindef.h>
struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};
typedef _FILETIME FILETIME;
typedef _FILETIME* PFILETIME;
typedef _FILETIME* LPFILETIME;
// <windef.h>
struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};
typedef tagRECT RECT;
typedef tagRECT* LPRECT;
typedef const tagRECT* LPCRECT;
struct tagPOINT {
    LONG x;
    LONG y;
};
typedef tagPOINT POINT;
typedef tagPOINT* LPPOINT;
struct tagSIZE {
    LONG cx;
    LONG cy;
};
typedef tagSIZE SIZE;
// <synchapi.h>
typedef RTL_SRWLOCK SRWLOCK;
typedef RTL_SRWLOCK* PSRWLOCK;
// <winbase.h>
struct _FILE_ID_INFO {
    ULONGLONG VolumeSerialNumber;
    FILE_ID_128 FileId;
};
typedef _FILE_ID_INFO FILE_ID_INFO;

static_assert(sizeof(LONG) == 4 && sizeof(DWORD) == 4 && sizeof(LONG64) == 8 && sizeof(HANDLE) == 8,
              "the Win32 value types keep their Windows widths");
static_assert(sizeof(WCHAR) == 2, "wchar_t holds UTF-16 code units (-fshort-wchar)");
