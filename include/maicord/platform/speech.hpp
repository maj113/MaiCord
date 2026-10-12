#pragma once
#include "maicord/core/slice.hpp"

struct ISpVoice;

namespace maicord {
// Windows text-to-speech (SAPI 5) for "Speak message". Opened lazily on the
// UI thread on first use; speaking is asynchronous and a new request
// interrupts the previous one.
struct Speech {
    ISpVoice* voice;
    bool attempted, com_initialized;
};
bool speech_available(Speech*) noexcept;
bool speech_speak(Speech*, Utf8View text) noexcept;
void speech_release(Speech*) noexcept;
} // namespace maicord
