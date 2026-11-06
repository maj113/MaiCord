// MaiCord theme document schema.
//
// {"version": "2.x", "name": "...", "base": "dark" | "light" | "amoled",
//  "palette":    {"<name>": colour, ...}            up to 32 author colours
//  "colors":     {"<core>": colour, ...}            core palette (Theme fields)
//  "semantic":   {"<group>": {"<token>": colour}}   ui/theme/tokens.inc tier
//  "scale":      {"space_4": 8, "radius_md": 8, ...} DIP scales
//  "motion":     {"fast": 120 | {"duration_ms": 120, "curve": "ease_out"}}
//  "components": {"icons": {"muted": colour, "weight": 1.25},
//                 "settings": {"focus_ring": colour, "modal_radius": 12, ...}}
//  "background": {...}, "typography": {...}, "metrics": {...},   background may name a package effect, its knobs,
//                its universal settings and the knobs a theme with variants follows ("follow"; ClientThemeEffect)
//  "variants":   {"night"|"dawn"|"day"|"dusk": {"base": ..., "colors": {...}, "months": [12 x {...}]}}}
//                core colours through the day; see ClientThemeVariants
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
#include "maicord/app/settings/client_theme_document.hpp"
#include "maicord/render/effects/background_effect.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include <stddef.h>
#include "maicord/core/utf.hpp"


namespace maicord {
namespace {
constexpr char kBases[] = "dark\0light\0amoled\0";
constexpr char kEffects[] = "none\0stars\0comets\0stars_and_comets\0";
static_assert(name_list_count(kEffects) == kBackgroundEffectCount);
constexpr char kFaces[] =
    "normal\0display\0serif\0rounded\0mono\0hand\0techno\0bangers\0biorhyme\0cherry_bomb\0chicle\0compagnon\0"
    "museo\0medieval\0pixelify\0ribes\0sinistre\0zilla\0playpen\0new_rocker\0kalam\0";
static_assert(name_list_count(kFaces) == static_cast<u32>(UiTextFace::count));
constexpr char kTypographyNames[] = "body_size\0heading_1_ratio\0heading_2_ratio\0heading_3_ratio\0"
                                    "subtext_ratio\0mono_ratio\0body_face\0code_face\0";
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
bool effect_slug_valid(const char* text, u32 bytes) noexcept {
    if (!bytes || bytes > kClientThemeEffectSlugBytes)
        return false;
    for (u32 i = 0; i < bytes; ++i) {
        const char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && i)))
            return false;
    }
    return true;
}
bool effect_valid(const ClientThemeEffect& effect) noexcept {
    const u32 slug = c_string_length(effect.slug);
    if (slug > kClientThemeEffectSlugBytes || effect.knob_count > kClientThemeEffectKnobMaximum ||
        effect.universal > 31 || effect.speed > 60 || effect.opacity > 100 || effect.resolution > kEffectResolutionSteps ||
        effect.frame_rate > kEffectFrameRateSteps || effect.blend > 2 || effect.month_knob[kClientThemeEffectKnobIdBytes] ||
        effect.phase_knob[kClientThemeEffectKnobIdBytes] ||
        (!slug && (effect.knob_count || effect.universal || effect.month_knob[0] || effect.phase_knob[0])))
        return false;
    if (slug && !effect_slug_valid(effect.slug, slug))
        return false;
    for (u32 i = 0; i < effect.knob_count; ++i)
        if (!effect.knobs[i].id[0] || !is_finite(effect.knobs[i].number) || effect.knobs[i].rgb > 0xffffffu)
            return false;
    for (const u8 phase : effect.phases)
        if (phase > kClientThemePhaseCount)
            return false;
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
        if (!is_finite(color->red) || !is_finite(color->green) || !is_finite(color->blue) || !is_finite(color->alpha))
            return false;
    }
    for (const auto member : kTypographyValues)
        if (!is_finite(document.typography.*member))
            return false;
    const auto metrics = ui_theme_metric_descriptors();
    for (u32 i = 0; i < metrics.count; ++i) {
        float value{};
        ui_theme_metric_get(document.metrics, metrics[i].id, &value);
        if (!is_finite(value))
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
constexpr char kCurves[] = "linear\0ease_out\0ease_in_out\0css_ease_out\0spring\0";
constexpr u32 kCurveCount = name_list_count(kCurves);
static_assert(kCurveCount == static_cast<u32>(UiMotionCurve::spring) + 1);
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
    variants_section,
    section_count
};
// Component colours are stored resolved (packed RGBA), so they are read after
// the semantic tier.
#define THEME_SETTINGS_COLORS(X)                                                                           \
    X(text_strong) X(text_subtle) X(nav_header) X(border_subtle) X(border_normal) X(mod_subtle)            \
    X(mod_normal) X(mod_strong) X(input_background) X(input_border) X(brand_hover) X(critical)             \
    X(critical_text) X(critical_tint) X(positive) X(focus_ring) X(scrim) X(switch_track) X(switch_border)  \
    X(slider_track) X(radio_border) X(modal_background) X(sidebar_background) X(card_fill) X(card_border)   \
    X(row_hover) X(positive_tint) X(warning_tint)
