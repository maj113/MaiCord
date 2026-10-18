#pragma once

// user32: windows, messages, input and the clipboard, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_types.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#endif

// <winuser.h>
#define MAKEINTRESOURCEA(i) ((LPSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCEW(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE MAKEINTRESOURCEW
#define RT_RCDATA MAKEINTRESOURCE(10)
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_SHOWMINNOACTIVE 7
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10
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
#define VK_NUMPAD5 0x65
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
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F12 0x7B
#define VK_F13 0x7C
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
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWLP_USERDATA (-21)
#define WM_NULL 0x0000
#define WM_DESTROY 0x0002
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_WININICHANGE 0x001A
#define WM_SETTINGCHANGE WM_WININICHANGE
#define WM_CANCELMODE 0x001F
#define WM_SETCURSOR 0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_GETOBJECT 0x003D
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_COPYDATA 0x004A
#define WM_CONTEXTMENU 0x007B
#define WM_SETICON 0x0080
#define WM_NCCREATE 0x0081
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCACTIVATE 0x0086
#define WM_NCMOUSEMOVE 0x00A0
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_NCLBUTTONUP 0x00A2
#define WM_NCLBUTTONDBLCLK 0x00A3
#define WM_NCRBUTTONDOWN 0x00A4
#define WM_NCRBUTTONUP 0x00A5
#define WM_KEYFIRST 0x0100
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_UNICHAR 0x0109
#define WM_KEYLAST 0x0109
#define WM_IME_STARTCOMPOSITION 0x010D
#define WM_IME_ENDCOMPOSITION 0x010E
#define WM_IME_COMPOSITION 0x010F
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
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
#define WHEEL_DELTA 120
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#define WHEEL_PAGESCROLL (UINT_MAX)
#define GET_XBUTTON_WPARAM(wParam) (HIWORD(wParam))
#define XBUTTON1 0x0001
#define WM_CAPTURECHANGED 0x0215
#define WM_ENTERSIZEMOVE 0x0231
#define WM_EXITSIZEMOVE 0x0232
#define WM_DROPFILES 0x0233
#define WM_POINTERDOWN 0x0246
#define WM_IME_SETCONTEXT 0x0281
#define WM_IME_KEYUP 0x0291
#define WM_NCMOUSEHOVER 0x02A0
#define WM_NCMOUSELEAVE 0x02A2
#define WM_MOUSELEAVE 0x02A3
#define WM_DPICHANGED 0x02E0
#define WM_CUT 0x0300
#define WM_UNDO 0x0304
#define WM_GETTITLEBARINFOEX 0x033F
#define WM_APP 0x8000
#define WM_USER 0x0400
#define HTNOWHERE 0
#define HTCLIENT 1
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTMINBUTTON 8
#define HTMAXBUTTON 9
#define HTLEFT 10
#define HTRIGHT 11
#define HTTOP 12
#define HTTOPLEFT 13
#define HTTOPRIGHT 14
#define HTBOTTOM 15
#define HTBOTTOMLEFT 16
#define HTBOTTOMRIGHT 17
#define HTCLOSE 20
#define SMTO_NORMAL 0x0000
#define SMTO_BLOCK 0x0001
#define SMTO_ABORTIFHUNG 0x0002
#define ICON_SMALL 0
#define ICON_BIG 1
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define MK_LBUTTON 0x0001
#define MK_SHIFT 0x0004
#define TME_LEAVE 0x00000002
#define TME_NONCLIENT 0x00000010
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_VISIBLE 0x10000000L
#define WS_CAPTION 0x00C00000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_ACCEPTFILES 0x00000010L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#define WS_EX_NOACTIVATE 0x08000000L
#define CS_DBLCLKS 0x0008
#define CF_DIB 8
#define CF_UNICODETEXT 13
#define CF_HDROP 15
#define CF_DIBV5 17
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define PM_NOYIELD 0x0002
#define HWND_MESSAGE ((HWND)-3)
#define CW_USEDEFAULT ((int)0x80000000)
#define FLASHW_STOP 0
#define FLASHW_TRAY 0x00000002
#define FLASHW_TIMERNOFG 0x0000000C
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_NOOWNERZORDER 0x0200
#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)1)
#define MAPVK_VK_TO_VSC (0)
#define MWMO_INPUTAVAILABLE 0x0004
#define QS_KEY 0x0001
#define QS_MOUSEMOVE 0x0002
#define QS_MOUSEBUTTON 0x0004
#define QS_POSTMESSAGE 0x0008
#define QS_TIMER 0x0010
#define QS_PAINT 0x0020
#define QS_SENDMESSAGE 0x0040
#define QS_HOTKEY 0x0080
#define QS_RAWINPUT 0x0400
#define QS_TOUCH 0x0800
#define QS_POINTER 0x1000
#define QS_MOUSE (QS_MOUSEMOVE | QS_MOUSEBUTTON)
#define QS_INPUT (QS_MOUSE | QS_KEY | QS_RAWINPUT | QS_TOUCH | QS_POINTER)
#define QS_ALLINPUT (QS_INPUT | QS_POSTMESSAGE | QS_TIMER | QS_PAINT | QS_HOTKEY | QS_SENDMESSAGE)
#define SM_CXICON 11
#define SM_CXFRAME 32
#define SM_CXDOUBLECLK 36
#define SM_CYDOUBLECLK 37
#define SM_MENUDROPALIGNMENT 40
#define SM_CXSMICON 49
#define SM_CXPADDEDBORDER 92
#define TPM_RIGHTBUTTON 0x0002L
#define TPM_LEFTALIGN 0x0000L
#define TPM_RIGHTALIGN 0x0008L
#define TPM_BOTTOMALIGN 0x0020L
#define TPM_NONOTIFY 0x0080L
#define TPM_RETURNCMD 0x0100L
#define MB_OK 0x00000000L
#define MB_ICONHAND 0x00000010L
#define MB_ICONERROR MB_ICONHAND
#define GW_OWNER 4
#define MF_SEPARATOR 0x00000800L
#define MF_UNCHECKED 0x00000000L
#define MF_CHECKED 0x00000008L
#define MF_STRING 0x00000000L
#define SC_MINIMIZE 0xF020
#define SC_MAXIMIZE 0xF030
#define SC_CLOSE 0xF060
#define SC_RESTORE 0xF120
#define SC_KEYMENU 0xF100
#define IDC_ARROW MAKEINTRESOURCE(32512)
#define IDC_IBEAM MAKEINTRESOURCE(32513)
#define IDC_SIZEWE MAKEINTRESOURCE(32644)
#define IDC_SIZENS MAKEINTRESOURCE(32645)
#define IDC_SIZEALL MAKEINTRESOURCE(32646)
#define IDC_HAND MAKEINTRESOURCE(32649)
#define LR_DEFAULTCOLOR 0x00000000
#define GR_GDIOBJECTS 0
#define GR_USEROBJECTS 1
#define SPI_GETWHEELSCROLLLINES 0x0068
#define SPI_GETCLIENTAREAANIMATION 0x1042
#define MONITOR_DEFAULTTONEAREST 0x00000002

