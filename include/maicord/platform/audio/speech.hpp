#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
// Text-to-speech for "Speak message". Opened lazily on the UI thread on first
// use; speaking is asynchronous and a new request interrupts the previous one.
// Windows speaks through SAPI 5. Linux has no speech engine yet, so speech_speak
// returns false there (speech-dispatcher is the later backend); callers already
// treat false as "speech unavailable".
struct Speech {
    void* voice; // The backend's speech object, null while closed or unavailable.
    bool attempted, initialized;
};
bool speech_speak(Speech*, Utf8View text) noexcept;
void speech_release(Speech*) noexcept;
} // namespace maicord
