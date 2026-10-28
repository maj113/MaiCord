#include "maicord/ui/theme_styles.hpp"
#include "maicord/ui/theme.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include <stddef.h>
#include "maicord/ui/background_color_math.hpp"
#include "maicord/ui/background_presentation.hpp"

namespace maicord {
namespace {
// A fixed, typed schema. These tables are visited on import/apply only; frame
// consumers read their resolved numeric styles/revisions directly.
#define THEME_COLORS(X)                                                                                    \
    X(base_lowest)                                                                                         \
    X(base_lower) X(base_low) X(surface) X(surface_high) X(interactive) X(interactive_hover) X(accent)     \
        X(text_primary) X(avatar_placeholder) X(notification) X(syntax_keyword) X(syntax_type)             \
            X(syntax_string) X(syntax_number) X(syntax_comment) X(syntax_operator) X(syntax_builtin)       \
                X(syntax_preprocessor) X(text_secondary)
constexpr u32 geometry = ui_theme_style_geometry | ui_theme_style_paint;
constexpr u32 paint = ui_theme_style_paint;
#define THEME_METRICS(X)                                                                                   \
    X(shell_rail_width, 48, 112, geometry)                                                                 \
    X(shell_sidebar_width, 160, 400, geometry) X(shell_member_width, 160, 400, geometry) X(                \
        shell_header_height, 40, 72, geometry) X(shell_gutter, 0, 24, geometry) X(control_height, 28, 64,  \
                                                                                  geometry)                \
        X(control_radius, 0, 24, paint) X(edit_radius, 0, 24, paint) X(compact_radius, 0, 16, paint) X(    \
            composer_padding, 2, 16, geometry) X(message_left, 0, 48, geometry)                            \
            X(message_right, 0, 64, geometry) X(message_top, 0, 40, geometry) X(                           \
                message_bottom,                                                                            \
                0, 24, geometry) X(message_grouped_top, 0, 24, geometry)                                   \
                X(message_grouped_bottom, 0, 24, geometry) X(message_avatar, 24, 64, geometry) X(          \
                    message_avatar_gap,                                                                    \
                    4, 32, geometry) X(message_rich_gap, 0, 32, geometry)                                  \
                    X(form_width, 320, 960, geometry) X(form_field_padding, 4, 24, geometry)               \
                        X(form_group_gap, 4, 48, geometry) X(form_single_height, 32, 72, geometry) X(      \
                            form_multiline_height, 80, 320,                                                \
                            geometry) X(form_footer_height, 40, 96, geometry)                              \
                            X(menu_padding, 2, 16, geometry) X(menu_gap, 0, 24, geometry)                  \
                                X(menu_icon, 16, 32, geometry) X(menu_compact_height, 28, 56, geometry) X( \
                                    menu_row_height, 32, 64, geometry) X(menu_detail_height, 40, 96,       \
                                                                         geometry)                         \
                                    X(menu_radius, 0, 32, paint) X(audio_padding, 4, 20, geometry)         \
                                        X(audio_gap, 2, 16, geometry) X(audio_target, 32, 48, geometry) X( \
                                            audio_minimum_width, 160,                                      \
                                            320, geometry) X(audio_maximum_width, 320, 720, geometry)      \
                                            X(audio_title_height, 16, 36,                                  \
                                              geometry) X(audio_time_width, 80, 160, geometry)             \
                                                X(audio_volume_width, 48, 128,                             \
                                                  geometry) X(audio_radius, 0, 24, paint)                  \
                                                    X(poll_padding, 4, 24,                                 \
                                                      geometry) X(poll_gap, 2, 20, geometry)               \
                                                        X(poll_answer_padding, 4, 20,                      \
                                                          geometry) X(poll_answer_gap, 2, 16, geometry)    \
                                                            X(poll_minimum_width, 128, 320, geometry)      \
                                                                X(poll_maximum_width, 320, 720, geometry)  \
                                                                    X(poll_target, 32, 56, geometry) X(    \
                                                                        poll_marker, 12, 32, geometry)     \
                                                                        X(poll_emoji, 16, 32, geometry)    \
                                                                            X(poll_radius, 0, 24, paint)
enum class TokenGroup : u8 {
#define MAICORD_THEME_GROUP(group) group,
#include "maicord/ui/theme_tokens.inc"
    count
};
// Every document key name, family by family in ID order. Trailing
// underscores (C++ keyword members) are dropped from the pooled key.
constexpr const char* kNameSource[]{
#define NAME(name) #name,
    THEME_COLORS(NAME)
#undef NAME
#define MAICORD_THEME_GROUP(group) #group,
#include "maicord/ui/theme_tokens.inc"
#define MAICORD_THEME_TOKEN(group, name, dark, light) #name,
#include "maicord/ui/theme_tokens.inc"
#define MAICORD_THEME_SCALE(name, value, minimum, maximum) #name,
#include "maicord/ui/theme_tokens.inc"
#define MAICORD_THEME_MOTION(name, duration, curve) #name,
#include "maicord/ui/theme_tokens.inc"
#define METRIC_NAME(name, low, high, domain) #name,
    THEME_METRICS(METRIC_NAME)
#undef METRIC_NAME
};
constexpr u32 kColorCount = static_cast<u32>(UiThemeColor::count);
constexpr u32 kGroupCount = static_cast<u32>(TokenGroup::count);
constexpr u32 kTokenCount = static_cast<u32>(UiThemeToken::count);
constexpr u32 kScaleCount = static_cast<u32>(UiThemeScaleToken::count);
constexpr u32 kMotionCount = static_cast<u32>(UiThemeMotionToken::count);
constexpr u32 kMetricCount = static_cast<u32>(UiThemeMetric::count);
constexpr u32 kFirstGroup = kColorCount, kFirstToken = kFirstGroup + kGroupCount,
              kFirstScale = kFirstToken + kTokenCount, kFirstMotion = kFirstScale + kScaleCount,
              kFirstMetric = kFirstMotion + kMotionCount, kNameCount = kFirstMetric + kMetricCount;
static_assert(sizeof(kNameSource) / sizeof(kNameSource[0]) == kNameCount);
struct NamePool {
    char text[4096]{};
    u16 offsets[kNameCount]{};
    u32 used{};
};
// Equal names share one pooled string.
consteval NamePool build_name_pool() noexcept {
    NamePool pool{};
    for (u32 i = 0; i < kNameCount; ++i) {
        const char* name = kNameSource[i];
        u32 length = c_string_length(name);
        if (length && name[length - 1] == '_')
            --length;
        bool shared{};
        for (u32 j = 0; j < i && !shared; ++j) {
            const u32 at = pool.offsets[j];
            u32 k{};
            while (k < length && pool.text[at + k] == name[k])
                ++k;
            if (k == length && !pool.text[at + k]) {
                pool.offsets[i] = static_cast<u16>(at);
                shared = true;
            }
        }
        if (shared)
            continue;
        pool.offsets[i] = static_cast<u16>(pool.used);
        for (u32 k = 0; k < length; ++k)
            pool.text[pool.used++] = name[k];
        ++pool.used; // the terminator
    }
    return pool;
}
constexpr NamePool kBuiltNames = build_name_pool();
template <u32 Bytes> struct PoolText {
    char text[Bytes];
};
template <u32 Bytes> consteval PoolText<Bytes> pool_text() noexcept {
    PoolText<Bytes> pool{};
    for (u32 i = 0; i < Bytes; ++i)
        pool.text[i] = kBuiltNames.text[i];
    return pool;
}
constexpr auto kNames = pool_text<kBuiltNames.used>();
constexpr u16 name_slot(u32 index) noexcept {
    return kBuiltNames.offsets[index];
}
#define COLOR_DESCRIPTOR(name)                                                                             \
    {name_slot(static_cast<u32>(UiThemeColor::name)), UiThemeColor::name},
constexpr UiThemeColorDescriptor colors[]{THEME_COLORS(COLOR_DESCRIPTOR)};
#undef COLOR_DESCRIPTOR
#define METRIC_DESCRIPTOR(name, low, high, domain)                                                         \
    {name_slot(kFirstMetric + static_cast<u32>(UiThemeMetric::name)), low, high, domain, UiThemeMetric::name},
constexpr UiThemeMetricDescriptor metrics[]{THEME_METRICS(METRIC_DESCRIPTOR)};
#undef METRIC_DESCRIPTOR
static_assert(sizeof(colors) / sizeof(colors[0]) == kColorCount);
constexpr u16 kGroupNames[]{
#define MAICORD_THEME_GROUP(group) name_slot(kFirstGroup + static_cast<u32>(TokenGroup::group)),
#include "maicord/ui/theme_tokens.inc"
};
// Every token's dark and light default, in ID order, before deduplication.
#define P(id) {{}, 1, static_cast<u8>(UiThemeColor::id), UiThemeTokenSourceKind::core}
#define PA(id, a) {{}, a, static_cast<u8>(UiThemeColor::id), UiThemeTokenSourceKind::core}
#define T(group, name)                                                                                     \
    {{}, 1, static_cast<u8>(UiThemeToken::group##_##name), UiThemeTokenSourceKind::token}
#define RGBA(r, g, b, a) {{r, g, b, a}, 1, 0, UiThemeTokenSourceKind::literal}
constexpr u32 kSourceCount = 2 * kTokenCount;
constexpr UiThemeTokenSource kTokenSources[kSourceCount]{
#define MAICORD_THEME_TOKEN(group, name, dark, light) UiThemeTokenSource dark, UiThemeTokenSource light,
#include "maicord/ui/theme_tokens.inc"
};
#undef P
#undef PA
#undef T
#undef RGBA
consteval bool same_source(const UiThemeTokenSource& a, const UiThemeTokenSource& b) noexcept {
    return a.kind == b.kind && a.alpha == b.alpha && a.target == b.target && a.literal.red == b.literal.red &&
           a.literal.green == b.literal.green && a.literal.blue == b.literal.blue &&
           a.literal.alpha == b.literal.alpha;
}
// Equal defaults share one slot: literals first, then references.
struct SourceSlots {
    UiThemeTokenSource unique[kSourceCount]{};
    u8 slot[kSourceCount]{};
    u32 literals{}, references{};
};
consteval SourceSlots dedupe_sources() noexcept {
    SourceSlots slots{};
    u32 count{};
    for (u32 pass = 0; pass < 2; ++pass) {
        const u32 first = count;
        for (u32 i = 0; i < kSourceCount; ++i) {
            if ((kTokenSources[i].kind == UiThemeTokenSourceKind::literal) != (pass == 0))
                continue;
            u32 at = first;
            while (at < count && !same_source(slots.unique[at], kTokenSources[i]))
                ++at;
            if (at == count)
                slots.unique[count++] = kTokenSources[i];
            slots.slot[i] = static_cast<u8>(at);
        }
        if (!pass)
            slots.literals = count;
    }
    slots.references = count - slots.literals;
    return slots;
}
constexpr SourceSlots kSlots = dedupe_sources();
static_assert(kSlots.literals + kSlots.references <= 255);
struct ReferenceSource {
    float alpha;
    u8 target;
    UiThemeTokenSourceKind kind;
};
template <typename Row, u32 Count> struct SourceTable {
    Row row[Count];
};
consteval auto literal_table() noexcept {
    SourceTable<Color, kSlots.literals> table{};
    for (u32 i = 0; i < kSlots.literals; ++i)
        table.row[i] = kSlots.unique[i].literal;
    return table;
}
consteval auto reference_table() noexcept {
    SourceTable<ReferenceSource, kSlots.references> table{};
    for (u32 i = 0; i < kSlots.references; ++i) {
        const auto& source = kSlots.unique[kSlots.literals + i];
        table.row[i] = {source.alpha, source.target, source.kind};
    }
    return table;
}
constexpr auto kLiterals = literal_table();
constexpr auto kReferences = reference_table();
UiThemeTokenSource default_source(u32 slot) noexcept {
    if (slot < kSlots.literals)
        return {kLiterals.row[slot], 1, 0, UiThemeTokenSourceKind::literal};
    const auto& reference = kReferences.row[slot - kSlots.literals];
    return {{}, reference.alpha, reference.target, reference.kind};
}
#define MAICORD_THEME_TOKEN(group, name, dark, light)                                                      \
    {name_slot(kFirstToken + static_cast<u32>(UiThemeToken::group##_##name)),                              \
     static_cast<u8>(TokenGroup::group),                                                                   \
     kSlots.slot[2 * static_cast<u32>(UiThemeToken::group##_##name)],                                      \
     kSlots.slot[2 * static_cast<u32>(UiThemeToken::group##_##name) + 1], UiThemeToken::group##_##name},
constexpr UiThemeTokenDescriptor tokens[]{
#include "maicord/ui/theme_tokens.inc"
};
#define MAICORD_THEME_SCALE(name, value, minimum, maximum)                                                 \
    {name_slot(kFirstScale + static_cast<u32>(UiThemeScaleToken::name)), minimum, maximum,                 \
     UiThemeScaleToken::name},
constexpr UiThemeScaleDescriptor scales[]{
#include "maicord/ui/theme_tokens.inc"
};
#define MAICORD_THEME_MOTION(name, duration, curve)                                                        \
    {name_slot(kFirstMotion + static_cast<u32>(UiThemeMotionToken::name)), UiThemeMotionToken::name},
constexpr UiThemeMotionDescriptor motions[]{
#include "maicord/ui/theme_tokens.inc"
};
static_assert(sizeof(tokens) / sizeof(tokens[0]) == kTokenCount);
static_assert(sizeof(scales) / sizeof(scales[0]) == kScaleCount);
static_assert(sizeof(motions) / sizeof(motions[0]) == kMotionCount);
static_assert(sizeof(metrics) / sizeof(metrics[0]) == kMetricCount);
bool between(float value, float minimum, float maximum) noexcept {
    // Comparisons reject NaN and either infinity without a CRT math dependency.
    return value >= minimum && value <= maximum;
}
bool color_valid(const Color& c) noexcept {
    return between(c.red, 0, 1) && between(c.green, 0, 1) && between(c.blue, 0, 1) &&
           between(c.alpha, 0, 1);
}
bool color_equal(const Color& a, const Color& b) noexcept {
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}
u32 advance(u32 value) noexcept {
    return value == ~0u ? 1u : value + 1u;
}
} // namespace
Utf8View ui_theme_name(u16 offset) noexcept {
    const char* data = kNames.text + offset;
    const u32 bytes = c_string_length(data);
    return {data, bytes};
}
Utf8View ui_theme_token_group_name(u8 group) noexcept {
    return ui_theme_name(kGroupNames[group]);
}
ConstSlice<UiThemeColorDescriptor> ui_theme_color_descriptors() noexcept {
    return {colors, static_cast<u32>(UiThemeColor::count)};
}
// The core palette, the semantic tokens, the scales and the metrics are each
// stored as one field per ID, in ID order; the accessors index them.
#define COLOR_SLOT(name)                                                                                       static_assert(offsetof(Theme, name) == static_cast<u32>(UiThemeColor::name) * sizeof(Color));
THEME_COLORS(COLOR_SLOT)
#undef COLOR_SLOT
const Color* ui_theme_color_get(const Theme& theme, UiThemeColor id) noexcept {
    return id < UiThemeColor::count ? &theme.base_lowest + static_cast<u32>(id) : nullptr;
}
bool ui_theme_color_set(Theme* theme, UiThemeColor id, const Color& color) noexcept {
    if (!theme || !color_valid(color) || id >= UiThemeColor::count)
        return false;
    (&theme->base_lowest)[static_cast<u32>(id)] = color;
    return true;
}
// Palette validity/equality cover every paint tier: core colours, semantic
// tokens, scales and motion.
bool ui_theme_palette_valid(const Theme& theme) noexcept {
    for (const auto& descriptor : colors) {
        const auto* color = ui_theme_color_get(theme, descriptor.id);
        if (!color || !color_valid(*color))
            return false;
    }
    for (const auto& descriptor : tokens)
        if (!color_valid(*ui_theme_token_get(theme, descriptor.id)))
            return false;
    for (const auto& descriptor : scales) {
        float value{};
        if (!ui_theme_scale_get(theme.scale, descriptor.id, &value) ||
            !between(value, descriptor.minimum(), descriptor.maximum()))
            return false;
    }
    auto motion = theme.motion;
    for (const auto& descriptor : motions)
        if (!ui_motion_style_valid(*ui_theme_motion_slot(&motion, descriptor.id)))
            return false;
    return true;
}
bool ui_theme_palette_equal(const Theme& a, const Theme& b) noexcept {
    for (const auto& descriptor : colors)
        if (!color_equal(*ui_theme_color_get(a, descriptor.id), *ui_theme_color_get(b, descriptor.id)))
            return false;
    for (const auto& descriptor : tokens)
        if (!color_equal(*ui_theme_token_get(a, descriptor.id), *ui_theme_token_get(b, descriptor.id)))
            return false;
    for (const auto& descriptor : scales) {
        float left{}, right{};
        ui_theme_scale_get(a.scale, descriptor.id, &left);
        ui_theme_scale_get(b.scale, descriptor.id, &right);
        if (left != right)
            return false;
    }
    auto ma = a.motion, mb = b.motion;
    for (const auto& descriptor : motions) {
        const auto* x = ui_theme_motion_slot(&ma, descriptor.id);
        const auto* y = ui_theme_motion_slot(&mb, descriptor.id);
        if (x->duration_ms != y->duration_ms || x->curve != y->curve)
            return false;
    }
    return true;
}
ConstSlice<UiThemeTokenDescriptor> ui_theme_token_descriptors() noexcept {
    return {tokens, static_cast<u32>(UiThemeToken::count)};
}
#define MAICORD_THEME_TOKEN(group, name, dark, light)                                                          static_assert(offsetof(UiThemeTokens, group) + offsetof(UiTheme_##group##_tokens, name) ==                                static_cast<u32>(UiThemeToken::group##_##name) * sizeof(Color));
#include "maicord/ui/theme_tokens.inc"
const Color* ui_theme_token_get(const Theme& theme, UiThemeToken id) noexcept {
    return id < UiThemeToken::count ? reinterpret_cast<const Color*>(&theme.tokens) + static_cast<u32>(id)
                                    : nullptr;
}
namespace {
struct TokenResolution {
    const Theme* theme;
    const UiThemeTokenSource* overrides;
    Color values[static_cast<u32>(UiThemeToken::count)];
    u8 state[static_cast<u32>(UiThemeToken::count)];
    // Reference chain length below each resolved token, so memoized tokens
    // still bound the total depth of a later chain through them.
    u8 depth[static_cast<u32>(UiThemeToken::count)];
    UiThemeTokenResult result;
};
bool resolve_token(TokenResolution& r, u32 id) noexcept {
    if (r.state[id] == 2)
        return true;
    if (r.state[id] == 1) {
        r.result = {UiThemeTokenError::cycle, static_cast<UiThemeToken>(id)};
        return false;
    }
    r.state[id] = 1;
    const auto& descriptor = tokens[id];
    const UiThemeTokenSource source =
        r.overrides && r.overrides[id].kind != UiThemeTokenSourceKind::inherit
            ? r.overrides[id]
            : default_source(r.theme->appearance == ThemeAppearance::light ? descriptor.light : descriptor.dark);
    const Color* reference = nullptr;
    if (source.kind == UiThemeTokenSourceKind::literal)
        reference = &source.literal;
    else if (source.kind == UiThemeTokenSourceKind::core && source.target < static_cast<u8>(UiThemeColor::count))
        reference = ui_theme_color_get(*r.theme, static_cast<UiThemeColor>(source.target));
    else if (source.kind == UiThemeTokenSourceKind::token && source.target < static_cast<u8>(UiThemeToken::count)) {
        if (!resolve_token(r, source.target))
            return false;
        r.depth[id] = static_cast<u8>(r.depth[source.target] + 1);
        if (r.depth[id] > kUiThemeTokenMaximumDepth) {
            r.result = {UiThemeTokenError::depth, static_cast<UiThemeToken>(id)};
            return false;
        }
        reference = &r.values[source.target];
    }
    const Color value = reference ? source.kind == UiThemeTokenSourceKind::literal
                                        ? *reference
                                        : ui_theme_color_alpha(*reference, source.alpha)
                                  : Color{-1, -1, -1, -1};
    if (!color_valid(value)) {
        r.result = {UiThemeTokenError::invalid_input, static_cast<UiThemeToken>(id)};
        return false;
    }
    r.values[id] = value;
    r.state[id] = 2;
    return true;
}
} // namespace
UiThemeTokenResult ui_theme_tokens_resolve(Theme* theme, const UiThemeTokenSource* overrides) noexcept {
    if (!theme)
        return {UiThemeTokenError::invalid_input};
    TokenResolution r{theme, overrides, {}, {}, {}, {}};
    for (u32 i = 0; i < static_cast<u32>(UiThemeToken::count); ++i)
        if (!resolve_token(r, i))
            return r.result;
    // The schema orders both the resolved array and the typed token fields.
    static_assert(sizeof(theme->tokens) == sizeof(r.values));
#define MAICORD_THEME_TOKEN(group, name, dark, light) \
    static_assert(offsetof(UiThemeTokens, group.name) == static_cast<u32>(UiThemeToken::group##_##name) * sizeof(Color));
#include "maicord/ui/theme_tokens.inc"
    copy_bytes(&theme->tokens, r.values, sizeof(r.values));
    return {};
}
ConstSlice<UiThemeScaleDescriptor> ui_theme_scale_descriptors() noexcept {
    return {scales, static_cast<u32>(UiThemeScaleToken::count)};
}
#define MAICORD_THEME_SCALE(name, value, minimum, maximum)                                                     static_assert(offsetof(UiThemeScale, name) == static_cast<u32>(UiThemeScaleToken::name) * sizeof(float));
#include "maicord/ui/theme_tokens.inc"
bool ui_theme_scale_get(const UiThemeScale& scale, UiThemeScaleToken id, float* output) noexcept {
    if (!output || id >= UiThemeScaleToken::count)
        return false;
    *output = reinterpret_cast<const float*>(&scale)[static_cast<u8>(id)];
    return true;
}
bool ui_theme_scale_set(UiThemeScale* scale, UiThemeScaleToken id, float input) noexcept {
    if (!scale || id >= UiThemeScaleToken::count)
        return false;
    const auto& descriptor = scales[static_cast<u8>(id)];
    if (!between(input, descriptor.minimum(), descriptor.maximum()))
        return false;
    reinterpret_cast<float*>(scale)[static_cast<u8>(id)] = input;
    return true;
}
ConstSlice<UiThemeMotionDescriptor> ui_theme_motion_descriptors() noexcept {
    return {motions, static_cast<u32>(UiThemeMotionToken::count)};
}
UiMotionStyle* ui_theme_motion_slot(UiThemeMotion* motion, UiThemeMotionToken id) noexcept {
    if (!motion)
        return nullptr;
    switch (id) {
#define MAICORD_THEME_MOTION(name, duration, curve)                                                        \
    case UiThemeMotionToken::name:                                                                         \
        return &motion->name;
#include "maicord/ui/theme_tokens.inc"
    default:
        return nullptr;
    }
}
float ui_theme_contrast_ratio(const Color& foreground, const Color& background) noexcept {
    const float a = foreground.alpha;
    const Color composed{foreground.red * a + background.red * (1 - a),
                         foreground.green * a + background.green * (1 - a),
                         foreground.blue * a + background.blue * (1 - a), 1};
    const double x = background_color_luminance(composed), y = background_color_luminance(background);
    return static_cast<float>((maximum(x, y) + .05) / (minimum(x, y) + .05));
}
u32 ui_theme_contrast_warnings(const Theme& theme, UiThemeContrastWarning* output, u32 capacity) noexcept {
    // WCAG AA: 4.5:1 for body text; 3:1 for secondary/large text and accents.
    static constexpr UiThemeContrastWarning pairs[]{
        {UiThemeToken::text_normal, UiThemeToken::background_primary, 0, 4.5f},
        {UiThemeToken::text_normal, UiThemeToken::background_secondary, 0, 4.5f},
        {UiThemeToken::text_normal, UiThemeToken::background_floating, 0, 4.5f},
        {UiThemeToken::text_muted, UiThemeToken::background_primary, 0, 3},
        {UiThemeToken::text_muted, UiThemeToken::background_secondary, 0, 3},
        {UiThemeToken::text_link, UiThemeToken::background_primary, 0, 3},
        {UiThemeToken::text_danger, UiThemeToken::background_primary, 0, 3},
        {UiThemeToken::text_on_accent, UiThemeToken::accent_brand, 0, 3},
        {UiThemeToken::emphasis_text, UiThemeToken::emphasis_fill, 0, 3},
        {UiThemeToken::emphasis_outline, UiThemeToken::background_floating, 0, 3},
    };
    u32 count{};
    for (auto pair : pairs) {
        pair.ratio = ui_theme_contrast_ratio(*ui_theme_token_get(theme, pair.foreground),
                                             *ui_theme_token_get(theme, pair.background));
        if (pair.ratio >= pair.minimum)
            continue;
        if (output && count < capacity)
            output[count] = pair;
        ++count;
    }
    return count;
}
namespace {
Color composite(Color top, Color bottom) noexcept {
    const float a = top.alpha;
    return {top.red * a + bottom.red * (1 - a), top.green * a + bottom.green * (1 - a),
            top.blue * a + bottom.blue * (1 - a), 1};
}
bool readable_over(Color foreground, Color first, Color second, float minimum_ratio) noexcept {
    return ui_theme_contrast_ratio(foreground, first) >= minimum_ratio &&
           ui_theme_contrast_ratio(foreground, second) >= minimum_ratio;
}
} // namespace
bool ui_theme_gradient_emphasis(const Theme& theme, UiThemeEmphasis* output) noexcept {
    Color stops[5]{};
    if (!output || !ui_background_palette(theme, stops))
        return false;
    // Hue: the most chromatic stop (OKLab), so muted ramps borrow their accent
    // from the one colour that carries it.
    double best = -1;
    BackgroundOklab hue{};
    for (u32 i = 0; i < theme.background.color_count; ++i) {
        const auto lab = background_color_oklab(stops[i]);
        const double squared = lab.a * lab.a + lab.b * lab.b;
        if (squared > best) {
            best = squared;
            hue = lab;
        }
    }
    if (best < .02 * .02)
        return false;
    const double chroma = background_color_root(best, 2);
    const double target = chroma * 1.25 < .09 ? .09 : chroma * 1.25 > .18 ? .18 : chroma * 1.25;
    const double a = hue.a / chroma * target, b = hue.b / chroma * target;
    const bool light = theme.appearance == ThemeAppearance::light;
    const Color floating = theme.tokens.background.floating, primary = theme.tokens.background.primary;
    UiThemeEmphasis value{};
    // Fill: the brightest lightness at which the accent label stays 4.5:1.
    value.text = theme.tokens.text.on_accent;
    bool labelled = false;
    for (u32 step = 0; step <= 20 && !labelled; ++step) {
        value.fill = background_color_from_oklab({.74 - step * .02, a, b});
        labelled = ui_theme_contrast_ratio(value.text, value.fill) >= 4.5f;
    }
    if (!labelled) {
        value.fill = background_color_from_oklab({light ? .5 : .62, a, b});
        const Color white = background_color_rgb24(0xffffff), black = background_color_rgb24(0);
        value.text = ui_theme_contrast_ratio(white, value.fill) >= ui_theme_contrast_ratio(black, value.fill)
                         ? white
                         : black;
    }
    // Outline: a non-text indicator, 3:1 against the surfaces it sits on.
    bool visible = false;
    for (u32 step = 0; step <= 30 && !visible; ++step) {
        value.outline = background_color_from_oklab({light ? .56 - step * .016 : .62 + step * .012, a, b});
        visible = readable_over(value.outline, floating, primary, 3);
    }
    // Selected tint: the strongest alpha that keeps normal text 4.5:1 even
    // after the highest elevation boost.
    value.selected = value.outline;
    float alpha = light ? .18f : .24f;
    for (; alpha > .04f; alpha -= .02f) {
        Color boosted = value.outline;
        boosted.alpha = minimum(1.f, alpha * kUiThemeEmphasisElevationBoost);
        if (readable_over(theme.tokens.text.normal, composite(boosted, floating), composite(boosted, primary),
                          4.5f))
            break;
    }
    value.selected.alpha = alpha;
    value.tint = background_color_from_oklab({light ? .5 : .66, a, b});
    *output = value;
    return true;
}
bool ui_theme_resolve_emphasis(Theme* theme) noexcept {
    UiThemeEmphasis derived{};
    if (!theme || !ui_theme_gradient_emphasis(*theme, &derived))
        return false;
    // Table defaults over the current palette identify untouched values.
    TokenResolution defaults{theme, nullptr, {}, {}, {}, {}};
    // Neutral white/black glass leans halfway to the hue, keeping its alpha.
    const auto glass = [&](Color neutral) noexcept {
        return Color{(neutral.red + derived.tint.red) * .5f, (neutral.green + derived.tint.green) * .5f,
                     (neutral.blue + derived.tint.blue) * .5f, neutral.alpha};
    };
    const struct {
        UiThemeToken id;
        Color* slot;
        Color value;
    } targets[]{
        {UiThemeToken::emphasis_fill, &theme->tokens.emphasis.fill, derived.fill},
        {UiThemeToken::emphasis_text, &theme->tokens.emphasis.text, derived.text},
        {UiThemeToken::emphasis_outline, &theme->tokens.emphasis.outline, derived.outline},
        {UiThemeToken::emphasis_selected, &theme->tokens.emphasis.selected, derived.selected},
        {UiThemeToken::border_focus, &theme->tokens.border.focus, derived.outline},
        {UiThemeToken::mention_background, &theme->tokens.mention.background,
         ui_theme_color_alpha(derived.outline, theme->tokens.mention.background.alpha)},
        {UiThemeToken::mention_bar, &theme->tokens.mention.bar,
         ui_theme_color_alpha(derived.outline, theme->tokens.mention.bar.alpha)},
        {UiThemeToken::state_tint, &theme->tokens.state.tint, derived.tint},
        {UiThemeToken::state_hover, &theme->tokens.state.hover, glass(theme->tokens.state.hover)},
        {UiThemeToken::state_pressed, &theme->tokens.state.pressed, glass(theme->tokens.state.pressed)},
        {UiThemeToken::fill_subtle, &theme->tokens.fill.subtle, glass(theme->tokens.fill.subtle)},
        {UiThemeToken::fill_muted, &theme->tokens.fill.muted, glass(theme->tokens.fill.muted)},
    };
    for (const auto& target : targets) {
        const u32 id = static_cast<u32>(target.id);
        if (resolve_token(defaults, id) && color_equal(*target.slot, defaults.values[id]))
            *target.slot = target.value;
    }
    return true;
}
ConstSlice<UiThemeMetricDescriptor> ui_theme_metric_descriptors() noexcept {
    return {metrics, static_cast<u32>(UiThemeMetric::count)};
}
#define METRIC_SLOT(name, low, high, domain)                                                                   static_assert(offsetof(UiThemeMetrics, name) == static_cast<u32>(UiThemeMetric::name) * sizeof(float));
THEME_METRICS(METRIC_SLOT)
#undef METRIC_SLOT
bool ui_theme_metric_get(const UiThemeMetrics& value, UiThemeMetric id, float* output) noexcept {
    if (!output || id >= UiThemeMetric::count)
        return false;
    *output = reinterpret_cast<const float*>(&value)[static_cast<u8>(id)];
    return true;
}
bool ui_theme_metric_set(UiThemeMetrics* output, UiThemeMetric id, float value) noexcept {
    if (!output || id >= UiThemeMetric::count)
        return false;
    const auto& descriptor = metrics[static_cast<u8>(id)];
    if (!between(value, descriptor.minimum(), descriptor.maximum()))
        return false;
    reinterpret_cast<float*>(output)[static_cast<u8>(id)] = value;
    return true;
}
bool ui_theme_typography_valid(const UiThemeTypography& value) noexcept {
    return between(value.body_size, 12, 24) && between(value.heading_1_ratio, 1, 2.5f) &&
           between(value.heading_2_ratio, 1, 2) && between(value.heading_3_ratio, 1, 1.75f) &&
           value.heading_1_ratio >= value.heading_2_ratio &&
           value.heading_2_ratio >= value.heading_3_ratio && between(value.subtext_ratio, .65f, 1) &&
           between(value.mono_ratio, .7f, 1.25f) && value.body_face < UiTextFace::count &&
           value.code_face < UiTextFace::count;
}
bool ui_theme_typography_equal(const UiThemeTypography& a, const UiThemeTypography& b) noexcept {
    return a.body_size == b.body_size && a.heading_1_ratio == b.heading_1_ratio &&
           a.heading_2_ratio == b.heading_2_ratio && a.heading_3_ratio == b.heading_3_ratio &&
           a.subtext_ratio == b.subtext_ratio && a.mono_ratio == b.mono_ratio &&
           a.body_face == b.body_face && a.code_face == b.code_face;
}
bool ui_theme_metrics_valid(const UiThemeMetrics& value) noexcept {
    for (const auto& descriptor : metrics) {
        float current{};
        if (!ui_theme_metric_get(value, descriptor.id, &current) ||
            !between(current, descriptor.minimum(), descriptor.maximum()))
            return false;
    }
    return value.audio_minimum_width <= value.audio_maximum_width &&
           value.poll_minimum_width <= value.poll_maximum_width &&
           value.menu_compact_height <= value.menu_row_height &&
           value.menu_row_height <= value.menu_detail_height &&
           value.form_single_height >= value.form_field_padding * 2 + 12;
}
bool ui_theme_metrics_equal(const UiThemeMetrics& a, const UiThemeMetrics& b) noexcept {
    for (const auto& descriptor : metrics) {
        float left{}, right{};
        if (!ui_theme_metric_get(a, descriptor.id, &left) ||
            !ui_theme_metric_get(b, descriptor.id, &right) || left != right)
            return false;
    }
    return true;
}
float ui_theme_text_size(const UiThemeTypography& typography, float em, u32 flags) noexcept {
    // The typography must already be validated: no schema scan in a glyph
    // loop. Flag priority is fixed; the picker em is constant.
    if (flags & text_style_picker_emoji)
        return text_style_picker_emoji_size(flags);
    if (flags & text_style_jumbo_emoji)
        return em * 3;
    // A folded span size (two heading bits) is a fixed rem ratio.
    if (const float ratio = text_style_size_code_ratio(flags))
        return em * ratio;
    if (flags & text_style_heading_1)
        return em * typography.heading_1_ratio;
    if (flags & text_style_heading_2)
        return em * typography.heading_2_ratio;
    if (flags & text_style_heading_3)
        return em * typography.heading_3_ratio;
    if (flags & text_style_subtext)
        return em * typography.subtext_ratio;
    if (flags & text_style_monospace)
        return em * typography.mono_ratio;
    return em;
}
UiThemeStylesResult ui_theme_styles_apply(UiResolvedThemeStyles* output,
                                          const UiThemeTypography& typography,
                                          const UiThemeMetrics& value) noexcept {
    if (!output || !ui_theme_typography_valid(typography) || !ui_theme_metrics_valid(value))
        return {UiThemeStylesError::invalid_input};
    u32 changes = ui_theme_style_none;
    if (!output->ready || !ui_theme_typography_equal(output->typography, typography))
        changes |= ui_theme_style_typography | geometry;
    for (const auto& descriptor : metrics) {
        float previous{}, next{};
        ui_theme_metric_get(output->metrics, descriptor.id, &previous);
        ui_theme_metric_get(value, descriptor.id, &next);
        if (!output->ready || previous != next)
            changes |= descriptor.changes;
    }
    if (!changes)
        return {};
    UiResolvedThemeStyles candidate = *output;
    candidate.typography = typography;
    candidate.metrics = value;
    candidate.revision = advance(candidate.revision);
    if (changes & ui_theme_style_typography)
        candidate.typography_revision = advance(candidate.typography_revision);
    if (changes & ui_theme_style_geometry)
        candidate.geometry_revision = advance(candidate.geometry_revision);
    if (changes & ui_theme_style_paint)
        candidate.paint_revision = advance(candidate.paint_revision);
    candidate.ready = true;
    copy_value(output, candidate);
    return {UiThemeStylesError::none, changes};
}
#undef THEME_COLORS
#undef THEME_METRICS
} // namespace maicord
