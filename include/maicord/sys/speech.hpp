#pragma once

// SAPI text-to-speech, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <sapi.h>
#endif

typedef WCHAR SPPHONEID;

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
enum SPVISEMES {
    SP_VISEME_0 = 0,
    SP_VISEME_1 = (SP_VISEME_0 + 1),
    SP_VISEME_2 = (SP_VISEME_1 + 1),
    SP_VISEME_3 = (SP_VISEME_2 + 1),
    SP_VISEME_4 = (SP_VISEME_3 + 1),
    SP_VISEME_5 = (SP_VISEME_4 + 1),
    SP_VISEME_6 = (SP_VISEME_5 + 1),
    SP_VISEME_7 = (SP_VISEME_6 + 1),
    SP_VISEME_8 = (SP_VISEME_7 + 1),
    SP_VISEME_9 = (SP_VISEME_8 + 1),
    SP_VISEME_10 = (SP_VISEME_9 + 1),
    SP_VISEME_11 = (SP_VISEME_10 + 1),
    SP_VISEME_12 = (SP_VISEME_11 + 1),
    SP_VISEME_13 = (SP_VISEME_12 + 1),
    SP_VISEME_14 = (SP_VISEME_13 + 1),
    SP_VISEME_15 = (SP_VISEME_14 + 1),
    SP_VISEME_16 = (SP_VISEME_15 + 1),
    SP_VISEME_17 = (SP_VISEME_16 + 1),
    SP_VISEME_18 = (SP_VISEME_17 + 1),
    SP_VISEME_19 = (SP_VISEME_18 + 1),
    SP_VISEME_20 = (SP_VISEME_19 + 1),
};
struct __declspec(uuid("BE7A9CCE-5F9E-11D2-960F-00C04F8EE628")) __declspec(novtable) ISpEventSource : ISpNotifySource {
    MAICORD_COM_UNUSED(SetInterest);
    MAICORD_COM_UNUSED(GetEvents);
    MAICORD_COM_UNUSED(GetInfo);
};
struct SPVOICESTATUS {
    ULONG ulCurrentStream;
    ULONG ulLastStreamQueued;
    HRESULT hrLastResult;
    DWORD dwRunningState;
    ULONG ulInputWordPos;
    ULONG ulInputWordLen;
    ULONG ulInputSentPos;
    ULONG ulInputSentLen;
    LONG lBookmarkId;
    SPPHONEID PhonemeId;
    SPVISEMES VisemeId;
    DWORD dwReserved1;
    DWORD dwReserved2;
};
enum SPEAKFLAGS {
    SPF_ASYNC = (1L << 0),
    SPF_PURGEBEFORESPEAK = (1L << 1),
    SPF_IS_FILENAME = (1L << 2),
    SPF_IS_XML = (1L << 3),
    SPF_IS_NOT_XML = (1L << 4),
    SPF_PERSIST_XML = (1L << 5),
    SPF_NLP_SPEAK_PUNC = (1L << 6),
    SPF_PARSE_SAPI = (1L << 7),
    SPF_PARSE_SSML = (1L << 8),
    SPF_NLP_MASK = SPF_NLP_SPEAK_PUNC,
    SPF_PARSE_MASK = (SPF_PARSE_SAPI | SPF_PARSE_SSML),
    SPF_VOICE_MASK = (((((((SPF_ASYNC | SPF_PURGEBEFORESPEAK) | SPF_IS_FILENAME) | SPF_IS_XML) | SPF_IS_NOT_XML) | SPF_NLP_MASK) | SPF_PERSIST_XML) | SPF_PARSE_MASK),
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
    virtual HRESULT __stdcall GetStatus(SPVOICESTATUS* pStatus, LPWSTR* ppszLastBookmark) = 0;
};
MAICORD_SYS_TYPES_END
