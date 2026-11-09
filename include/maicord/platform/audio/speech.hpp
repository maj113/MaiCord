#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Text-to-speech for "Speak message". Opened lazily on the UI thread on first
// use; speaking is asynchronous and a new request interrupts the previous one.
// Windows speaks through SAPI 5, Linux through speech-dispatcher (libspeechd,
// opened with dlopen; src/platform/audio/speechd). Without a speech engine
// speech_speak returns false; callers treat that as "speech unavailable".
// `rate` is -10..10 on SAPI's scale (each step about 1.1x, 10 three times as
// fast), relative to the rate the system's speech settings give: 0 keeps it.
struct Speech {
    void* voice; // The backend's speech object, null while closed or unavailable.
    bool attempted, initialized;
    // The rate last applied, and (SAPI) the system's own rate read when the voice opened.
    i16 rate, base;
};
bool speech_speak(Speech*, Utf8View text, i32 rate) noexcept;
void speech_release(Speech*) noexcept;
} // namespace maicord
