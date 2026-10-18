#include "maicord/ui/theme_styles.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/ui/audio_controls.hpp"
#include "maicord/ui/poll_controls.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/scene.hpp"

namespace maicord {
void test_theme_styles(void (*check)(bool, const char*) noexcept) noexcept {
    const UiThemeTypography typography{};
    UiThemeMetrics metrics{};
    check(ui_theme_typography_valid(typography) && ui_theme_metrics_valid(metrics) &&
              ui_theme_palette_valid(kDefaultTheme) && ui_theme_palette_valid(kLightTheme) &&
              ui_theme_palette_valid(kAmoledTheme),
          "Theme schema current shipped palettes and typography/metrics defaults are valid");
    check(metrics.shell_rail_width == kGuildChannelShellLayoutMetrics.guild_rail_width &&
              metrics.shell_sidebar_width == kGuildChannelShellLayoutMetrics.navigation_width &&
              metrics.shell_member_width == kGuildChannelShellLayoutMetrics.member_sidebar_width &&
              metrics.shell_header_height == kGuildChannelShellLayoutMetrics.title_bar_height &&
              metrics.control_radius == kUiControlCornerRadius &&
              metrics.edit_radius == kUiEditCornerRadius && metrics.menu_padding == kUiMenuPadding &&
              metrics.menu_row_height == kUiMenuRowHeight && metrics.menu_radius == kUiMenuCornerRadius &&
              metrics.composer_padding == kUiComposerPadding,
          "Theme metric defaults preserve actual shell and shared control/menu/composer primitives");
    check(metrics.form_width == kUiDefaultSettingsStyle.form_width &&
              metrics.form_field_padding == kUiDefaultSettingsStyle.field_padding &&
              metrics.form_single_height == kUiDefaultSettingsStyle.single_height &&
              metrics.form_multiline_height == kUiDefaultSettingsStyle.multiline_height &&
              metrics.form_footer_height == kUiDefaultSettingsStyle.footer_height &&
              metrics.audio_padding == kDefaultUiAudioControlsStyle.padding &&
              metrics.audio_target == kDefaultUiAudioControlsStyle.target &&
              metrics.audio_minimum_width == kDefaultUiAudioControlsStyle.minimum_width &&
              metrics.audio_maximum_width == kDefaultUiAudioControlsStyle.maximum_width &&
              metrics.poll_padding == kDefaultUiPollStyle.padding &&
              metrics.poll_marker == kDefaultUiPollStyle.marker &&
              metrics.poll_emoji == kDefaultUiPollStyle.emoji_extent,
          "Theme component metric defaults match existing form/audio/poll styles");
    const u32 flags[]{text_style_none,
                      text_style_picker_emoji,
                      text_style_jumbo_emoji,
                      text_style_heading_1,
                      text_style_heading_2,
                      text_style_heading_3,
                      text_style_subtext,
                      text_style_monospace,
                      text_style_heading_1 | text_style_monospace,
                      text_style_subtext | text_style_monospace};
    bool sizes_equal = true;
    const float ems[]{12.f, 16.f, 24.f};
    for (float em : ems)
        for (u32 flag : flags)
            sizes_equal =
                sizes_equal && ui_theme_text_size(typography, em, flag) == text_style_font_size(em, flag);
    check(sizes_equal,
          "Theme default semantic font ratios exactly preserve native heading/subtext/mono/emoji priority");
    UiThemeTypography selected = typography;
    bool fonts_valid = true;
    for (u32 face = 0; face < static_cast<u32>(UiTextFace::count); ++face) {
        selected.body_face = static_cast<UiTextFace>(face);
        selected.code_face = static_cast<UiTextFace>(face);
        fonts_valid = fonts_valid && ui_theme_typography_valid(selected);
    }
    selected.code_face = UiTextFace::count;
    check(fonts_valid && !ui_theme_typography_valid(selected),
          "Theme font schema accepts all existing licensed faces and rejects outside catalog");
    selected = typography;
    selected.body_size = 11.99f;
    check(!ui_theme_typography_valid(selected), "Theme body text below12DIP is rejected");
    selected.body_size = 24.01f;
    check(!ui_theme_typography_valid(selected), "Theme body text above24DIP is rejected");
    selected = typography;
    selected.heading_2_ratio = 1.6f;
    check(!ui_theme_typography_valid(selected), "Theme heading hierarchy rejects inverted ratios");
    u32 nonfinite_bits = 0x7fc00000u;
    float nonfinite{};
    copy_bytes(&nonfinite, &nonfinite_bits, sizeof(nonfinite));
    selected = typography;
    selected.mono_ratio = nonfinite;
    check(!ui_theme_typography_valid(selected), "Theme NaN typography fails closed without math runtime");

    const auto descriptors = ui_theme_metric_descriptors();
    bool vocabulary = descriptors.count == static_cast<u32>(UiThemeMetric::count);
    for (u32 i = 0; i < descriptors.count; ++i) {
        const auto& descriptor = descriptors[i];
        float value{};
        vocabulary = vocabulary && static_cast<u32>(descriptor.id) == i && descriptor.name.bytes &&
                     ui_theme_metric_get(metrics, descriptor.id, &value) && value >= descriptor.minimum &&
                     value <= descriptor.maximum;
        for (u32 j = 0; j < i; ++j)
            vocabulary = vocabulary && !utf8_equal(descriptor.name, descriptors[j].name);
        const UiThemeMetrics before = metrics;
        vocabulary = vocabulary && !ui_theme_metric_set(&metrics, descriptor.id, descriptor.minimum - 1) &&
                     !ui_theme_metric_set(&metrics, descriptor.id, descriptor.maximum + 1) &&
                     !ui_theme_metric_set(&metrics, descriptor.id, nonfinite) &&
                     ui_theme_metrics_equal(metrics, before);
    }
    check(vocabulary, "Theme fixed metric vocabulary has exact stable IDs/unique names/default ranges and "
                      "transactional invalid scalar setters");
    float untouched = 7;
    check(!ui_theme_metric_get(metrics, UiThemeMetric::count, &untouched) && untouched == 7 &&
              !ui_theme_metric_set(&metrics, UiThemeMetric::count, 7),
          "Theme unknown metric cannot alias a valid numeric token");
    UiThemeMetrics invalid = metrics;
    invalid.menu_compact_height = 56;
    invalid.menu_row_height = 32;
    check(!ui_theme_metrics_valid(invalid),
          "Theme menu row hierarchy rejects compact rows larger than regular rows");
    invalid = metrics;
    invalid.form_field_padding = 24;
    invalid.form_single_height = 40;
    check(!ui_theme_metrics_valid(invalid),
          "Theme form metrics retain a positive readable text area after padding");

    UiResolvedThemeStyles styles{};
    const auto first = ui_theme_styles_apply(&styles, typography, metrics);
    check(first.ok() &&
              first.changes ==
                  (ui_theme_style_typography | ui_theme_style_geometry | ui_theme_style_paint) &&
              styles.ready && styles.revision == 1 && styles.typography_revision == 1 &&
              styles.geometry_revision == 1 && styles.paint_revision == 1,
          "Theme first apply publishes one resolved bank with every relevant domain");
    const UiResolvedThemeStyles initial = styles;
    bool retained = true;
    for (u32 frame = 0; frame < 64; ++frame) {
        const auto same = ui_theme_styles_apply(&styles, typography, metrics);
        retained = retained && same.ok() && !same.changed() && styles.revision == initial.revision &&
                   styles.typography_revision == initial.typography_revision &&
                   styles.geometry_revision == initial.geometry_revision &&
                   styles.paint_revision == initial.paint_revision;
    }
    check(retained, "Theme64 exact repeated apply receipts preserve all revisions without "
                    "publication/layout invalidation");
    metrics.audio_radius = 12;
    const auto rounded = ui_theme_styles_apply(&styles, typography, metrics);
    check(rounded.ok() && rounded.changes == ui_theme_style_paint && styles.typography_revision == 1 &&
              styles.geometry_revision == 1 && styles.paint_revision == 2,
          "Theme radius-only change invalidates paint while retaining font and measurement authority");
    metrics.poll_padding = 20;
    const auto spaced = ui_theme_styles_apply(&styles, typography, metrics);
    check(spaced.ok() && spaced.changes == (ui_theme_style_geometry | ui_theme_style_paint) &&
              styles.typography_revision == 1 && styles.geometry_revision == 2,
          "Theme poll spacing invalidates geometry/paint without rebuilding unchanged font resources");
    selected = typography;
    selected.body_size = 20;
    selected.body_face = UiTextFace::rounded;
    const auto font = ui_theme_styles_apply(&styles, selected, metrics);
    check(font.ok() && font.changes == first.changes && styles.typography_revision == 2 &&
              styles.geometry_revision == 3 && styles.typography.body_size == 20 &&
              styles.typography.body_face == UiTextFace::rounded,
          "Theme typography changes explicitly invalidate text, geometry and paint together");
    u8 snapshot[sizeof(UiResolvedThemeStyles)]{};
    copy_bytes(snapshot, &styles, sizeof(styles));
    invalid = metrics;
    invalid.poll_target = nonfinite;
    check(!ui_theme_styles_apply(&styles, selected, invalid).ok() &&
              bytes_equal(snapshot, &styles, sizeof(styles)),
          "Theme invalid full candidate leaves every resolved byte/revision unchanged");
    styles.revision = styles.typography_revision = styles.geometry_revision = styles.paint_revision = ~0u;
    selected.body_size = 21;
    check(ui_theme_styles_apply(&styles, selected, metrics).ok() && styles.revision == 1 &&
              styles.typography_revision == 1 && styles.geometry_revision == 1 &&
              styles.paint_revision == 1,
          "Theme revision rollover preserves nonzero published-domain sentinel");

    const auto color_descriptors = ui_theme_color_descriptors();
    Theme palette = kDefaultTheme;
    bool colors_valid = color_descriptors.count == 20;
    for (u32 i = 0; i < color_descriptors.count; ++i) {
        const auto& descriptor = color_descriptors[i];
        const Theme before = palette;
        const Color replacement{.1f, .2f, .3f, .4f};
        colors_valid = colors_valid && static_cast<u32>(descriptor.id) == i && descriptor.name.bytes &&
                       ui_theme_color_set(&palette, descriptor.id, replacement);
        for (u32 j = 0; j < color_descriptors.count; ++j) {
            const auto* current = ui_theme_color_get(palette, color_descriptors[j].id);
            const auto* expected =
                j == i ? &replacement : ui_theme_color_get(before, color_descriptors[j].id);
            colors_valid = colors_valid && current && expected && current->red == expected->red &&
                           current->green == expected->green && current->blue == expected->blue &&
                           current->alpha == expected->alpha;
        }
    }
    check(colors_valid && ui_theme_palette_valid(palette),
          "Theme20 named palette tokens edit exactly one semantic color while preserving all others");
    const Theme before = palette;
    check(!ui_theme_color_set(&palette, UiThemeColor::accent, {nonfinite, 0, 0, 1}) &&
              !ui_theme_color_set(&palette, UiThemeColor::accent, {0, 0, 0, 1.01f}) &&
              !ui_theme_color_set(&palette, UiThemeColor::count, {0, 0, 0, 1}) &&
              ui_theme_palette_equal(before, palette),
          "Theme NaN/out-of-range alpha/unknown palette tokens preserve complete foreground palette");
    palette.background = background_theme_defaults();
    check(ui_theme_palette_equal(before, palette),
          "Theme foreground equality excludes independently retained background descriptor");
}
} // namespace maicord
