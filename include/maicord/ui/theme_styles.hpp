#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/ui/text_style.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {
struct Theme;
struct Color;
enum class UiThemeColor : u8 {
    base_lowest,
    base_lower,
    base_low,
    surface,
    surface_high,
    interactive,
    interactive_hover,
    accent,
    text_primary,
    avatar_placeholder,
    notification,
    syntax_keyword,
    syntax_type,
    syntax_string,
    syntax_number,
    syntax_comment,
    syntax_operator,
    syntax_builtin,
    syntax_preprocessor,
    text_secondary,
    count
};
struct UiThemeColorDescriptor {
    UiThemeColor id;
    Utf8View name;
};
ConstSlice<UiThemeColorDescriptor> ui_theme_color_descriptors() noexcept;
const Color* ui_theme_color_get(const Theme&, UiThemeColor) noexcept;
bool ui_theme_color_set(Theme*, UiThemeColor, const Color&) noexcept;
bool ui_theme_palette_valid(const Theme&) noexcept;
bool ui_theme_palette_equal(const Theme&, const Theme&) noexcept;

// Semantic tier. IDs follow maicord/ui/theme_tokens.inc order.
enum class UiThemeToken : u8 {
#define MAICORD_THEME_TOKEN(group, name, dark, light) group##_##name,
#include "maicord/ui/theme_tokens.inc"
    count
};
enum class UiThemeTokenSourceKind : u8 {
    inherit,
    literal,
    core,
    token
};
// A token value: a literal colour, or a core/semantic reference whose alpha
// is multiplied by alpha. inherit selects the table default for the appearance.
struct UiThemeTokenSource {
    Color literal;
    float alpha;
    u8 target;
    UiThemeTokenSourceKind kind;
};
struct UiThemeTokenDescriptor {
    UiThemeToken id;
    Utf8View group, name;
    UiThemeTokenSource dark, light;
};
inline constexpr u32 kUiThemeTokenMaximumDepth = 8;
ConstSlice<UiThemeTokenDescriptor> ui_theme_token_descriptors() noexcept;
const Color* ui_theme_token_get(const Theme&, UiThemeToken) noexcept;
enum class UiThemeTokenError : u8 {
    none,
    invalid_input,
    cycle,
    depth
};
struct UiThemeTokenResult {
    UiThemeTokenError error{};
    UiThemeToken token{UiThemeToken::count};
    bool ok() const noexcept {
        return error == UiThemeTokenError::none;
    }
};
// Apply-time only: resolves every token over the theme's core palette from
// overrides (UiThemeToken::count entries, or null for table defaults).
// References may point forward; cycles and chains deeper than the maximum
// fail and preserve the theme.
UiThemeTokenResult ui_theme_tokens_resolve(Theme*, const UiThemeTokenSource* overrides) noexcept;

