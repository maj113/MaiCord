// Text-to-speech on Linux: no speech engine yet, so "Speak message" reports that speech is unavailable and the
// caller logs it. The backend to add is speech-dispatcher (libspeechd, loaded with dlopen like the other system
// libraries); SAPI (src/platform/audio/speech.cpp) stays Windows-only.
#include "maicord/core/memory.hpp"
#include "maicord/platform/audio/speech.hpp"

namespace maicord {

bool speech_speak(Speech* speech, Utf8View) noexcept {
    if (speech)
        speech->attempted = true;
    return false;
}

void speech_release(Speech* speech) noexcept {
    if (speech)
        reset_value(speech);
}

} // namespace maicord