#define THEME_SETTINGS_SIZES(X)                                                                            \
    X(modal_max_width) X(modal_viewport_ratio) X(modal_ratio_threshold) X(modal_inset_top)                 \
    X(modal_inset_side) X(modal_inset_bottom) X(modal_radius) X(modal_compact_width) X(sidebar_width)      \
    X(sidebar_padding) X(sidebar_top) X(search_height) X(nav_header_height) X(nav_item_height)             \
    X(nav_item_radius) X(nav_item_gap) X(nav_item_padding) X(nav_icon) X(separator_margin)                 \
    X(panel_padding_x) X(panel_padding_top) X(panel_padding_bottom) X(panel_max_width) X(title_gap)        \
    X(category_gap) X(category_heading_gap) X(row_padding) X(label_gap) X(control_gap) X(field_gap)        \
    X(switch_width) X(switch_height) X(switch_thumb) X(slider_bar) X(slider_grabber) X(slider_mark_height) \
    X(radio_size) X(radio_gap) X(radio_spacing) X(control_height) X(control_radius) X(button_small)        \
    X(button_medium) X(button_padding) X(swatch) X(swatch_gap) X(close_size) X(notice_padding)             \
    X(card_radius) X(card_inset) X(row_min_height) X(segment_padding) X(select_width) X(slider_width)      \
    X(value_width) X(status_icon) X(card_gap) X(switch_ms) X(switch_reduced_ms) X(modal_enter_ms)          \
    X(flash_ms) X(flash_fade_ms)