typedef void(__stdcall* TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);
typedef BOOL(__stdcall* WNDENUMPROC)(HWND, LPARAM);

MAICORD_SYS_TYPES_BEGIN
struct tagCREATESTRUCTW;
struct tagLASTINPUTINFO;
struct tagMONITORINFO;
struct tagNCCALCSIZE_PARAMS;
struct tagPAINTSTRUCT;
struct tagTITLEBARINFOEX;
struct tagTPMPARAMS;
struct tagTRACKMOUSEEVENT;
struct tagWINDOWPLACEMENT;
struct tagWINDOWPOS;
struct tagWNDCLASSEXW;
struct tagWNDCLASSW;

// <winuser.h>
struct tagWNDCLASSEXW {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
};
typedef tagWNDCLASSEXW WNDCLASSEXW;
struct tagWNDCLASSW {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
};
typedef tagWNDCLASSW WNDCLASSW;
typedef tagMSG* LPMSG;
struct tagWINDOWPOS {
    HWND hwnd;
    HWND hwndInsertAfter;
    int x;
    int y;
    int cx;
    int cy;
    UINT flags;
};
typedef tagWINDOWPOS WINDOWPOS;
typedef tagWINDOWPOS* PWINDOWPOS;
struct tagNCCALCSIZE_PARAMS {
    RECT rgrc[3];
    PWINDOWPOS lppos;
};
typedef tagNCCALCSIZE_PARAMS NCCALCSIZE_PARAMS;
struct tagTRACKMOUSEEVENT {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
};
typedef tagTRACKMOUSEEVENT TRACKMOUSEEVENT;
typedef tagTRACKMOUSEEVENT* LPTRACKMOUSEEVENT;
struct tagPAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
};
typedef tagPAINTSTRUCT PAINTSTRUCT;
typedef tagPAINTSTRUCT* LPPAINTSTRUCT;
struct tagCREATESTRUCTW {
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy;
    int cx;
    int y;
    int x;
    LONG style;
    LPCWSTR lpszName;
    LPCWSTR lpszClass;
    DWORD dwExStyle;
};
typedef tagCREATESTRUCTW CREATESTRUCTW;
struct tagWINDOWPLACEMENT {
    UINT length;
    UINT flags;
    UINT showCmd;
    POINT ptMinPosition;
    POINT ptMaxPosition;
    RECT rcNormalPosition;
};
typedef tagWINDOWPLACEMENT WINDOWPLACEMENT;
typedef struct {
    UINT cbSize;
    HWND hwnd;
    DWORD dwFlags;
    UINT uCount;
    DWORD dwTimeout;
} FLASHWINFO;
typedef FLASHWINFO* PFLASHWINFO;
struct tagLASTINPUTINFO {
    UINT cbSize;
    DWORD dwTime;
};
typedef tagLASTINPUTINFO LASTINPUTINFO;
typedef tagLASTINPUTINFO* PLASTINPUTINFO;
struct tagTPMPARAMS {
    UINT cbSize;
    RECT rcExclude;
};
typedef tagTPMPARAMS TPMPARAMS;
typedef TPMPARAMS* LPTPMPARAMS;
struct tagMONITORINFO {
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
};
typedef tagMONITORINFO MONITORINFO;
typedef tagMONITORINFO* LPMONITORINFO;
struct tagTITLEBARINFOEX {
    DWORD cbSize;
    RECT rcTitleBar;
    DWORD rgstate[6];
    RECT rgrect[6];
};
typedef tagTITLEBARINFOEX TITLEBARINFOEX;
typedef tagTITLEBARINFOEX* PTITLEBARINFOEX;
MAICORD_SYS_TYPES_END

