#pragma once

// Win32 scalar types, handles and the records most headers name, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <WinSock2.h>
#include <wtypesbase.h>
#define SECURITY_WIN32
#include <sspi.h>
#endif

// <minwindef.h>
#define MAX_PATH 260
#define FALSE 0
#define TRUE 1
#define CALLBACK __stdcall
#define WINAPI __stdcall
#define MAKEWORD(a, b) ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
#define MAKELONG(a, b) ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))

struct HKEY__;
struct HINSTANCE__;
struct HRSRC__;
struct HKL__;
struct HWND__;
struct HBITMAP__;
struct HBRUSH__;
struct HDC__;
struct HENHMETAFILE__;
struct HFONT__;
struct HICON__;
struct HMENU__;
struct HMONITOR__;
typedef unsigned long ULONG;
typedef ULONG* PULONG;
typedef unsigned short USHORT;
typedef unsigned char UCHAR;
typedef UCHAR* PUCHAR;
typedef unsigned long DWORD;
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
typedef unsigned __int64 UINT64;
typedef __int64 INT_PTR;
typedef unsigned __int64 UINT_PTR;
typedef __int64 LONG_PTR;
typedef unsigned __int64 ULONG_PTR;
typedef unsigned __int64* PULONG_PTR;
typedef ULONG_PTR SIZE_T;
typedef ULONG_PTR* PSIZE_T;
typedef ULONG_PTR DWORD_PTR;
typedef ULONG_PTR* PDWORD_PTR;
typedef __int64 LONG64;
typedef unsigned __int64 ULONG64;
typedef unsigned __int64* PULONG64;
typedef unsigned __int64 DWORD64;
typedef unsigned __int64* PDWORD64;
typedef void* PVOID;
typedef char CHAR;
typedef short SHORT;
typedef long LONG;
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
typedef long HRESULT;
typedef DWORD LCID;
typedef __int64 LONGLONG;
typedef unsigned __int64 ULONGLONG;
typedef BYTE BOOLEAN;
typedef DWORD ACCESS_MASK;
typedef DWORD TP_VERSION;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef HANDLE* LPHANDLE;
typedef HANDLE HGLOBAL;
typedef HANDLE HLOCAL;
typedef INT_PTR(__stdcall* FARPROC)();
typedef WORD ATOM;
typedef struct HKEY__* HKEY;
typedef HKEY* PHKEY;
typedef struct HINSTANCE__* HINSTANCE;
typedef HINSTANCE HMODULE;
typedef struct HRSRC__* HRSRC;
typedef struct HKL__* HKL;
typedef struct HWND__* HWND;
typedef void* HGDIOBJ;
typedef struct HBITMAP__* HBITMAP;
typedef struct HBRUSH__* HBRUSH;
typedef struct HDC__* HDC;
typedef struct HENHMETAFILE__* HENHMETAFILE;
typedef struct HFONT__* HFONT;
typedef struct HICON__* HICON;
typedef struct HMENU__* HMENU;
typedef struct HMONITOR__* HMONITOR;
typedef HICON HCURSOR;
typedef DWORD COLORREF;
typedef LRESULT(__stdcall* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef LONG SCODE;
typedef UINT_PTR SOCKET;
typedef LONG SECURITY_STATUS;

MAICORD_SYS_TYPES_BEGIN
struct _FILETIME;
struct _FILE_ID_128;
struct _FILE_ID_INFO;
struct _POINTL;
struct _RTL_SRWLOCK;
struct tagCOPYDATASTRUCT;
struct tagMINMAXINFO;
struct tagMSG;
struct tagPOINT;
struct tagRECT;
struct tagSIZE;

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
struct _POINTL {
    LONG x;
    LONG y;
};
typedef _POINTL POINTL;
struct tagSIZE {
    LONG cx;
    LONG cy;
};
typedef tagSIZE SIZE;
typedef SIZE SIZEL;
// <synchapi.h>
typedef RTL_SRWLOCK SRWLOCK;
// <winbase.h>
struct _FILE_ID_INFO {
    ULONGLONG VolumeSerialNumber;
    FILE_ID_128 FileId;
};
typedef _FILE_ID_INFO FILE_ID_INFO;
// <winuser.h>
struct tagMSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
};
typedef tagMSG MSG;
struct tagMINMAXINFO {
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
};
typedef tagMINMAXINFO MINMAXINFO;
struct tagCOPYDATASTRUCT {
    ULONG_PTR dwData;
    DWORD cbData;
    PVOID lpData;
};
typedef tagCOPYDATASTRUCT COPYDATASTRUCT;
MAICORD_SYS_TYPES_END

// Inline definitions; the SDK's own with MAICORD_SDK_INCLUDES.
#if !defined(MAICORD_SDK_INCLUDES)
extern "C" __inline
void *
LongToHandle(
    const long h
    )
{
    return((void *) (INT_PTR) h );
}
#endif

namespace maicord {
// WM_APP: private window messages start here.
inline constexpr UINT kWindowMessageApp = 0x8000;
} // namespace maicord
