#pragma once

#include "maicord/ui/geometry.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {

// The composer's voice message state. While a recording exists the editor
// row becomes the recording bar: a pulsing record dot, the timer, the live
// (or, for review, the recorded) waveform and a hint or status line. The
// host owns the recording; this module is geometry, paint and text only.
enum class UiComposerVoiceMode : u8 {
    none,      // Ordinary composer.
    available, // Empty draft: the microphone replaces Send.
    recording,
    review,    // Stopped: preview, delete or send.
    sending,   // Uploading, then queued.
    failed,    // Upload failed: retry or delete.
};
inline constexpr u32 kUiComposerVoiceLevels = 64;
inline constexpr u32 kUiComposerVoiceWaveform = 256;
struct UiComposerVoiceView {
    UiComposerVoiceMode mode;
    bool playing;
    bool held;        // Hold-to-record: releasing over the button sends.
    bool hold_inside;
    bool pulse;       // Record dot phase (blinks at 2 Hz while recording).
    bool error;       // status is an error.
    u32 elapsed_ms;   // Recorded duration.
    u32 position_ms;  // Preview position (review).
    float progress;   // Upload progress 0..1 (sending).
    u32 level_count;  // Newest live levels in levels[0..level_count), oldest first.
    u8 levels[kUiComposerVoiceLevels];
    u8 waveform[kUiComposerVoiceWaveform]; // Review: the recording's waveform.
    Utf8View status;  // Hint, notice or error; borrowed until the next update.
};
constexpr bool ui_composer_voice_bar_visible(UiComposerVoiceMode mode) noexcept {
    return mode != UiComposerVoiceMode::none && mode != UiComposerVoiceMode::available;
}
struct UiComposerVoiceBarGeometry {
    UiRect dot, time, waveform, status;
};
UiComposerVoiceBarGeometry ui_composer_voice_bar_geometry(UiRect bar, const UiComposerVoiceView&) noexcept;
// Bars drawn for a waveform span; the newest live levels fill from the right.
u32 ui_composer_voice_bar_count(float waveform_width) noexcept;
bool ui_composer_voice_bar_paint(const UiComposerVoiceView&, UiRect bar, const Theme&, PaintStream*) noexcept;
// Timer ("0:07", or position/duration while previewing) and the status line.
bool ui_composer_voice_bar_text(const UiComposerVoiceView&, UiRect bar, UiRect clip, UiWidgetReference owner,
                                const Theme&, UiTextOutput*) noexcept;

} // namespace maicord
