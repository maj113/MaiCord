#pragma once
#include "maicord/core/utf.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/composer_voice_bar.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/message_composer_scene.hpp"

namespace maicord::app_detail {
// Composer voice messages: hold the microphone to record and release to send
// (releasing elsewhere cancels), tap it to record hands-free, stop to review
// and preview, delete with the trash control or Escape. The recording runs in
// the account module; sending uploads it through the attachment pipeline and
// queues an IS_VOICE_MESSAGE send.

// The scene's voice view for this frame (null: the ordinary composer).
// channel_actions are the active channel row's actions.
const UiComposerVoiceView* application_voice_message_view(u32 channel_actions, bool sending_restricted) noexcept;
// Button pointer events on the composer's widgets; over_send is whether the
// pointer is over Send (the microphone). activated is the release activation.
void application_voice_message_pointer(UiButtonPointerEvent event, bool over_send, bool activated) noexcept;
// Composer control activation while a voice mode applies; false to fall through.
bool application_voice_message_activate(MessageComposerControl control) noexcept;
// A click on the recording bar (the editor row): toggles the preview.
bool application_voice_message_bar_click() noexcept;
// Escape cancels and Enter sends while a recording exists; typing is held.
bool application_voice_message_input(UiInputKind kind, u32 code, bool repeated) noexcept;
void application_voice_message_channel_changed(u64 channel_id) noexcept;
void application_voice_message_deactivated() noexcept;
bool application_voice_message_pump() noexcept;
u32 application_voice_message_wait_timeout(u64 now_ms) noexcept;
// A notice for the composer status row after the flow ends (microphone
// errors); empty otherwise.
Utf8View application_voice_message_notice(bool* error) noexcept;
// Account teardown: drop the recording and any upload.
void application_voice_message_release() noexcept;
} // namespace maicord::app_detail