enum class UiThemeScaleToken : u8 {
#define MAICORD_THEME_SCALE(name, value, minimum, maximum) name,
#include "maicord/ui/theme_tokens.inc"
    count
};
struct UiThemeScaleDescriptor {
    UiThemeScaleToken id;
    Utf8View name;
    float value, minimum, maximum;
};
ConstSlice<UiThemeScaleDescriptor> ui_theme_scale_descriptors() noexcept;
bool ui_theme_scale_get(const UiThemeScale&, UiThemeScaleToken, float*) noexcept;
bool ui_theme_scale_set(UiThemeScale*, UiThemeScaleToken, float) noexcept;
enum class UiThemeMotionToken : u8 {
#define MAICORD_THEME_MOTION(name, duration, curve) name,
#include "maicord/ui/theme_tokens.inc"
    count
};
struct UiThemeMotionDescriptor {
    UiThemeMotionToken id;
    Utf8View name;
};
ConstSlice<UiThemeMotionDescriptor> ui_theme_motion_descriptors() noexcept;
UiMotionStyle* ui_theme_motion_slot(UiThemeMotion*, UiThemeMotionToken) noexcept;
// WCAG 2 contrast of an opaque or translucent foreground over a background.
float ui_theme_contrast_ratio(Color foreground, Color background) noexcept;
struct UiThemeContrastWarning {
    UiThemeToken foreground, background;
    float ratio, minimum;
};
// Advisory checks of key text/background pairs; never blocks a theme.
// Returns the total failing pair count, writing up to capacity entries.
u32 ui_theme_contrast_warnings(const Theme&, UiThemeContrastWarning*, u32 capacity) noexcept;
// Accent treatment from a gradient. The emphasis hue is the most chromatic
// background stop; fill lightness is searched until text.on_accent (or the
// better opaque endpoint) reaches 4.5:1, outline until it reaches 3:1 over the
// floating and primary backgrounds, and the selected tint alpha until normal
// text keeps 4.5:1 even at the highest elevation boost. Writes nothing and
// returns false for a flat or achromatic theme.
struct UiThemeEmphasis {
    Color fill, text, outline, selected;
};
bool ui_theme_gradient_emphasis(const Theme&, UiThemeEmphasis*) noexcept;
// Publication-time projection: every emphasis token (and border.focus) whose
// value still equals its table default over the current palette is replaced
// by the gradient-derived value. Authored overrides and flat themes keep their
// values. Idempotent; runs on the published copy so documents never bake it.
bool ui_theme_resolve_emphasis(Theme*) noexcept;
// Selected-tint alpha boost at the highest elevation (ui_elevation_selected).
inline constexpr float kUiThemeEmphasisElevationBoost = 1.5f;
// Fonts are restricted to the existing licensed catalog. One base em serves UI
// and message body text; semantic flags select these ratios, not another font bank.
struct UiThemeTypography {
    float body_size{16}, heading_1_ratio{1.5f}, heading_2_ratio{1.25f}, heading_3_ratio{1.1f};
    float subtext_ratio{.8125f}, mono_ratio{.9375f};
    UiTextFace body_face{UiTextFace::normal}, code_face{UiTextFace::mono};
};
// Semantic DIP dimensions only. Colors/background stay in Theme; product labels,
// capacities, permissions and widget identities are not theme data. Defaults
// match the shared primitives. Consumers project these into their own styles.
struct UiThemeMetrics {
    float shell_rail_width{72}, shell_sidebar_width{240}, shell_member_width{240}, shell_header_height{48},
        shell_gutter{0};
    float control_height{32}, control_radius{6}, edit_radius{8}, compact_radius{4}, composer_padding{4};
    float message_left{18}, message_right{24}, message_top{14}, message_bottom{1};
    float message_grouped_top{0}, message_grouped_bottom{1}, message_avatar{40}, message_avatar_gap{14},
        message_rich_gap{8};
    float form_width{680}, form_field_padding{12}, form_group_gap{24}, form_single_height{40},
        form_multiline_height{160}, form_footer_height{64};
    float menu_padding{6}, menu_gap{8}, menu_icon{20}, menu_compact_height{32}, menu_row_height{36},
        menu_detail_height{56}, menu_radius{10};
    float audio_padding{8}, audio_gap{6}, audio_target{32}, audio_minimum_width{160},
        audio_maximum_width{480};
    float audio_title_height{20}, audio_time_width{112}, audio_volume_width{64}, audio_radius{8};
    float poll_padding{16}, poll_gap{8}, poll_answer_padding{16}, poll_answer_gap{8};
    float poll_minimum_width{128}, poll_maximum_width{472}, poll_target{38}, poll_marker{24},
        poll_emoji{24}, poll_radius{8};
};

enum class UiThemeMetric : u8 {
    shell_rail_width,
    shell_sidebar_width,
    shell_member_width,
    shell_header_height,
    shell_gutter,
    control_height,
    control_radius,
    edit_radius,
    compact_radius,
    composer_padding,
    message_left,
    message_right,
    message_top,
    message_bottom,
    message_grouped_top,
    message_grouped_bottom,
    message_avatar,
    message_avatar_gap,
    message_rich_gap,
    form_width,
    form_field_padding,
    form_group_gap,
    form_single_height,
    form_multiline_height,
    form_footer_height,
    menu_padding,
    menu_gap,
    menu_icon,
    menu_compact_height,
    menu_row_height,
    menu_detail_height,
    menu_radius,
    audio_padding,
    audio_gap,
    audio_target,
    audio_minimum_width,
    audio_maximum_width,
    audio_title_height,
    audio_time_width,
    audio_volume_width,
    audio_radius,
    poll_padding,
    poll_gap,
    poll_answer_padding,
    poll_answer_gap,
    poll_minimum_width,
    poll_maximum_width,
    poll_target,
    poll_marker,
    poll_emoji,
    poll_radius,
    count
};
enum UiThemeStyleChange : u32 {
    ui_theme_style_none = 0,
    ui_theme_style_typography = 1u << 0,
    ui_theme_style_geometry = 1u << 1,
    ui_theme_style_paint = 1u << 2,
};
struct UiThemeMetricDescriptor {
    UiThemeMetric id;
    Utf8View name;
    float minimum, maximum;
    u32 changes;
};
// Fixed compile-time vocabulary for the bounded document parser. IDs append
// only; names are the portable wire keys.
ConstSlice<UiThemeMetricDescriptor> ui_theme_metric_descriptors() noexcept;
bool ui_theme_metric_get(const UiThemeMetrics&, UiThemeMetric, float*) noexcept;
bool ui_theme_metric_set(UiThemeMetrics*, UiThemeMetric, float) noexcept;
bool ui_theme_typography_valid(const UiThemeTypography&) noexcept;
bool ui_theme_typography_equal(const UiThemeTypography&, const UiThemeTypography&) noexcept;
bool ui_theme_metrics_valid(const UiThemeMetrics&) noexcept;
bool ui_theme_metrics_equal(const UiThemeMetrics&, const UiThemeMetrics&) noexcept;
float ui_theme_text_size(const UiThemeTypography&, float em, u32 style_flags) noexcept;

