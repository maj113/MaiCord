#include "maicord/platform/speech.hpp"

#include <objbase.h>
#include <sapi.h>

namespace maicord {
namespace {
constexpr CLSID kSpVoiceClass = {0x96749377, 0x3391, 0x11D2, {0x9E, 0xE3, 0x00, 0xC0, 0x4F, 0x79, 0x73, 0x96}};
constexpr IID kSpVoiceInterface = {0x6C44DF74, 0x72B9, 0x4992, {0xA1, 0xEC, 0xEF, 0x99, 0x6E, 0x04, 0x22, 0xD4}};
constexpr u32 kSpeechMaximumChars = 4096;
} // namespace

bool speech_available(Speech* speech) noexcept {
    if (!speech)
        return false;
    if (speech->voice)
        return true;
    if (speech->attempted)
        return false;
    speech->attempted = true;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    speech->com_initialized = SUCCEEDED(apartment);
    void* voice = nullptr;
    if (FAILED(CoCreateInstance(kSpVoiceClass, nullptr, CLSCTX_INPROC_SERVER, kSpVoiceInterface, &voice)) || !voice)
        return false;
    speech->voice = static_cast<ISpVoice*>(voice);
    return true;
}

bool speech_speak(Speech* speech, Utf8View text) noexcept {
    if (!text.data || !text.bytes || !speech_available(speech))
        return false;
    wchar_t* wide = static_cast<wchar_t*>(
        VirtualAlloc(nullptr, (kSpeechMaximumChars + 1) * sizeof(wchar_t), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!wide)
        return false;
    // Over-long messages are cut at a code point boundary the converter accepts.
    u32 bytes = text.bytes;
    int count = 0;
    while (bytes && (count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data, static_cast<int>(bytes),
                                                 wide, static_cast<int>(kSpeechMaximumChars))) <= 0) {
        bytes = bytes > kSpeechMaximumChars ? kSpeechMaximumChars : bytes - 1;
        while (bytes && (static_cast<u8>(text.data[bytes]) & 0xC0) == 0x80)
            --bytes;
    }
    bool spoken = false;
    if (count > 0) {
        wide[count] = 0;
        spoken = SUCCEEDED(speech->voice->Speak(wide, SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr));
    }
    VirtualFree(wide, 0, MEM_RELEASE);
    return spoken;
}

void speech_release(Speech* speech) noexcept {
    if (!speech)
        return;
    if (speech->voice) {
        speech->voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
        speech->voice->Release();
    }
    if (speech->com_initialized)
        CoUninitialize();
    *speech = {};
}
} // namespace maicord