// <winuser.h>
extern "C" __declspec(dllimport) int __cdecl wsprintfA(LPSTR, LPCSTR, ...);
extern "C" __declspec(dllimport) int WINAPI ToUnicodeEx(UINT wVirtKey, UINT wScanCode, const BYTE* lpKeyState,
    LPWSTR pwszBuff, int cchBuff, UINT wFlags, HKL dwhkl);
extern "C" __declspec(dllimport) HKL WINAPI GetKeyboardLayout(DWORD idThread);
extern "C" __declspec(dllimport) UINT WINAPI RegisterWindowMessageW(LPCWSTR lpString);
extern "C" __declspec(dllimport) BOOL WINAPI TrackMouseEvent(LPTRACKMOUSEEVENT lpEventTrack);
extern "C" __declspec(dllimport) BOOL WINAPI GetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin,
    UINT wMsgFilterMax);
extern "C" __declspec(dllimport) BOOL WINAPI TranslateMessage(const MSG* lpMsg);
extern "C" __declspec(dllimport) LRESULT WINAPI DispatchMessageW(const MSG* lpMsg);
extern "C" __declspec(dllimport) BOOL WINAPI PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin,
    UINT wMsgFilterMax, UINT wRemoveMsg);
extern "C" __declspec(dllimport) DWORD WINAPI GetMessagePos();
extern "C" __declspec(dllimport) LONG WINAPI GetMessageTime();
extern "C" __declspec(dllimport) LRESULT WINAPI SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
extern "C" __declspec(dllimport) LRESULT WINAPI SendMessageTimeoutW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam,
    UINT fuFlags, UINT uTimeout, PDWORD_PTR lpdwResult);
