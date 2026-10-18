// MaiCord theme document schema.
//
// {"version": "2.x", "name": "...", "base": "dark" | "light" | "amoled",
//  "palette":    {"<name>": colour, ...}            up to 32 author colours
//  "colors":     {"<core>": colour, ...}            core palette (Theme fields)
//  "semantic":   {"<group>": {"<token>": colour}}   theme_tokens.inc tier
//  "scale":      {"space_4": 8, "radius_md": 8, ...} DIP scales
//  "motion":     {"fast": 120 | {"duration_ms": 120, "curve": "ease_out"}}
//  "components": {"icons": {"muted": colour, "weight": 1.25},
//                 "settings": {"focus_ring": colour, "modal_radius": 12, ...}}
//  "background": {...}, "typography": {...}, "metrics": {...}}
//
// Colours are [r, g, b, a] arrays in 0..1, "#rgb", "#rgba", "#rrggbb" or
// "#rrggbbaa", or references "{palette.x}", "{colors.accent}" or
// "{semantic.text.muted}", optionally "@0.5" to multiply alpha. Core colours may
// reference the palette; semantic tokens may reference any tier, forward
// included, up to kUiThemeTokenMaximumDepth with cycle rejection; component
// colours may reference any resolved tier. Any 2.x minor is read; the retired
// integer "version": 1 is rejected. Unknown keys are counted and ignored;
// missing fields inherit the base, and semantic tokens absent from the
// document re-derive from their table defaults over the final core palette.
// assets/themes/example.maicord-theme.json lists every token with defaults.
#include "maicord/app/client_theme_document.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"


