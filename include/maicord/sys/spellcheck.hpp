#pragma once

// The spell checking API, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/ole.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <spellcheck.h>
#endif

MAICORD_SYS_TYPES_BEGIN
struct IEnumSpellingError;
struct ISpellChecker;
struct ISpellCheckerFactory;
struct ISpellingError;

// <spellcheck.h>
enum CORRECTIVE_ACTION {
    CORRECTIVE_ACTION_NONE = 0,
};
struct __declspec(uuid("B7C82D61-FBE8-4B47-9B27-6C0D2E0DE0A3")) __declspec(novtable) ISpellingError : IUnknown {
    MAICORD_COM_UNUSED(get_StartIndex);
    MAICORD_COM_UNUSED(get_Length);
    virtual HRESULT __stdcall get_CorrectiveAction(CORRECTIVE_ACTION* value) = 0;
};
struct __declspec(uuid("803E3BD4-2828-4410-8290-418D1D73C762")) __declspec(novtable) IEnumSpellingError : IUnknown {
    virtual HRESULT __stdcall Next(ISpellingError** value) = 0;
};
struct __declspec(uuid("B6FD0B71-E2BC-4653-8D05-F197E412770B")) __declspec(novtable) ISpellChecker : IUnknown {
    MAICORD_COM_UNUSED(get_LanguageTag);
    virtual HRESULT __stdcall Check(LPCWSTR text, IEnumSpellingError** value) = 0;
    virtual HRESULT __stdcall Suggest(LPCWSTR word, IEnumString** value) = 0;
    virtual HRESULT __stdcall Add(LPCWSTR word) = 0;
    virtual HRESULT __stdcall Ignore(LPCWSTR word) = 0;
};
struct __declspec(uuid("8E018A9D-2415-4677-BF08-794EA61F94BB")) __declspec(novtable) ISpellCheckerFactory : IUnknown {
    MAICORD_COM_UNUSED(get_SupportedLanguages);
    virtual HRESULT __stdcall IsSupported(LPCWSTR languageTag, BOOL* value) = 0;
    virtual HRESULT __stdcall CreateSpellChecker(LPCWSTR languageTag, ISpellChecker** value) = 0;
};
MAICORD_SYS_TYPES_END
