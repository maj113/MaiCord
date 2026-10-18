#include "maicord/app/client_theme_document.hpp"
#include "maicord/core/memory.hpp"

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
    const char bad_utf8[]{'{',
                          '"',
                          'v',
                          'e',
                          'r',
                          's',
                          'i',
                          'o',
                          'n',
                          '"',
                          ':',
                          '"',
                          '2',
                          '.',
                          '0',
                          '"',
                          ',',
                          '"',
                          'n',
                          'a',
                          'm',
                          'e',
                          '"',
                          ':',
                          '"',
                          static_cast<char>(0xc0),
                          static_cast<char>(0xaf),
                          '"',
                          '}'};
    check(refused({bad_utf8, sizeof(bad_utf8)}, defaults),
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
}
} // namespace maicord
