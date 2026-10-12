#pragma once

#include "maicord/ui/audio_controls.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/model/message_rich_store.hpp"

namespace maicord {
// Optional caller waveform scratch is borrowed only through the synchronous
// paint. Empty scratch projects labels/geometry without decoding waveform data.
bool ui_audio_attachment_project(const MessageRichStore&, const MessageRichAttachment&, u64 generation,
                                 u64 revision, bool voice, UiRect bounds, const Theme&,
                                 Slice<u8> waveform_scratch, UiAudioControlsProjection*) noexcept;
bool ui_audio_controls_paint(const UiAudioControlsProjection&, const UiAudioControlsStyle&, UiRect clip,
                             const Theme&, UiWidgetReference owner, PaintStream*,
                             UiAudioControl focus = UiAudioControl::none,
                             UiAudioControl hover = UiAudioControl::none,
                             bool keyboard_focus = false) noexcept;
// Copies generated labels into the caller's text stream. A temporary projection
// never leaves pointers into its own time buffer in a retained command.
bool ui_audio_controls_text_output(const UiAudioControlsProjection&, UiRect clip, UiWidgetReference owner,
                                   UiTextOutput*) noexcept;

struct UiAudioScene;
bool ui_audio_scene_paint(const UiAudioScene&, const Theme&, PaintStream*) noexcept;
bool ui_audio_scene_text_output(const UiAudioScene&, UiTextOutput*) noexcept;
u64 ui_audio_scene_paint_revision(const UiAudioScene&) noexcept;
u64 ui_audio_scene_text_revision(const UiAudioScene&) noexcept;
} // namespace maicord
