#pragma once

#include "maicord/core/win32_types.hpp"

// The user32 input and window functions and constants MaiCord calls, declared
// exactly as the Windows SDK declares them (x64) so <Windows.h> may come before
// or after. src/core/win32_sdk_conformance.cpp checks them against the SDK.
// Window creation, painting and the clipboard still include <Windows.h>.

typedef short SHORT;
typedef unsigned char BYTE;
typedef wchar_t WCHAR;
typedef WCHAR* LPWSTR;
typedef __int64 LONG_PTR;
typedef ULONG_PTR DWORD_PTR;
typedef HICON HCURSOR;
struct tagTRACKMOUSEEVENT;
typedef tagTRACKMOUSEEVENT TRACKMOUSEEVENT;

// Messages.
#define WM_NULL 0x0000
#define WM_DESTROY 0x0002
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_CANCELMODE 0x001F
#define WM_SETCURSOR 0x0020
#define WM_NCHITTEST 0x0084
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_COPYDATA 0x004A
#define WM_CONTEXTMENU 0x007B
#define WM_GETOBJECT 0x003D
#define WM_KEYFIRST 0x0100
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_UNICHAR 0x0109
#define WM_KEYLAST 0x0109
#define WM_TIMER 0x0113
#define WM_MOUSEFIRST 0x0200
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSEWHEEL 0x020A
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_XBUTTONDBLCLK 0x020D
#define WM_MOUSEHWHEEL 0x020E
#define WM_MOUSELAST 0x020E
#define WM_CAPTURECHANGED 0x0215
#define WM_ENTERSIZEMOVE 0x0231
#define WM_EXITSIZEMOVE 0x0232
#define WM_DROPFILES 0x0233
#define WM_POINTERDOWN 0x0246
#define WM_MOUSELEAVE 0x02A3
#define WM_DPICHANGED 0x02E0
#define WM_CUT 0x0300
#define WM_UNDO 0x0304
#define WM_IME_STARTCOMPOSITION 0x010D
#define WM_IME_ENDCOMPOSITION 0x010E
#define WM_IME_COMPOSITION 0x010F
#define WM_IME_SETCONTEXT 0x0281
#define WM_IME_KEYUP 0x0291

// Virtual keys.
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F4 0x73
#define VK_F6 0x75
#define VK_F10 0x79
#define VK_F24 0x87
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE

// Message parameters.
#define MK_LBUTTON 0x0001
#define MK_SHIFT 0x0004
#define XBUTTON1 0x0001
#define HTCLIENT 1
#define WHEEL_DELTA 120
#define WHEEL_PAGESCROLL (UINT_MAX)
#define TME_LEAVE 0x00000002
#define WA_INACTIVE 0
#define SPI_GETWHEELSCROLLLINES 0x0068
#define SPI_GETCLIENTAREAANIMATION 0x1042
#define MAPVK_VK_TO_VSC (0)
#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define MAKELONG(a,b) ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define MAKELPARAM(l,h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define MAKEWPARAM(l,h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#define GET_XBUTTON_WPARAM(wParam) (HIWORD(wParam))

