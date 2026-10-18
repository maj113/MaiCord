#include "maicord/platform/spell_checker.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/spellcheck.hpp"

namespace maicord {
namespace {
constexpr CLSID kSpellCheckerFactoryClass = {0x7AB36653, 0x1796, 0x484B, {0xBD, 0xFA, 0xE7, 0x4F, 0x1D, 0xB7, 0xC1, 0xDC}};
constexpr IID kSpellCheckerFactoryInterface = {0x8E018A9D, 0x2415, 0x4677, {0xBF, 0x08, 0x79, 0x4E, 0xA6, 0x1F, 0x94, 0xBB}};
constexpr u32 kWordChars = kSpellCheckerWordBytes + 1;

bool copy_tag(Slice<wchar_t> output, const wchar_t* tag, u32 length) noexcept {
    if (!length || length >= output.count)
        return false;
    copy_bytes(output.data, tag, length * sizeof(wchar_t));
    output[length] = 0;
    return true;
}
// UTF-8 word to a NUL-terminated UTF-16 buffer; rejects empty/oversized words.
bool widen(Utf8View word, wchar_t (&output)[kWordChars]) noexcept {
    if (!word.data || !word.bytes || word.bytes > kSpellCheckerWordBytes)
        return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, word.data, static_cast<int>(word.bytes),
                                          output, static_cast<int>(kWordChars - 1));
    if (count <= 0)
        return false;
    output[count] = 0;
    return true;
}
bool factory_supports(void* context, const wchar_t* tag) noexcept {
    BOOL supported = FALSE;
    return SUCCEEDED(static_cast<ISpellCheckerFactory*>(context)->IsSupported(tag, &supported)) && supported;
}
} // namespace

bool spell_checker_choose_language(const wchar_t* locale, bool (*supported)(void*, const wchar_t*) noexcept,
                                   void* context, Slice<wchar_t> output) noexcept {
    if (!supported || !output.data || output.count < 2)
        return false;
    wchar_t candidate[kSpellCheckerLanguageChars]{};
    const u32 length = locale ? wide_length(locale, kSpellCheckerLanguageChars - 1) : 0;
    if (length) {
        copy_bytes(candidate, locale, length * sizeof(wchar_t));
        if (supported(context, candidate))
            return copy_tag(output, candidate, length);
        u32 base = 0;
        while (base < length && candidate[base] != L'-' && candidate[base] != L'_')
            ++base;
        if (base && base < length) {
            candidate[base] = 0;
            if (supported(context, candidate))
                return copy_tag(output, candidate, base);
        }
    }
    constexpr wchar_t fallback[] = L"en-US";
    return supported(context, fallback) && copy_tag(output, fallback, 5);
}

bool spell_checker_open(SpellChecker* spell) noexcept {
    if (!spell)
        return false;
    if (spell->checker)
        return true;
    if (spell->attempted)
        return false;
    spell->attempted = true;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    spell->com_initialized = SUCCEEDED(apartment);
    void* factory = nullptr;
    if (FAILED(CoCreateInstance(kSpellCheckerFactoryClass, nullptr, CLSCTX_INPROC_SERVER,
                                kSpellCheckerFactoryInterface, &factory)) ||
        !factory)
        return false;
    spell->factory = static_cast<ISpellCheckerFactory*>(factory);
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (!GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH))
        locale[0] = 0;
    ISpellChecker* checker = nullptr;
    if (!spell_checker_choose_language(locale, factory_supports, spell->factory,
                                       {spell->language, kSpellCheckerLanguageChars}) ||
        FAILED(spell->factory->CreateSpellChecker(spell->language, &checker)) || !checker)
        return false;
    spell->checker = checker;
    return true;
}

SpellCheckerVerdict spell_checker_check(SpellChecker* spell, Utf8View word) noexcept {
    wchar_t text[kWordChars];
    if (!widen(word, text) || !spell_checker_open(spell))
        return SpellCheckerVerdict::unavailable;
    IEnumSpellingError* errors = nullptr;
    if (FAILED(spell->checker->Check(text, &errors)) || !errors)
        return SpellCheckerVerdict::unavailable;
    ISpellingError* error = nullptr;
    const HRESULT next = errors->Next(&error);
    bool misspelled = false;
    if (next == S_OK && error) {
        CORRECTIVE_ACTION action = CORRECTIVE_ACTION_NONE;
        misspelled = SUCCEEDED(error->get_CorrectiveAction(&action)) && action != CORRECTIVE_ACTION_NONE;
        error->Release();
    }
    errors->Release();
    return misspelled ? SpellCheckerVerdict::misspelled : SpellCheckerVerdict::correct;
}

u32 spell_checker_suggest(SpellChecker* spell, Utf8View word, Slice<char> storage, Slice<Utf8View> output) noexcept {
    wchar_t text[kWordChars];
    if (!storage.data || !output.data || !output.count || !widen(word, text) || !spell_checker_open(spell))
        return 0;
    IEnumString* list = nullptr;
    if (FAILED(spell->checker->Suggest(text, &list)) || !list)
        return 0;
    u32 count = 0, used = 0;
    while (count < output.count) {
        LPOLESTR value = nullptr;
        ULONG fetched = 0;
        if (list->Next(1, &value, &fetched) != S_OK || !fetched || !value)
            break;
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, storage.data + used,
                                              static_cast<int>(storage.count - used), nullptr, nullptr);
        CoTaskMemFree(value);
        if (bytes <= 1)
            continue; // empty, invalid or too long for the remaining storage
        output[count++] = {storage.data + used, static_cast<u32>(bytes - 1)};
        used += static_cast<u32>(bytes - 1);
    }
    list->Release();
    return count;
}

bool spell_checker_add(SpellChecker* spell, Utf8View word) noexcept {
    wchar_t text[kWordChars];
    return widen(word, text) && spell_checker_open(spell) && SUCCEEDED(spell->checker->Add(text));
}

bool spell_checker_ignore(SpellChecker* spell, Utf8View word) noexcept {
    wchar_t text[kWordChars];
    return widen(word, text) && spell_checker_open(spell) && SUCCEEDED(spell->checker->Ignore(text));
}

void spell_checker_release(SpellChecker* spell) noexcept {
    if (!spell)
        return;
    com_release(spell->checker);
    com_release(spell->factory);
    if (spell->com_initialized)
        CoUninitialize();
    *spell = {};
}

} // namespace maicord
