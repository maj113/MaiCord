#pragma once

// Shell: shellapi, known folders, file dialogs and the taskbar, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/ole.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

// <shellapi.h>
#define NIN_SELECT (WM_USER + 0)
#define NINF_KEY 0x1
#define NIN_KEYSELECT (NIN_SELECT | NINF_KEY)
#define NIM_ADD 0x00000000
#define NIM_MODIFY 0x00000001
#define NIM_DELETE 0x00000002
#define NIM_SETVERSION 0x00000004
#define NOTIFYICON_VERSION_4 4
#define NIF_MESSAGE 0x00000001
#define NIF_ICON 0x00000002
#define NIF_TIP 0x00000004
#define NIF_SHOWTIP 0x00000080
#define ABM_GETAUTOHIDEBAREX 0x0000000b
#define ABE_LEFT 0
#define ABE_TOP 1
#define ABE_RIGHT 2
#define ABE_BOTTOM 3

// <shtypes.h>
#define PIDLIST_ABSOLUTE LPITEMIDLIST

// <shlobj_core.h>
#define CFSTR_FILEDESCRIPTORW TEXT("FileGroupDescriptorW")
#define CFSTR_FILECONTENTS TEXT("FileContents")

struct HDROP__;
typedef struct HDROP__* HDROP;
typedef DWORD SICHINTF;
typedef DWORD FILEOPENDIALOGOPTIONS;

MAICORD_SYS_TYPES_BEGIN
struct IFileDialog;
struct IFileDialogEvents;
struct IFileOpenDialog;
struct IModalWindow;
struct IShellItem;
struct IShellItemArray;
struct ITaskbarList2;
struct ITaskbarList3;
struct ITaskbarList;