namespace maicord {
namespace {
constexpr Utf8View kBases[]{utf8_literal("dark"), utf8_literal("light"), utf8_literal("amoled")};
constexpr Utf8View kEffects[]{utf8_literal("none"), utf8_literal("stars"), utf8_literal("comets"),
                              utf8_literal("stars_and_comets")};
static_assert(sizeof(kEffects) / sizeof(kEffects[0]) == kBackgroundEffectCount);
constexpr Utf8View kFaces[]{
    utf8_literal("normal"),      utf8_literal("display"),    utf8_literal("serif"),
    utf8_literal("rounded"),     utf8_literal("mono"),       utf8_literal("hand"),
    utf8_literal("techno"),      utf8_literal("bangers"),    utf8_literal("biorhyme"),
    utf8_literal("cherry_bomb"), utf8_literal("chicle"),     utf8_literal("compagnon"),
    utf8_literal("museo"),       utf8_literal("medieval"),   utf8_literal("pixelify"),
    utf8_literal("ribes"),       utf8_literal("sinistre"),   utf8_literal("zilla"),
    utf8_literal("playpen"),     utf8_literal("new_rocker"), utf8_literal("kalam")};
static_assert(sizeof(kFaces) / sizeof(kFaces[0]) == static_cast<u32>(UiTextFace::count));
constexpr Utf8View kTypographyNames[]{utf8_literal("body_size"),       utf8_literal("heading_1_ratio"),
                                      utf8_literal("heading_2_ratio"), utf8_literal("heading_3_ratio"),
                                      utf8_literal("subtext_ratio"),   utf8_literal("mono_ratio"),
                                      utf8_literal("body_face"),       utf8_literal("code_face")};
constexpr float UiThemeTypography::* kTypographyValues[]{
    &UiThemeTypography::body_size,       &UiThemeTypography::heading_1_ratio,
    &UiThemeTypography::heading_2_ratio, &UiThemeTypography::heading_3_ratio,
    &UiThemeTypography::subtext_ratio,   &UiThemeTypography::mono_ratio};
constexpr float kTypographyMinimum[]{12, 1, 1, 1, .65f, .7f};
constexpr float kTypographyMaximum[]{24, 2.5f, 2, 1.75f, 1, 1.25f};
bool name_valid(Utf8View text) noexcept {
    if (!text.data || !text.bytes || text.bytes > kClientThemeNameMaximumBytes)
        return false;
    u32 at{};
    while (at < text.bytes) {
        const auto scalar = utf8_decode_one(text.data + at, text.bytes - at);
        if (!scalar.valid || scalar.codepoint < 32 || (scalar.codepoint >= 127 && scalar.codepoint <= 159))
            return false;
        at += scalar.width;
    }
    return true;
}
bool background_valid(const BackgroundTheme& background) noexcept {
    if (background_theme_valid(background))
        return true;
    if (background.color_count || background.angle_degrees || background.intensity ||
        static_cast<u32>(background.effect) >= kBackgroundEffectCount ||
        !background_effect_settings_valid(background_effect_settings(background)))
        return false;
    for (const auto color : background.colors)
        if (color)
            return false;
    return true; // A flat surface may have either a bounded effect or none.
}
bool real_values_finite(const ClientThemeDocument& document) noexcept {
    const auto colors = ui_theme_color_descriptors();
    for (u32 i = 0; i < colors.count; ++i) {
        const auto* color = ui_theme_color_get(document.theme, colors[i].id);
        if (!color || !is_finite(color->red) || !is_finite(color->green) || !is_finite(color->blue) ||
            !is_finite(color->alpha))
            return false;
    }
    for (const auto member : kTypographyValues)
        if (!is_finite(document.typography.*member))
            return false;
    const auto metrics = ui_theme_metric_descriptors();
    for (u32 i = 0; i < metrics.count; ++i) {
        float value{};
        if (!ui_theme_metric_get(document.metrics, metrics[i].id, &value) || !is_finite(value))
            return false;
    }
    return true;
}
// The core reader already validates JSON number grammar. Bounded decimal
// conversion uses no locale or CRT. Values are checked again after float cast.
bool decimal(JsonValue value, float* output) noexcept {
    if (value.kind != JsonValueKind::number || !value.source.data || !value.source.bytes ||
        value.source.bytes > 256)
        return false;
    u32 at{};
    const bool negative = value.source.data[0] == '-';
    if (negative)
        ++at;
    double mantissa{};
    i32 fractional{};
    bool fraction{};
    while (at < value.source.bytes && value.source.data[at] != 'e' && value.source.data[at] != 'E') {
        const char c = value.source.data[at++];
        if (c == '.') {
            fraction = true;
            continue;
        }
        if (c < '0' || c > '9')
            return false;
        mantissa = mantissa * 10 + (c - '0');
        if (fraction)
            ++fractional;
    }
    i32 exponent{};
    if (at < value.source.bytes) {
        ++at;
        bool minus{};
        if (at < value.source.bytes && (value.source.data[at] == '+' || value.source.data[at] == '-'))
            minus = value.source.data[at++] == '-';
        while (at < value.source.bytes) {
            const char c = value.source.data[at++];
            if (c < '0' || c > '9')
                return false;
            exponent = minimum(4096, exponent * 10 + (c - '0'));
        }
        if (minus)
            exponent = -exponent;
    }
    exponent -= fractional;
    if (exponent > 512 && mantissa != 0)
        return false;
    if (exponent < -512)
        mantissa = 0;
    else {
        while (exponent > 0) {
            mantissa *= 10;
            --exponent;
            if (mantissa > 1e300)
                return false;
        }
        while (exponent < 0) {
            mantissa /= 10;
            ++exponent;
        }
    }
    const float result = static_cast<float>(negative ? -mantissa : mantissa);
    if (!is_finite(result))
        return false;
    *output = result;
    return true;
}
constexpr u32 kPaletteCapacity = 32;
constexpr u32 kTokenCount = static_cast<u32>(UiThemeToken::count);
constexpr Utf8View kCurves[]{utf8_literal("linear"), utf8_literal("ease_out"), utf8_literal("ease_in_out"),
                             utf8_literal("css_ease_out"), utf8_literal("spring")};
static_assert(sizeof(kCurves) / sizeof(kCurves[0]) == static_cast<u32>(UiMotionCurve::spring) + 1);
// Root keys: sections follow the three scalars. Order here is only identity;
// sections are applied in kSectionOrder so references resolve independently
// of member order in the file.
enum Section : u32 {
    colors_section,
    background_section,
    typography_section,
    metrics_section,
    palette_section,
    semantic_section,
    scale_section,
    motion_section,
    components_section,
    section_count
};
// Component colours are stored resolved (packed RGBA), so they are read after
// the semantic tier.
#define THEME_SETTINGS_COLORS(X)                                                                           \
    X(text_strong) X(text_subtle) X(nav_header) X(border_subtle) X(border_normal) X(mod_subtle)            \
    X(mod_normal) X(mod_strong) X(input_background) X(input_border) X(brand_hover) X(critical)             \
    X(critical_text) X(critical_tint) X(positive) X(focus_ring) X(scrim) X(switch_track) X(switch_border)  \
    X(slider_track) X(radio_border) X(modal_background) X(sidebar_background)
#define THEME_SETTINGS_SIZES(X)                                                                            \
    X(modal_max_width) X(modal_viewport_ratio) X(modal_ratio_threshold) X(modal_inset_top)                 \
    X(modal_inset_side) X(modal_inset_bottom) X(modal_radius) X(modal_compact_width) X(sidebar_width)      \
    X(sidebar_padding) X(sidebar_top) X(search_height) X(nav_header_height) X(nav_item_height)             \
    X(nav_item_radius) X(nav_item_gap) X(nav_item_padding) X(nav_icon) X(separator_margin)                 \
    X(panel_padding_x) X(panel_padding_top) X(panel_padding_bottom) X(panel_max_width) X(title_gap)        \
    X(category_gap) X(category_heading_gap) X(row_padding) X(label_gap) X(control_gap) X(field_gap)        \
    X(switch_width) X(switch_height) X(switch_thumb) X(slider_bar) X(slider_grabber) X(slider_mark_height) \
    X(radio_size) X(radio_gap) X(radio_spacing) X(control_height) X(control_radius) X(button_small)        \
    X(button_medium) X(button_padding) X(swatch) X(swatch_gap) X(close_size) X(close_caption_gap)          \
    X(notice_padding) X(switch_ms) X(switch_reduced_ms) X(modal_enter_ms) X(flash_ms) X(flash_fade_ms)
#define NAME(field) utf8_literal(#field),
#define COLOR_MEMBER(field) &UiThemeSettings::field,
#define SIZE_MEMBER(field) &UiThemeSettings::field,
constexpr Utf8View kSettingsColorNames[]{THEME_SETTINGS_COLORS(NAME)};
constexpr u32 UiThemeSettings::* kSettingsColors[]{THEME_SETTINGS_COLORS(COLOR_MEMBER)};
constexpr Utf8View kSettingsSizeNames[]{THEME_SETTINGS_SIZES(NAME)};
constexpr u16 UiThemeSettings::* kSettingsSizes[]{THEME_SETTINGS_SIZES(SIZE_MEMBER)};
#undef NAME
#undef COLOR_MEMBER
#undef SIZE_MEMBER
constexpr u32 kSettingsColorCount = sizeof(kSettingsColors) / sizeof(kSettingsColors[0]);
constexpr u32 kSettingsSizeCount = sizeof(kSettingsSizes) / sizeof(kSettingsSizes[0]);
static_assert(kSettingsColorCount <= 64 && kSettingsSizeCount <= 64);
// Every field is listed: only the trailing alignment padding is unnamed.
static_assert((kSettingsColorCount * sizeof(u32) + kSettingsSizeCount * sizeof(u16) + 3) / 4 * 4 ==
              sizeof(UiThemeSettings));
constexpr Utf8View kIconRoles[]{utf8_literal("normal"),  utf8_literal("muted"),   utf8_literal("accent"),
                                utf8_literal("danger"),  utf8_literal("success"), utf8_literal("warning"),
                                utf8_literal("on_accent")};
static_assert(sizeof(kIconRoles) / sizeof(kIconRoles[0]) == sizeof(UiThemeIcons::colors) / sizeof(u32));
bool settings_size_valid(u32 index, u32 value) noexcept {
    // Index 1 is the modal viewport ratio, a percentage; durations share the size bound.
    return index == 1 ? value >= 10 && value <= 100 : value <= 4096;
}
bool components_equal(const ClientThemeDocument& a, const ClientThemeDocument& b) noexcept {
    if (!bytes_equal(&a.icons, &b.icons, sizeof(a.icons)))
        return false;
    for (const auto member : kSettingsColors)
        if (a.settings.*member != b.settings.*member)
            return false;
    for (const auto member : kSettingsSizes)
        if (a.settings.*member != b.settings.*member)
            return false;
    return true;
}
bool components_valid(const UiThemeIcons& icons, const UiThemeSettings& settings) noexcept {
    if (!(icons.weight >= .25f && icons.weight <= 4))
        return false;
    for (u32 i = 0; i < kSettingsSizeCount; ++i)
        if (!settings_size_valid(i, settings.*kSettingsSizes[i]))
            return false;
    return true;
}
struct PaletteEntry {
    JsonString name;
    Color color;
};
struct Parse {
    JsonIndexedDocument document{};
    ClientThemeDocument candidate{};
    ClientThemeDocumentError error{};
    u32 offset{}, unsupported{}, root_seen{};
    JsonValue sections[section_count]{};
    PaletteEntry palette[kPaletteCapacity]{};
    u32 palette_count{};
    UiThemeTokenSource overrides[kTokenCount]{};
    u32 override_offsets[kTokenCount]{};
    bool fail(ClientThemeDocumentError next, u32 at) noexcept {
        error = next;
        offset = at;
        return false;
    }
    bool read(JsonReadResult result) noexcept {
        return result.ok() || (error != ClientThemeDocumentError::none
                                   ? false
                                   : fail(ClientThemeDocumentError::json, result.error_offset));
    }
};
bool duplicate(Parse& parse, u64& seen, u32 id, u32 offset) noexcept {
    if (id >= 64)
        return parse.fail(ClientThemeDocumentError::range, offset);
    const u64 bit = u64{1} << id;
    if (seen & bit)
        return parse.fail(ClientThemeDocumentError::duplicate_field, offset);
    seen |= bit;
    return true;
}
u32 key_index(JsonString name, const Utf8View* names, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (json_string_equals_ascii(name, names[i]))
            return i;
    return count;
}
bool enum_value(Parse& parse, JsonValue value, const Utf8View* names, u32 count, u32* output) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    const u32 id = key_index(string, names, count);
    if (id == count)
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    *output = id;
    return true;
}
bool float_value(Parse& parse, JsonValue value, float* output) noexcept {
    if (value.kind != JsonValueKind::number)
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    if (!decimal(value, output))
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    return true;
}
struct SectionVisit {
    Parse* parse{};
    u64 seen{}, extra{};
};
struct ColorArray {
    Parse* parse{};
    float values[4]{};
    u32 count{};
};
bool color_component(void* context, u32 index, JsonValue value) noexcept {
    auto& array = *static_cast<ColorArray*>(context);
    if (index >= 4)
        return array.parse->fail(ClientThemeDocumentError::range, value.source_offset);
    float number{};
    if (!float_value(*array.parse, value, &number))
        return false;
    if (number < 0 || number > 1)
        return array.parse->fail(ClientThemeDocumentError::range, value.source_offset);
    array.values[index] = number;
    ++array.count;
    return true;
}
bool hex_color(Utf8View text, Color* output) noexcept {
    u32 rgba = 0;
    if (text.bytes < 4 || text.data[0] != '#' || !color_hex_parse(text, true, &rgba))
        return false;
    *output = {static_cast<float>(rgba >> 24) / 255.f, static_cast<float>((rgba >> 16) & 255) / 255.f,
               static_cast<float>((rgba >> 8) & 255) / 255.f, static_cast<float>(rgba & 255) / 255.f};
    return true;
}
// Plain decimal in [0, 1] for the reference alpha suffix.
bool unit_decimal(Utf8View text, float* output) noexcept {
    // One correctly rounded division, so "0.12" equals the literal .12f.
    double mantissa{}, divisor = 1;
    bool fraction{}, digit{};
    if (text.bytes > 12)
        return false;
    for (u32 i = 0; i < text.bytes; ++i) {
        const char c = text.data[i];
        if (c == '.' && !fraction) {
            fraction = true;
            continue;
        }
        if (c < '0' || c > '9')
            return false;
        digit = true;
        mantissa = mantissa * 10 + (c - '0');
        if (fraction)
            divisor *= 10;
    }
    const float value = static_cast<float>(mantissa / divisor);
    if (!digit || value > 1)
        return false;
    *output = value;
    return true;
}
enum class Reference : u8 {
    palette_only,
    core,
    any
};
// Parses a colour value. Section rules: core colours reference the palette;
// semantic tokens reference any tier. The source keeps references unresolved.
bool color_value(Parse& parse, JsonValue value, Reference allowed, UiThemeTokenSource* output) noexcept {
    if (value.kind == JsonValueKind::array) {
        ColorArray array{&parse};
        if (!parse.read(json_visit_indexed_array(parse.document, value, kClientThemeDocumentMaximumDepth,
                                                 color_component, &array)))
            return false;
        if (array.count != 4)
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        *output = {{array.values[0], array.values[1], array.values[2], array.values[3]},
                   1, 0, UiThemeTokenSourceKind::literal};
        return true;
    }
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    char buffer[96];
    const auto decoded = json_string_decode_utf8(string, {buffer, sizeof(buffer)});
    if (!decoded.ok() || !decoded.bytes)
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    const Utf8View text{buffer, decoded.bytes};
    if (buffer[0] == '#') {
        Color color{};
        if (!hex_color(text, &color))
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        *output = {color, 1, 0, UiThemeTokenSourceKind::literal};
        return true;
    }
    u32 close{};
    while (close < text.bytes && buffer[close] != '}')
        ++close;
    float alpha = 1;
    if (buffer[0] != '{' || close == text.bytes ||
        (close + 1 < text.bytes &&
         (buffer[close + 1] != '@' || !unit_decimal({buffer + close + 2, text.bytes - close - 2}, &alpha))))
        return parse.fail(ClientThemeDocumentError::reference, value.source_offset);
    // Split "tier.first[.second]".
    Utf8View parts[3]{};
    u32 count{}, start = 1;
    for (u32 i = 1; i <= close; ++i)
        if (i == close || buffer[i] == '.') {
            if (count == 3 || i == start)
                return parse.fail(ClientThemeDocumentError::reference, value.source_offset);
            parts[count++] = {buffer + start, i - start};
            start = i + 1;
        }
    if (count == 2 && utf8_equal(parts[0], utf8_literal("palette"))) {
        for (u32 i = 0; i < parse.palette_count; ++i)
            if (json_string_equals_ascii(parse.palette[i].name, parts[1])) {
                *output = {ui_theme_color_alpha(parse.palette[i].color, alpha), 1, 0,
                           UiThemeTokenSourceKind::literal};
                return true;
            }
    } else if (count == 2 && allowed != Reference::palette_only && utf8_equal(parts[0], utf8_literal("colors"))) {
        const auto colors = ui_theme_color_descriptors();
        for (u32 i = 0; i < colors.count; ++i)
            if (utf8_equal(colors[i].name, parts[1])) {
                *output = {{}, alpha, static_cast<u8>(colors[i].id), UiThemeTokenSourceKind::core};
                return true;
            }
    } else if (count == 3 && allowed == Reference::any && utf8_equal(parts[0], utf8_literal("semantic"))) {
        const auto tokens = ui_theme_token_descriptors();
        for (u32 i = 0; i < tokens.count; ++i)
            if (utf8_equal(tokens[i].group, parts[1]) && utf8_equal(tokens[i].name, parts[2])) {
                *output = {{}, alpha, static_cast<u8>(tokens[i].id), UiThemeTokenSourceKind::token};
                return true;
            }
    }
    return parse.fail(ClientThemeDocumentError::reference, value.source_offset);
}
bool color_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    const auto tokens = ui_theme_color_descriptors();
    for (u32 i = 0; i < tokens.count; ++i) {
        const auto& token = tokens[i];
        if (!json_string_equals_ascii(name, token.name))
            continue;
        if (!duplicate(parse, section.seen, static_cast<u32>(token.id), name.source_offset))
            return false;
        UiThemeTokenSource source{};
        if (!color_value(parse, value, Reference::palette_only, &source))
            return false;
        return ui_theme_color_set(&parse.candidate.theme, token.id, source.literal);
    }
    ++parse.unsupported;
    return true;
}
bool palette_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<SectionVisit*>(context)->parse;
    // Escaped names are rejected, so encoded bytes are the identity.
    if (parse.palette_count == kPaletteCapacity || name.has_escapes || !name.encoded.bytes ||
        name.encoded.bytes > 64)
        return parse.fail(ClientThemeDocumentError::range, name.source_offset);
    for (u32 i = 0; i < parse.palette_count; ++i)
        if (utf8_equal(name.encoded, parse.palette[i].name.encoded))
            return parse.fail(ClientThemeDocumentError::duplicate_field, name.source_offset);
    UiThemeTokenSource source{};
    if (!color_value(parse, value, Reference::palette_only, &source))
        return false;
    parse.palette[parse.palette_count++] = {name, source.literal};
    return true;
}
struct GroupVisit {
    Parse* parse;
    Utf8View group;
};
bool semantic_token_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& visit = *static_cast<GroupVisit*>(context);
    auto& parse = *visit.parse;
    const auto tokens = ui_theme_token_descriptors();
    for (u32 i = 0; i < tokens.count; ++i) {
        if (!utf8_equal(tokens[i].group, visit.group) || !json_string_equals_ascii(name, tokens[i].name))
            continue;
        if (parse.overrides[i].kind != UiThemeTokenSourceKind::inherit)
            return parse.fail(ClientThemeDocumentError::duplicate_field, name.source_offset);
        parse.override_offsets[i] = value.source_offset;
        return color_value(parse, value, Reference::any, &parse.overrides[i]);
    }
    ++parse.unsupported;
    return true;
}
bool semantic_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    const auto tokens = ui_theme_token_descriptors();
    for (u32 i = 0, group = 0; i < tokens.count; ++i) {
        if (i && utf8_equal(tokens[i].group, tokens[i - 1].group))
            continue;
        if (json_string_equals_ascii(name, tokens[i].group)) {
            if (!duplicate(parse, section.seen, group, name.source_offset))
                return false;
            if (value.kind != JsonValueKind::object)
                return parse.fail(ClientThemeDocumentError::type, value.source_offset);
            GroupVisit visit{&parse, tokens[i].group};
            return parse.read(json_visit_indexed_object(parse.document, value, kClientThemeDocumentMaximumDepth,
                                                        semantic_token_member, &visit));
        }
        ++group;
    }
    ++parse.unsupported;
    return true;
}
bool scale_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    const auto scales = ui_theme_scale_descriptors();
    for (u32 i = 0; i < scales.count; ++i) {
        if (!json_string_equals_ascii(name, scales[i].name))
            continue;
        if (!duplicate(parse, section.seen, i, name.source_offset))
            return false;
        float number{};
        if (!float_value(parse, value, &number))
            return false;
        if (!ui_theme_scale_set(&parse.candidate.theme.scale, scales[i].id, number))
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        return true;
    }
    ++parse.unsupported;
    return true;
}
struct MotionVisit {
    Parse* parse;
    UiMotionStyle* style;
    u64 seen;
};
bool motion_field(void* context, JsonString name, JsonValue value) noexcept {
    auto& visit = *static_cast<MotionVisit*>(context);
    auto& parse = *visit.parse;
    constexpr Utf8View keys[]{utf8_literal("duration_ms"), utf8_literal("curve")};
    const u32 id = key_index(name, keys, 2);
    if (id == 2) {
        ++parse.unsupported;
        return true;
    }
    if (!duplicate(parse, visit.seen, id, name.source_offset))
        return false;
    if (id == 1) {
        u32 curve{};
        if (!enum_value(parse, value, kCurves, sizeof(kCurves) / sizeof(kCurves[0]), &curve))
            return false;
        visit.style->curve = static_cast<UiMotionCurve>(curve);
        return true;
    }
    u64 duration{};
    if (!json_value_to_u64(value, &duration))
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    if (duration > kUiMotionMaximumDurationMs)
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    visit.style->duration_ms = static_cast<u32>(duration);
    return true;
}
bool motion_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    const auto motions = ui_theme_motion_descriptors();
    for (u32 i = 0; i < motions.count; ++i) {
        if (!json_string_equals_ascii(name, motions[i].name))
            continue;
        if (!duplicate(parse, section.seen, i, name.source_offset))
            return false;
        auto* style = ui_theme_motion_slot(&parse.candidate.theme.motion, motions[i].id);
        if (value.kind == JsonValueKind::number) {
            MotionVisit visit{&parse, style, 0};
            return motion_field(&visit, {utf8_literal("duration_ms"), name.source_offset, false}, value);
        }
        if (value.kind != JsonValueKind::object)
            return parse.fail(ClientThemeDocumentError::type, value.source_offset);
        MotionVisit visit{&parse, style, 0};
        return parse.read(json_visit_indexed_object(parse.document, value, kClientThemeDocumentMaximumDepth,
                                                    motion_field, &visit));
    }
    ++parse.unsupported;
    return true;
}
bool metric_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    const auto metrics = ui_theme_metric_descriptors();
    for (u32 i = 0; i < metrics.count; ++i) {
        const auto& metric = metrics[i];
        if (!json_string_equals_ascii(name, metric.name))
            continue;
        if (!duplicate(parse, section.seen, static_cast<u32>(metric.id), name.source_offset))
            return false;
        float number{};
        if (!float_value(parse, value, &number))
            return false;
        if (number < metric.minimum || number > metric.maximum ||
            !ui_theme_metric_set(&parse.candidate.metrics, metric.id, number))
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        return true;
    }
    ++parse.unsupported;
    return true;
}
bool typography_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    const u32 id = key_index(name, kTypographyNames, 8);
    if (id == 8) {
        ++parse.unsupported;
        return true;
    }
    if (!duplicate(parse, section.seen, id, name.source_offset))
        return false;
    if (id < 6) {
        float number{};
        if (!float_value(parse, value, &number))
            return false;
        if (number < kTypographyMinimum[id] || number > kTypographyMaximum[id])
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        parse.candidate.typography.*kTypographyValues[id] = number;
        return true;
    }
    u32 face{};
    if (!enum_value(parse, value, kFaces, static_cast<u32>(UiTextFace::count), &face))
        return false;
    (id == 6 ? parse.candidate.typography.body_face : parse.candidate.typography.code_face) =
        static_cast<UiTextFace>(face);
    return true;
}
struct PaletteArray {
    Parse* parse{};
    u32 colors[5]{}, count{};
};
bool palette_color(void* context, u32 index, JsonValue value) noexcept {
    auto& array = *static_cast<PaletteArray*>(context);
    u64 number{};
    if (index >= 5)
        return array.parse->fail(ClientThemeDocumentError::range, value.source_offset);
    if (!json_value_to_u64(value, &number))
        return array.parse->fail(ClientThemeDocumentError::type, value.source_offset);
    if (number > 0xffffff)
        return array.parse->fail(ClientThemeDocumentError::range, value.source_offset);
    array.colors[index] = static_cast<u32>(number);
    ++array.count;
    return true;
}
bool background_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    constexpr Utf8View keys[]{utf8_literal("colors"),    utf8_literal("angle_degrees"),
                              utf8_literal("intensity"), utf8_literal("effect"),
                              utf8_literal("speed"),     utf8_literal("strength"),
                              utf8_literal("density")};
    const u32 id = key_index(name, keys, 7);
    if (id == 7) {
        ++parse.unsupported;
        return true;
    }
    if (!duplicate(parse, section.seen, id, name.source_offset))
        return false;
    auto& background = parse.candidate.theme.background;
    if (!id) {
        if (value.kind != JsonValueKind::array)
            return parse.fail(ClientThemeDocumentError::type, value.source_offset);
        PaletteArray array{&parse};
        if (!parse.read(json_visit_indexed_array(parse.document, value, kClientThemeDocumentMaximumDepth,
                                                 palette_color, &array)))
            return false;
        background.color_count = static_cast<u8>(array.count);
        for (u32 i = 0; i < 5; ++i)
            background.colors[i] = array.colors[i];
        return true;
    }
    if (id == 3) {
        JsonString effect_name{};
        if (!json_value_as_string(value, &effect_name))
            return parse.fail(ClientThemeDocumentError::type, value.source_offset);
        // A name outside the catalog (a removed effect) means no effect.
        const u32 effect = key_index(effect_name, kEffects, kBackgroundEffectCount);
        if (effect == kBackgroundEffectCount)
            ++parse.unsupported;
        background.effect =
            effect == kBackgroundEffectCount ? BackgroundEffect::none : static_cast<BackgroundEffect>(effect);
        return true;
    }
    u64 number{};
    if (!json_value_to_u64(value, &number))
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    if (number > (id == 1 ? 360u : 100u))
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    switch (id) {
    case 1:
        background.angle_degrees = static_cast<u16>(number);
        break;
    case 2:
        background.intensity = static_cast<u8>(number);
        break;
    case 4:
        background.speed = static_cast<u8>(number);
        break;
    case 5:
        background.strength = static_cast<u8>(number);
        break;
    case 6:
        background.density = static_cast<u8>(number);
        break;
    default:
        return false;
    }
    return true;
}
u32 pack(Color color) noexcept {
    const auto channel = [](float value) noexcept { return static_cast<u32>(value * 255 + .5f) & 255; };
    const u32 packed = channel(color.red) << 24 | channel(color.green) << 16 | channel(color.blue) << 8 |
                       channel(color.alpha);
    return packed ? packed : 1; // zero means "derive"; keep an authored transparent black distinct
}
// Component values resolve against the finished semantic tier.
bool resolved_color(Parse& parse, JsonValue value, u32* output) noexcept {
    UiThemeTokenSource source{};
    if (!color_value(parse, value, Reference::any, &source))
        return false;
    const Color* reference = source.kind == UiThemeTokenSourceKind::core
                                 ? ui_theme_color_get(parse.candidate.theme, static_cast<UiThemeColor>(source.target))
                             : source.kind == UiThemeTokenSourceKind::token
                                 ? ui_theme_token_get(parse.candidate.theme, static_cast<UiThemeToken>(source.target))
                                 : nullptr;
    *output = pack(reference ? ui_theme_color_alpha(*reference, source.alpha) : source.literal);
    return true;
}
bool icon_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    constexpr u32 count = sizeof(kIconRoles) / sizeof(kIconRoles[0]);
    const u32 id = json_string_equals_ascii(name, utf8_literal("weight")) ? count : key_index(name, kIconRoles, count);
    if (id == count && !json_string_equals_ascii(name, utf8_literal("weight"))) {
        ++parse.unsupported;
        return true;
    }
    if (!duplicate(parse, section.seen, id, name.source_offset))
        return false;
    if (id < count)
        return resolved_color(parse, value, &parse.candidate.icons.colors[id]);
    float weight{};
    if (!float_value(parse, value, &weight))
        return false;
    if (!(weight >= .25f && weight <= 4))
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    parse.candidate.icons.weight = weight;
    return true;
}
bool settings_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    const u32 color = key_index(name, kSettingsColorNames, kSettingsColorCount);
    if (color < kSettingsColorCount)
        return duplicate(parse, section.seen, color, name.source_offset) &&
               resolved_color(parse, value, &(parse.candidate.settings.*kSettingsColors[color]));
    const u32 size = key_index(name, kSettingsSizeNames, kSettingsSizeCount);
    if (size == kSettingsSizeCount) {
        ++parse.unsupported;
        return true;
    }
    if (section.extra & (u64{1} << size))
        return parse.fail(ClientThemeDocumentError::duplicate_field, name.source_offset);
    section.extra |= u64{1} << size;
    u64 number{};
    if (!json_value_to_u64(value, &number))
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    if (number > 0xffff || !settings_size_valid(size, static_cast<u32>(number)))
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    parse.candidate.settings.*kSettingsSizes[size] = static_cast<u16>(number);
    return true;
}
bool components_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    constexpr Utf8View keys[]{utf8_literal("icons"), utf8_literal("settings")};
    const u32 id = key_index(name, keys, 2);
    if (id == 2) {
        ++parse.unsupported;
        return true;
    }
    if (!duplicate(parse, section.seen, id, name.source_offset))
        return false;
    if (value.kind != JsonValueKind::object)
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    SectionVisit nested{&parse};
    return parse.read(json_visit_indexed_object(parse.document, value, kClientThemeDocumentMaximumDepth,
                                                id ? settings_member : icon_member, &nested));
}
bool root_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(context);
    constexpr Utf8View keys[]{utf8_literal("version"),    utf8_literal("name"),
                              utf8_literal("base"),       utf8_literal("colors"),
                              utf8_literal("background"), utf8_literal("typography"),
                              utf8_literal("metrics"),    utf8_literal("palette"),
                              utf8_literal("semantic"),   utf8_literal("scale"),
                              utf8_literal("motion"),     utf8_literal("components")};
    constexpr u32 count = sizeof(keys) / sizeof(keys[0]);
    static_assert(count == 3 + section_count);
    const u32 id = key_index(name, keys, count);
    if (id == count) {
        ++parse.unsupported;
        return true;
    }
    if (parse.root_seen & (1u << id))
        return parse.fail(ClientThemeDocumentError::duplicate_field, name.source_offset);
    parse.root_seen |= 1u << id;
    if (!id) {
        u64 version{};
        JsonString string{};
        // Integer versions belong to the retired first schema.
        if (json_value_to_u64(value, &version))
            return parse.fail(ClientThemeDocumentError::unsupported_version, value.source_offset);
        if (!json_value_as_string(value, &string))
            return parse.fail(ClientThemeDocumentError::type, value.source_offset);
        // "2" or "2.<minor>": any minor of the second schema is readable.
        char text[16];
        const auto decoded = json_string_decode_utf8(string, {text, sizeof(text)});
        bool supported = decoded.ok() && decoded.bytes && text[0] == '2' &&
                         (decoded.bytes == 1 || (decoded.bytes > 2 && text[1] == '.'));
        for (u32 i = 2; supported && i < decoded.bytes; ++i)
            supported = text[i] >= '0' && text[i] <= '9';
        if (!supported)
            return parse.fail(ClientThemeDocumentError::unsupported_version, value.source_offset);
        return true;
    }
    if (id == 1) {
        JsonString string{};
        if (!json_value_as_string(value, &string))
            return parse.fail(ClientThemeDocumentError::type, value.source_offset);
        const auto decoded =
            json_string_decode_utf8(string, {parse.candidate.name, kClientThemeNameMaximumBytes});
        if (!decoded.ok() || !name_valid({parse.candidate.name, decoded.bytes}))
            return parse.fail(ClientThemeDocumentError::text, value.source_offset);
        parse.candidate.name_bytes = decoded.bytes;
        parse.candidate.name[decoded.bytes] = 0;
        return true;
    }
    if (id == 2) {
        u32 base{};
        if (!enum_value(parse, value, kBases, 3, &base))
            return false;
        parse.candidate.base = static_cast<ClientThemeBase>(base);
        return true;
    }
    if (value.kind != JsonValueKind::object)
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    parse.sections[id - 3] = value;
    static_assert(colors_section == 0 && components_section == 8);
    return true;
}
struct Writer {
    char* data{};
    u32 used{};
    void character(char c) noexcept {
        if (data)
            data[used] = c;
        ++used;
    }
    void text(Utf8View text) noexcept {
        for (u32 i = 0; i < text.bytes; ++i)
            character(text.data[i]);
    }
    void quoted(Utf8View text) noexcept {
        character('"');
        for (u32 i = 0; i < text.bytes; ++i) {
            if (text.data[i] == '"' || text.data[i] == '\\')
                character('\\');
            character(text.data[i]);
        }
        character('"');
    }
    void hex(u32 rgba) noexcept {
        constexpr char digits[] = "0123456789abcdef";
        character('"');
        character('#');
        for (u32 shift = 32; shift;) {
            shift -= 4;
            character(digits[(rgba >> shift) & 15]);
        }
        character('"');
    }
    void number(u64 value) noexcept {
        char digits[20];
        text({digits, decimal_write(digits, value)});
    }
    // Exact finite IEEE float -> decimal rational, including subnormals. Base10
    // integer arithmetic avoids locale, lossy fixed precision and CRT formatters.
    void real(float value) noexcept {
        u32 bits{};
        copy_bytes(&bits, &value, sizeof(bits));
        if (bits >> 31)
            character('-');
        const u32 exponent = (bits >> 23) & 255;
        u32 mantissa = bits & 0x7fffff;
        if (exponent)
            mantissa |= 0x800000;
        if (!mantissa) {
            character('0');
            return;
        }
        const i32 power = exponent ? static_cast<i32>(exponent) - 150 : -149;
        u8 digits[192]{};
        u32 count{};
        do {
            digits[count++] = static_cast<u8>(mantissa % 10);
            mantissa /= 10;
        } while (mantissa);
        const u32 shifts = static_cast<u32>(power < 0 ? -power : power), multiplier = power < 0 ? 5 : 2;
        for (u32 shift = 0; shift < shifts; ++shift) {
            u32 carry{};
            for (u32 i = 0; i < count; ++i) {
                const u32 next = digits[i] * multiplier + carry;
                digits[i] = static_cast<u8>(next % 10);
                carry = next / 10;
            }
            while (carry) {
                digits[count++] = static_cast<u8>(carry % 10);
                carry /= 10;
            }
        }
        u32 places = power < 0 ? shifts : 0, first{};
        while (places && first < count && !digits[first]) {
            ++first;
            --places;
        }
        const u32 remaining = count - first;
        if (!places) {
            for (u32 i = count; i > first; --i)
                character(static_cast<char>('0' + digits[i - 1]));
            return;
        }
        if (remaining <= places) {
            text(utf8_literal("0."));
            for (u32 i = remaining; i < places; ++i)
                character('0');
        }
        for (u32 i = count; i > first; --i) {
            if (remaining > places && i - first == places)
                character('.');
            character(static_cast<char>('0' + digits[i - 1]));
        }
    }
};
void write_color(Writer& writer, const Color& color) noexcept {
    writer.character('[');
    writer.real(color.red);
    writer.character(',');
    writer.real(color.green);
    writer.character(',');
    writer.real(color.blue);
    writer.character(',');
    writer.real(color.alpha);
    writer.character(']');
}
void write_document(Writer& writer, const ClientThemeDocument& document) noexcept {
    writer.text(utf8_literal("{\"version\":\"2.0\",\"name\":"));
    writer.quoted({document.name, document.name_bytes});
    writer.text(utf8_literal(",\"base\":"));
    writer.quoted(kBases[static_cast<u32>(document.base)]);
    writer.text(utf8_literal(",\"colors\":{"));
    bool comma{};
    const auto tokens = ui_theme_color_descriptors();
    for (u32 i = 0; i < tokens.count; ++i) {
        const auto& token = tokens[i];
        if (comma)
            writer.character(',');
        comma = true;
        writer.quoted(token.name);
        writer.character(':');
        write_color(writer, *ui_theme_color_get(document.theme, token.id));
    }
    // Canonical semantic values are resolved literals: a stored document never
    // depends on table defaults that a later build might change.
    writer.text(utf8_literal("},\"semantic\":{"));
    const auto semantic = ui_theme_token_descriptors();
    for (u32 i = 0; i < semantic.count; ++i) {
        const bool first = !i || !utf8_equal(semantic[i].group, semantic[i - 1].group);
        if (first) {
            if (i)
                writer.text(utf8_literal("},"));
            writer.quoted(semantic[i].group);
            writer.text(utf8_literal(":{"));
        } else
            writer.character(',');
        writer.quoted(semantic[i].name);
        writer.character(':');
        write_color(writer, *ui_theme_token_get(document.theme, semantic[i].id));
    }
    writer.text(utf8_literal("}},\"scale\":{"));
    const auto scales = ui_theme_scale_descriptors();
    for (u32 i = 0; i < scales.count; ++i) {
        if (i)
            writer.character(',');
        float value{};
        ui_theme_scale_get(document.theme.scale, scales[i].id, &value);
        writer.quoted(scales[i].name);
        writer.character(':');
        writer.real(value);
    }
    writer.text(utf8_literal("},\"motion\":{"));
    const auto motions = ui_theme_motion_descriptors();
    auto motion = document.theme.motion;
    for (u32 i = 0; i < motions.count; ++i) {
        if (i)
            writer.character(',');
        const auto* style = ui_theme_motion_slot(&motion, motions[i].id);
        writer.quoted(motions[i].name);
        writer.text(utf8_literal(":{\"duration_ms\":"));
        writer.number(style->duration_ms);
        writer.text(utf8_literal(",\"curve\":"));
        writer.quoted(kCurves[static_cast<u32>(style->curve)]);
        writer.character('}');
    }
    writer.text(utf8_literal("},\"background\":{\"colors\":["));
    const auto& background = document.theme.background;
    for (u32 i = 0; i < background.color_count; ++i) {
        if (i)
            writer.character(',');
        writer.number(background.colors[i]);
    }
    writer.text(utf8_literal("],\"angle_degrees\":"));
    writer.number(background.angle_degrees);
    writer.text(utf8_literal(",\"intensity\":"));
    writer.number(background.intensity);
    writer.text(utf8_literal(",\"effect\":"));
    writer.quoted(kEffects[static_cast<u32>(background.effect)]);
    writer.text(utf8_literal(",\"speed\":"));
    writer.number(background.speed);
    writer.text(utf8_literal(",\"strength\":"));
    writer.number(background.strength);
    writer.text(utf8_literal(",\"density\":"));
    writer.number(background.density);
    writer.text(utf8_literal("},\"typography\":{"));
    for (u32 i = 0; i < 8; ++i) {
        if (i)
            writer.character(',');
        writer.quoted(kTypographyNames[i]);
        writer.character(':');
        if (i < 6)
            writer.real(document.typography.*kTypographyValues[i]);
        else
            writer.quoted(kFaces[static_cast<u32>(i == 6 ? document.typography.body_face
                                                         : document.typography.code_face)]);
    }
    writer.text(utf8_literal("},\"metrics\":{"));
    comma = false;
    const auto metrics = ui_theme_metric_descriptors();
    for (u32 i = 0; i < metrics.count; ++i) {
        const auto& metric = metrics[i];
        if (comma)
            writer.character(',');
        comma = true;
        float value{};
        ui_theme_metric_get(document.metrics, metric.id, &value);
        writer.quoted(metric.name);
        writer.character(':');
        writer.real(value);
    }
    // Components carry only authored (non-default) values.
    writer.text(utf8_literal("},\"components\":{\"icons\":{"));
    comma = false;
    for (u32 i = 0; i < sizeof(kIconRoles) / sizeof(kIconRoles[0]); ++i)
        if (document.icons.colors[i]) {
            if (comma)
                writer.character(',');
            comma = true;
            writer.quoted(kIconRoles[i]);
            writer.character(':');
            writer.hex(document.icons.colors[i]);
        }
    if (document.icons.weight != UiThemeIcons{}.weight) {
        if (comma)
            writer.character(',');
        writer.text(utf8_literal("\"weight\":"));
        writer.real(document.icons.weight);
    }
    writer.text(utf8_literal("},\"settings\":{"));
    comma = false;
    constexpr UiThemeSettings settings_defaults{};
    for (u32 i = 0; i < kSettingsColorCount; ++i)
        if (const u32 packed = document.settings.*kSettingsColors[i]) {
            if (comma)
                writer.character(',');
            comma = true;
            writer.quoted(kSettingsColorNames[i]);
            writer.character(':');
            writer.hex(packed);
        }
    for (u32 i = 0; i < kSettingsSizeCount; ++i)
        if (document.settings.*kSettingsSizes[i] != settings_defaults.*kSettingsSizes[i]) {
            if (comma)
                writer.character(',');
            comma = true;
            writer.quoted(kSettingsSizeNames[i]);
            writer.character(':');
            writer.number(document.settings.*kSettingsSizes[i]);
        }
    writer.text(utf8_literal("}}}"));
}
} // namespace
ClientThemeDocument client_theme_document_defaults(ClientThemeBase base) noexcept {
    ClientThemeDocument document{};
    document.base =
        static_cast<u32>(base) <= static_cast<u32>(ClientThemeBase::amoled) ? base : ClientThemeBase::dark;
    document.theme = document.base == ClientThemeBase::light    ? kLightTheme
                     : document.base == ClientThemeBase::amoled ? kAmoledTheme
                                                                : kDefaultTheme;
    document.theme.styles = nullptr;
    document.theme.style_revision = 0;
    document.theme.appearance =
        document.base == ClientThemeBase::light ? ThemeAppearance::light : ThemeAppearance::dark;
    constexpr auto name = utf8_literal("Custom theme");
    copy_bytes(document.name, name.data, name.bytes);
    document.name_bytes = name.bytes;
    return document;
}
bool client_theme_document_valid(const ClientThemeDocument& document) noexcept {
    return static_cast<u32>(document.base) <= static_cast<u32>(ClientThemeBase::amoled) &&
           document.theme.appearance ==
               (document.base == ClientThemeBase::light ? ThemeAppearance::light : ThemeAppearance::dark) &&
           document.name_bytes <= kClientThemeNameMaximumBytes && document.name[document.name_bytes] == 0 &&
           name_valid({document.name, document.name_bytes}) && ui_theme_palette_valid(document.theme) &&
           real_values_finite(document) && background_valid(document.theme.background) &&
           ui_theme_typography_valid(document.typography) && ui_theme_metrics_valid(document.metrics) &&
           components_valid(document.icons, document.settings);
}
bool client_theme_document_equal(const ClientThemeDocument& a, const ClientThemeDocument& b) noexcept {
    return a.base == b.base && a.name_bytes == b.name_bytes &&
           a.name_bytes <= kClientThemeNameMaximumBytes && bytes_equal(a.name, b.name, a.name_bytes) &&
           ui_theme_palette_equal(a.theme, b.theme) &&
           background_theme_equal(a.theme.background, b.theme.background) &&
           ui_theme_typography_equal(a.typography, b.typography) &&
           ui_theme_metrics_equal(a.metrics, b.metrics) && components_equal(a, b);
}
ClientThemeDocumentResult client_theme_document_parse(Utf8View input, const ClientThemeDocument& defaults,
                                                      Slice<u32> scratch,
                                                      ClientThemeDocument* output) noexcept {
    if (!output || !input.bytes || input.bytes > kClientThemeDocumentMaximumBytes || !scratch.data ||
        !scratch.count || scratch.count > kClientThemeDocumentMaximumBytes ||
        !client_theme_document_valid(defaults))
        return {ClientThemeDocumentError::invalid_input};
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(input.data), input.bytes},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))},
        {reinterpret_cast<const u8*>(output), sizeof(*output)},
        {reinterpret_cast<const u8*>(&defaults), sizeof(defaults)}};
    for (const auto range : ranges)
        if (!byte_range_valid(range))
            return {ClientThemeDocumentError::invalid_input};
    if (byte_ranges_overlap(ranges[0], ranges[1]) || byte_ranges_overlap(ranges[0], ranges[2]) ||
        byte_ranges_overlap(ranges[1], ranges[2]) || byte_ranges_overlap(ranges[1], ranges[3]))
        return {ClientThemeDocumentError::invalid_input};
    const auto scanned = json_scan_structurals_avx1(input, scratch);
    if (!scanned.ok())
        return {scanned.error == JsonScanError::output_overflow ? ClientThemeDocumentError::capacity
                                                                : ClientThemeDocumentError::json,
                scanned.error_offset};
    Parse parse{{input, {scratch.data, scanned.count}}, defaults};
    const auto read = json_visit_root_object(input, parse.document.structurals,
                                             kClientThemeDocumentMaximumDepth, root_member, &parse);
    if (!read.ok())
        return {parse.error == ClientThemeDocumentError::none ? ClientThemeDocumentError::json
                                                              : parse.error,
                parse.error == ClientThemeDocumentError::none ? read.error_offset : parse.offset,
                parse.unsupported};
    if (!(parse.root_seen & 1))
        return {ClientThemeDocumentError::missing_version, 0, parse.unsupported};
    if (parse.candidate.base != defaults.base) {
        auto next = client_theme_document_defaults(parse.candidate.base);
        if (parse.root_seen & 2) {
            next.name_bytes = parse.candidate.name_bytes;
            copy_bytes(next.name, parse.candidate.name, next.name_bytes + 1);
        }
        parse.candidate = next;
    }
    parse.candidate.theme.styles = nullptr;
    parse.candidate.theme.style_revision = 0;
    parse.candidate.theme.appearance =
        parse.candidate.base == ClientThemeBase::light ? ThemeAppearance::light : ThemeAppearance::dark;
    constexpr JsonMemberVisitor visitors[]{color_member,   background_member, typography_member,
                                           metric_member,  palette_member,    semantic_member,
                                           scale_member,   motion_member,     components_member};
    constexpr u32 kSectionOrder[]{palette_section,    colors_section,  semantic_section,
                                  background_section, typography_section, metrics_section,
                                  scale_section,      motion_section,  components_section};
    static_assert(sizeof(kSectionOrder) / sizeof(kSectionOrder[0]) == section_count);
    for (const u32 i : kSectionOrder) {
        if (i == components_section) {
            const auto resolved = ui_theme_tokens_resolve(&parse.candidate.theme, parse.overrides);
            if (!resolved.ok())
                return {resolved.error == UiThemeTokenError::invalid_input ? ClientThemeDocumentError::range
                                                                           : ClientThemeDocumentError::cycle,
                        resolved.token < UiThemeToken::count
                            ? parse.override_offsets[static_cast<u32>(resolved.token)]
                            : 0,
                        parse.unsupported};
        }
        if (!parse.sections[i].source.bytes)
            continue;
        SectionVisit section{&parse};
        if (!parse.read(json_visit_indexed_object(parse.document, parse.sections[i],
                                                  kClientThemeDocumentMaximumDepth, visitors[i], &section)))
            return {parse.error, parse.offset, parse.unsupported};
    }
    if (!client_theme_document_valid(parse.candidate))
        return {ClientThemeDocumentError::range, 0, parse.unsupported};
    *output = parse.candidate;
    return {ClientThemeDocumentError::none, 0, parse.unsupported};
}
ClientThemeDocumentResult client_theme_document_write(const ClientThemeDocument& document,
                                                      Slice<char> output, u32* bytes) noexcept {
    if (!bytes || output.count > kClientThemeDocumentMaximumBytes || (!output.data && output.count) ||
        !client_theme_document_valid(document))
        return {ClientThemeDocumentError::invalid_input};
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(&document), sizeof(document)},
                                  {reinterpret_cast<const u8*>(output.data), output.count},
                                  {reinterpret_cast<const u8*>(bytes), sizeof(*bytes)}};
    for (u32 i = 0; i < 3; ++i) {
        if (!byte_range_valid(ranges[i]))
            return {ClientThemeDocumentError::invalid_input};
        for (u32 j = 0; j < i; ++j)
            if (byte_ranges_overlap(ranges[i], ranges[j]))
                return {ClientThemeDocumentError::invalid_input};
    }
    Writer size{};
    write_document(size, document);
    if (size.used > kClientThemeDocumentMaximumBytes)
        return {ClientThemeDocumentError::capacity};
    if (!output.data && !output.count) {
        *bytes = size.used;
        return {};
    }
    if (size.used > output.count)
        return {ClientThemeDocumentError::capacity};
    Writer actual{output.data};
    write_document(actual, document);
    *bytes = actual.used;
    return {};
}
} // namespace maicord
