#pragma once
// TEMPORARY probe aid: opaque handle types so units blocked behind shell headers show their own errors.
typedef void* HWND;
typedef void* HICON;
typedef void* HCURSOR;
typedef void* HBRUSH;
typedef void* HINSTANCE;
typedef void* HDC;
typedef void* HKEY;
typedef void* HMENU;
typedef void* HKL;
typedef unsigned short ATOM;
typedef void* HMONITOR;
typedef void* HBITMAP;
typedef void* HENHMETAFILE;
typedef void* HRSRC;
typedef void* HGLOBAL;
typedef long long (*WNDPROC)(void*, unsigned, unsigned long long, long long);
struct tagMSG { void* hwnd; unsigned message; unsigned long long wParam; long long lParam; unsigned time; int x, y; };
typedef tagMSG MSG;
struct tagCOPYDATASTRUCT { unsigned long long dwData; unsigned cbData; void* lpData; };
typedef tagCOPYDATASTRUCT COPYDATASTRUCT;
union _LARGE_INTEGER { struct { unsigned LowPart; int HighPart; }; long long QuadPart; };
typedef _LARGE_INTEGER LARGE_INTEGER;
union _ULARGE_INTEGER { struct { unsigned LowPart; unsigned HighPart; }; unsigned long long QuadPart; };
typedef _ULARGE_INTEGER ULARGE_INTEGER;
namespace maicord { struct WindowObject; }
inline HWND win32_hwnd(maicord::WindowObject* window) { return window; }
inline maicord::WindowObject* win32_window(HWND window) { return static_cast<maicord::WindowObject*>(window); }
