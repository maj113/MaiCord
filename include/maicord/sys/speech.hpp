#pragma once

// SAPI text-to-speech, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <sapi.h>
#endif

MAICORD_SYS_TYPES_BEGIN
struct ISpEventSource;
struct ISpNotifySource;
struct ISpVoice;

// <sapi.h>
struct __declspec(uuid("5EFF4AEF-8487-11D2-961C-00C04F8EE628")) __declspec(novtable) ISpNotifySource : IUnknown {
    MAICORD_COM_UNUSED(SetNotifySink);
    MAICORD_COM_UNUSED(SetNotifyWindowMessage);
    MAICORD_COM_UNUSED(SetNotifyCallbackFunction);
    MAICORD_COM_UNUSED(SetNotifyCallbackInterface);
    MAICORD_COM_UNUSED(SetNotifyWin32Event);
    MAICORD_COM_UNUSED(WaitForNotifyEvent);
    MAICORD_COM_UNUSED(GetNotifyEventHandle);
};
struct __declspec(uuid("BE7A9CCE-5F9E-11D2-960F-00C04F8EE628")) __declspec(novtable) ISpEventSource : ISpNotifySource {
    MAICORD_COM_UNUSED(SetInterest);
    MAICORD_COM_UNUSED(GetEvents);
    MAICORD_COM_UNUSED(GetInfo);
};
enum SPEAKFLAGS {
    SPF_ASYNC = (1L << 0),
    SPF_PURGEBEFORESPEAK = (1L << 1),
    SPF_IS_NOT_XML = (1L << 4),
};
struct __declspec(uuid("6C44DF74-72B9-4992-A1EC-EF996E0422D4")) __declspec(novtable) ISpVoice : ISpEventSource {
    MAICORD_COM_UNUSED(SetOutput);
    MAICORD_COM_UNUSED(GetOutputObjectToken);
    MAICORD_COM_UNUSED(GetOutputStream);
    MAICORD_COM_UNUSED(Pause);
    MAICORD_COM_UNUSED(Resume);
    MAICORD_COM_UNUSED(SetVoice);
    MAICORD_COM_UNUSED(GetVoice);
    virtual HRESULT __stdcall Speak(LPCWSTR pwcs, DWORD dwFlags, ULONG* pulStreamNumber) = 0;
    MAICORD_COM_UNUSED(SpeakStream);
    MAICORD_COM_UNUSED(GetStatus);
};
MAICORD_SYS_TYPES_END