extern "C" __declspec(dllimport) BOOL WINAPI PostMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
extern "C" __declspec(dllimport) BOOL WINAPI PostThreadMessageW(DWORD idThread, UINT Msg, WPARAM wParam, LPARAM lParam);
extern "C" __declspec(dllimport) LRESULT WINAPI DefWindowProcW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
extern "C" __declspec(dllimport) void WINAPI PostQuitMessage(int nExitCode);
extern "C" __declspec(dllimport) UINT WINAPI GetDoubleClickTime();
extern "C" __declspec(dllimport) ATOM WINAPI RegisterClassW(const WNDCLASSW* lpWndClass);
extern "C" __declspec(dllimport) BOOL WINAPI UnregisterClassW(LPCWSTR lpClassName, HINSTANCE hInstance);
extern "C" __declspec(dllimport) ATOM WINAPI RegisterClassExW(const WNDCLASSEXW*);
extern "C" __declspec(dllimport) HWND WINAPI CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName,
    LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu,
    HINSTANCE hInstance, LPVOID lpParam);
extern "C" __declspec(dllimport) BOOL WINAPI IsWindow(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI DestroyWindow(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI ShowWindow(HWND hWnd, int nCmdShow);
extern "C" __declspec(dllimport) BOOL WINAPI FlashWindowEx(PFLASHWINFO pfwi);
extern "C" __declspec(dllimport) BOOL WINAPI SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx,
    int cy, UINT uFlags);
extern "C" __declspec(dllimport) BOOL WINAPI GetWindowPlacement(HWND hWnd, WINDOWPLACEMENT* lpwndpl);
extern "C" __declspec(dllimport) BOOL WINAPI SetWindowPlacement(HWND hWnd, const WINDOWPLACEMENT* lpwndpl);
extern "C" __declspec(dllimport) BOOL WINAPI IsWindowVisible(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI IsIconic(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI IsZoomed(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI OpenClipboard(HWND hWndNewOwner);
extern "C" __declspec(dllimport) BOOL WINAPI CloseClipboard();
extern "C" __declspec(dllimport) DWORD WINAPI GetClipboardSequenceNumber();
extern "C" __declspec(dllimport) HANDLE WINAPI SetClipboardData(UINT uFormat, HANDLE hMem);
extern "C" __declspec(dllimport) HANDLE WINAPI GetClipboardData(UINT uFormat);
extern "C" __declspec(dllimport) UINT WINAPI RegisterClipboardFormatW(LPCWSTR lpszFormat);
extern "C" __declspec(dllimport) BOOL WINAPI EmptyClipboard();
extern "C" __declspec(dllimport) BOOL WINAPI IsClipboardFormatAvailable(UINT format);
extern "C" __declspec(dllimport) HWND WINAPI SetFocus(HWND hWnd);
extern "C" __declspec(dllimport) HWND WINAPI GetFocus();
extern "C" __declspec(dllimport) SHORT WINAPI GetKeyState(int nVirtKey);
extern "C" __declspec(dllimport) SHORT WINAPI GetAsyncKeyState(int vKey);
extern "C" __declspec(dllimport) BOOL WINAPI GetKeyboardState(PBYTE lpKeyState);
extern "C" __declspec(dllimport) BOOL WINAPI SetKeyboardState(LPBYTE lpKeyState);
extern "C" __declspec(dllimport) BOOL WINAPI GetLastInputInfo(PLASTINPUTINFO plii);
extern "C" __declspec(dllimport) UINT WINAPI MapVirtualKeyW(UINT uCode, UINT uMapType);
extern "C" __declspec(dllimport) HWND WINAPI GetCapture();
extern "C" __declspec(dllimport) HWND WINAPI SetCapture(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI ReleaseCapture();
extern "C" __declspec(dllimport) DWORD WINAPI MsgWaitForMultipleObjects(DWORD nCount, const HANDLE* pHandles,
    BOOL fWaitAll, DWORD dwMilliseconds, DWORD dwWakeMask);
extern "C" __declspec(dllimport) DWORD WINAPI MsgWaitForMultipleObjectsEx(DWORD nCount, const HANDLE* pHandles,
    DWORD dwMilliseconds, DWORD dwWakeMask, DWORD dwFlags);
extern "C" __declspec(dllimport) UINT_PTR WINAPI SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse,
    TIMERPROC lpTimerFunc);
extern "C" __declspec(dllimport) BOOL WINAPI KillTimer(HWND hWnd, UINT_PTR uIDEvent);
extern "C" __declspec(dllimport) int WINAPI GetSystemMetrics(int nIndex);
extern "C" __declspec(dllimport) int WINAPI GetSystemMetricsForDpi(int nIndex, UINT dpi);
extern "C" __declspec(dllimport) HMENU WINAPI CreatePopupMenu();
extern "C" __declspec(dllimport) BOOL WINAPI DestroyMenu(HMENU hMenu);
extern "C" __declspec(dllimport) BOOL WINAPI AppendMenuW(HMENU hMenu, UINT uFlags, UINT_PTR uIDNewItem,
    LPCWSTR lpNewItem);
extern "C" __declspec(dllimport) BOOL WINAPI TrackPopupMenuEx(HMENU hMenu, UINT uFlags, int x, int y, HWND hwnd,
    LPTPMPARAMS lptpm);
extern "C" __declspec(dllimport) BOOL WINAPI SetMenuDefaultItem(HMENU hMenu, UINT uItem, UINT fByPos);
extern "C" __declspec(dllimport) BOOL WINAPI UpdateWindow(HWND hWnd);
extern "C" __declspec(dllimport) HWND WINAPI GetForegroundWindow();
extern "C" __declspec(dllimport) BOOL WINAPI SetForegroundWindow(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI AllowSetForegroundWindow(DWORD dwProcessId);
extern "C" __declspec(dllimport) HDC WINAPI BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint);
extern "C" __declspec(dllimport) BOOL WINAPI EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint);
extern "C" __declspec(dllimport) BOOL WINAPI GetUpdateRect(HWND hWnd, LPRECT lpRect, BOOL bErase);
extern "C" __declspec(dllimport) BOOL WINAPI InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase);
extern "C" __declspec(dllimport) BOOL WINAPI ValidateRect(HWND hWnd, const RECT* lpRect);
extern "C" __declspec(dllimport) BOOL WINAPI SetWindowTextW(HWND hWnd, LPCWSTR lpString);
extern "C" __declspec(dllimport) int WINAPI GetWindowTextLengthW(HWND hWnd);
extern "C" __declspec(dllimport) BOOL WINAPI GetClientRect(HWND hWnd, LPRECT lpRect);
extern "C" __declspec(dllimport) BOOL WINAPI GetWindowRect(HWND hWnd, LPRECT lpRect);
extern "C" __declspec(dllimport) BOOL WINAPI AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, BOOL bMenu,
    DWORD dwExStyle);
extern "C" __declspec(dllimport) BOOL WINAPI AdjustWindowRectExForDpi(LPRECT lpRect, DWORD dwStyle, BOOL bMenu,
    DWORD dwExStyle, UINT dpi);
extern "C" __declspec(dllimport) int WINAPI MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption, UINT uType);
extern "C" __declspec(dllimport) BOOL WINAPI MessageBeep(UINT uType);
extern "C" __declspec(dllimport) HCURSOR WINAPI SetCursor(HCURSOR hCursor);
extern "C" __declspec(dllimport) BOOL WINAPI GetCursorPos(LPPOINT lpPoint);
extern "C" __declspec(dllimport) HCURSOR WINAPI GetCursor();
extern "C" __declspec(dllimport) UINT WINAPI GetCaretBlinkTime();
extern "C" __declspec(dllimport) BOOL WINAPI ClientToScreen(HWND hWnd, LPPOINT lpPoint);
extern "C" __declspec(dllimport) BOOL WINAPI ScreenToClient(HWND hWnd, LPPOINT lpPoint);
extern "C" __declspec(dllimport) HWND WINAPI WindowFromPoint(POINT Point);
extern "C" __declspec(dllimport) DWORD WINAPI GetSysColor(int nIndex);
extern "C" __declspec(dllimport) LONG WINAPI GetWindowLongW(HWND hWnd, int nIndex);
extern "C" __declspec(dllimport) LONG_PTR WINAPI GetWindowLongPtrW(HWND hWnd, int nIndex);
extern "C" __declspec(dllimport) LONG_PTR WINAPI SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong);
extern "C" __declspec(dllimport) HWND WINAPI GetParent(HWND hWnd);
extern "C" __declspec(dllimport) HWND WINAPI FindWindowExW(HWND hWndParent, HWND hWndChildAfter, LPCWSTR lpszClass,
    LPCWSTR lpszWindow);
