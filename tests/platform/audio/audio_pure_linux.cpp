// The audio tests of the Linux build that need no sound server: the transport-free media ring and clock conversions
// the Windows suite shares (tests/platform/audio/media_audio.cpp), and the arithmetic of the PulseAudio transport.
#include "../../support/test_output.hpp"

namespace maicord {

using Check = void (*)(bool, const char*) noexcept;

void test_media_audio(Check check) noexcept;
void test_pulse_timing(Check check) noexcept;

namespace {

u32 g_failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_audio_pure_entry) {
    maicord::test_media_audio(maicord::check);
    maicord::test_pulse_timing(maicord::check);
    maicord::test_exit(maicord::g_failures, "maicord_audio_pure");
}