extern "C" __declspec(dllimport) SHORT WINAPI GetKeyState(int nVirtKey);
extern "C" __declspec(dllimport) SHORT WINAPI GetAsyncKeyState(int vKey);
extern "C" __declspec(dllimport) BOOL WINAPI GetKeyboardState(BYTE* lpKeyState);
extern "C" __declspec(dllimport) BOOL WINAPI SetKeyboardState(BYTE* lpKeyState);
extern "C" __declspec(dllimport) UINT WINAPI MapVirtualKeyW(UINT uCode, UINT uMapType);
extern "C" __declspec(dllimport) HWND WINAPI GetCapture();
extern "C" __declspec(dllimport) HWND WINAPI SetCapture(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI ReleaseCapture();
extern "C" __declspec(dllimport) HWND WINAPI GetFocus();
extern "C" __declspec(dllimport) HWND WINAPI SetFocus(HWND hWnd);
extern "C" __declspec(dllimport) HWND WINAPI GetForegroundWindow();
extern "C" __declspec(dllimport) BOOL WINAPI SetForegroundWindow(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI AllowSetForegroundWindow(DWORD dwProcessId);
extern "C" __declspec(dllimport) BOOL WINAPI GetCursorPos(POINT* lpPoint);
extern "C" __declspec(dllimport) BOOL WINAPI ScreenToClient(HWND hWnd, POINT* lpPoint);
extern "C" __declspec(dllimport) BOOL WINAPI ClientToScreen(HWND hWnd, POINT* lpPoint);
extern "C" __declspec(dllimport) DWORD WINAPI GetMessagePos();
extern "C" __declspec(dllimport) BOOL WINAPI PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
extern "C" __declspec(dllimport) LRESULT WINAPI SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
extern "C" __declspec(dllimport) BOOL WINAPI IsWindow(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI IsWindowVisible(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI IsIconic(HWND hWnd);
extern "C" __declspec(dllimport) UINT WINAPI GetDpiForWindow(HWND hwnd);
extern "C" __declspec(dllimport) UINT WINAPI GetCaretBlinkTime();
extern "C" __declspec(dllimport) BOOL WINAPI MessageBeep(UINT uType);
extern "C" __declspec(dllimport) BOOL WINAPI SystemParametersInfoW(UINT uiAction, UINT uiParam, void* pvParam,
    UINT fWinIni);
extern "C" __declspec(dllimport) BOOL WINAPI TrackMouseEvent(TRACKMOUSEEVENT* lpEventTrack);
extern "C" __declspec(dllimport) LONG WINAPI GetMessageTime();
extern "C" __declspec(dllimport) UINT WINAPI GetDoubleClickTime();

// Windows and cursors.
#define WM_APP 0x8000
#define MAKEINTRESOURCEW(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE MAKEINTRESOURCEW
#define IDC_ARROW MAKEINTRESOURCE(32512)
#define IDC_IBEAM MAKEINTRESOURCE(32513)
#define IDC_SIZEWE MAKEINTRESOURCE(32644)
#define IDC_SIZENS MAKEINTRESOURCE(32645)
#define IDC_SIZEALL MAKEINTRESOURCE(32646)
#define IDC_HAND MAKEINTRESOURCE(32649)
#define SM_CXDOUBLECLK 36
#define SM_CYDOUBLECLK 37
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOW 5
#define SW_RESTORE 9
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_NOOWNERZORDER 0x0200
#define HWND_TOP ((HWND)0)
#define GW_OWNER 4
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWLP_USERDATA (-21)
extern "C" __declspec(dllimport) HCURSOR WINAPI SetCursor(HCURSOR hCursor);
extern "C" __declspec(dllimport) HCURSOR WINAPI LoadCursorW(HINSTANCE hInstance, const wchar_t* lpCursorName);
extern "C" __declspec(dllimport) int WINAPI GetSystemMetrics(int nIndex);
extern "C" __declspec(dllimport) BOOL WINAPI GetClientRect(HWND hWnd, RECT* lpRect);
extern "C" __declspec(dllimport) BOOL WINAPI GetWindowRect(HWND hWnd, RECT* lpRect);
extern "C" __declspec(dllimport) BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow);
extern "C" __declspec(dllimport) BOOL WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx,
    int cy, UINT uFlags);
extern "C" __declspec(dllimport) HWND WINAPI GetWindow(HWND hWnd, UINT uCmd);
extern "C" __declspec(dllimport) LONG_PTR WINAPI GetWindowLongPtrW(HWND hWnd, int nIndex);
extern "C" __declspec(dllimport) LONG_PTR WINAPI SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong);

namespace maicord {

// TRACKMOUSEEVENT.
struct Win32TrackMouseEvent {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;

    TRACKMOUSEEVENT* native() noexcept {
        return reinterpret_cast<TRACKMOUSEEVENT*>(this);
    }
};

} // namespace maicord