extern "C" __declspec(dllimport) BOOL WINAPI EnumWindows(WNDENUMPROC lpEnumFunc, LPARAM lParam);
extern "C" __declspec(dllimport) BOOL WINAPI EnumThreadWindows(DWORD dwThreadId, WNDENUMPROC lpfn, LPARAM lParam);
extern "C" __declspec(dllimport) int WINAPI GetClassNameW(HWND hWnd, LPWSTR lpClassName, int nMaxCount);
extern "C" __declspec(dllimport) DWORD WINAPI GetWindowThreadProcessId(HWND hWnd, LPDWORD lpdwProcessId);
extern "C" __declspec(dllimport) HWND WINAPI GetWindow(HWND hWnd, UINT uCmd);
extern "C" __declspec(dllimport) HCURSOR WINAPI LoadCursorW(HINSTANCE hInstance, LPCWSTR lpCursorName);
extern "C" __declspec(dllimport) BOOL WINAPI DestroyIcon(HICON hIcon);
extern "C" __declspec(dllimport) HICON WINAPI CreateIconFromResourceEx(PBYTE presbits, DWORD dwResSize, BOOL fIcon,
    DWORD dwVer, int cxDesired, int cyDesired, UINT Flags);
extern "C" __declspec(dllimport) DWORD WINAPI GetGuiResources(HANDLE hProcess, DWORD uiFlags);
extern "C" __declspec(dllimport) BOOL WINAPI SystemParametersInfoW(UINT uiAction, UINT uiParam, PVOID pvParam,
    UINT fWinIni);
extern "C" __declspec(dllimport) HMONITOR WINAPI MonitorFromWindow(HWND hwnd, DWORD dwFlags);
extern "C" __declspec(dllimport) HMONITOR WINAPI MonitorFromRect(LPCRECT lprc, DWORD dwFlags);
extern "C" __declspec(dllimport) BOOL WINAPI GetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFO lpmi);
extern "C" __declspec(dllimport) UINT WINAPI GetDpiForWindow(HWND hwnd);

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