// Icon tokens in UiIconRole order: normal, muted, accent, danger, success,
// warning, on_accent. Packed 0xRRGGBBAA; zero keeps the palette-derived colour.
struct UiThemeIcons {
    u32 colors[7]{};
    float weight{1};
};
// Settings surface tokens. Colours are packed 0xRRGGBBAA and zero derives the
// colour from the palette; sizes are DIPs, durations milliseconds and ratios
// percent. Resolve through ui_settings_tokens
// (maicord/ui/controls/settings_tokens.hpp).
struct UiThemeSettings {
    u32 text_strong{}, text_subtle{}, nav_header{}, border_subtle{}, border_normal{}, mod_subtle{},
        mod_normal{}, mod_strong{}, input_background{}, input_border{}, brand_hover{}, critical{},
        critical_text{}, critical_tint{}, positive{}, focus_ring{}, scrim{}, switch_track{}, switch_border{},
        slider_track{}, radio_border{}, modal_background{}, sidebar_background{};
    u16 modal_max_width{1280}, modal_viewport_ratio{80}, modal_ratio_threshold{1080}, modal_inset_top{56},
        modal_inset_side{24}, modal_inset_bottom{24}, modal_radius{12}, modal_compact_width{560};
    u16 sidebar_width{248}, sidebar_padding{12}, sidebar_top{16}, search_height{32}, nav_header_height{28},
        nav_item_height{32}, nav_item_radius{6}, nav_item_gap{2}, nav_item_padding{10}, nav_icon{18},
        separator_margin{8};
    u16 panel_padding_x{40}, panel_padding_top{28}, panel_padding_bottom{60}, panel_max_width{740},
        title_gap{24}, category_gap{36}, category_heading_gap{12}, row_padding{18}, label_gap{4},
        control_gap{24}, field_gap{8};
    u16 switch_width{48}, switch_height{24}, switch_thumb{16}, slider_bar{4}, slider_grabber{16},
        slider_mark_height{24}, radio_size{20}, radio_gap{12}, radio_spacing{12}, control_height{40},
        control_radius{8}, button_small{32}, button_medium{40}, button_padding{16}, swatch{60}, swatch_gap{16},
        close_size{36}, close_caption_gap{6}, notice_padding{12};
    u16 switch_ms{300}, switch_reduced_ms{200}, modal_enter_ms{320}, flash_ms{2000}, flash_fade_ms{200};
};
struct UiResolvedThemeStyles {
    UiThemeTypography typography{};
    UiThemeMetrics metrics{};
    u32 revision{}, typography_revision{}, geometry_revision{}, paint_revision{};
    bool ready{};
    UiThemeIcons icons{};
    UiThemeSettings settings{};
};
enum class UiThemeStylesError : u8 {
    none,
    invalid_input
};
struct UiThemeStylesResult {
    UiThemeStylesError error{};
    u32 changes{};
    bool ok() const noexcept {
        return error == UiThemeStylesError::none;
    }
    bool changed() const noexcept {
        return changes != 0;
    }
};
// Apply-time only, allocation-free and transactional. Invalid input preserves
// all bytes and revisions. Identical input performs no publication/invalidation.
// Typography changes include geometry+paint; radii alone invalidate paint only.
// Palette/background revisions are owned by the enclosing theme document.
UiThemeStylesResult ui_theme_styles_apply(UiResolvedThemeStyles*, const UiThemeTypography&,
                                          const UiThemeMetrics&) noexcept;
static_assert(sizeof(UiResolvedThemeStyles) <= 512);
} // namespace maicord
