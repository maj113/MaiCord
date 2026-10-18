#pragma once

// Win32 types that headers name in declarations, declared exactly as the
// Windows SDK declares them (x64, STRICT handles) so <Windows.h> may come
// before or after. Sources that call Win32 or need complete SDK structures
// include <Windows.h> themselves.
#define WINAPI __stdcall
#define CALLBACK __stdcall
typedef int BOOL;
#define FALSE 0
#define TRUE 1
typedef unsigned short WORD;
typedef long LONG;
typedef __int64 LONG64;
typedef long HRESULT;
typedef LONG SECURITY_STATUS;
typedef unsigned int UINT;
typedef unsigned long DWORD;
typedef unsigned __int64 ULONG_PTR;
typedef unsigned __int64 WPARAM;
typedef __int64 LPARAM;
typedef __int64 LRESULT;
typedef unsigned __int64 SOCKET;
typedef void* HANDLE;
typedef HANDLE HGLOBAL;
struct HICON__;
typedef HICON__* HICON;
struct HINSTANCE__;
typedef HINSTANCE__* HINSTANCE;
typedef HINSTANCE HMODULE;
struct HKEY__;
typedef HKEY__* HKEY;
struct HWND__;
typedef HWND__* HWND;
typedef LRESULT(__stdcall* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
struct _FILETIME;
typedef _FILETIME FILETIME;
struct _FILE_ID_INFO;
typedef _FILE_ID_INFO FILE_ID_INFO;
struct tagPOINT;
typedef tagPOINT POINT;
struct tagRECT;
typedef tagRECT RECT;
struct tagMSG;
typedef tagMSG MSG;
struct tagMINMAXINFO;
typedef tagMINMAXINFO MINMAXINFO;
struct tagCOPYDATASTRUCT;
typedef tagCOPYDATASTRUCT COPYDATASTRUCT;
struct _RTL_SRWLOCK;
typedef _RTL_SRWLOCK RTL_SRWLOCK;
typedef RTL_SRWLOCK SRWLOCK;

namespace maicord {
// WM_APP: private window messages start here.
inline constexpr UINT kWindowMessageApp = 0x8000;
} // namespace maicord
