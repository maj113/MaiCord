#include "maicord/ui/audio_controls.hpp"
#include "maicord/ui/forum_post_scene.hpp"
#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/theme_editor.hpp"
#include "maicord/ui/theme_styles.hpp"

namespace maicord {
const UiThemeMetrics& ui_theme_metrics(const UiResolvedThemeStyles* bank) noexcept {
    return bank && bank->ready ? bank->metrics : kUiThemeMetricsDefault;
}
UiAudioControlsStyle ui_theme_audio_style(const UiResolvedThemeStyles* bank) noexcept {
    const auto& m = ui_theme_metrics(bank);
    UiAudioControlsStyle style{};
    style.padding = m.audio_padding;
    style.gap = m.audio_gap;
    const float text_scale = bank && bank->ready ? maximum(1.f, bank->typography.body_size / 16.f) : 1.f;
    style.target = maximum(m.audio_target, 32.f * text_scale);
    style.minimum_width =
        maximum(m.audio_minimum_width, 2 * style.padding + 3 * style.target + 2 * style.gap);
    style.maximum_width = maximum(m.audio_maximum_width, style.minimum_width);
    style.title_height = maximum(m.audio_title_height, 20.f * text_scale);
    style.volume_width = maximum(m.audio_volume_width, style.target);
    style.time_width = maximum(m.audio_time_width, style.volume_width);
    style.corner_radius = m.audio_radius;
    return style;
}
UiPollStyle ui_theme_poll_style(const UiResolvedThemeStyles* bank) noexcept {
    const auto& m = ui_theme_metrics(bank);
    UiPollStyle style{};
    style.padding = m.poll_padding;
    style.gap = m.poll_gap;
    style.answer_padding = m.poll_answer_padding;
    style.answer_gap = m.poll_answer_gap;
    style.minimum_width = m.poll_minimum_width;
    style.maximum_width = m.poll_maximum_width;
    style.target = m.poll_target;
    style.marker = m.poll_marker;
    style.emoji_extent = m.poll_emoji;
    style.radius = m.poll_radius;
    return style;
}
UiPollVotersStyle ui_theme_poll_voters_style(const UiResolvedThemeStyles* bank) noexcept {
    const auto& m = ui_theme_metrics(bank);
    UiPollVotersStyle style{};
    style.padding = m.poll_padding + 4;
    style.gap = m.poll_gap;
    style.target = m.poll_target;
    style.row_height = maximum(m.poll_target, 44 + (m.control_height - 32));
    style.avatar_extent = minimum(style.row_height, maximum(16.f, m.message_avatar - 8));
    style.radius = m.control_radius;
    style.header_height = maximum(48.f, style.target + 16);
    return style;
}
UiSettingsStyle ui_theme_settings_style(const UiResolvedThemeStyles* bank) noexcept {
    const auto& m = ui_theme_metrics(bank);
    return {m.form_width,
            m.form_field_padding,
            m.form_group_gap,
            maximum(m.control_height, m.form_single_height),
            maximum(96.f, maximum(m.form_multiline_height, m.form_single_height)),
            m.edit_radius,
            m.control_radius,
            maximum(64.f, m.form_footer_height)};
}
ForumPostStyle ui_theme_forum_style(const UiResolvedThemeStyles* bank) noexcept {
    const auto& m = ui_theme_metrics(bank);
    ForumPostStyle style{};
    style.maximum_width = maximum(320.f, m.form_width - 120);
    style.inset = m.menu_padding + 10;
    style.padding = m.form_field_padding + 8;
    style.gap = maximum(4.f, m.form_group_gap - 14);
    style.title_height = maximum(m.control_height, m.form_single_height + 4);
    style.body_height = m.form_multiline_height + 16;
    style.corner_radius = m.control_radius;
    return style;
}
UiThemeEditorStyle ui_theme_editor_style(const UiResolvedThemeStyles* bank) noexcept {
    const auto& m = ui_theme_metrics(bank);
    return {maximum(320.f, m.form_width - 260),
            m.form_multiline_height * 4,
            m.form_field_padding + 8,
            m.form_group_gap * .5f,
            m.edit_radius * 2,
            m.control_radius + 2};
}
UiAudioControlsStyle ui_theme_audio_style(const Theme& theme) noexcept {
    return ui_theme_audio_style(theme.styles);
}
UiPollStyle ui_theme_poll_style(const Theme& theme) noexcept {
    return ui_theme_poll_style(theme.styles);
}
UiPollVotersStyle ui_theme_poll_voters_style(const Theme& theme) noexcept {
    return ui_theme_poll_voters_style(theme.styles);
}
UiSettingsStyle ui_theme_settings_style(const Theme& theme) noexcept {
    return ui_theme_settings_style(theme.styles);
}
ForumPostStyle ui_theme_forum_style(const Theme& theme) noexcept {
    return ui_theme_forum_style(theme.styles);
}
UiThemeEditorStyle ui_theme_editor_style(const Theme& theme) noexcept {
    return ui_theme_editor_style(theme.styles);
}
} // namespace maicord
