#pragma once

// The user32 and dwmapi calls screen and window capture makes (the sources, the cursor, window pictures), declared as
// the SDK declares them; see maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_types.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <dwmapi.h>
#endif

// <winuser.h>
#define CURSOR_SHOWING 0x00000001
#define DI_NORMAL 0x0003
#define PW_RENDERFULLCONTENT 0x00000002
#define MONITORINFOF_PRIMARY 0x00000001

typedef BOOL(CALLBACK* MONITORENUMPROC)(HMONITOR, HDC, LPRECT, LPARAM);

MAICORD_SYS_TYPES_BEGIN
// <winuser.h>
struct tagCURSORINFO {
    DWORD cbSize;
    DWORD flags;
    HCURSOR hCursor;
    POINT ptScreenPos;
};
typedef tagCURSORINFO CURSORINFO;
typedef tagCURSORINFO* PCURSORINFO;
struct _ICONINFO {
    BOOL fIcon;
    DWORD xHotspot;
    DWORD yHotspot;
    HBITMAP hbmMask;
    HBITMAP hbmColor;
};
typedef _ICONINFO ICONINFO;
typedef _ICONINFO* PICONINFO;
// <dwmapi.h>
enum DWMWINDOWATTRIBUTE {
    DWMWA_CLOAKED = 14,
};
MAICORD_SYS_TYPES_END

// <winuser.h>
extern "C" __declspec(dllimport) int WINAPI GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount);
extern "C" __declspec(dllimport) BOOL WINAPI EnumDisplayMonitors(HDC hdc, LPCRECT lprcClip, MONITORENUMPROC lpfnEnum,
    LPARAM dwData);
extern "C" __declspec(dllimport) HWND WINAPI GetShellWindow(void);
extern "C" __declspec(dllimport) BOOL WINAPI GetCursorInfo(PCURSORINFO pci);
extern "C" __declspec(dllimport) BOOL WINAPI GetIconInfo(HICON hIcon, PICONINFO piconinfo);
extern "C" __declspec(dllimport) BOOL WINAPI DrawIconEx(HDC hdc, int xLeft, int yTop, HICON hIcon, int cxWidth,
    int cyWidth, UINT istepIfAniCur, HBRUSH hbrFlickerFreeDraw, UINT diFlags);
extern "C" __declspec(dllimport) BOOL WINAPI PrintWindow(HWND hwnd, HDC hdcBlt, UINT nFlags);
// <dwmapi.h>: dwmapi.dll is resolved at run time (GetProcAddress), so the image never imports it.
extern "C" __declspec(dllimport) HRESULT WINAPI DwmGetWindowAttribute(HWND hwnd, DWORD dwAttribute, PVOID pvAttribute,
    DWORD cbAttribute);
