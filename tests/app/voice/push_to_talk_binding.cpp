#include "maicord/app/voice/push_to_talk_binding.hpp"
#include "maicord/platform/window/null.hpp"
#include "../../support/test_output.hpp"

// Push to talk beyond the focused window on Linux (voice_push_to_talk_binding_update): the key is handed to the
// GlobalShortcuts portal for as long as a call is open in push-to-talk mode, follows a changed key, and is released when
// the call ends or the mode changes. The headless window records the binds (null_window_probe).

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_push_to_talk_binding_entry) {
    using namespace maicord;
    null_window_reset();
    constexpr u8 kKeyA = 'A', kKeyB = 'B';
    voice_push_to_talk_binding_update(false, VoiceInputMode::push_to_talk, kKeyA);
    check(null_window_probe().shortcut_binds == 0 && null_window_probe().shortcut_clears == 0,
          "no call: nothing is bound or cleared");
    voice_push_to_talk_binding_update(true, VoiceInputMode::voice_activity, kKeyA);
    voice_push_to_talk_binding_update(true, VoiceInputMode::push_to_talk, 0);
    check(null_window_probe().shortcut_binds == 0, "voice activity, or no key: nothing is bound");

    voice_push_to_talk_binding_update(true, VoiceInputMode::push_to_talk, kKeyA);
    check(null_window_probe().shortcut_binds == 1 && null_window_probe().shortcut_key == kKeyA,
          "a call in push-to-talk mode with a key binds it");
    voice_push_to_talk_binding_update(true, VoiceInputMode::push_to_talk, kKeyA);
    voice_push_to_talk_binding_update(true, VoiceInputMode::push_to_talk, kKeyA);
    check(null_window_probe().shortcut_binds == 1, "the same request is not made again");

    voice_push_to_talk_binding_update(true, VoiceInputMode::push_to_talk, kKeyB);
    check(null_window_probe().shortcut_binds == 2 && null_window_probe().shortcut_key == kKeyB,
          "a changed key is bound in its place");

    voice_push_to_talk_binding_update(true, VoiceInputMode::voice_activity, kKeyB);
    check(null_window_probe().shortcut_clears == 1 && null_window_probe().shortcut_key == 0,
          "leaving push-to-talk mode releases the binding");
    voice_push_to_talk_binding_update(false, VoiceInputMode::voice_activity, kKeyB);
    check(null_window_probe().shortcut_clears == 1, "nothing bound: nothing to release");

    voice_push_to_talk_binding_update(true, VoiceInputMode::push_to_talk, kKeyA);
    voice_push_to_talk_binding_update(false, VoiceInputMode::push_to_talk, kKeyA);
    check(null_window_probe().shortcut_binds == 3 && null_window_probe().shortcut_clears == 2 &&
              null_window_probe().shortcut_key == 0,
          "the end of the call releases it, and the next call binds it again");
    test_exit(g_failures, "push_to_talk_binding");
}
