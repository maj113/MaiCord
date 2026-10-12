#pragma once

#include "maicord/ui/theme_styles.hpp"
#include "maicord/ui/audio_controls.hpp"
#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/forum_post_scene.hpp"
#include "maicord/ui/theme_editor.hpp"

namespace maicord {
// Apply-time bank projection. Null/dormant banks reproduce the original native
// component defaults. No ownership, registry lookup, hashing or allocation.
const UiThemeMetrics& ui_theme_metrics(const UiResolvedThemeStyles*) noexcept;
UiAudioControlsStyle ui_theme_audio_style(const UiResolvedThemeStyles*) noexcept;
UiPollStyle ui_theme_poll_style(const UiResolvedThemeStyles*) noexcept;
UiPollVotersStyle ui_theme_poll_voters_style(const UiResolvedThemeStyles*) noexcept;
UiSettingsStyle ui_theme_settings_style(const UiResolvedThemeStyles*) noexcept;
ForumPostStyle ui_theme_forum_style(const UiResolvedThemeStyles*) noexcept;
UiThemeEditorStyle ui_theme_editor_style(const UiResolvedThemeStyles*) noexcept;
UiAudioControlsStyle ui_theme_audio_style(const Theme&) noexcept;
UiPollStyle ui_theme_poll_style(const Theme&) noexcept;
UiPollVotersStyle ui_theme_poll_voters_style(const Theme&) noexcept;
UiSettingsStyle ui_theme_settings_style(const Theme&) noexcept;
ForumPostStyle ui_theme_forum_style(const Theme&) noexcept;
UiThemeEditorStyle ui_theme_editor_style(const Theme&) noexcept;
} // namespace maicord
