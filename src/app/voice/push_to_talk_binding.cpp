#include "maicord/app/voice/push_to_talk_binding.hpp"

#include "maicord/platform/window/wayland.hpp"

namespace maicord {
namespace {

u8 g_bound_key; // the Key handed to the portal, zero for none

} // namespace

void voice_push_to_talk_binding_update(bool in_call, VoiceInputMode mode, u8 key) noexcept {
    const u8 wanted = in_call && mode == VoiceInputMode::push_to_talk ? key : u8{0};
    if (wanted == g_bound_key)
        return;
    // Whether the compositor takes it or not, the same request is not made again until it changes: a failed bind leaves
    // the key a focused-window key.
    g_bound_key = wanted;
    if (wanted)
        window_wayland_global_shortcut(wanted, 0, "Push to talk");
    else
        window_wayland_global_shortcut_clear();
}

} // namespace maicord
