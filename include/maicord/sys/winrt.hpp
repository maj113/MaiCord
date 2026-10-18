#pragma once

// Windows Runtime activation and HSTRING, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <NotificationActivationCallback.h>
#endif

struct HSTRING__;
typedef HSTRING__* HSTRING;

MAICORD_SYS_TYPES_BEGIN
struct HSTRING_HEADER;
struct INotificationActivationCallback;
struct NOTIFICATION_USER_INPUT_DATA;

// <NotificationActivationCallback.h>
struct NOTIFICATION_USER_INPUT_DATA {
    LPCWSTR Key;
    LPCWSTR Value;
};
struct __declspec(uuid("53E31837-6600-4A81-9395-75CFFE746F94")) __declspec(novtable) INotificationActivationCallback : IUnknown {
    virtual HRESULT __stdcall Activate(LPCWSTR appUserModelId, LPCWSTR invokedArgs,
        const NOTIFICATION_USER_INPUT_DATA* data, ULONG count) = 0;
};
// <hstring.h>
struct HSTRING_HEADER {
    union {
        PVOID Reserved1;
        char Reserved2[24];
    } Reserved;
};
// <roapi.h>
enum RO_INIT_TYPE {
    RO_INIT_SINGLETHREADED = 0,
    RO_INIT_MULTITHREADED = 1
};
MAICORD_SYS_TYPES_END

// <roapi.h>
extern "C" __declspec(dllimport) HRESULT WINAPI RoInitialize(RO_INIT_TYPE initType);
extern "C" __declspec(dllimport) void WINAPI RoUninitialize();
extern "C" __declspec(dllimport) HRESULT WINAPI RoGetActivationFactory(HSTRING activatableClassId, const IID& iid,
    void** factory);

// <winstring.h>
extern "C" HRESULT WINAPI WindowsDeleteString(HSTRING string);

// Inline definitions; the SDK's own with MAICORD_SDK_INCLUDES.
#if !defined(MAICORD_SDK_INCLUDES)
__inline HRESULT Initialize(  RO_INIT_TYPE initType
             = RO_INIT_SINGLETHREADED
            )
        {
            return RoInitialize(initType);
        }
__inline void Uninitialize()
        {
            RoUninitialize();
        }
#endif