#define NAME(field) #field "\0"
#define FIELD_OFFSET(field) offsetof(UiThemeSettings, field),
constexpr char kSettingsColorNames[] = THEME_SETTINGS_COLORS(NAME);
constexpr char kSettingsSizeNames[] = THEME_SETTINGS_SIZES(NAME);
constexpr u32 kSettingsColorCount = name_list_count(kSettingsColorNames);
constexpr u32 kSettingsSizeCount = name_list_count(kSettingsSizeNames);
static_assert(kSettingsColorCount <= 64 && kSettingsSizeCount <= 64);
// Every field is listed: only the trailing alignment padding is unnamed.
constexpr u32 kSettingsNamedBytes = kSettingsColorCount * sizeof(u32) + kSettingsSizeCount * sizeof(u16);
static_assert((kSettingsNamedBytes + 3) / 4 * 4 == sizeof(UiThemeSettings));
// The colours then the sizes are consecutive fields in list order, so a name index addresses its field.
static_assert([]() constexpr {
    constexpr usize colors[]{THEME_SETTINGS_COLORS(FIELD_OFFSET)};
    constexpr usize sizes[]{THEME_SETTINGS_SIZES(FIELD_OFFSET)};
    for (u32 i = 0; i < kSettingsColorCount; ++i)
        if (colors[i] != i * sizeof(u32))
            return false;
    for (u32 i = 0; i < kSettingsSizeCount; ++i)
        if (sizes[i] != kSettingsColorCount * sizeof(u32) + i * sizeof(u16))
            return false;
    return true;
}());
#undef NAME
#undef FIELD_OFFSET
inline const u32* settings_colors(const UiThemeSettings& settings) noexcept {
    return reinterpret_cast<const u32*>(&settings);
}
inline u32* settings_colors(UiThemeSettings& settings) noexcept {
    return reinterpret_cast<u32*>(&settings);
}
inline const u16* settings_sizes(const UiThemeSettings& settings) noexcept {
    return reinterpret_cast<const u16*>(settings_colors(settings) + kSettingsColorCount);
}
inline u16* settings_sizes(UiThemeSettings& settings) noexcept {
    return reinterpret_cast<u16*>(settings_colors(settings) + kSettingsColorCount);
}
constexpr char kIconRoles[] = "normal\0muted\0accent\0danger\0success\0warning\0on_accent\0";
constexpr u32 kIconRoleCount = sizeof(UiThemeIcons::colors) / sizeof(u32);
static_assert(name_list_count(kIconRoles) == kIconRoleCount);
bool settings_size_valid(u32 index, u32 value) noexcept {
    // Index 1 is the modal viewport ratio, a percentage; durations share the size bound.
    return index == 1 ? value >= 10 && value <= 100 : value <= 4096;
}
bool components_equal(const ClientThemeDocument& a, const ClientThemeDocument& b) noexcept {
    return bytes_equal(&a.icons, &b.icons, sizeof(a.icons)) &&
           bytes_equal(&a.settings, &b.settings, kSettingsNamedBytes);
}
bool components_valid(const UiThemeIcons& icons, const UiThemeSettings& settings) noexcept {
    if (!(icons.weight >= .25f && icons.weight <= 4))
        return false;
    const u16* sizes = settings_sizes(settings);
    for (u32 i = 0; i < kSettingsSizeCount; ++i)
        if (!settings_size_valid(i, sizes[i]))
            return false;
    return true;
}
bool variants_valid(const ClientThemeVariants& variants) noexcept {
    for (const u8 base : variants.bases)
        if (base > static_cast<u32>(ClientThemeBase::amoled) + 1)
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
u32 key_index(JsonString name, const char* names) noexcept {
    u32 index{};
    for (NameList list{names}; list; list.next(), ++index)
        if (json_string_equals_ascii(name, list.name()))
            return index;
    return index;
}
bool enum_value(Parse& parse, JsonValue value, const char* names, u32 count, u32* output) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string))
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    const u32 id = key_index(string, names);
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
    *output = color_from_rgba(rgba);
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
            if (utf8_equal(colors[i].name(), parts[1])) {
                *output = {{}, alpha, static_cast<u8>(colors[i].id), UiThemeTokenSourceKind::core};
                return true;
            }
    } else if (count == 3 && allowed == Reference::any && utf8_equal(parts[0], utf8_literal("semantic"))) {
        const auto tokens = ui_theme_token_descriptors();
        for (u32 i = 0; i < tokens.count; ++i)
            if (utf8_equal(tokens[i].group(), parts[1]) && utf8_equal(tokens[i].name(), parts[2])) {
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
        if (!json_string_equals_ascii(name, token.name()))
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
    u32 group;
};
bool semantic_token_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& visit = *static_cast<GroupVisit*>(context);
    auto& parse = *visit.parse;
    const auto tokens = ui_theme_token_descriptors();
    for (u32 i = 0; i < tokens.count; ++i) {
        if (tokens[i].group_index != visit.group || !json_string_equals_ascii(name, tokens[i].name()))
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
    for (u32 i = 0; i < tokens.count; ++i) {
        const u32 group = tokens[i].group_index;
        if (i && group == tokens[i - 1].group_index)
            continue;
        if (json_string_equals_ascii(name, tokens[i].group())) {
            if (!duplicate(parse, section.seen, group, name.source_offset))
                return false;
            if (value.kind != JsonValueKind::object)
                return parse.fail(ClientThemeDocumentError::type, value.source_offset);
            GroupVisit visit{&parse, group};
            return parse.read(json_visit_indexed_object(parse.document, value, kClientThemeDocumentMaximumDepth,
                                                        semantic_token_member, &visit));
        }
    }
    ++parse.unsupported;
    return true;
}
bool scale_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    const auto scales = ui_theme_scale_descriptors();
    for (u32 i = 0; i < scales.count; ++i) {
        if (!json_string_equals_ascii(name, scales[i].name()))
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
    static constexpr char keys[] = "duration_ms\0curve\0";
    const u32 id = key_index(name, keys);
    if (id == 2) {
        ++parse.unsupported;
        return true;
    }
    if (!duplicate(parse, visit.seen, id, name.source_offset))
        return false;
    if (id == 1) {
        u32 curve{};
        if (!enum_value(parse, value, kCurves, kCurveCount, &curve))
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
        if (!json_string_equals_ascii(name, motions[i].name()))
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
        if (!json_string_equals_ascii(name, metric.name()))
            continue;
        if (!duplicate(parse, section.seen, static_cast<u32>(metric.id), name.source_offset))
            return false;
        float number{};
        if (!float_value(parse, value, &number))
            return false;
        if (number < metric.minimum() || number > metric.maximum() ||
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
    const u32 id = key_index(name, kTypographyNames);
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
bool effect_knob_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(context);
    auto& effect = parse.candidate.effect;
    auto& knob = effect.knobs[effect.knob_count < kClientThemeEffectKnobMaximum ? effect.knob_count : 0];
    char id[kClientThemeEffectKnobIdBytes + 1];
    const auto decoded = json_string_decode_utf8(name, {id, kClientThemeEffectKnobIdBytes});
    if (effect.knob_count == kClientThemeEffectKnobMaximum || !decoded.ok() || !decoded.bytes)
        return parse.fail(ClientThemeDocumentError::range, name.source_offset);
    id[decoded.bytes] = 0;
    for (u32 i = 0; i < effect.knob_count; ++i)
        if (c_string_length(effect.knobs[i].id) == decoded.bytes && bytes_equal(effect.knobs[i].id, id, decoded.bytes))
            return parse.fail(ClientThemeDocumentError::duplicate_field, name.source_offset);
    knob = {};
    copy_bytes(knob.id, id, decoded.bytes + 1);
    JsonString text{};
    bool flag{};
    if (value.kind == JsonValueKind::number) {
        if (!float_value(parse, value, &knob.number))
            return false;
    } else if (json_value_to_bool(value, &flag)) {
        knob.number = flag ? 1.f : 0.f;
    } else if (json_value_as_string(value, &text)) {
        char hex[8];
        const auto colour = json_string_decode_utf8(text, {hex, 7});
        if (!colour.ok() || colour.bytes != 7 || hex[0] != '#')
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        u32 rgb{};
        for (u32 i = 1; i < 7; ++i) {
            const char c = hex[i];
            const u32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (d == 16)
                return parse.fail(ClientThemeDocumentError::range, value.source_offset);
            rgb = rgb << 4 | d;
        }
        knob.color = true;
        knob.rgb = rgb;
    } else {
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    }
    if (!is_finite(knob.number) || knob.number < -1e6f || knob.number > 1e6f)
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    ++effect.knob_count;
    return true;
}
bool effect_universal_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(context);
    auto& effect = parse.candidate.effect;
    static constexpr char keys[] = "speed\0resolution\0fps\0opacity\0blend\0";
    const u32 id = key_index(name, keys);
    if (id == 5 || (effect.universal & (1u << id)))
        return parse.fail(id == 5 ? ClientThemeDocumentError::range : ClientThemeDocumentError::duplicate_field,
                          name.source_offset);
    if (id == 4) {
        JsonString text{};
        if (!json_value_as_string(value, &text))
            return parse.fail(ClientThemeDocumentError::type, value.source_offset);
        u32 blend = key_index(text, "normal\0screen\0add\0");
        if (blend == 3)
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        effect.blend = static_cast<u8>(blend);
        effect.universal |= theme_effect_blend;
        return true;
    }
    float number{};
    if (!float_value(parse, value, &number))
        return false;
    if (id == 0) {
        if (!(number >= 0 && number <= 3))
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        effect.speed = static_cast<u8>(number * 20 + .5f);
        effect.universal |= theme_effect_speed;
    } else if (id == 3) {
        // A fraction up to 1, or a percent above it.
        const float percent = number <= 1 ? number * 100 : number;
        if (!(number >= 0 && percent <= 100))
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        effect.opacity = static_cast<u8>(percent + .5f);
        effect.universal |= theme_effect_opacity;
    } else {
        const u8* table = id == 1 ? kEffectResolutionPercent : kEffectFramesPerSecond;
        u32 step = 0;
        for (u32 i = 0; i < 5; ++i)
            if (static_cast<float>(table[i]) == number)
                step = i + 1;
        if (!step)
            return parse.fail(ClientThemeDocumentError::range, value.source_offset);
        (id == 1 ? effect.resolution : effect.frame_rate) = static_cast<u8>(step);
        effect.universal |= id == 1 ? theme_effect_resolution : theme_effect_frame_rate;
    }
    return true;
}
constexpr char kFollowPhases[] = "auto\0night\0dawn\0day\0dusk\0";
bool follow_phase(void* context, u32 index, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(context);
    JsonString text{};
    if (!json_value_as_string(value, &text))
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    const u32 phase = key_index(text, kFollowPhases);
    if (index >= kClientThemeFollowPhaseMaximum || phase > kClientThemePhaseCount)
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    parse.candidate.effect.phases[index] = static_cast<u8>(phase);
    return true;
}
// "follow": {"month": "<knob>", "phase": "<knob>", "phases": [...]}; see ClientThemeEffect.
bool effect_follow_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(context);
    auto& effect = parse.candidate.effect;
    const u32 id = key_index(name, "month\0phase\0phases\0");
    if (id == 3) {
        ++parse.unsupported;
        return true;
    }
    if (id == 2) {
        if (value.kind != JsonValueKind::array)
            return parse.fail(ClientThemeDocumentError::type, value.source_offset);
        return parse.read(
            json_visit_indexed_array(parse.document, value, kClientThemeDocumentMaximumDepth, follow_phase, &parse));
    }
    char* knob = id ? effect.phase_knob : effect.month_knob;
    JsonString text{};
    if (!json_value_as_string(value, &text))
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    const auto decoded = json_string_decode_utf8(text, {knob, kClientThemeEffectKnobIdBytes});
    if (!decoded.ok() || !decoded.bytes)
        return parse.fail(ClientThemeDocumentError::range, value.source_offset);
    knob[decoded.bytes] = 0;
    return true;
}
bool background_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    static constexpr char keys[] =
        "colors\0angle_degrees\0intensity\0effect\0speed\0strength\0density\0knobs\0universal\0follow\0";
    const u32 id = key_index(name, keys);
    if (id == 10) {
        ++parse.unsupported;
        return true;
    }
    if (!duplicate(parse, section.seen, id, name.source_offset))
        return false;
    if (id >= 7) {
        if (value.kind != JsonValueKind::object)
            return parse.fail(ClientThemeDocumentError::type, value.source_offset);
        return parse.read(json_visit_indexed_object(parse.document, value, kClientThemeDocumentMaximumDepth,
                                                    id == 7   ? effect_knob_member
                                                    : id == 8 ? effect_universal_member
                                                              : effect_follow_member,
                                                    &parse));
    }
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
        // A built-in effect, or a package effect named by its slug (used while no built-in effect is on).
        const u32 effect = key_index(effect_name, kEffects);
        background.effect =
            effect == kBackgroundEffectCount ? BackgroundEffect::none : static_cast<BackgroundEffect>(effect);
        if (effect == kBackgroundEffectCount) {
            char slug[kClientThemeEffectSlugBytes + 1];
            const auto decoded = json_string_decode_utf8(effect_name, {slug, kClientThemeEffectSlugBytes});
            if (decoded.ok() && effect_slug_valid(slug, decoded.bytes)) {
                copy_bytes(parse.candidate.effect.slug, slug, decoded.bytes);
                parse.candidate.effect.slug[decoded.bytes] = 0;
            } else {
                ++parse.unsupported;
            }
        }
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
    constexpr u32 count = kIconRoleCount;
    const u32 id = key_index(name, kIconRoles); // The weight key shares the past-the-roles index.
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
    const u32 color = key_index(name, kSettingsColorNames);
    if (color < kSettingsColorCount)
        return duplicate(parse, section.seen, color, name.source_offset) &&
               resolved_color(parse, value, &settings_colors(parse.candidate.settings)[color]);
    const u32 size = key_index(name, kSettingsSizeNames);
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
    settings_sizes(parse.candidate.settings)[size] = static_cast<u16>(number);
    return true;
}
bool components_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    static constexpr char keys[] = "icons\0settings\0";
    const u32 id = key_index(name, keys);
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
// ---- variants: {"night"|"dawn"|"day"|"dusk": {"base", "colors", "months": [12 objects of colours]}} ----------
constexpr char kPhases[] = "night\0dawn\0day\0dusk\0";
static_assert(name_list_count(kPhases) == kClientThemePhaseCount);
struct VariantVisit {
    Parse* parse;
    u32 phase;
    u64 seen;
    u32* colors;
};
bool variant_color_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& visit = *static_cast<VariantVisit*>(context);
    auto& parse = *visit.parse;
    const auto tokens = ui_theme_color_descriptors();
    for (u32 i = 0; i < tokens.count; ++i) {
        if (!json_string_equals_ascii(name, tokens[i].name()))
            continue;
        UiThemeTokenSource source{};
        const u32 id = static_cast<u32>(tokens[i].id);
        if (!duplicate(parse, visit.seen, id, name.source_offset) ||
            !color_value(parse, value, Reference::palette_only, &source))
            return false;
        visit.colors[id] = pack(source.literal);
        return true;
    }
    ++parse.unsupported;
    return true;
}
// One set of core colours: the phase's own (set 0) or a month's (1..12).
bool variant_colors(Parse& parse, u32 phase, u32 set, JsonValue value) noexcept {
    if (value.kind != JsonValueKind::object)
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    VariantVisit visit{&parse, phase, 0, parse.candidate.variants.colors[phase][set]};
    return parse.read(json_visit_indexed_object(parse.document, value, kClientThemeDocumentMaximumDepth,
                                                variant_color_member, &visit));
}
bool variant_month(void* context, u32 index, JsonValue value) noexcept {
    auto& months = *static_cast<VariantVisit*>(context);
    if (index >= kClientThemeColorSets - 1)
        return months.parse->fail(ClientThemeDocumentError::range, value.source_offset);
    months.seen = index + 1;
    return variant_colors(*months.parse, months.phase, index + 1, value);
}
bool phase_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& visit = *static_cast<VariantVisit*>(context);
    auto& parse = *visit.parse;
    const u32 id = key_index(name, "base\0colors\0months\0");
    if (id == 3) {
        ++parse.unsupported;
        return true;
    }
    if (!duplicate(parse, visit.seen, id, name.source_offset))
        return false;
    if (!id) {
        u32 base{};
        if (!enum_value(parse, value, kBases, 3, &base))
            return false;
        parse.candidate.variants.bases[visit.phase] = static_cast<u8>(base + 1);
        return true;
    }
    if (id == 1)
        return variant_colors(parse, visit.phase, 0, value);
    if (value.kind != JsonValueKind::array)
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    VariantVisit months{&parse, visit.phase, 0, nullptr};
    return parse.read(json_visit_indexed_array(parse.document, value, kClientThemeDocumentMaximumDepth,
                                               variant_month, &months)) &&
           (months.seen == kClientThemeColorSets - 1 ||
            parse.fail(ClientThemeDocumentError::range, value.source_offset));
}
bool variants_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& section = *static_cast<SectionVisit*>(context);
    auto& parse = *section.parse;
    const u32 phase = key_index(name, kPhases);
    if (phase == kClientThemePhaseCount) {
        ++parse.unsupported;
        return true;
    }
    if (!duplicate(parse, section.seen, phase, name.source_offset))
        return false;
    if (value.kind != JsonValueKind::object)
        return parse.fail(ClientThemeDocumentError::type, value.source_offset);
    // A phase that names no base keeps the root's.
    parse.candidate.variants.bases[phase] = static_cast<u8>(static_cast<u32>(parse.candidate.base) + 1);
    VariantVisit visit{&parse, phase, 0, nullptr};
    return parse.read(json_visit_indexed_object(parse.document, value, kClientThemeDocumentMaximumDepth,
                                                phase_member, &visit));
}
bool root_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(context);
    static constexpr char keys[] = "version\0name\0base\0colors\0background\0typography\0metrics\0palette\0"
                                   "semantic\0scale\0motion\0components\0variants\0";
    constexpr u32 count = name_list_count(keys);
    static_assert(count == 3 + section_count);
    const u32 id = key_index(name, keys);
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
    static_assert(colors_section == 0 && variants_section == 9);
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
    __declspec(noinline) void text(Utf8View text) noexcept {
        for (u32 i = 0; i < text.bytes; ++i)
            character(text.data[i]);
    }
    __declspec(noinline) void quoted(Utf8View text) noexcept {
        character('"');
        for (u32 i = 0; i < text.bytes; ++i) {
            if (text.data[i] == '"' || text.data[i] == '\\')
                character('\\');
            character(text.data[i]);
        }
        character('"');
    }
    // "#" and `bits`/4 lowercase digits, quoted.
    void hex(u32 value, u32 bits) noexcept {
        character('"');
        character('#');
        for (u32 shift = bits; shift;) {
            shift -= 4;
            character(ascii_hex_digit(value >> shift));
        }
        character('"');
    }
    void number(u64 value) noexcept {
        char digits[20];
        text({digits, decimal_write(digits, value)});
    }
    // Starts an object member: a comma after the first, then "name":.
    void key(bool& comma, const Utf8View& name) noexcept {
        if (comma)
            character(',');
        comma = true;
        quoted(name);
        character(':');
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
    writer.quoted(name_list_at(kBases, static_cast<u32>(document.base)));
    writer.text(utf8_literal(",\"colors\":{"));
    bool comma{};
    const auto tokens = ui_theme_color_descriptors();
    for (u32 i = 0; i < tokens.count; ++i) {
        writer.key(comma, tokens[i].name());
        write_color(writer, *ui_theme_color_get(document.theme, tokens[i].id));
    }
    // Canonical semantic values are resolved literals: a stored document never
    // depends on table defaults that a later build might change.
    writer.text(utf8_literal("},\"semantic\":{"));
    const auto semantic = ui_theme_token_descriptors();
    for (u32 i = 0; i < semantic.count; ++i) {
        const bool first = !i || semantic[i].group_index != semantic[i - 1].group_index;
        if (first) {
            if (i)
                writer.text(utf8_literal("},"));
            writer.quoted(semantic[i].group());
            writer.text(utf8_literal(":{"));
        } else
            writer.character(',');
        writer.quoted(semantic[i].name());
        writer.character(':');
        write_color(writer, *ui_theme_token_get(document.theme, semantic[i].id));
    }
    writer.text(utf8_literal("}},\"scale\":{"));
    const auto scales = ui_theme_scale_descriptors();
    comma = false;
    for (u32 i = 0; i < scales.count; ++i) {
        float value{};
        ui_theme_scale_get(document.theme.scale, scales[i].id, &value);
        writer.key(comma, scales[i].name());
        writer.real(value);
    }
    writer.text(utf8_literal("},\"motion\":{"));
    const auto motions = ui_theme_motion_descriptors();
    auto motion = document.theme.motion;
    comma = false;
    for (u32 i = 0; i < motions.count; ++i) {
        const auto* style = ui_theme_motion_slot(&motion, motions[i].id);
        writer.key(comma, motions[i].name());
        writer.text(utf8_literal("{\"duration_ms\":"));
        writer.number(style->duration_ms);
        writer.text(utf8_literal(",\"curve\":"));
        writer.quoted(name_list_at(kCurves, static_cast<u32>(style->curve)));
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
    const auto& package = document.effect;
    if (package.slug[0])
        writer.quoted({package.slug, c_string_length(package.slug)});
    else
        writer.quoted(name_list_at(kEffects, static_cast<u32>(background.effect)));
    writer.text(utf8_literal(",\"speed\":"));
    writer.number(background.speed);
    writer.text(utf8_literal(",\"strength\":"));
    writer.number(background.strength);
    writer.text(utf8_literal(",\"density\":"));
    writer.number(background.density);
    if (package.slug[0]) {
        writer.text(utf8_literal(",\"knobs\":{"));
        comma = false;
        for (u32 i = 0; i < package.knob_count; ++i) {
            writer.key(comma, {package.knobs[i].id, c_string_length(package.knobs[i].id)});
            if (package.knobs[i].color)
                writer.hex(package.knobs[i].rgb, 24);
            else
                writer.real(package.knobs[i].number);
        }
        writer.text(utf8_literal("},\"universal\":{"));
        comma = false;
        if (package.universal & theme_effect_speed) {
            writer.key(comma, utf8_literal("speed"));
            writer.real(static_cast<float>(package.speed) * .05f);
        }
        if (package.universal & theme_effect_resolution) {
            writer.key(comma, utf8_literal("resolution"));
            writer.number(kEffectResolutionPercent[package.resolution - 1]);
        }
        if (package.universal & theme_effect_frame_rate) {
            writer.key(comma, utf8_literal("fps"));
            writer.number(kEffectFramesPerSecond[package.frame_rate - 1]);
        }
        if (package.universal & theme_effect_opacity) {
            writer.key(comma, utf8_literal("opacity"));
            writer.real(static_cast<float>(package.opacity) * .01f);
        }
        if (package.universal & theme_effect_blend) {
            writer.key(comma, utf8_literal("blend"));
            writer.quoted(name_list_at("normal\0screen\0add\0", package.blend));
        }
        writer.character('}');
        u32 phases = kClientThemeFollowPhaseMaximum;
        while (phases && !package.phases[phases - 1])
            --phases;
        if (package.month_knob[0] || package.phase_knob[0] || phases) {
            writer.text(utf8_literal(",\"follow\":{"));
            comma = false;
            if (package.month_knob[0]) {
                writer.key(comma, utf8_literal("month"));
                writer.quoted({package.month_knob, c_string_length(package.month_knob)});
            }
            if (package.phase_knob[0]) {
                writer.key(comma, utf8_literal("phase"));
                writer.quoted({package.phase_knob, c_string_length(package.phase_knob)});
            }
            if (phases) {
                writer.key(comma, utf8_literal("phases"));
                for (u32 i = 0; i < phases; ++i) {
                    writer.character(i ? ',' : '[');
                    writer.quoted(name_list_at(kFollowPhases, package.phases[i]));
                }
                writer.character(']');
            }
            writer.character('}');
        }
    }
    writer.text(utf8_literal("},\"typography\":{"));
    comma = false;
    for (u32 i = 0; i < 8; ++i) {
        writer.key(comma, name_list_at(kTypographyNames, i));
        if (i < 6)
            writer.real(document.typography.*kTypographyValues[i]);
        else
            writer.quoted(name_list_at(kFaces, static_cast<u32>(i == 6 ? document.typography.body_face
                                                                       : document.typography.code_face)));
    }
    writer.text(utf8_literal("},\"metrics\":{"));
    comma = false;
    const auto metrics = ui_theme_metric_descriptors();
    for (u32 i = 0; i < metrics.count; ++i) {
        float value{};
        ui_theme_metric_get(document.metrics, metrics[i].id, &value);
        writer.key(comma, metrics[i].name());
        writer.real(value);
    }
    // Components carry only authored (non-default) values.
    writer.text(utf8_literal("},\"components\":{\"icons\":{"));
    comma = false;
    for (u32 i = 0; i < kIconRoleCount; ++i)
        if (document.icons.colors[i]) {
            writer.key(comma, name_list_at(kIconRoles, i));
            writer.hex(document.icons.colors[i], 32);
        }
    if (document.icons.weight != UiThemeIcons{}.weight) {
        writer.key(comma, utf8_literal("weight"));
        writer.real(document.icons.weight);
    }
    writer.text(utf8_literal("},\"settings\":{"));
    comma = false;
    const u32* colors = settings_colors(document.settings);
    const u16 *sizes = settings_sizes(document.settings), *defaults = settings_sizes(kUiThemeSettingsDefault);
    for (u32 i = 0; i < kSettingsColorCount; ++i)
        if (const u32 packed = colors[i]) {
            writer.key(comma, name_list_at(kSettingsColorNames, i));
            writer.hex(packed, 32);
        }
    for (u32 i = 0; i < kSettingsSizeCount; ++i)
        if (sizes[i] != defaults[i]) {
            writer.key(comma, name_list_at(kSettingsSizeNames, i));
            writer.number(sizes[i]);
        }
    writer.text(utf8_literal("}}"));
    // Only a document with variants writes them, so every other document is byte-for-byte what it was.
    const auto& variants = document.variants;
    if (client_theme_document_scheduled(document)) {
        writer.text(utf8_literal(",\"variants\":{"));
        comma = false;
        const auto descriptors = ui_theme_color_descriptors();
        for (u32 phase = 0; phase < kClientThemePhaseCount; ++phase) {
            if (!variants.bases[phase])
                continue;
            writer.key(comma, name_list_at(kPhases, phase));
            writer.text(utf8_literal("{\"base\":"));
            writer.quoted(name_list_at(kBases, variants.bases[phase] - 1u));
            bool months{};
            for (u32 set = 1; set < kClientThemeColorSets; ++set)
                for (const u32 packed : variants.colors[phase][set])
                    months = months || packed;
            for (u32 set = 0; set < (months ? kClientThemeColorSets : 1u); ++set) {
                writer.text(set == 0 ? utf8_literal(",\"colors\":{")
                            : set == 1 ? utf8_literal(",\"months\":[{")
                                       : utf8_literal(",{"));
                bool first{};
                for (u32 i = 0; i < descriptors.count; ++i)
                    if (const u32 packed = variants.colors[phase][set][static_cast<u32>(descriptors[i].id)]) {
                        writer.key(first, descriptors[i].name());
                        writer.hex(packed, 32);
                    }
                writer.character('}');
            }
            writer.text(months ? utf8_literal("]}") : utf8_literal("}"));
        }
        writer.character('}');
    }
    writer.character('}');
}
// A base's built-in look, as a document holds it.
Theme base_theme(ClientThemeBase base) noexcept {
    Theme theme = base == ClientThemeBase::light    ? kLightTheme
                  : base == ClientThemeBase::amoled ? ui_theme_amoled()
                                                    : kDefaultTheme;
    theme.styles = nullptr;
    theme.style_revision = 0;
    theme.appearance = base == ClientThemeBase::light ? ThemeAppearance::light : ThemeAppearance::dark;
    return theme;
}
// One phase in one month (0: none), or the root look for a phase the document leaves out.
Theme phase_look(const ClientThemeDocument& document, u32 phase, u32 month) noexcept {
    const u32 base = document.variants.bases[phase];
    if (!base)
        return document.theme;
    Theme theme = base_theme(static_cast<ClientThemeBase>(base - 1));
    theme.scale = document.theme.scale;
    theme.motion = document.theme.motion;
    theme.background = document.theme.background;
    const auto& sets = document.variants.colors[phase];
    for (u32 i = 0; i < static_cast<u32>(UiThemeColor::count); ++i)
        if (const u32 packed = sets[month][i] ? sets[month][i] : sets[0][i])
            ui_theme_color_set(&theme, static_cast<UiThemeColor>(i), color_from_rgba(packed));
    ui_theme_tokens_resolve(&theme, nullptr);
    return theme;
}
} // namespace
ClientThemeDocument client_theme_document_defaults(ClientThemeBase base) noexcept {
    ClientThemeDocument document{};
    document.base =
        static_cast<u32>(base) <= static_cast<u32>(ClientThemeBase::amoled) ? base : ClientThemeBase::dark;
    document.theme = base_theme(document.base);
    constexpr auto name = utf8_literal("Custom theme");
    copy_bytes(document.name, name.data, name.bytes);
    document.name_bytes = name.bytes;
    return document;
}
void client_theme_mix(Theme* to, const Theme& from, float amount) noexcept {
    // The core palette and the semantic tier are consecutive colours at the start of a Theme.
    static_assert(offsetof(Theme, tokens) == static_cast<usize>(UiThemeColor::count) * sizeof(Color) &&
                  offsetof(Theme, scale) == offsetof(Theme, tokens) + sizeof(UiThemeTokens) &&
                  sizeof(UiThemeTokens) % sizeof(Color) == 0);
    auto* a = reinterpret_cast<float*>(to);
    const auto* b = reinterpret_cast<const float*>(&from);
    for (u32 i = 0; i < offsetof(Theme, scale) / sizeof(float); ++i)
        a[i] = b[i] + (a[i] - b[i]) * amount;
}
namespace {
u8 mix_byte(u32 from, u32 to, float amount) noexcept {
    return static_cast<u8>(static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * amount + .5f);
}
// A background gradient `amount` of the way through a glide (see client_theme_glide).
void glide_background(BackgroundTheme& to, BackgroundTheme from, float amount) noexcept {
    if (from.effect != to.effect || from.speed != to.speed || from.strength != to.strength ||
        from.density != to.density || (!from.color_count && !to.color_count))
        return;
    if (from.color_count == to.color_count) {
        for (u32 i = 0; i < to.color_count; ++i) {
            u32 color = 0;
            for (u32 shift = 0; shift < 24; shift += 8)
                color |= u32{mix_byte(from.colors[i] >> shift & 255, to.colors[i] >> shift & 255, amount)} << shift;
            to.colors[i] = color;
        }
        // The angle turns the short way round.
        i32 turn = static_cast<i32>(to.angle_degrees % 360) - static_cast<i32>(from.angle_degrees % 360);
        turn = turn > 180 ? turn - 360 : turn < -180 ? turn + 360 : turn;
        const float angle = static_cast<float>(from.angle_degrees % 360) + static_cast<float>(turn) * amount;
        to.angle_degrees = static_cast<u16>(static_cast<i32>(angle + 360.5f) % 360);
        to.intensity = mix_byte(from.intensity, to.intensity, amount);
        return;
    }
    // Against a flat look the gradient fades in or out; two different gradients pass through flat halfway.
    const bool incoming = !from.color_count || (to.color_count && amount >= .5f);
    const float strength = !from.color_count ? amount : !to.color_count ? 1 - amount : incoming ? 2 * amount - 1 : 1 - 2 * amount;
    if (!incoming)
        to = from;
    to.intensity = mix_byte(0, to.intensity, strength);
}
} // namespace
bool client_theme_glide(Theme* to, const Theme& from, u64 elapsed_ms, u32 duration_ms) noexcept {
    if (elapsed_ms >= duration_ms)
        return false;
    const float amount =
        ui_motion_curve_sample(UiMotionCurve::ease_in_out, static_cast<float>(elapsed_ms) / static_cast<float>(duration_ms));
    client_theme_mix(to, from, amount);
    glide_background(to->background, from.background, amount);
    return true;
}
Theme client_theme_document_at(const ClientThemeDocument& document, u32 minute_of_day, u32 month, u32 day) noexcept {
    const auto at = client_theme_phase_at(minute_of_day);
    month = month - 1u < kClientThemeColorSets - 1u ? month : 0u;
    // From the 5th to the 30th a month turns toward the next one's colours, as Silk Waves' sky does.
    float turn = ((static_cast<float>(day) + static_cast<float>(minute_of_day % 1440u) / 1440.f) - 5.f) / 25.f;
    turn = !month || turn < 0 ? 0.f : turn > 1 ? 1.f : turn;
    turn = turn * turn * (3 - 2 * turn);
    const auto look = [&](u32 phase) noexcept {
        Theme theme = phase_look(document, phase, month);
        if (turn > 0) {
            Theme next = phase_look(document, phase, month % 12u + 1u);
            client_theme_mix(&next, theme, turn);
            return next;
        }
        return theme;
    };
    if (at.amount == 0)
        return look(at.from);
    Theme to = look(at.to);
    const Theme from = look(at.from);
    // Across a change of base the colours don't pass through grey: the nearer phase shows whole.
    if (from.appearance != to.appearance)
        return at.amount < .5f ? from : to;
    client_theme_mix(&to, from, at.amount);
    return to;
}
ClientThemeClock client_theme_follow(const ClientThemeEffect& effect, ClientThemeClock clock, float month,
                                     float phase) noexcept {
    // Day 1 is before the month starts turning; a phase shows whole at its keyframe (client_theme_phase_at).
    if (month >= 1 && month <= 12)
        clock.month = static_cast<u32>(month), clock.day = 1;
    if (phase >= 0 && phase < kClientThemeFollowPhaseMaximum)
        if (const u32 held = effect.phases[static_cast<u32>(phase)])
            clock.minute = (held - 1) * 360;
    return clock;
}
bool client_theme_document_valid(const ClientThemeDocument& document) noexcept {
    return static_cast<u32>(document.base) <= static_cast<u32>(ClientThemeBase::amoled) &&
           document.theme.appearance ==
               (document.base == ClientThemeBase::light ? ThemeAppearance::light : ThemeAppearance::dark) &&
           document.name_bytes <= kClientThemeNameMaximumBytes && document.name[document.name_bytes] == 0 &&
           name_valid({document.name, document.name_bytes}) && ui_theme_palette_valid(document.theme) &&
           real_values_finite(document) && background_valid(document.theme.background) &&
           effect_valid(document.effect) &&
           ui_theme_typography_valid(document.typography) && ui_theme_metrics_valid(document.metrics) &&
           components_valid(document.icons, document.settings) && variants_valid(document.variants);
}
bool client_theme_document_equal(const ClientThemeDocument& a, const ClientThemeDocument& b) noexcept {
    return a.base == b.base && a.name_bytes == b.name_bytes &&
           a.name_bytes <= kClientThemeNameMaximumBytes && bytes_equal(a.name, b.name, a.name_bytes) &&
           ui_theme_palette_equal(a.theme, b.theme) &&
           background_theme_equal(a.theme.background, b.theme.background) &&
           bytes_equal(&a.effect, &b.effect, sizeof(a.effect)) &&
           ui_theme_typography_equal(a.typography, b.typography) &&
           ui_theme_metrics_equal(a.metrics, b.metrics) && components_equal(a, b) &&
           bytes_equal(&a.variants, &b.variants, sizeof(a.variants));
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
    // Variants are the document's own: a document without them never inherits the defaults'.
    parse.candidate.variants = {};
    parse.candidate.theme.styles = nullptr;
    parse.candidate.theme.style_revision = 0;
    parse.candidate.theme.appearance =
        parse.candidate.base == ClientThemeBase::light ? ThemeAppearance::light : ThemeAppearance::dark;
    static constexpr JsonMemberVisitor visitors[]{color_member,   background_member, typography_member,
                                           metric_member,  palette_member,    semantic_member,
                                           scale_member,   motion_member,     components_member,
                                           variants_member};
    static constexpr u32 kSectionOrder[]{palette_section,    colors_section,  semantic_section,
                                  background_section, typography_section, metrics_section,
                                  scale_section,      motion_section,  components_section,
                                  variants_section};
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
                                                      const Slice<char>& output, u32* bytes) noexcept {
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
