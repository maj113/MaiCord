#pragma once

namespace maicord {
struct ForumPostStyle;
struct Theme;
struct UiAudioControlsStyle;
struct UiPollStyle;
struct UiPollVotersStyle;
struct UiResolvedThemeStyles;
struct UiSettingsStyle;
struct UiThemeEditorStyle;
struct UiThemeMetrics;

// Apply-time bank projection. Null/dormant banks yield the built-in component
// defaults. No ownership, registry lookup, hashing or allocation.
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