// <shellapi.h>
struct _AppBarData {
    DWORD cbSize;
    HWND hWnd;
    UINT uCallbackMessage;
    UINT uEdge;
    RECT rc;
    LPARAM lParam;
};
typedef _AppBarData APPBARDATA;
typedef _AppBarData* PAPPBARDATA;
struct _NOTIFYICONDATAW {
    DWORD cbSize;
    HWND hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    HICON hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    union {
        UINT uTimeout;
        UINT uVersion;
    };
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    HICON hBalloonIcon;
};
typedef _NOTIFYICONDATAW NOTIFYICONDATAW;
typedef _NOTIFYICONDATAW* PNOTIFYICONDATAW;
// <shtypes.h>
#pragma pack(push, 1)
struct _SHITEMID {
    USHORT cb;
    BYTE abID[1];
};
#pragma pack(pop)
typedef _SHITEMID SHITEMID;
#pragma pack(push, 1)
struct _ITEMIDLIST {
    SHITEMID mkid;
};
#pragma pack(pop)
typedef _ITEMIDLIST ITEMIDLIST;
typedef ITEMIDLIST __unaligned* LPITEMIDLIST;
typedef const ITEMIDLIST __unaligned* LPCITEMIDLIST;
// <shobjidl_core.h>
enum _SIGDN {
    SIGDN_PARENTRELATIVEPARSING = (int) 0x80018001,
    SIGDN_DESKTOPABSOLUTEPARSING = (int) 0x80028000,
    SIGDN_PARENTRELATIVEEDITING = (int) 0x80031001,
    SIGDN_DESKTOPABSOLUTEEDITING = (int) 0x8004c000,
    SIGDN_FILESYSPATH = (int) 0x80058000,
    SIGDN_URL = (int) 0x80068000,
    SIGDN_PARENTRELATIVEFORADDRESSBAR = (int) 0x8007c001,
    SIGDN_PARENTRELATIVE = (int) 0x80080001,
    SIGDN_PARENTRELATIVEFORUI = (int) 0x80094001
};
typedef _SIGDN SIGDN;
struct __declspec(uuid("43826d1e-e718-42ee-bc55-a1e261c37bfe")) __declspec(novtable) IShellItem : IUnknown {
    virtual HRESULT __stdcall BindToHandler(IBindCtx* pbc, const GUID& bhid, const IID& riid, void** ppv) = 0;
    virtual HRESULT __stdcall GetParent(IShellItem** ppsi) = 0;
    virtual HRESULT __stdcall GetDisplayName(SIGDN sigdnName, LPWSTR* ppszName) = 0;
    MAICORD_COM_UNUSED(GetAttributes);
    virtual HRESULT __stdcall Compare(IShellItem* psi, SICHINTF hint, int* piOrder) = 0;
};
struct __declspec(uuid("b63ea76d-1f85-456f-a19c-48159efa858b")) __declspec(novtable) IShellItemArray : IUnknown {
    virtual HRESULT __stdcall BindToHandler(IBindCtx* pbc, const GUID& bhid, const IID& riid, void** ppvOut) = 0;
    MAICORD_COM_UNUSED(GetPropertyStore);
    MAICORD_COM_UNUSED(GetPropertyDescriptionList);
    MAICORD_COM_UNUSED(GetAttributes);
    virtual HRESULT __stdcall GetCount(DWORD* pdwNumItems) = 0;
    virtual HRESULT __stdcall GetItemAt(DWORD dwIndex, IShellItem** ppsi) = 0;
};
struct __declspec(uuid("56FDF342-FD6D-11d0-958A-006097C9A090")) __declspec(novtable) ITaskbarList : IUnknown {
    virtual HRESULT __stdcall HrInit() = 0;
    MAICORD_COM_UNUSED(AddTab);
    MAICORD_COM_UNUSED(DeleteTab);
    MAICORD_COM_UNUSED(ActivateTab);
    MAICORD_COM_UNUSED(SetActiveAlt);
};
struct __declspec(uuid("602D4995-B13A-429b-A66E-1935E44F4317")) __declspec(novtable) ITaskbarList2 : ITaskbarList {
    MAICORD_COM_UNUSED(MarkFullscreenWindow);
};
struct __declspec(uuid("ea1afb91-9e28-4b86-90e9-9e9f8a5eefaf")) __declspec(novtable) ITaskbarList3 : ITaskbarList2 {
    MAICORD_COM_UNUSED(SetProgressValue);
    MAICORD_COM_UNUSED(SetProgressState);
    MAICORD_COM_UNUSED(RegisterTab);
    MAICORD_COM_UNUSED(UnregisterTab);
    MAICORD_COM_UNUSED(SetTabOrder);
    MAICORD_COM_UNUSED(SetTabActive);
    MAICORD_COM_UNUSED(ThumbBarAddButtons);
    MAICORD_COM_UNUSED(ThumbBarUpdateButtons);
    MAICORD_COM_UNUSED(ThumbBarSetImageList);
    virtual HRESULT __stdcall SetOverlayIcon(HWND hwnd, HICON hIcon, LPCWSTR pszDescription) = 0;
};
struct __declspec(uuid("b4db1657-70d7-485e-8e3e-6fcb5a5c1802")) __declspec(novtable) IModalWindow : IUnknown {
    virtual HRESULT __stdcall Show(HWND hwndOwner) = 0;
};
enum FDE_OVERWRITE_RESPONSE {
    FDEOR_DEFAULT = 0,
    FDEOR_REFUSE = 2
};
enum FDE_SHAREVIOLATION_RESPONSE {
    FDESVR_DEFAULT = 0,
};
struct __declspec(uuid("973510db-7d7f-452b-8975-74a85828d354")) __declspec(novtable) IFileDialogEvents : IUnknown {
    virtual HRESULT __stdcall OnFileOk(IFileDialog* pfd) = 0;
    virtual HRESULT __stdcall OnFolderChanging(IFileDialog* pfd, IShellItem* psiFolder) = 0;
    virtual HRESULT __stdcall OnFolderChange(IFileDialog* pfd) = 0;
    virtual HRESULT __stdcall OnSelectionChange(IFileDialog* pfd) = 0;
    virtual HRESULT __stdcall OnShareViolation(IFileDialog* pfd, IShellItem* psi,
        FDE_SHAREVIOLATION_RESPONSE* pResponse) = 0;
    virtual HRESULT __stdcall OnTypeChange(IFileDialog* pfd) = 0;
    virtual HRESULT __stdcall OnOverwrite(IFileDialog* pfd, IShellItem* psi, FDE_OVERWRITE_RESPONSE* pResponse) = 0;
};
enum _FILEOPENDIALOGOPTIONS {
    FOS_OVERWRITEPROMPT = 0x2,
    FOS_NOCHANGEDIR = 0x8,
    FOS_FORCEFILESYSTEM = 0x40,
    FOS_ALLOWMULTISELECT = 0x200,
    FOS_PATHMUSTEXIST = 0x800,
    FOS_FILEMUSTEXIST = 0x1000,
    FOS_NOREADONLYRETURN = 0x8000,
    FOS_NOTESTFILECREATE = 0x10000,
    FOS_NODEREFERENCELINKS = 0x100000,
    FOS_SUPPORTSTREAMABLEITEMS = 0x80000000
};
struct __declspec(uuid("42f85136-db7e-439c-85f1-e4075d135fc8")) __declspec(novtable) IFileDialog : IModalWindow {
    MAICORD_COM_UNUSED(SetFileTypes);
    MAICORD_COM_UNUSED(SetFileTypeIndex);
    MAICORD_COM_UNUSED(GetFileTypeIndex);
    virtual HRESULT __stdcall Advise(IFileDialogEvents* pfde, DWORD* pdwCookie) = 0;
    virtual HRESULT __stdcall Unadvise(DWORD dwCookie) = 0;
    virtual HRESULT __stdcall SetOptions(FILEOPENDIALOGOPTIONS fos) = 0;
    virtual HRESULT __stdcall GetOptions(FILEOPENDIALOGOPTIONS* pfos) = 0;
    MAICORD_COM_UNUSED(SetDefaultFolder);
    MAICORD_COM_UNUSED(SetFolder);
    MAICORD_COM_UNUSED(GetFolder);
    MAICORD_COM_UNUSED(GetCurrentSelection);
    virtual HRESULT __stdcall SetFileName(LPCWSTR pszName) = 0;
    MAICORD_COM_UNUSED(GetFileName);
    virtual HRESULT __stdcall SetTitle(LPCWSTR pszTitle) = 0;
    MAICORD_COM_UNUSED(SetOkButtonLabel);
    MAICORD_COM_UNUSED(SetFileNameLabel);
    virtual HRESULT __stdcall GetResult(IShellItem** ppsi) = 0;
    MAICORD_COM_UNUSED(AddPlace);
    MAICORD_COM_UNUSED(SetDefaultExtension);
    virtual HRESULT __stdcall Close(HRESULT hr) = 0;
    MAICORD_COM_UNUSED(SetClientGuid);
    MAICORD_COM_UNUSED(ClearClientData);
    MAICORD_COM_UNUSED(SetFilter);
};
struct __declspec(uuid("d57c7288-d4ad-4768-be02-9d969532d960")) __declspec(novtable) IFileOpenDialog : IFileDialog {
    virtual HRESULT __stdcall GetResults(IShellItemArray** ppenum) = 0;
};
class __declspec(uuid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")) FileOpenDialog;
class __declspec(uuid("C0B4E2F3-BA21-4773-8DBA-335EC946EB8B")) FileSaveDialog;
// <shlobj_core.h>
#pragma pack(push, 1)
typedef enum {
    FD_ATTRIBUTES = 0x00000004,
    FD_FILESIZE = 0x00000040,
    FD_UNICODE = (int) 0x80000000
} FD_FLAGS;
#pragma pack(pop)
#pragma pack(push, 1)
struct _FILEDESCRIPTORW {
    DWORD dwFlags;
    CLSID clsid;
    SIZEL sizel;
    POINTL pointl;
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    WCHAR cFileName[260];
};
#pragma pack(pop)
typedef _FILEDESCRIPTORW FILEDESCRIPTORW;
#pragma pack(push, 1)
struct _FILEGROUPDESCRIPTORW {
    UINT cItems;
    FILEDESCRIPTORW fgd[1];
};
#pragma pack(pop)
typedef _FILEGROUPDESCRIPTORW FILEGROUPDESCRIPTORW;
#pragma pack(push, 1)
struct _DROPFILES {
    DWORD pFiles;
    POINT pt;
    BOOL fNC;
    BOOL fWide;
};
#pragma pack(pop)
typedef _DROPFILES DROPFILES;
MAICORD_SYS_TYPES_END

// <shellapi.h>
extern "C" __declspec(dllimport) UINT WINAPI DragQueryFileW(HDROP hDrop, UINT iFile, LPWSTR lpszFile, UINT cch);
extern "C" __declspec(dllimport) void WINAPI DragFinish(HDROP hDrop);
extern "C" __declspec(dllimport) UINT_PTR WINAPI SHAppBarMessage(DWORD dwMessage, PAPPBARDATA pData);
extern "C" __declspec(dllimport) HINSTANCE WINAPI ShellExecuteW(HWND hwnd, LPCWSTR lpOperation, LPCWSTR lpFile,
    LPCWSTR lpParameters, LPCWSTR lpDirectory, INT nShowCmd);
extern "C" __declspec(dllimport) BOOL WINAPI Shell_NotifyIconW(DWORD dwMessage, PNOTIFYICONDATAW lpData);

// <shobjidl_core.h>
extern "C" __declspec(dllimport) HRESULT WINAPI SHCreateItemFromParsingName(PCWSTR pszPath, IBindCtx* pbc,
    const IID& riid, void** ppv);
extern "C" __declspec(dllimport) HRESULT WINAPI SetCurrentProcessExplicitAppUserModelID(PCWSTR AppID);

// <shlguid.h>
extern "C" const GUID BHID_DataObject;

// <shlobj_core.h>
extern "C" __declspec(dllimport) void WINAPI ILFree(LPITEMIDLIST pidl);
extern "C" __declspec(dllimport) LPITEMIDLIST WINAPI ILCreateFromPathW(PCWSTR pszPath);
extern "C" __declspec(dllimport) HRESULT WINAPI SHOpenFolderAndSelectItems(LPCITEMIDLIST pidlFolder, UINT cidl,
    LPCITEMIDLIST* apidl, DWORD dwFlags);
