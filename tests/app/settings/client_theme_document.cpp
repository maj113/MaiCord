#include "maicord/app/settings/client_theme_document.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/effects/background_effect.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/ui/theme/presets.hpp"

namespace maicord {
namespace {
u32 scratch[4096];
char exported[kClientThemeDocumentMaximumBytes], rewritten[kClientThemeDocumentMaximumBytes];
ClientThemeDocumentResult parse(Utf8View input, const ClientThemeDocument& defaults,
                                ClientThemeDocument* output) noexcept {
    return client_theme_document_parse(input, defaults, {scratch, 4096}, output);
}
bool refused(Utf8View input, const ClientThemeDocument& defaults) noexcept {
    auto output = defaults;
    const auto before = output;
    const auto result = parse(input, defaults, &output);
    return !result.ok() && bytes_equal(&output, &before, sizeof(output));
}
bool same_color(const Color& a, u32 rgb) noexcept {
    const Color b = color_from_rgb24(rgb);
    const auto near = [](float x, float y) noexcept { return x - y < .002f && y - x < .002f; };
    return near(a.red, b.red) && near(a.green, b.green) && near(a.blue, b.blue);
}
bool contains(const char* text, u32 bytes, Utf8View needle) noexcept {
    for (u32 i = 0; i + needle.bytes <= bytes; ++i)
        if (bytes_equal(text + i, needle.data, needle.bytes))
            return true;
    return false;
}
// A theme that changes with the time of day (ClientThemeVariants): the phase schedule, the document's variants, the
// look at a time and month, and the effects' appearance following its base.
void test_variants(const ClientThemeDocument& defaults, void (*check)(bool, const char*) noexcept) noexcept {
    const auto at = [](u32 hour, u32 minute = 0) noexcept { return client_theme_phase_at(hour * 60 + minute); };
    const auto is = [](ClientThemePhaseMix mix, u32 from, u32 to, float amount) noexcept {
        return mix.from == from && mix.to == to && mix.amount - amount < 1e-5f && amount - mix.amount < 1e-5f;
    };
    check(is(at(0), theme_phase_night, theme_phase_dawn, 0) && is(at(23, 30), theme_phase_night, theme_phase_dawn, 0) &&
              is(at(1), theme_phase_night, theme_phase_dawn, 0) && is(at(3), theme_phase_night, theme_phase_dawn, .5f) &&
              is(at(6), theme_phase_dawn, theme_phase_day, 0) && is(at(8), theme_phase_dawn, theme_phase_day, .15625f) &&
              is(at(9), theme_phase_dawn, theme_phase_day, .5f) && is(at(12), theme_phase_day, theme_phase_dusk, 0) &&
              is(at(15), theme_phase_day, theme_phase_dusk, .5f) && is(at(18), theme_phase_dusk, theme_phase_night, 0) &&
              is(at(21), theme_phase_dusk, theme_phase_night, .5f) && at(22, 59).amount > .99f &&
              is(client_theme_phase_at(1440 + 180), theme_phase_night, theme_phase_dawn, .5f),
          "theme variants: phases hold around 00, 06, 12 and 18 and blend on a smoothstep, halfway at 03, 09, 15 and 21");
    // Night and day; dawn and dusk are the root look. March gives the night its own accent.
    const auto source = utf8_literal(
        "{\"version\":\"2.0\",\"name\":\"Phases\",\"base\":\"dark\",\"palette\":{\"sun\":\"#ffcc00\"},"
        "\"colors\":{\"base_low\":\"#202020\"},\"variants\":{"
        "\"night\":{\"colors\":{\"base_low\":\"#050608\",\"text_primary\":\"#eeeeee\"},"
        "\"months\":[{},{},{\"accent\":\"#ff8040\"},{},{},{},{},{},{},{},{},{}]},"
        "\"day\":{\"base\":\"light\",\"colors\":{\"base_low\":\"#f4f1ea\",\"accent\":\"{palette.sun}\"},\"noon\":1},"
        "\"noon\":{}}}");
    ClientThemeDocument document{};
    const auto read = parse(source, defaults, &document);
    const auto& v = document.variants;
    check(read.ok() && read.unsupported_fields == 2 && client_theme_document_scheduled(document) &&
              v.bases[theme_phase_night] == 1 && v.bases[theme_phase_day] == 2 && !v.bases[theme_phase_dawn] &&
              !v.bases[theme_phase_dusk] && v.colors[theme_phase_night][0][static_cast<u32>(UiThemeColor::base_low)] == 0x050608ff &&
              v.colors[theme_phase_night][3][static_cast<u32>(UiThemeColor::accent)] == 0xff8040ff &&
              v.colors[theme_phase_day][0][static_cast<u32>(UiThemeColor::accent)] == 0xffcc00ff &&
              document.theme.appearance == ThemeAppearance::dark && same_color(document.theme.base_low, 0x202020),
          "theme variants: phases read their base (the root's when absent), colours (palette references included) "
          "and months; unknown phases and members are counted");
    u32 bytes{};
    ClientThemeDocument again{};
    check(client_theme_document_write(document, {exported, sizeof(exported)}, &bytes).ok() &&
              contains(exported, bytes, utf8_literal(",\"variants\":{\"night\":{\"base\":\"dark\",\"colors\":{")) &&
              parse({exported, bytes}, defaults, &again).ok() && client_theme_document_equal(document, again),
          "theme variants: the canonical document carries them and reads back equal");
    ClientThemeDocument plain{};
    u32 plain_bytes{};
    check(parse(utf8_literal("{\"version\":\"2.0\",\"name\":\"Plain\"}"), document, &plain).ok() &&
              !client_theme_document_scheduled(plain) &&
              client_theme_document_write(plain, {exported, sizeof(exported)}, &plain_bytes).ok() &&
              !contains(exported, plain_bytes, utf8_literal("variants")) && exported[plain_bytes - 1] == '}' &&
              exported[plain_bytes - 2] == '}' && exported[plain_bytes - 3] == '}',
          "theme variants: a document without them inherits none and writes as it always did");
    const auto variant = [](const char* phase) noexcept {
        static char text[256];
        constexpr auto head = utf8_literal("{\"version\":\"2.0\",\"variants\":{");
        copy_bytes(text, head.data, head.bytes);
        const u32 length = c_string_length(phase);
        copy_bytes(text + head.bytes, phase, length);
        text[head.bytes + length] = '}';
        text[head.bytes + length + 1] = '}';
        return Utf8View{text, head.bytes + length + 2};
    };
    check(refused(variant("\"night\":{\"base\":\"sepia\"}"), defaults) &&
              refused(variant("\"night\":{\"colors\":[]}"), defaults) && refused(variant("\"night\":[]"), defaults) &&
              refused(variant("\"night\":{\"months\":[{},{}]}"), defaults) &&
              refused(variant("\"night\":{\"months\":[{},{},{},{},{},{},{},{},{},{},{},{},{}]}"), defaults) &&
              refused(variant("\"night\":{\"colors\":{\"accent\":\"#12\"}}"), defaults) &&
              refused(variant("\"night\":{\"colors\":{\"accent\":\"#123\",\"accent\":\"#456\"}}"), defaults) &&
              refused(variant("\"night\":{},\"night\":{}"), defaults) &&
              refused(variant("\"night\":{\"colors\":{\"accent\":\"{semantic.text.normal}\"}}"), defaults),
          "theme variants: bases, types, twelve months, colours, duplicates and core-only references are checked");

    // The look through the day, on 1 March (before the month turns).
    const auto look = [&](u32 minute, u32 month, u32 day = 1) noexcept { return client_theme_document_at(document, minute, month, day); };
    const Theme night = look(0, 3), noon = look(12 * 60, 3), april = look(0, 4), three = look(3 * 60, 3);
    const Theme eight = look(8 * 60, 3), nine = look(9 * 60, 3), dawn = look(6 * 60, 3);
    check(night.appearance == ThemeAppearance::dark && same_color(night.base_low, 0x050608) &&
              same_color(night.accent, 0xff8040) && same_color(night.tokens.text.normal, 0xeeeeee) &&
              same_color(night.tokens.background.primary, 0x050608) && !same_color(april.accent, 0xff8040) &&
              bytes_equal(&april.accent, &kDefaultTheme.accent, sizeof(Color)) &&
              noon.appearance == ThemeAppearance::light && same_color(noon.base_low, 0xf4f1ea) &&
              same_color(noon.accent, 0xffcc00) && bytes_equal(&noon.text_primary, &kLightTheme.text_primary, sizeof(Color)) &&
              dawn.appearance == ThemeAppearance::dark && same_color(dawn.base_low, 0x202020),
          "theme variants: each phase is its base's look with its colours and the month's; a phase left out is the root");
    Theme halfway = dawn;
    client_theme_mix(&halfway, night, .5f);
    check(three.appearance == ThemeAppearance::dark &&
              bytes_equal(&three, &halfway, __builtin_offsetof(Theme, scale)) && three.base_low.red > night.base_low.red &&
              three.base_low.red < dawn.base_low.red,
          "theme variants: between phases of one base every colour mixes on the smoothstep");
    check(eight.appearance == ThemeAppearance::dark && same_color(eight.base_low, 0x202020) &&
              nine.appearance == ThemeAppearance::light && same_color(nine.base_low, 0xf4f1ea),
          "theme variants: across a change of base the nearer phase shows whole, never a grey between");
    // From the 5th to the 30th the month turns toward the next one's colours, as Silk Waves' sky does, so the last
    // minute of March is April's look and the 1st of April continues from there.
    Theme turning = april;
    client_theme_mix(&turning, night, .648f); // the 20th at 00:00: x = 0.6 on the smoothstep
    const Theme fourth = look(0, 3, 4), twentieth = look(0, 3, 20), last = look(1439, 3, 31), first = look(0, 4, 1);
    const auto near = [](float a, float b) noexcept { return a - b < 1e-4f && b - a < 1e-4f; };
    check(bytes_equal(&fourth, &night, __builtin_offsetof(Theme, scale)),
          "theme variants: a month holds its colours until the 5th");
    check(near(twentieth.accent.green, turning.accent.green) && near(twentieth.accent.red, turning.accent.red) &&
              twentieth.accent.green < night.accent.green && twentieth.accent.green > april.accent.green,
          "theme variants: from the 5th to the 30th a month turns toward the next on a smoothstep");
    check(same_color(last.base_low, 0x050608) && near(last.accent.red, first.accent.red) &&
              near(last.accent.green, first.accent.green) && near(last.accent.blue, first.accent.blue),
          "theme variants: a month meets the next month's colours at its end");
    // The base flip drives the effects' appearance: it glides from night to day with the theme, whatever Windows
    // says, and reduced motion settles it at once.
    EffectAmbient ambient{};
    float values[4]{};
    EffectAmbientSources sources{{2026, 3, 1, 8, 0}, eight.appearance == ThemeAppearance::dark, true, true};
    effect_ambient_sample(&ambient, effect_use_appearance, sources, 1000, true, false, values);
    const bool before_flip = values[effect_ambient_dark] == 1;
    sources.theme_dark = nine.appearance == ThemeAppearance::dark;
    effect_ambient_sample(&ambient, effect_use_appearance, sources, 2000, true, false, values);
    effect_ambient_sample(&ambient, effect_use_appearance, sources, 2000 + kEffectAmbientEaseMs / 2, true, false, values);
    const float halfway_dark = values[effect_ambient_dark];
    effect_ambient_sample(&ambient, effect_use_appearance, sources, 2000 + kEffectAmbientEaseMs, true, false, values);
    const bool day = values[effect_ambient_dark] == 0;
    sources.theme_dark = true;
    effect_ambient_sample(&ambient, effect_use_appearance, sources, 9000, false, false, values);
    check(before_flip && halfway_dark > .45f && halfway_dark < .55f && day && values[effect_ambient_dark] == 1,
          "theme variants: the base flip glides the effects' dark from 1 to 0 under a dark Windows; reduced motion snaps");

    // A theme following its effect's knobs (background.follow) reads and writes them; while the effect draws, a held
    // month or phase replaces the clock's, and the knobs' automatic options (or the effect not drawing, -1) keep it.
    const auto linked_source = utf8_literal(
        "{\"version\":\"2.0\",\"name\":\"Linked\",\"background\":{\"effect\":\"silk-waves\",\"follow\":{\"month\":"
        "\"month\",\"phase\":\"sky\",\"phases\":[\"auto\",\"auto\",\"day\",\"night\"]}},\"variants\":{"
        "\"night\":{\"colors\":{\"base_low\":\"#050608\"},\"months\":[{},{},{\"accent\":\"#ff8040\"},{},{},{},{},{},{},{},"
        "{},{}]},\"day\":{\"base\":\"light\",\"colors\":{\"base_low\":\"#f4f1ea\"}}}}");
    ClientThemeDocument linked{}, linked_again{};
    u32 linked_bytes{};
    const auto& follow = linked.effect;
    check(parse(linked_source, defaults, &linked).ok() && bytes_equal(follow.month_knob, "month", 6) &&
              bytes_equal(follow.phase_knob, "sky", 4) && !follow.phases[0] && !follow.phases[1] &&
              follow.phases[2] == theme_phase_day + 1 && follow.phases[3] == theme_phase_night + 1 && !follow.phases[4] &&
              client_theme_document_write(linked, {exported, sizeof(exported)}, &linked_bytes).ok() &&
              contains(exported, linked_bytes,
                       utf8_literal(",\"follow\":{\"month\":\"month\",\"phase\":\"sky\",\"phases\":[\"auto\",\"auto\","
                                    "\"day\",\"night\"]}")) &&
              parse({exported, linked_bytes}, defaults, &linked_again).ok() &&
              client_theme_document_equal(linked, linked_again),
          "theme follow: the effect's month and phase knobs and the phase each option holds read and write back equal");
    const auto linked_with = [](const char* follow_member) noexcept {
        static char text[256];
        constexpr auto head = utf8_literal("{\"version\":\"2.0\",\"background\":{\"effect\":\"silk-waves\",\"follow\":");
        copy_bytes(text, head.data, head.bytes);
        const u32 length = c_string_length(follow_member);
        copy_bytes(text + head.bytes, follow_member, length);
        text[head.bytes + length] = '}';
        text[head.bytes + length + 1] = '}';
        return Utf8View{text, head.bytes + length + 2};
    };
    check(refused(linked_with("{\"phases\":[\"noon\"]}"), defaults) && refused(linked_with("{\"month\":3}"), defaults) &&
              refused(linked_with("{\"month\":\"\"}"), defaults) && refused(linked_with("[]"), defaults) &&
              refused(linked_with("{\"phases\":[\"auto\",\"auto\",\"auto\",\"auto\",\"auto\",\"auto\",\"auto\","
                                  "\"auto\",\"auto\",\"auto\",\"auto\",\"auto\",\"auto\",\"auto\",\"auto\",\"auto\","
                                  "\"day\"]}"),
                      defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"effect\":\"stars\",\"follow\":{\"month\":"
                                   "\"month\"}}}"),
                      defaults),
          "theme follow: knob names, phase names, sixteen options and a package effect are checked");
    const auto shows = [&](ClientThemeClock clock) noexcept {
        return client_theme_document_at(linked, clock.minute, clock.month, clock.day);
    };
    const auto same = [](const ClientThemeClock& a, const ClientThemeClock& b) noexcept {
        return a.minute == b.minute && a.month == b.month && a.day == b.day;
    };
    const ClientThemeClock at_midnight{0, 4, 20}, at_noon{12 * 60, 4, 20};
    const ClientThemeClock held_march = client_theme_follow(follow, at_midnight, 3, 0);
    const ClientThemeClock held_day = client_theme_follow(follow, at_midnight, 0, 2);
    const ClientThemeClock held_night = client_theme_follow(follow, at_noon, 0, 3);
    const auto keeps = [&](float month, float phase) noexcept {
        return same(client_theme_follow(follow, at_noon, month, phase), at_noon);
    };
    check(keeps(-1, -1) && keeps(0, 0) && keeps(0, 1) && keeps(13, 4) &&
              shows(at_noon).appearance == ThemeAppearance::light && shows(at_midnight).appearance == ThemeAppearance::dark,
          "theme follow: the automatic options, the effect not drawing and values out of range keep the schedule");
    check(same(held_march, {0, 3, 1}) && same_color(shows(held_march).accent, 0xff8040) &&
              !same_color(shows(at_midnight).accent, 0xff8040),
          "theme follow: a month knob holds that month's colours, without the turn toward the next");
    check(same(held_day, {12 * 60, 4, 20}) && shows(held_day).appearance == ThemeAppearance::light &&
              same_color(shows(held_day).base_low, 0xf4f1ea) && same(held_night, {0, 4, 20}) &&
              shows(held_night).appearance == ThemeAppearance::dark && same_color(shows(held_night).base_low, 0x050608),
          "theme follow: Day and Night hold their phase whole, its base included, whatever the hour");
}
// MAICORD_THEME_CHECK_FILE may name a theme with variants (the catalog's Silk Waves theme): it reads with no member
// skipped, its canonical form (what themes.bin stores) fits and reads back equal, and every quarter hour, blends
// included, passes MaiCord's contrast warnings, with muted text at 4.5:1 or better on every surface.
void test_theme_file(void (*check)(bool, const char*) noexcept) noexcept {
    wchar_t path[600];
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_THEME_CHECK_FILE", path, 600);
    if (!length || length >= 600)
        return;
    static char text[kClientThemeDocumentMaximumBytes];
    DWORD bytes = 0;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    const bool read = file != INVALID_HANDLE_VALUE && ReadFile(file, text, sizeof(text), &bytes, nullptr);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    ClientThemeDocument document{}, stored{};
    const auto result = parse({text, bytes}, client_theme_document_defaults(), &document);
    u32 canonical = 0;
    static u32 structurals[kClientThemeDocumentMaximumBytes]; // one per byte, as themes.bin's reader has
    check(read && result.ok() && !result.unsupported_fields && client_theme_document_scheduled(document) &&
              client_theme_document_write(document, {exported, sizeof(exported)}, &canonical).ok() &&
              client_theme_document_parse({exported, canonical}, client_theme_document_defaults(),
                                          {structurals, kClientThemeDocumentMaximumBytes}, &stored)
                  .ok() &&
              client_theme_document_equal(document, stored),
          "theme file: reads whole, and its canonical form fits and reads back equal");
    bool ok = result.ok() && client_theme_document_scheduled(document);
    // Every quarter hour on the 1st, 12th, 20th and 31st: the phases, their mixes and the turn toward next month.
    constexpr u32 kDays[]{1, 12, 20, 31};
    u32 looks = 0;
    for (u32 month = 1; ok && month <= 12; ++month)
        for (const u32 day : kDays)
            for (u32 minute = 0; ok && minute < 1440; minute += 15, ++looks) {
                Theme theme = client_theme_document_at(document, minute, month, day);
                ui_theme_resolve_emphasis(&theme);
                const auto& t = theme.tokens;
                ok = !ui_theme_contrast_warnings(theme, nullptr, 0);
                const Color* const surfaces[]{&theme.base_lowest, &theme.base_lower, &theme.base_low, &theme.surface,
                                              &theme.surface_high};
                for (const Color* surface : surfaces)
                    ok = ok && ui_theme_contrast_ratio(t.text.normal, *surface) >= 4.5f &&
                         ui_theme_contrast_ratio(t.text.muted, *surface) >= 4.5f;
            }
    check(ok && looks == 12 * 4 * 96, "theme file: every quarter hour on four days of every month passes the contrast "
                                      "checks, muted text 4.5:1 on every surface");
}
} // namespace
void test_client_theme_document(void (*check)(bool, const char*) noexcept) noexcept {
    auto defaults = client_theme_document_defaults();
    check(client_theme_document_valid(defaults) && defaults.base == ClientThemeBase::dark &&
              defaults.theme.appearance == ThemeAppearance::dark,
          "theme document: defaults are complete typed dark data with no runtime style pointer");
    defaults.theme.accent = {.125f, .25f, .375f, .5f};
    defaults.theme = ui_theme_resolve_defaults(defaults.theme); // semantic tokens follow core colours
    defaults.typography.body_size = 18;
    defaults.metrics.message_left = 22;
    ClientThemeDocument output{};
    const auto partial =
        parse(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"text_primary\":[1,0,0,1]},"
                           "\"typography\":{\"code_face\":\"serif\"},\"metrics\":{\"control_radius\":9}}"),
              defaults, &output);
    check(partial.ok() && !partial.unsupported_fields && output.theme.text_primary.red == 1 &&
              output.theme.text_primary.green == 0 && output.theme.accent.alpha == .5f &&
              output.typography.body_size == 18 && output.typography.code_face == UiTextFace::serif &&
              output.metrics.message_left == 22 && output.metrics.control_radius == 9 &&
              output.theme.appearance == ThemeAppearance::dark,
          "theme document: partial same-base fields inherit caller values and authored red text cannot "
          "choose appearance");
    const auto changed = parse(utf8_literal("{\"colors\":{\"text_primary\":[1,0,0,1]},"
                                            "\"metrics\":{\"message_left\":20},\"base\":\"light\","
                                            "\"version\":\"2.0\",\"name\":\"Lumi\\u00e8re\"}"),
                               defaults, &output);
    check(changed.ok() && output.base == ClientThemeBase::light &&
              output.theme.appearance == ThemeAppearance::light &&
              output.theme.surface.red == kLightTheme.surface.red &&
              output.theme.accent.alpha == kLightTheme.accent.alpha && output.theme.text_primary.red == 1 &&
              output.typography.body_size == 16 && output.metrics.message_left == 20 &&
              output.name_bytes == 8 && output.name[4] == static_cast<char>(0xc3),
          "theme document: changed base resets built-in inheritance before out-of-order overrides and owns "
          "decoded UTF8 name");
    const auto future =
        parse(utf8_literal(
                  "{\"version\":\"2.0\",\"future\":{\"nested\":[1,true,null]},"
                  "\"colors\":{\"future_surface\":{\"value\":4}},\"metrics\":{\"future_gap\":100},"
                  "\"typography\":{\"future_face\":\"custom\"},\"background\":{\"future_effect\":false}}"),
              defaults, &output);
    check(
        future.ok() && future.unsupported_fields == 5 && client_theme_document_equal(output, defaults),
        "theme document: valid future tokens are counted and inert while preserving current typed values");
    check(refused(utf8_literal("{\"version\":\"2.0\",\"future\":{\"value\":[1,]}}"), defaults),
          "theme document: inert future values still require complete valid JSON");
    check(refused(utf8_literal("{\"name\":\"Missing\"}"), defaults) &&
              refused(utf8_literal("{\"version\":2}"), defaults) &&
              refused(utf8_literal("{\"version\":1,\"colors\":{\"accent\":[0,0,0,1]}}"), defaults) &&
              refused(utf8_literal("{\"version\":\"1\"}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"version\":\"2.0\"}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"colors\":{},\"colors\":{}}"), defaults),
          "theme document: schema version types (including the retired integer version 1) missing fields and duplicate known roots fail atomically");
    check(refused(
              utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":[0,0,0,1],\"\\u0061ccent\":[1,1,1,1]}}"),
              defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":[0,0,1]}}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":[0,0,0,1,1]}}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":[1.1,0,0,1]}}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":[false,0,0,1]}}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"colors\":{\"accent\":[1e309,0,0,1]}}"), defaults),
          "theme document: escaped semantic duplicates exact RGBA arity scalar types range and nonfinite "
          "conversion are rejected");
    check(
        refused(utf8_literal("{\"version\":\"2.0\",\"typography\":{\"body_size\":11}}"), defaults) &&
            refused(utf8_literal("{\"version\":\"2.0\",\"typography\":{\"body_face\":\"downloaded\"}}"),
                    defaults) &&
            refused(utf8_literal(
                        "{\"version\":\"2.0\",\"typography\":{\"heading_1_ratio\":1,\"heading_2_ratio\":2}}"),
                    defaults) &&
            refused(utf8_literal("{\"version\":\"2.0\",\"metrics\":{\"audio_minimum_width\":300,\"audio_maximum_"
                                 "width\":200}}"),
                    defaults) &&
            refused(utf8_literal("{\"version\":\"2.0\",\"metrics\":{\"control_radius\":1,\"control_radius\":2}}"),
                    defaults),
        "theme document: font catalog finite em ranges geometry relationships and repeated metric keys are "
        "strict");
    check(refused(utf8_literal(
                      "{\"version\":\"2.0\",\"metrics\":{\"menu_compact_height\":56,\"menu_row_height\":32}}"),
                  defaults) &&
              refused(
                  utf8_literal(
                      "{\"version\":\"2.0\",\"metrics\":{\"form_field_padding\":24,\"form_single_height\":40}}"),
                  defaults),
          "theme document: individually bounded component sizes must also satisfy full geometry "
          "relationships");
    check(parse(utf8_literal(
                    "{\"version\":\"2.0\",\"typography\":{\"heading_3_ratio\":1.7,"
                    "\"heading_2_ratio\":1.8,\"heading_1_ratio\":2},\"metrics\":{\"menu_row_height\":60,"
                    "\"menu_detail_height\":80}}"),
                defaults, &output)
              .ok(),
          "theme document: cross-field typography and geometry validation happens after all overrides "
          "independent of order");
    check(refused(utf8_literal("{\"version\":\"2.0\",\"name\":\"Bad\\nName\"}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"name\":\"\\u0000\"}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"name\":\"\\ud800\"}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"name\":\"\"}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\"} trailing"), defaults),
          "theme document: empty control invalid Unicode names and incomplete documents preserve output");
    constexpr auto bad_utf8 = utf8_literal("{\"version\":\"2.0\",\"name\":\"\xc0\xaf\"}");
    check(refused(bad_utf8, defaults),
          "theme document: raw invalid UTF8 is rejected before owned name publication");
    check(parse(utf8_literal("{\"version\":\"2.0\",\"background\":{\"colors\":[1122867,4478310],"
                             "\"angle_degrees\":360,\"intensity\":80,\"effect\":\"stars_and_comets\","
                             "\"speed\":20,\"strength\":30,\"density\":40}}"),
                defaults, &output)
                  .ok() &&
              output.theme.background.color_count == 2 && output.theme.background.colors[1] == 4478310 &&
              output.theme.background.effect == BackgroundEffect::stars_and_comets,
          "theme document: explicit bounded gradient palette direction and selected effect settings are "
          "typed");
    ClientThemeDocument removed{};
    bool removed_effects = true;
    constexpr Utf8View removed_names[]{
        utf8_literal("{\"version\":\"2.0\",\"background\":{\"effect\":\"aurora\"}}"),
        utf8_literal("{\"version\":\"2.0\",\"background\":{\"effect\":\"dust\"}}"),
        utf8_literal("{\"version\":\"2.0\",\"background\":{\"effect\":\"black_hole\"}}")};
    for (const auto name : removed_names)
        removed_effects = removed_effects && parse(name, defaults, &removed).ok() &&
                          removed.theme.background.effect == BackgroundEffect::none;
    check(removed_effects, "theme document: a removed effect name falls back to no effect instead of "
                           "rejecting the document");
    // A package effect: the slug, its knob values and the universal settings, kept through the canonical writer.
    ClientThemeDocument packaged{};
    const bool packaged_ok =
        parse(utf8_literal("{\"version\":\"2.0\",\"background\":{\"effect\":\"rain-2\",\"knobs\":{\"cell\":18.5,"
                           "\"mirror\":true,\"tint\":\"#1AFF59\"},\"universal\":{\"speed\":1.5,\"resolution\":75,"
                           "\"fps\":30,\"opacity\":0.5,\"blend\":\"screen\"}}}"),
              defaults, &packaged)
            .ok() &&
        packaged.effect.slug[0] == 'r' && packaged.effect.knob_count == 3 && packaged.effect.knobs[0].number == 18.5f &&
        packaged.effect.knobs[1].number == 1.f && packaged.effect.knobs[2].color &&
        packaged.effect.knobs[2].rgb == 0x1aff59 && packaged.effect.speed == 30 && packaged.effect.resolution == 4 &&
        packaged.effect.frame_rate == 4 && packaged.effect.opacity == 50 && packaged.effect.blend == 1 &&
        packaged.theme.background.effect == BackgroundEffect::none;
    u32 packaged_bytes = 0;
    ClientThemeDocument reread{};
    const bool packaged_round_trip =
        packaged_ok && client_theme_document_write(packaged, {exported, kClientThemeDocumentMaximumBytes}, &packaged_bytes).ok() &&
        parse({exported, packaged_bytes}, defaults, &reread).ok() && client_theme_document_equal(packaged, reread);
    check(packaged_ok && packaged_round_trip,
          "theme document: a package effect slug, knob values and universal settings parse and survive a rewrite");
    check(refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"effect\":\"rain\",\"knobs\":{\"a\":1,\"a\":2}}}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"universal\":{\"fps\":29}}}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"universal\":{\"speed\":4}}}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"knobs\":{\"c\":\"#12\"}}}"), defaults) &&
              refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"knobs\":[1]}}"), defaults),
          "theme document: duplicate knobs, off-table universal values and malformed knob values are refused");
    check(
        refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"effect\":3}}"), defaults) &&
            refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"speed\":101}}"), defaults) &&
            refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"colors\":[16777216]}}"), defaults) &&
            refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"colors\":[1,2,3,4,5,6]}}"), defaults) &&
            refused(utf8_literal("{\"version\":\"2.0\",\"background\":{\"colors\":[],\"angle_degrees\":1}}"),
                    defaults),
        "theme document: effects and palettes have exact bounded native domains without script or asset "
        "inputs");
    u32 length{};
    const auto measured = client_theme_document_write(output, {}, &length);
    u32 bytes{};
    const auto written = client_theme_document_write(output, {exported, sizeof(exported)}, &bytes);
    ClientThemeDocument roundtrip{};
    const auto decoded = parse({exported, bytes}, client_theme_document_defaults(), &roundtrip);
    u32 rewritten_bytes{};
    const auto canonical =
        client_theme_document_write(roundtrip, {rewritten, sizeof(rewritten)}, &rewritten_bytes);
    check(measured.ok() && length == bytes && written.ok() && decoded.ok() && canonical.ok() &&
              client_theme_document_equal(output, roundtrip) && bytes == rewritten_bytes &&
              bytes_equal(exported, rewritten, bytes),
          "theme document: complete resolved export roundtrips every semantic field into stable canonical "
          "UTF8 JSON");
    const auto colors = ui_theme_color_descriptors();
    const auto metrics = ui_theme_metric_descriptors();
    bool coverage = colors.count == 20 && metrics.count == 51;
    for (u32 i = 0; i < colors.count; ++i) {
        const auto* color = ui_theme_color_get(roundtrip.theme, colors[i].id);
        coverage = coverage && color && color->red == ui_theme_color_get(output.theme, colors[i].id)->red;
    }
    for (u32 i = 0; i < metrics.count; ++i) {
        float a{}, b{};
        coverage = coverage && ui_theme_metric_get(output.metrics, metrics[i].id, &a) &&
                   ui_theme_metric_get(roundtrip.metrics, metrics[i].id, &b) && a == b;
    }
    check(coverage, "theme document: portable serializer includes all20 palette tokens and all51 native "
                    "spacing metrics");
    bool faces = true;
    for (u32 i = 0; i < static_cast<u32>(UiTextFace::count); ++i) {
        auto source = defaults;
        source.typography.body_face = source.typography.code_face = static_cast<UiTextFace>(i);
        u32 count{};
        auto destination = defaults;
        faces = faces && client_theme_document_write(source, {exported, sizeof(exported)}, &count).ok() &&
                parse({exported, count}, defaults, &destination).ok() &&
                client_theme_document_equal(source, destination);
    }
    check(faces, "theme document: all21 existing font faces are portable owned data and roundtrip exactly");
    auto fractional = defaults;
    u32 smallest_bits = 1;
    copy_bytes(&fractional.theme.accent.red, &smallest_bits, sizeof(smallest_bits));
    fractional.theme.accent.green = .1f;
    fractional.theme.accent.blue = .99999994f;
    u32 exact_bytes{};
    check(client_theme_document_write(fractional, {exported, sizeof(exported)}, &exact_bytes).ok() &&
              parse({exported, exact_bytes}, defaults, &roundtrip).ok() &&
              client_theme_document_equal(fractional, roundtrip),
          "theme document: finite float colors including subnormal values retain exact typed values "
          "without fixed-precision loss");
    char small[8]{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    const char small_before[8]{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    u32 unchanged = 123;
    check(client_theme_document_write(defaults, {small, sizeof(small)}, &unchanged).error ==
                  ClientThemeDocumentError::capacity &&
              unchanged == 123 && bytes_equal(small, small_before, sizeof(small)),
          "theme document: insufficient export capacity preserves destination and reported byte count");
    auto nonfinite = defaults;
    const u32 nan_bits = 0x7fc00000;
    copy_bytes(&nonfinite.theme.accent.red, &nan_bits, sizeof(nan_bits));
    check(client_theme_document_write(nonfinite, {small, sizeof(small)}, &unchanged).error ==
                  ClientThemeDocumentError::invalid_input &&
              unchanged == 123 && bytes_equal(small, small_before, sizeof(small)),
          "theme document: nonfinite authored data cannot serialize or partially overwrite export output");
    auto preserved = defaults;
    const auto before = preserved;
    u32 tiny_scratch[1]{};
    check(client_theme_document_parse(utf8_literal("{\"version\":\"2.0\"}"), defaults, {tiny_scratch, 1},
                                      &preserved)
                      .error == ClientThemeDocumentError::capacity &&
              bytes_equal(&preserved, &before, sizeof(preserved)),
          "theme document: caller-owned index overflow is bounded and preserves complete output");
    check(!client_theme_document_parse({exported, kClientThemeDocumentMaximumBytes + 1}, defaults,
                                       {scratch, 4096}, &preserved)
               .ok(),
          "theme document: oversized files are refused before parsing or touching scratch");
    char long_name[160]{};
    constexpr auto name_prefix = utf8_literal("{\"version\":\"2.0\",\"name\":\"");
    copy_bytes(long_name, name_prefix.data, name_prefix.bytes);
    for (u32 i = 0; i < kClientThemeNameMaximumBytes + 1; ++i)
        long_name[name_prefix.bytes + i] = 'x';
    long_name[name_prefix.bytes + kClientThemeNameMaximumBytes] = '"';
    long_name[name_prefix.bytes + kClientThemeNameMaximumBytes + 1] = '}';
    check(
        parse({long_name, name_prefix.bytes + kClientThemeNameMaximumBytes + 2}, defaults, &output).ok() &&
            output.name_bytes == kClientThemeNameMaximumBytes,
        "theme document: owned UTF8 name accepts exact128-byte capacity");
    long_name[name_prefix.bytes + kClientThemeNameMaximumBytes] = 'x';
    long_name[name_prefix.bytes + kClientThemeNameMaximumBytes + 1] = '"';
    long_name[name_prefix.bytes + kClientThemeNameMaximumBytes + 2] = '}';
    check(refused({long_name, name_prefix.bytes + kClientThemeNameMaximumBytes + 3}, defaults),
          "theme document: over-capacity names cannot truncate or partially publish");
    auto in_place = defaults;
    check(parse(utf8_literal("{\"version\":\"2.0\",\"metrics\":{\"control_radius\":7}}"), in_place, &in_place)
                  .ok() &&
              in_place.metrics.control_radius == 7,
          "theme document: in-place apply copies defaults before atomic candidate publication");
    UiResolvedThemeStyles live_styles{};
    auto live_defaults = defaults;
    live_defaults.theme.styles = &live_styles;
    live_defaults.theme.style_revision = 987;
    check(
        parse(utf8_literal("{\"version\":\"2.0\"}"), live_defaults, &output).ok() && !output.theme.styles &&
            !output.theme.style_revision && client_theme_document_equal(output, defaults),
        "theme document: parser strips runtime style references and revision counters from inherited data");
    test_variants(defaults, check);
    test_theme_file(check);
}
} // namespace maicord
