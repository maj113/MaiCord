// Text-to-speech on Linux: "Speak message" says the text through speech-dispatcher (libspeechd, loaded with dlopen like the
// other system libraries; SAPI, src/platform/audio/speech.cpp, stays Windows-only). It keeps the Windows semantics:
// opened lazily on the first use, asynchronous, a new request interrupts the previous one, release stops it, and a system
// without a speech engine answers false (the caller logs "speech_unavailable"). The voice, rate and language are the
// user's: speech-dispatcher's own configuration (and a per-client section for "maicord"), as SAPI takes the voice and the
// rate of the Windows speech settings.
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/core/test_hook.hpp"
#include "speechd.hpp"

namespace maicord {
namespace speechd {
namespace {

// 0: not tried, 1: loaded, 2: not available. Speech is used from the UI thread only.
u32 g_state;
SharedLibrary g_library;
Api g_api;

} // namespace

const Api* library() noexcept {
    if (g_state == 1)
        return &g_api;
    if (g_state == 2)
        return nullptr;
    g_state = 2;
    // A test can name another soname (the no-library path); users and operators have MAICORD_LIBRARY_PATH.
    const char* soname = test_hook("MAICORD_SPEECHD_LIBRARY");
    if (!soname || !*soname)
        soname = "libspeechd.so.2";
    if (!shared_library_open(&g_library, soname)) {
        log_write(LogLevel::warning, "speech", "libspeechd.so.2 is not installed: text-to-speech is unavailable");
        return nullptr;
    }
    const char* missing = nullptr;
    Api api{};
#define MAICORD_SPEECHD_RESOLVE(result, name, arguments)                                                               \
    if (!missing) {                                                                                                    \
        void* address = shared_library_symbol(g_library, #name);                                                       \
        if (address)                                                                                                   \
            api.name = reinterpret_cast<decltype(api.name)>(address);                                                  \
        else                                                                                                           \
            missing = #name;                                                                                           \
    }
    MAICORD_SPEECHD_FUNCTIONS(MAICORD_SPEECHD_RESOLVE)
#undef MAICORD_SPEECHD_RESOLVE
    if (missing) {
        log_write(LogLevel::warning, "speech", "libspeechd lacks a function: text-to-speech is unavailable");
        log_write(LogLevel::warning, "speech", missing);
        shared_library_close(&g_library);
        return nullptr;
    }
    g_api = api;
    g_state = 1;
    return &g_api;
}

bool speak(Speech* speech, const Api* (*load)() noexcept, Utf8View text) noexcept {
    if (!speech || !text.data || !text.bytes)
        return false;
    if (!speech->voice && speech->attempted)
        return false;
    // The text as a C string: its valid UTF-8 prefix within the maximum, up to the first NUL.
    const Utf8Validation validated = utf8_validate(text);
    char buffer[kTextMaximumBytes + 1];
    const u32 length = utf8_copy_truncated(buffer, kTextMaximumBytes,
                                           Utf8View{text.data, validated.valid ? text.bytes : validated.error_offset});
    buffer[length] = 0;
    if (buffer[0] == 0)
        return false;
    const Api* api = load();
    if (!api)
        return false;
    if (!speech->voice) {
        speech->attempted = true;
        char* error = nullptr;
        speech->voice = api->spd_open2("maicord", "speech", nullptr, kModeSingle, nullptr, 1, &error);
        if (error) {
            if (!speech->voice)
                log_write(LogLevel::warning, "speech", error);
            free(error);
        }
        if (!speech->voice)
            return false;
    }
    auto* connection = static_cast<Connection*>(speech->voice);
    api->spd_cancel(connection);
    if (api->spd_say(connection, kPriorityText, buffer) >= 0)
        return true;
    // The daemon went away: the connection is useless, and the next call opens another.
    api->spd_close(connection);
    speech->voice = nullptr;
    speech->attempted = false;
    return false;
}

void release(Speech* speech, const Api* api) noexcept {
    if (!speech)
        return;
    if (speech->voice && api) {
        auto* connection = static_cast<Connection*>(speech->voice);
        api->spd_cancel(connection);
        api->spd_close(connection);
    }
    reset_value(speech);
}

} // namespace speechd

bool speech_speak(Speech* speech, Utf8View text) noexcept {
    return speechd::speak(speech, speechd::library, text);
}

void speech_release(Speech* speech) noexcept {
    speechd::release(speech, speech && speech->voice ? speechd::library() : nullptr);
}

} // namespace maicord
