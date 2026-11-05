#pragma once

#include "maicord/app/voice/audio_settings.hpp"

namespace maicord {

// Push to talk beyond the focused window. Windows samples the key itself (GetAsyncKeyState behind global_key_down). On
// Linux a Wayland client sees keys only while one of its windows has the focus, so the key goes to the compositor as a
// GlobalShortcuts portal shortcut ("Push to talk"; window_wayland_global_shortcut) for as long as a call is open in
// push-to-talk mode, and global_key_down(key) reads true while the portal reports it held. The binding follows what is
// asked each turn: the call, the mode and the key; a compositor without the portal leaves the key focused-window only.
#if defined(_WIN32)
inline void voice_push_to_talk_binding_update(bool, VoiceInputMode, u8) noexcept {}
#else
void voice_push_to_talk_binding_update(bool in_call, VoiceInputMode mode, u8 key) noexcept;
#endif

} // namespace maicord
