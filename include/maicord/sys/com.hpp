#pragma once

// COM: GUIDs, IUnknown and the COM runtime, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/sys/crt.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <objbase.h>
#endif

// <guiddef.h>
#define REFGUID const GUID &
#define REFIID const IID &
#define REFCLSID const IID &
#define IsEqualIID(riid1, riid2) IsEqualGUID(riid1, riid2)

// <combaseapi.h>
#define IID_PPV_ARGS(ppType) __uuidof(**(ppType)), IID_PPV_ARGS_Helper(ppType)

// <guiddef.h>
#if !defined(MAICORD_SDK_INCLUDES)
#if defined(INITGUID)
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)                                                  \
    extern "C" const GUID __declspec(selectany) name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}
#else
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) extern "C" const GUID name
#endif
#endif

struct CO_MTA_USAGE_COOKIE__;
typedef WCHAR OLECHAR;
typedef OLECHAR* LPOLESTR;
typedef double DOUBLE;
typedef struct CO_MTA_USAGE_COOKIE__* CO_MTA_USAGE_COOKIE;

MAICORD_SYS_TYPES_BEGIN
struct IClassFactory;
struct IUnknown;

// <guiddef.h>
struct _GUID {
    unsigned long Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
};
typedef _GUID GUID;
typedef GUID* LPGUID;
typedef const GUID* LPCGUID;
typedef GUID IID;
typedef GUID CLSID;
// <combaseapi.h>
enum tagREGCLS {
    REGCLS_MULTIPLEUSE = 1,
};
enum tagCOINITBASE {
    COINITBASE_MULTITHREADED = 0x0
};
// <wtypesbase.h>
enum tagCLSCTX {
    CLSCTX_INPROC_SERVER = 0x1,
    CLSCTX_LOCAL_SERVER = 0x4,
    CLSCTX_ACTIVATE_X86_SERVER = 0x40000,
    CLSCTX_PS_DLL = 0x80000000
};
struct tagBLOB {
    ULONG cbSize;
    BYTE* pBlobData;
};
typedef tagBLOB BLOB;
// <unknwn.h>
typedef IUnknown* LPUNKNOWN;
struct __declspec(uuid("00000000-0000-0000-C000-000000000046")) __declspec(novtable) IUnknown {
    virtual HRESULT __stdcall QueryInterface(const IID& riid, void** ppvObject) = 0;
    virtual ULONG __stdcall AddRef() = 0;
    virtual ULONG __stdcall Release() = 0;
};
struct __declspec(uuid("00000001-0000-0000-C000-000000000046")) __declspec(novtable) IClassFactory : IUnknown {
    virtual HRESULT __stdcall CreateInstance(IUnknown* pUnkOuter, const IID& riid, void** ppvObject) = 0;
    virtual HRESULT __stdcall LockServer(BOOL fLock) = 0;
};
// <objbase.h>
enum tagCOINIT {
    COINIT_APARTMENTTHREADED = 0x2,
    COINIT_MULTITHREADED = COINITBASE_MULTITHREADED,
    COINIT_DISABLE_OLE1DDE = 0x4,
};
MAICORD_SYS_TYPES_END

// <combaseapi.h>
extern "C" __declspec(dllimport) void WINAPI CoUninitialize();
extern "C" __declspec(dllimport) HRESULT WINAPI CoInitializeEx(LPVOID pvReserved, DWORD dwCoInit);
extern "C" __declspec(dllimport) HRESULT WINAPI CoIncrementMTAUsage(CO_MTA_USAGE_COOKIE* pCookie);
extern "C" __declspec(dllimport) HRESULT WINAPI CoDecrementMTAUsage(CO_MTA_USAGE_COOKIE Cookie);
extern "C" __declspec(dllimport) HRESULT WINAPI CoRegisterClassObject(const IID& rclsid, LPUNKNOWN pUnk,
    DWORD dwClsContext, DWORD flags, LPDWORD lpdwRegister);
extern "C" __declspec(dllimport) HRESULT WINAPI CoRevokeClassObject(DWORD dwRegister);
extern "C" __declspec(dllimport) HRESULT WINAPI CoCreateInstance(const IID& rclsid, LPUNKNOWN pUnkOuter,
    DWORD dwClsContext, const IID& riid, LPVOID* ppv);
extern "C" __declspec(dllimport) int WINAPI StringFromGUID2(const GUID& rguid, LPOLESTR lpsz, int cchMax);
extern "C" __declspec(dllimport) HRESULT WINAPI CoCreateGuid(GUID* pguid);
extern "C" __declspec(dllimport) LPVOID WINAPI CoTaskMemAlloc(SIZE_T cb);
extern "C" __declspec(dllimport) void WINAPI CoTaskMemFree(LPVOID pv);

// Inline definitions; the SDK's own with MAICORD_SDK_INCLUDES.
#if !defined(MAICORD_SDK_INCLUDES)
extern "C" __inline int IsEqualGUID(const GUID & rguid1, const GUID & rguid2)
{
    return !memcmp(&rguid1, &rguid2, sizeof(GUID));
}
#endif

// Inline definitions; the SDK's own with MAICORD_SDK_INCLUDES.
#if !defined(MAICORD_SDK_INCLUDES)
// <guiddef.h>
__inline bool operator==(const GUID& guidOne, const GUID& guidOther) {
    return !!IsEqualGUID(guidOne, guidOther);
}
__inline bool operator!=(const GUID& guidOne, const GUID& guidOther) {
    return !(guidOne == guidOther);
}

// <combaseapi.h>
extern "C++" {
template <typename T> void** IID_PPV_ARGS_Helper(T** pp) {
    (void)static_cast<IUnknown*>(*pp);
    return reinterpret_cast<void**>(pp);
}
}
#endif
