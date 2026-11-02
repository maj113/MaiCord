#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "../../support/ui_scene_fixtures.hpp"
#include "maicord/ui/settings/schema.hpp"
#include "../../support/settings_schema_validation.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/widgets/button.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/theme/styles.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include <stddef.h>

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool close_to(float a, float b) noexcept {
    return a - b < .01f && b - a < .01f;
}
bool resolve_palette(void*, u64, u32 choice, BackgroundTheme* output) noexcept {
    *output = background_theme_defaults();
    output->angle_degrees = static_cast<u16>(choice * 37);
    return true;
}
// Flat themes as the host reports them: one repeated stop per theme.
constexpr u32 kFlatThemes[]{0xF2F3F5, 0x313338, 0x000000};
bool resolve_flat(void*, u64, u32 choice, BackgroundTheme* output) noexcept {
    if (choice >= 3)
        return false;
    *output = background_theme_defaults();
    output->color_count = 2;
    output->colors[0] = output->colors[1] = kFlatThemes[choice];
    output->colors[2] = output->colors[3] = output->colors[4] = 0;
    return true;
}
UiPoint center(UiRect rect) noexcept {
    return {(rect.left + rect.right) * .5f, (rect.top + rect.bottom) * .5f};
}

// Rings share their shape's centre of curvature: the stroke's inner edge sits
// the gap outside the shape with radius + gap.
bool concentric(const PaintQuad& ring, UiRect shape, float radius, float gap, float width) noexcept {
    const float out = gap + width;
    return close_to(ring.left, shape.left - out) && close_to(ring.top, shape.top - out) &&
           close_to(ring.right, shape.right + out) && close_to(ring.bottom, shape.bottom + out) &&
           close_to(ring.radius, radius + out) && close_to(ring.border_width, width) &&
           close_to(ring.radius - ring.border_width, radius + gap);
}
void test_control_rings(Check check, const UiSettingsTokens& tokens, PaintStream* paint) noexcept {
    const auto& m = tokens.metric;
    const UiRect track{100, 100, 100 + m.switch_width, 100 + m.switch_height};
    check(paint_stream_reset(paint, 1000, 1000) &&
              ui_switch_paint(paint, tokens, track, true, 1, false, {true, false, false, true}) && paint->count == 3 &&
              concentric(paint->quads[2], track, m.switch_height * .5f, m.focus_gap, m.focus_width),
          "settings controls: the switch focus ring is concentric with the track");
    const UiRect swatch{200, 200, 200 + m.swatch, 200 + m.swatch};
    check(paint_stream_reset(paint, 1000, 1000) &&
              ui_swatch_paint(paint, tokens, swatch, {.2f, .3f, .4f, 1}, nullptr, true, {true, false, false, true}) &&
              paint->count >= 4 &&
              concentric(paint->quads[2], swatch, m.control_radius, m.ring_gap, m.ring_width) &&
              concentric(paint->quads[3], swatch, m.control_radius, m.ring_gap + m.ring_width + m.focus_gap,
                         m.focus_width),
          "settings controls: swatch selection and focus rings are concentric with gaps between them");
    const UiRect cell{300, 300, 300 + m.color_cell, 300 + m.color_cell};
    check(paint_stream_reset(paint, 1000, 1000) &&
              ui_color_grid_paint(paint, tokens, cell, {1, 1, 1, 1}, true, {true, false, false, true}) &&
              paint->count >= 3 && concentric(paint->quads[1], cell, m.control_radius * .5f, m.ring_gap, m.ring_width) &&
              concentric(paint->quads[2], cell, m.control_radius * .5f, m.ring_gap + m.ring_width, m.focus_width),
          "settings controls: colour cells ring their selection and focus concentrically");
    const UiRect indicator{0, 0, m.radio_size, m.radio_size};
    check(paint_stream_reset(paint, 1000, 1000) &&
              ui_radio_paint(paint, tokens, indicator, true, {true, false, true, false}) && paint->count == 2 &&
              close_to(paint->quads[1].radius, m.radio_size * .5f - (paint->quads[1].left - indicator.left)) &&
              close_to(paint->quads[1].left - indicator.left, indicator.right - paint->quads[1].right),
          "settings controls: the selected radio dot is centred and concentric with its ring");
    const UiRect field{0, 0, 300, m.control_height};
    check(paint_stream_reset(paint, 1000, 1000) &&
              ui_input_shell_paint(paint, tokens, field, {true, false, false, true}, true) && paint->count == 2 &&
              paint->quads[1].border_width == 2 && paint->quads[1].border.red == tokens.color.critical_text.red &&
              paint_stream_reset(paint, 1000, 1000) &&
              ui_input_shell_paint(paint, tokens, field, {false, true, false, true}) && paint->count == 1,
          "settings controls: focused fields keep their error outline; disabled fields draw no focus");
}

void test_slider_input(Check check, const UiSettingsTokens& tokens, PaintStream* paint) noexcept {
    check(ui_slider_page_step(201) == 20 && ui_slider_page_step(361) == 36 && ui_slider_page_step(101) == 10 &&
              ui_slider_page_step(5) == 1 && ui_slider_page_step(2) == 1 && ui_slider_page_step(0) == 1,
          "settings slider: Page keys move a tenth of the range, at least one step");
    const UiRect bounds{100, 0, 420, ui_slider_height(tokens, false)};
    const auto g = ui_slider_geometry(tokens, bounds, 201, 50, false);
    const float center = (g.grabber.left + g.grabber.right) * .5f, middle = (g.grabber.top + g.grabber.bottom) * .5f;
    check(close_to(ui_slider_grab_offset(g.grabber, center + 5, middle), 5) &&
              close_to(ui_slider_grab_offset(g.grabber, center - 6, middle), -6) &&
              ui_slider_grab_offset(g.grabber, center + 40, middle) == 0 &&
              ui_slider_index_at(g, 201, center + 5 - ui_slider_grab_offset(g.grabber, center + 5, middle)) == 50,
          "settings slider: a press on the grabber keeps its offset so the value does not jump");
    const auto& m = tokens.metric;
    const auto grown = [&](UiControlState state) noexcept {
        if (!paint_stream_reset(paint, 1000, 1000) || !ui_slider_paint(paint, tokens, g, 201, kUiSettingsNoChoice, state))
            return -1.f;
        // The grabber is the last quad before any focus ring.
        const auto& grabber = paint->quads[state.focused ? paint->count - 2 : paint->count - 1];
        return grabber.right - grabber.left;
    };
    check(close_to(grown({true, false, false, false}), m.slider_grabber) &&
              close_to(grown({true, true, false, false}), m.slider_grabber + m.slider_grabber_grow) &&
              close_to(grown({true, false, true, false}), m.slider_grabber + m.slider_grabber_grow) &&
              close_to(grown({true, false, false, true}), m.slider_grabber + m.slider_grabber_grow) &&
              close_to(grown({false, true, false, false}), m.slider_grabber) &&
              close_to(paint->quads[0].bottom - paint->quads[0].top, m.slider_bar),
          "settings slider: hover, drag and focus grow the grabber and bar; disabled sliders stay at rest");
    // A host with its own slider sizes (the media volume) grows from them.
    auto small_tokens = tokens;
    small_tokens.metric.slider_grabber = 12;
    const auto small = ui_slider_geometry(small_tokens, bounds, 201, 50, false);
    check(paint_stream_reset(paint, 1000, 1000) &&
              ui_slider_paint(paint, small_tokens, small, 201, kUiSettingsNoChoice, {true, true, false, false}) &&
              close_to(paint->quads[paint->count - 1].right - paint->quads[paint->count - 1].left,
                       12 + m.slider_grabber_grow),
          "settings slider: hover growth follows an overridden grabber size");
    const auto marked = ui_slider_geometry(tokens, {100, 0, 420, ui_slider_height(tokens, true)}, 5, 2, true);
    const float bar_middle = (marked.bar.top + marked.bar.bottom) * .5f;
    u32 ticks = 0, first = 0, last = 0;
    bool centred = paint_stream_reset(paint, 1000, 1000) &&
                   ui_slider_paint(paint, tokens, marked, 5, 1, {true, false, false, false});
    for (u32 i = 0; centred && i < paint->count; ++i) {
        const auto& q = paint->quads[i];
        if (!close_to(q.right - q.left, 2) || q.bottom - q.top <= m.slider_bar)
            continue;
        centred = close_to((q.top + q.bottom) * .5f, bar_middle);
        first = ticks++ ? first : i;
        last = i;
    }
    check(centred && ticks == 5 && first > 0 && last + 1 < paint->count,
          "settings slider: stop marks centre on the track, over it and under the grabber");
}

void test_color_hex(Check check) noexcept {
    u32 rgba = 0;
    const auto parse = [&](const char* text, bool alpha) noexcept {
        const u32 bytes = c_string_length(text);
        rgba = 0x12345678;
        return color_hex_parse({text, bytes}, alpha, &rgba);
    };
    check(parse("#5865F2", false) && rgba == 0x5865F2FFu && parse("5865f2", false) && rgba == 0x5865F2FFu &&
              parse("#58f", false) && rgba == 0x5588FFFFu && parse("abc", false) && rgba == 0xAABBCCFFu,
          "colour hex: #RRGGBB and #RGB parse with or without the '#', in either case");
    check(parse("#5865F280", true) && rgba == 0x5865F280u && parse("#58f8", true) && rgba == 0x5588FF88u &&
              !parse("#5865F280", false) && rgba == 0x12345678u && !parse("#58f8", false),
          "colour hex: #RRGGBBAA and #RGBA parse only where the input takes alpha");
    check(!parse("", false) && !parse("#", false) && !parse("#12", false) && !parse("#12345", false) &&
              !parse("#1234567", true) && !parse("#12345G", false) && !parse(" #123456", false) &&
              !parse("##123456", false) && rgba == 0x12345678u,
          "colour hex: other lengths, non-hex digits and spaces are rejected and leave the value");
    char text[10]{};
    check(color_hex_format(0x0A0B0CFFu, true, {text, 10}) == 7 && bytes_equal(text, "#0A0B0C", 7) &&
              color_hex_format(0x0A0B0C80u, true, {text, 10}) == 9 && bytes_equal(text, "#0A0B0C80", 9) &&
              color_hex_format(0x0A0B0C80u, false, {text, 10}) == 7 && !color_hex_format(0x0A0B0C80u, true, {text, 8}),
          "colour hex: values format as #RRGGBB, with alpha digits only when translucent and allowed");
}

void test_controls(Check check) noexcept {
    const UiSettingsThemeTokens tokens(kDefaultTheme);
    const auto layout_tokens = ui_settings_layout_tokens(kDefaultTheme.styles);
    check(bytes_equal(&layout_tokens, static_cast<const UiSettingsLayoutTokens*>(&tokens),
                      sizeof(layout_tokens)),
          "settings controls: layout-only tokens preserve all geometry and motion values");
    // Every packed metric lane, including the unsigned high half and final tail.
    const u16 values[]{0, 1, 255, 256, 32767, 32768, 65535};
    bool exact = true;
    for (u32 shift = 0; shift < 7; ++shift) {
        UiResolvedThemeStyles varied{};
        varied.ready = true;
        auto* packed = reinterpret_cast<u8*>(&varied.settings) + offsetof(UiThemeSettings, modal_max_width);
        for (u32 i = 0; i < 57; ++i)
            copy_bytes(packed + i * sizeof(u16), &values[(i + shift) % 7], sizeof(u16));
        const auto resolved = ui_settings_layout_tokens(&varied);
        for (u32 i = 0; i < 57; ++i) {
            float actual;
            copy_bytes(&actual, reinterpret_cast<const u8*>(&resolved.metric) + i * sizeof(float), sizeof(float));
            const float expected = i == 1 ? static_cast<float>(values[(i + shift) % 7]) / 100
                                          : static_cast<float>(values[(i + shift) % 7]);
            exact = exact && bytes_equal(&actual, &expected, sizeof(float));
        }
        exact = exact && resolved.duration.switch_ms == varied.settings.switch_ms &&
                resolved.duration.flash_fade_ms == varied.settings.flash_fade_ms && resolved.metric.segment_size == 28;
    }
    check(exact, "settings controls: all packed metrics convert exactly without overwriting derived fields");
    // Settings and every dialog (Events included) share the official modal spring:
    // visually settled by half the token duration, complete at its end.
    const u32 enter = tokens.duration.modal_enter_ms;
    check(enter && ui_settings_modal_entrance(tokens.duration, 0) < .01f &&
              ui_settings_modal_entrance(tokens.duration, enter / 4) > .6f &&
              ui_settings_modal_entrance(tokens.duration, enter / 2) > .97f &&
              ui_settings_modal_entrance(tokens.duration, enter) == 1.f,
          "settings controls: modal and dialog entrances follow one spring timeline");
    const UiRect track{500 - tokens.metric.switch_width, 100 - tokens.metric.switch_height * .5f, 500,
                       100 + tokens.metric.switch_height * .5f};
    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 1000) &&
              ui_switch_paint(&paint, tokens, track, true, 1, false, {true, false, false, true}) && paint.count == 3,
          "settings controls: a focused switch paints track, thumb and focus ring");

    check(ui_settings_text_lines(utf8_literal("short"), 13, 400) == 1 &&
              ui_settings_text_lines(utf8_literal("A long description that certainly wraps onto more than one line"),
                                     13, 120) > 2 &&
              ui_settings_text_width(utf8_literal("mmmm"), 16) > ui_settings_text_width(utf8_literal("iiii"), 16),
          "settings controls: text estimates wrap by word and weight wide glyphs");

    // Chips reserve a colour dot only when asked (role lists), and leave room
    // for a whole chord label at the 13 DIP description size.
    const auto chord = utf8_literal("Ctrl+Alt+M");
    check(ui_admin_chip_width(tokens, chord, false) - tokens.metric.chip_padding * 2 >= 64 &&
              ui_admin_chip_width(tokens, chord, true) > ui_admin_chip_width(tokens, chord, false) &&
              close_to(ui_admin_chip_label_rect(tokens, {0, 0, 100, 20}, false).left, tokens.metric.chip_padding),
          "settings controls: dotless chips are symmetric and fit chord labels");
    test_control_rings(check, tokens, &paint);
    test_slider_input(check, tokens, &paint);
    test_color_hex(check);
    paint_stream_release(&paint);
}

constexpr Utf8View kThemes[]{utf8_literal("Light"), utf8_literal("Dark")};
constexpr UiSettingsSchemaNode kAppearance[]{
    {utf8_literal("appearance.theme"), utf8_literal("Theme"), utf8_literal("Light or dark."), utf8_literal("mode"),
     UiSettingsControlKind::palette, 0, 0, {kThemes, 2}, kUiSettingsNoChoice, 1, {}},
    {utf8_literal("appearance.messages"), utf8_literal("Messages"), {}, {}, UiSettingsControlKind::category, 0, 0,
     {}, kUiSettingsNoChoice, 0, {}},
    {utf8_literal("appearance.embeds"), utf8_literal("Show embeds"), utf8_literal("Link previews in chat."),
     utf8_literal("preview"), UiSettingsControlKind::toggle, 0, 0, {}, kUiSettingsNoChoice, 2, {}},
};
constexpr UiSettingsSchemaNode kAdvanced[]{
    {utf8_literal("advanced.developer"), utf8_literal("Developer mode"), utf8_literal("Copy ID in menus."),
     utf8_literal("debug"), UiSettingsControlKind::toggle, 0, 0, {}, kUiSettingsNoChoice, 3, {}},
    {utf8_literal("advanced.notice"), utf8_literal("Couldn't save"), {}, {}, UiSettingsControlKind::notice, 0, 0,
     {}, kUiSettingsNoChoice, 4, {}},
};
constexpr UiSettingsSchemaSection kSections[]{{utf8_literal("APP SETTINGS")}, {{}}};
constexpr UiSettingsSchemaPanel kPanels[]{
    {utf8_literal("appearance"), utf8_literal("Appearance"), utf8_literal("look"), UiIcon::palette, 0, {kAppearance, 3}},
    {utf8_literal("advanced"), utf8_literal("Advanced"), {}, UiIcon::code, 1, {kAdvanced, 2}},
};

void test_schema(Check check) noexcept {
    const UiSettingsSchema schema{{kSections, 2}, {kPanels, 2}};
    UiSettingsSchemaLocation location{};
    check(ui_settings_schema_valid(schema) && ui_settings_schema_find(schema, utf8_literal("appearance.embeds"), &location) &&
              location.panel == 0 && location.node == 2 &&
              ui_settings_schema_find(schema, utf8_literal("advanced"), &location) && location.panel == 1 &&
              location.node == kUiSettingsNoChoice && !ui_settings_schema_find(schema, utf8_literal("missing"), &location) &&
              ui_settings_schema_category(kPanels[0], 2) == 1 && ui_settings_schema_category(kPanels[0], 0) == kUiSettingsNoChoice,
          "settings schema: keys resolve panels and nodes; nodes know their category");
    UiSettingsSchemaNode duplicate[]{kAppearance[0], kAppearance[0]};
    const UiSettingsSchemaPanel duplicate_panel[]{{utf8_literal("p"), utf8_literal("P"), {}, UiIcon::code, 0, {duplicate, 2}}};
    check(!ui_settings_schema_valid({{kSections, 2}, {duplicate_panel, 1}}),
          "settings schema: duplicate keys are rejected so deep links never alias");
    UiSettingsSearchHit hits[8]{};
    const u32 embeds = ui_settings_schema_search(schema, utf8_literal("EMB"), {hits, 8});
    check(embeds == 1 && hits[0].panel == 0 && hits[0].node == 2 && hits[0].score == 4,
          "settings search: case-insensitive word prefix matches labels");
    const u32 messages = ui_settings_schema_search(schema, utf8_literal("mess prev"), {hits, 8});
    check(messages == 1 && hits[0].node == 2 && hits[0].score == 1,
          "settings search: every word may match a different field including the category");
    check(ui_settings_schema_search(schema, utf8_literal("look"), {hits, 8}) == 0 &&
              ui_settings_schema_search(schema, utf8_literal("save"), {hits, 8}) == 0 &&
              ui_settings_schema_search(schema, utf8_literal("ebm"), {hits, 8}) == 0,
          "settings search: panel keywords name only the panel; notices and non-prefixes never match");
    const u32 ranked = ui_settings_schema_search(schema, utf8_literal("de"), {hits, 8});
    check(ranked == 1 && hits[0].panel == 1 && hits[0].score == 4 &&
              ui_settings_search_text_matches(utf8_literal("sho emb"), utf8_literal("Show embeds")) &&
              !ui_settings_search_text_matches(utf8_literal("how"), utf8_literal("Show embeds")),
          "settings search: ranking and the word-prefix predicate");
}

struct Fixture {
    UiPage pages[3]{};
    UiPageRegistry registry{};
    Utf8View themes[3]{utf8_literal("Light"), utf8_literal("Dark"), utf8_literal("AMOLED")};
    Utf8View sizes[5]{utf8_literal("12px"), utf8_literal("14px"), utf8_literal("16px"), utf8_literal("18px"),
                      utf8_literal("20px")};
    Utf8View density[2]{utf8_literal("Cozy"), utf8_literal("Compact")};
    UiSettingsControl controls[8]{};
    UiSettingsControlPresentation presentation[8]{};
    UiSettingsPageContent content{};
    UiSettingsPageContent other{};
    UiSettingsNavEntry navigation[6]{};
};
void build_fixture(Fixture* f) noexcept {
    f->controls[0] = {1, utf8_literal("Theme"), utf8_literal("Light or dark."), UiSettingsControlKind::palette, true,
                      false, {f->themes, 3}, 1};
    f->controls[1] = {2, utf8_literal("Messages"), {}, UiSettingsControlKind::category, true, false};
    f->controls[2] = {3, utf8_literal("Show embeds"), utf8_literal("Show link previews in chat."),
                      UiSettingsControlKind::toggle, true, true};
    f->controls[3] = {4, utf8_literal("Font size"), {}, UiSettingsControlKind::slider, true, false, {f->sizes, 5}, 2};
    f->controls[4] = {5, utf8_literal("Message display"), {}, UiSettingsControlKind::radio, true, false,
                      {f->density, 2}, 0};
    f->controls[5] = {6, utf8_literal("Reset theme"), {}, UiSettingsControlKind::action, true, false, {}, 0,
                      utf8_literal("Reset")};
    f->controls[6] = {7, utf8_literal("Couldn't save settings"), {}, UiSettingsControlKind::notice, true, false, {},
                      0, utf8_literal("Try again")};
    f->controls[7] = {8, utf8_literal("Disabled"), {}, UiSettingsControlKind::toggle, false, false};
    for (auto& p : f->presentation)
        p = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    f->presentation[3] = {UiSettingsVariant::standard, ui_settings_presentation_marks, 2};
    f->presentation[6].variant = UiSettingsVariant::critical;
    f->content = {utf8_literal("Appearance"), {}, {f->controls, 8}, 1};
    f->content.presentation = f->presentation;
    f->other = {utf8_literal("Advanced"), {}, {}, 1};
    f->navigation[0] = {UiSettingsNavKind::header, false, UiIcon::count, utf8_literal("APP SETTINGS"), {}, 0};
    f->navigation[1] = {UiSettingsNavKind::page, false, UiIcon::palette, utf8_literal("Appearance"), {1, 1}, 0};
    f->navigation[2] = {UiSettingsNavKind::page, false, UiIcon::code, utf8_literal("Advanced"), {1, 2}, 0};
    f->navigation[3] = {UiSettingsNavKind::page, false, UiIcon::puzzle, utf8_literal("Unloaded"), {7, 1}, 0};
    f->navigation[4] = {UiSettingsNavKind::separator, false, UiIcon::count, {}, {}, 0};
    f->navigation[5] = {UiSettingsNavKind::action, true, UiIcon::log_out, utf8_literal("Log out"), {}, 9};
}

void test_modal_scene(Check check) noexcept {
    static Fixture f{};
    build_fixture(&f);
    UiSettingsScene scene{};
    const UiRect viewport{0, 0, 1400, 900};
    int palette_context = 0;
    check(ui_page_registry_initialize(&f.registry, {f.pages, 3}) &&
              ui_page_registry_register(&f.registry, {{1, 1}, utf8_literal("Appearance"), &f.content}).ok() &&
              ui_page_registry_register(&f.registry, {{1, 2}, utf8_literal("Advanced"), &f.other}).ok() &&
              ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal) &&
              ui_settings_scene_bind_navigation(&scene, {f.navigation, 6}, 1) &&
              ui_settings_scene_bind_palette(&scene, {&palette_context, resolve_palette, 1}) &&
              ui_settings_scene_update(&scene, &f.registry, viewport) && scene.opens == 1,
          "settings modal: catalog-shaped page with navigation builds");
    if (!scene.active)
        return;
    const UiPageKey page{1, 1};
    const UiSettingsTarget toggle{UiSettingsTargetKind::control, page, 3};
    const UiSettingsTarget slider{UiSettingsTargetKind::control, page, 4};
    const UiSettingsTarget radio{UiSettingsTargetKind::control, page, 5};
    const UiSettingsTarget palette{UiSettingsTargetKind::control, page, 1};
    const UiSettingsTarget action{UiSettingsTargetKind::control, page, 6};
    UiSettingsControlGeometry geometry{};
    const UiSettingsThemeTokens card_tokens(kDefaultTheme);
    check(ui_settings_scene_focused(scene).kind == UiSettingsTargetKind::search &&
              ui_settings_scene_focus_zone(scene) == UiSettingsFocusZone::search &&
              ui_settings_scene_control_geometry(scene, toggle, &geometry) && close_to(ui_rect_width(geometry.field), 48) &&
              close_to(geometry.field.right, scene.layout.column.right - card_tokens.metric.card_inset),
          "settings modal: search takes initial focus; switches sit inside their card's padding");
    const auto search = ui_widget_manager_layout_node(scene.widgets, scene.search_field);
    UiSettingsControlGeometry search_geometry{};
    check(search && ui_settings_scene_search_geometry(scene, &search_geometry) &&
              ui_settings_target_equal(ui_settings_scene_hit_test(scene, center(search->rectangle).x,
                                                                  center(search->rectangle).y),
                                       {UiSettingsTargetKind::search, {}, 0}) &&
              search_geometry.text.left > search->rectangle.left,
          "settings modal: the sidebar search field is a hit target with editor geometry");
    UiSettingsEditor query{{utf8_literal("emb"), 3, 3, true, true, true}, nullptr, utf8_literal("Search")};
    const auto clear = search ? ui_settings_search_clear_rect(search->rectangle) : UiRect{};
    check(ui_settings_scene_set_search(&scene, query) &&
              ui_settings_scene_hit_test(scene, center(clear).x, center(clear).y).kind ==
                  UiSettingsTargetKind::search_clear,
          "settings modal: typed queries expose a clear button inside the field");
    query.view.text = {};
    ui_settings_scene_set_search(&scene, query);

    // Zones cycle search -> selected page -> first panel row -> close.
    check(ui_settings_scene_move_zone(&scene, false) &&
              ui_settings_target_equal(scene.focus, {UiSettingsTargetKind::page, page, 0}) &&
              ui_settings_scene_move_within(&scene, false) &&
              ui_settings_target_equal(scene.focus, {UiSettingsTargetKind::page, {1, 2}, 0}) &&
              ui_settings_scene_move_within(&scene, false) &&
              ui_settings_target_equal(scene.focus, {UiSettingsTargetKind::action, {}, 9}) &&
              !ui_settings_scene_move_within(&scene, false) && ui_settings_scene_move_zone(&scene, false) &&
              ui_settings_target_equal(scene.focus, palette) && ui_settings_scene_move_within(&scene, false) &&
              ui_settings_target_equal(scene.focus, toggle) && ui_settings_scene_move_zone(&scene, false) &&
              scene.focus.kind == UiSettingsTargetKind::close && ui_settings_scene_move_zone(&scene, false) &&
              scene.focus.kind == UiSettingsTargetKind::search && ui_settings_scene_move_zone(&scene, true) &&
              scene.focus.kind == UiSettingsTargetKind::close,
          "settings modal: Tab zones and arrow roving skip headers, unregistered pages and categories");
    UiSettingsActivation activation{};
    UiSettingsSliderGeometry track{};
    check(ui_settings_scene_slider_geometry(scene, slider, &track) &&
              ui_settings_scene_slider_pointer(scene, slider, track.track.left - 40, &activation) &&
              activation.selected_choice == 0 &&
              ui_settings_scene_slider_pointer(scene, slider, track.track.right + 40, &activation) &&
              activation.selected_choice == 4 &&
              ui_settings_scene_slider_key(scene, slider, UiSettingsSliderKey::increase, &activation) &&
              activation.selected_choice == 3 &&
              ui_settings_scene_slider_key(scene, slider, UiSettingsSliderKey::page_decrease, &activation) &&
              activation.selected_choice == 1 &&
              ui_settings_scene_slider_key(scene, slider, UiSettingsSliderKey::page_increase, &activation) &&
              activation.selected_choice == 3 &&
              ui_settings_scene_slider_hit(scene, slider, center(track.thumb).x, center(track.thumb).y),
          "settings modal: sliders map pointer, arrow and Page keys to clamped choices");
    UiSettingsControlGeometry radio_geometry{};
    const UiSettingsThemeTokens tokens(kDefaultTheme);
    const bool radio_ok = ui_settings_scene_control_geometry(scene, radio, &radio_geometry);
    const auto compact = ui_radio_option(tokens, radio_geometry.field, 1);
    check(radio_ok && ui_settings_scene_radio_pointer(scene, radio, center(compact).x, center(compact).y, &activation) &&
              activation.selected_choice == 1 &&
              ui_settings_scene_slider_key(scene, radio, UiSettingsSliderKey::increase, &activation) &&
              activation.selected_choice == 1,
          "settings modal: radio rows select the option under the pointer or arrow");
    UiSettingsControlGeometry swatches{};
    const bool swatch_ok = ui_settings_scene_control_geometry(scene, palette, &swatches);
    const auto amoled = ui_swatch_rect(tokens, swatches.field, 2, ui_swatch_columns(tokens, ui_rect_width(swatches.field)));
    check(swatch_ok && ui_settings_scene_palette_pointer(scene, palette, center(amoled).x, center(amoled).y, &activation) &&
              activation.selected_choice == 2,
          "settings modal: theme swatches resolve the tile under the pointer");
    UiSettingsControlGeometry button{};
    check(ui_settings_scene_control_geometry(scene, action, &button) &&
              ui_settings_scene_button_hit(scene, action, center(button.field).x, center(button.field).y) &&
              !ui_settings_scene_button_hit(scene, action, button.control.left + 2, center(button.field).y) &&
              !ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, page, 8}) &&
              !ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, page, 2}),
          "settings modal: action rows activate on their button; disabled rows and categories take no focus");
    // Only the interactive part of a row answers the pointer: the switch, the
    // slider track, an option, a swatch or the row's button. The label and
    // empty row space hit nothing, so they neither hover nor activate.
    const auto hits = [&](UiSettingsTarget target, float x, float y) noexcept {
        return ui_settings_target_equal(ui_settings_scene_hit_test(scene, x, y), target);
    };
    const auto misses = [&](float x, float y) noexcept {
        return ui_settings_scene_hit_test(scene, x, y).kind == UiSettingsTargetKind::none;
    };
    UiSettingsControlGeometry switch_geometry{};
    const bool switch_ok = ui_settings_scene_control_geometry(scene, toggle, &switch_geometry);
    const auto knob = center(switch_geometry.field);
    check(switch_ok && hits(toggle, knob.x, knob.y) && hits(toggle, switch_geometry.control.left + 8, knob.y) &&
              hits(toggle, switch_geometry.field.left - 24, knob.y) && misses(scene.layout.column.left - 8, knob.y) &&
              close_to(switch_geometry.field.right, switch_geometry.control.right - tokens.metric.card_inset),
          "settings hit-test: a toggle row of a card answers on its whole row, its switch inset by the card padding");
    UiSettingsSliderGeometry slider_track{};
    UiSettingsControlGeometry slider_row{};
    const bool slider_ok = ui_settings_scene_slider_geometry(scene, slider, &slider_track) &&
                           ui_settings_scene_control_geometry(scene, slider, &slider_row);
    check(slider_ok && hits(slider, center(slider_track.thumb).x, center(slider_track.thumb).y) &&
              hits(slider, slider_track.track.left + 4, center(slider_track.track).y) &&
              misses(slider_row.control.left + 4, slider_row.control.top + 4),
          "settings hit-test: a slider row answers on its track, not its label");
    check(radio_ok && hits(radio, center(compact).x, center(compact).y) &&
              misses(radio_geometry.control.left + 4, radio_geometry.control.top + 4),
          "settings hit-test: radio rows answer on their options, not the heading");
    check(swatch_ok && hits(palette, center(amoled).x, center(amoled).y) &&
              misses(swatches.control.left + 4, swatches.control.top + 4),
          "settings hit-test: palettes answer on a swatch, not the heading");
    check(hits(action, center(button.field).x, center(button.field).y) &&
              misses(button.control.left + 4, center(button.field).y) &&
              misses(button.field.left - 12, center(button.field).y),
          "settings hit-test: action rows answer on their button only");
    // Hover follows the same region: the label hovers the row's switch too, the gap outside the column hovers nothing.
    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, {scene.layout.column.left - 8, knob.y});
    const bool outside_cold = !scene.widgets.hovered.generation;
    const auto hovered = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, knob);
    const auto label_hover = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move,
                                               {switch_geometry.field.left - 24, knob.y});
    check(outside_cold && hovered.target.generation && scene.widgets.hovered.slot == switch_geometry.owner.handle.slot &&
              !label_hover.changed,
          "settings hit-test: hovering anywhere on a card's toggle row hovers that row; outside the column hovers nothing");
    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});

    PaintStream paint{};
    UiTextCommand commands[96]{};
    UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    UiSettingsPaintRanges ranges{};
    bool painted = paint_stream_initialize(&paint) && paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                   ui_settings_scene_paint(scene, kDefaultTheme, &paint, 96, &ranges) &&
                   ui_text_command_stream_initialize(&text, {commands, 96}) &&
                   ui_settings_scene_text_output(scene, kDefaultTheme, &text_output);
    check(painted && ranges.modal_first_quad > 0 && ranges.modal_first_quad <= ranges.page_first_quad &&
              ranges.page_first_quad < ranges.page_end_quad && ranges.page_end_quad <= ranges.modal_end_quad &&
              ranges.modal_end_quad <= paint.count && !paint.rejected && paint.batch_count < kMaxPaintBatches / 2 &&
              paint.quads[0].fill.alpha > .7f && paint.quads[0].fill.alpha < .75f,
          "settings modal: scrim precedes the scaled dialog range which contains the page range");
    // Each theme swatch paints its own resolved theme: flat themes solid in
    // their own colour, so distinct themes never share a fill.
    UiSettingsControlGeometry themes{};
    bool solid = ui_settings_scene_bind_palette(&scene, {&palette_context, resolve_flat, 2}) &&
                 ui_settings_scene_control_geometry(scene, {UiSettingsTargetKind::control, page, 1}, &themes) &&
                 paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                 ui_settings_scene_paint(scene, kDefaultTheme, &paint, 96, nullptr);
    const UiSettingsThemeTokens swatch_tokens(kDefaultTheme);
    const u32 columns = ui_swatch_columns(swatch_tokens, ui_rect_width(themes.field));
    for (u32 choice = 0; solid && choice < 3; ++choice) {
        const auto rect = ui_swatch_rect(swatch_tokens, themes.field, choice, columns);
        const u32 rgb = kFlatThemes[choice];
        bool found = false;
        for (u32 i = 0; i < paint.count && !found; ++i) {
            const auto& q = paint.quads[i];
            found = close_to(q.left, rect.left) && close_to(q.top, rect.top) && close_to(q.right, rect.right) &&
                    q.border_width == 0 && close_to(q.fill.red * 255, static_cast<float>(rgb >> 16)) &&
                    close_to(q.fill.green * 255, static_cast<float>((rgb >> 8) & 255)) &&
                    close_to(q.fill.blue * 255, static_cast<float>(rgb & 255));
        }
        solid = found;
    }
    check(solid && ui_settings_scene_bind_palette(&scene, {&palette_context, resolve_palette, 1}),
          "settings modal: theme swatches fill with their own resolved theme colours");
    bool page_started = false, contiguous = true, header = false, escape = false, marks = false, logout = false;
    for (u32 i = 0; painted && i < text.count; ++i) {
        const bool owned = ui_settings_scene_page_owner(scene, commands[i].owner);
        contiguous = contiguous && (!page_started || owned);
        page_started = page_started || owned;
        header = header || utf8_equal(commands[i].text, utf8_literal("APP SETTINGS"));
        escape = escape || utf8_equal(commands[i].text, utf8_literal("Ctrl F"));
        marks = marks || utf8_equal(commands[i].text, utf8_literal("18px"));
        logout = logout || utf8_equal(commands[i].text, utf8_literal("Log out"));
    }
    check(painted && page_started && contiguous && header && escape && marks && logout,
          "settings modal: chrome text precedes contiguous page text; headers, the Ctrl F hint and marks render");

    const u32 revision = scene.revision, motion = scene.motion_revision;
    check(ui_settings_scene_animate_toggle(&scene, toggle, true, 1000) && scene.revision == revision &&
              scene.motion_revision != motion &&
              ui_settings_scene_animating(scene) && ui_settings_scene_tick(&scene, 1100, false) &&
              !ui_settings_scene_tick(&scene, 1400, false) && !ui_settings_scene_animating(scene),
          "settings modal: toggles animate for the token duration and then settle");
    check(ui_settings_scene_set_flash(&scene, 3, .5f) && scene.flash_control == 3 &&
              ui_settings_scene_set_flash(&scene, 3, 0) && scene.flash_control == 0 &&
              !ui_settings_scene_set_flash(&scene, 3, 0),
          "settings modal: flash strength publishes and clears");
    check(ui_settings_scene_reveal(&scene, {UiSettingsTargetKind::control, page, 7}) &&
              ui_settings_scene_select(&scene, {1, 2}) && scene.page_generation == 2 &&
              !ui_settings_scene_control(scene, toggle),
          "settings modal: reveal scrolls; selecting another page retires stale targets");
    const u64 opens = scene.opens;
    ui_settings_scene_release(&scene);
    check(scene.opens == opens && scene.presentation == UiSettingsPresentation::modal &&
              ui_settings_scene_update(&scene, &f.registry, viewport) && scene.opens == opens + 1,
          "settings modal: reopening keeps the presentation and counts a fresh entrance");
    UiSettingsNavEntry invalid{UiSettingsNavKind::action, false, UiIcon::count, utf8_literal("x"), {}, 0};
    check(!ui_settings_scene_bind_navigation(&scene, {&invalid, 1}, 2),
          "settings modal: navigation actions require an id");
    ui_settings_scene_release(&scene);
    paint_stream_release(&paint);
}

void test_layer_scene(Check check) noexcept {
    UiSettingsScene scene{};
    UiPage page{};
    UiPageRegistry registry{};
    UiSettingsEditor name{{utf8_literal("native-room"), 2, 5, true, true, true}, nullptr, utf8_literal("Channel name")};
    UiSettingsEditor topic{{utf8_literal("Topic\nSecond line"), 0, 0, true, false, false}, nullptr,
                           utf8_literal("Add a topic")};
    const Utf8View choices[]{utf8_literal("Off"), utf8_literal("5 seconds")};
    UiSettingsControl controls[]{
        {1, utf8_literal("Name"), {}, UiSettingsControlKind::edit, true, false, {}, 0, {}, &name},
        {2, utf8_literal("Topic"), {}, UiSettingsControlKind::multiline, true, false, {}, 0, {}, &topic},
        {3, utf8_literal("Slowmode"), {}, UiSettingsControlKind::choice, true, false, {choices, 2}, 0, {}},
        {4, utf8_literal("Age-restricted"), {}, UiSettingsControlKind::toggle, true, false}};
    UiSettingsFooter footer{{}, utf8_literal("Save changes"), utf8_literal("Reset"), true, true, false, false};
    UiSettingsPageContent content{utf8_literal("Overview"), {}, {controls, 4}, 1, kUiSettingsPageFormat, nullptr,
                                  &footer};
    content.navigation_label = utf8_literal("#native-room");
    const UiPageKey key{9, 1};
    const UiRect viewport{0, 0, 1280, 900};
    check(ui_page_registry_initialize(&registry, {&page, 1}) &&
              ui_page_registry_register(&registry, {key, utf8_literal("Overview"), &content}).ok() &&
              ui_settings_scene_update(&scene, &registry, viewport) &&
              scene.presentation == UiSettingsPresentation::layer && !scene.search_field.generation &&
              ui_settings_scene_focused(scene).kind == UiSettingsTargetKind::close,
          "settings layer: channel pages keep the full-window layer without search");
    const UiSettingsTarget name_target{UiSettingsTargetKind::control, key, 1};
    const UiSettingsTarget topic_target{UiSettingsTargetKind::control, key, 2};
    UiSettingsControlGeometry name_geometry{}, topic_geometry{}, hit{};
    const bool valid = ui_settings_scene_control_geometry(scene, name_target, &name_geometry) &&
                       ui_settings_scene_control_geometry(scene, topic_target, &topic_geometry);
    check(valid && close_to(ui_rect_height(name_geometry.field), 40) &&
              close_to(ui_rect_height(topic_geometry.field), 3 * kUiSettingsLabelLine + 24) &&
              close_to(name_geometry.text.left, name_geometry.field.left + 12) && topic_geometry.multiline &&
              ui_settings_scene_editor_hit(scene, center(name_geometry.clip).x, center(name_geometry.clip).y, &hit) &&
              ui_settings_target_equal(hit.target, name_target) &&
              !ui_settings_scene_editor_hit(scene, center(name_geometry.clip).x, name_geometry.field.top - 4, &hit),
          "settings layer: single-line editors are 40 DIP and multi-line ones start at three lines, with exact "
          "field hits");
    check(ui_settings_scene_focus(&scene, name_target) && name_geometry.scene_revision != scene.revision &&
              ui_settings_scene_control_geometry(scene, name_target, &name_geometry) &&
              name_geometry.scene_revision == scene.revision,
          "settings layer: geometry is fenced by scene revision");
    check(ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, key, 4}) &&
              ui_settings_scene_move_focus(&scene, false) && scene.focus.kind == UiSettingsTargetKind::reset &&
              ui_settings_scene_move_focus(&scene, false) && scene.focus.kind == UiSettingsTargetKind::save &&
              ui_settings_scene_move_focus(&scene, false) && scene.focus.kind == UiSettingsTargetKind::close,
          "settings layer: Tab order includes reset, save and close");
    const auto* save = ui_widget_manager_layout_node(scene.widgets, scene.footer_save);
    const auto* body = ui_widget_manager_layout_node(scene.widgets, scene.body);
    check(save && body && save->rectangle.top >= body->rectangle.bottom &&
              !ui_settings_scene_editor_hit(scene, save->rectangle.left + 1, save->rectangle.top + 1, &hit),
          "settings layer: the unsaved-changes bar sits below the scrolling rows");
    // A multi-line field grows a line at a time with its text, up to its
    // style height, and relayouts only when its line count changes.
    const auto topic_height = [&](Utf8View text) noexcept {
        topic.view.text = text;
        ++content.revision;
        UiSettingsControlGeometry geometry{};
        return ui_settings_scene_update(&scene, &registry, viewport) &&
                       ui_settings_scene_control_geometry(scene, topic_target, &geometry)
                   ? ui_rect_height(geometry.field)
                   : 0.f;
    };
    const u16 generation = scene.widgets.generation;
    const float five = topic_height(utf8_literal("one\ntwo\nthree\nfour\nfive"));
    const u16 grown_generation = scene.widgets.generation;
    const float same = topic_height(utf8_literal("one\ntwo\nthree\nfour\nfivE"));
    const u16 same_generation = scene.widgets.generation;
    const float capped = topic_height(utf8_literal("1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12"));
    check(close_to(five, 5 * kUiSettingsLabelLine + 24) && grown_generation != generation &&
              close_to(same, five) && same_generation == grown_generation && close_to(capped, 160),
          "settings layer: multi-line fields grow by line up to the style height");
    topic.view.text = utf8_literal("Topic\nSecond line");
    footer.saving = true;
    controls[0].enabled = false;
    ++content.revision;
    UiSettingsActivation activation{};
    check(ui_settings_scene_update(&scene, &registry, viewport) &&
              !ui_settings_scene_activation(scene, {UiSettingsTargetKind::save, key, 0}, false, &activation) &&
              !ui_settings_scene_editor_hit(scene, center(name_geometry.clip).x, center(name_geometry.clip).y, &hit),
          "settings layer: saving disables the footer and revoked editors");
    content.navigation_label = {"\xc0", 1};
    ++content.revision;
    check(!ui_settings_scene_update(&scene, &registry, viewport), "settings layer: invalid UTF-8 headings are rejected");
    content.navigation_label = utf8_literal("#renamed");
    footer.saving = false;
    controls[0].enabled = true;
    static UiSettingsControl deep[8192]{};
    for (u32 i = 0; i < 8192; ++i)
        deep[i] = {i + 1, utf8_literal("Bounded source row"), {}, UiSettingsControlKind::toggle, true, false};
    content.controls = {deep, 8192};
    ++content.revision;
    const UiSettingsTarget last{UiSettingsTargetKind::control, key, 8192};
    UiSettingsControlGeometry last_geometry{};
    check(ui_settings_scene_update(&scene, &registry, viewport) && scene.widgets.count < 96 &&
              scene.capacity <= kUiSettingsMaterializedWidgetLimit && scene.prefix_capacity == 8193 &&
              ui_settings_scene_focus(&scene, last) && ui_settings_scene_control_geometry(scene, last, &last_geometry) &&
              scene.content_offset > 100000,
          "settings layer: 8192 rows virtualize into a bounded widget bank");
    for (auto& control : deep) {
        control.kind = UiSettingsControlKind::multiline;
        control.editor = &topic;
    }
    ++content.revision;
    check(!ui_settings_scene_update(&scene, &registry, viewport), "settings layer: excessive extents are rejected");
    ui_settings_scene_release(&scene);
    check(!scene.storage.arena.base && !scene.active, "settings layer: release returns to a dormant scene");
}
// Single-page forms (rules, reports, events) present as compact dialogs: no
// sidebar or search, sized to their rows, with the actions in a footer band.
void test_dialog_scene(Check check) noexcept {
    const UiSettingsThemeTokens tokens(kDefaultTheme);
    const auto wide = ui_settings_modal_layout(tokens, {0, 0, 1200, 970}, UiSettingsPresentation::dialog);
    const auto narrow = ui_settings_modal_layout(tokens, {0, 0, 632, 720}, UiSettingsPresentation::dialog);
    const auto tiny = ui_settings_modal_layout(tokens, {0, 0, 500, 600}, UiSettingsPresentation::dialog);
    check(close_to(ui_rect_width(wide.modal), kUiSettingsDialogWidth) && close_to(wide.modal.left, 300) &&
              close_to(ui_rect_width(wide.sidebar), 0) && ui_rect_equal(wide.panel, wide.modal) &&
              close_to(wide.column.left - wide.modal.left, kUiSettingsDialogPadding) &&
              close_to(wide.modal.right - wide.column.right, kUiSettingsDialogPadding) &&
              wide.close.right <= wide.modal.right && wide.close.top >= wide.modal.top &&
              close_to(ui_rect_width(narrow.modal), 632 - 48) && tiny.compact && close_to(ui_rect_width(tiny.modal), 500),
          "settings dialog: a 600 DIP column without sidebar that narrows with the window and fills tiny ones");
    auto fitted = wide;
    ui_settings_dialog_fit(&fitted, {0, 0, 1200, 970}, 400);
    auto tall = wide;
    ui_settings_dialog_fit(&tall, {0, 0, 1200, 970}, 5000);
    check(close_to(ui_rect_height(fitted.modal), 400) && close_to(fitted.modal.top, 285) &&
              close_to(fitted.panel.bottom, fitted.modal.bottom) && close_to(fitted.column.bottom, fitted.modal.bottom) &&
              close_to(fitted.close.top - fitted.modal.top, wide.close.top - wide.modal.top) &&
              ui_rect_equal(tall.modal, wide.modal),
          "settings dialog: short content centres a fitted dialog; tall content keeps the inset height");

    static UiPage pages[1]{};
    static UiPageRegistry registry{};
    static Utf8View reasons[3]{utf8_literal("Spam"), utf8_literal("Harassment"), utf8_literal("Something else")};
    static UiSettingsControl controls[3]{};
    controls[0] = {1, utf8_literal("What's going on?"), utf8_literal("Pick the closest match."),
                   UiSettingsControlKind::category, true, false};
    // A radio group without a label of its own: its category names it.
    controls[1] = {2, {}, {}, UiSettingsControlKind::radio, true, false, {reasons, 3}, kUiSettingsNoChoice};
    controls[2] = {3, utf8_literal("Probe Bot"), utf8_literal("The reported message."), UiSettingsControlKind::notice,
                   true, false};
    static UiSettingsFooter footer{{}, utf8_literal("Next"), utf8_literal("Cancel"), true, true, false, false};
    static UiSettingsPageContent content{};
    content = {utf8_literal("Report message"), {}, {controls, 3}, 1, kUiSettingsPageFormat, nullptr, &footer};
    UiSettingsScene scene{};
    const UiRect viewport{0, 0, 1200, 970};
    check(ui_page_registry_initialize(&registry, {pages, 1}) &&
              ui_page_registry_register(&registry, {{9, 1}, content.title, &content}).ok() &&
              ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::dialog) &&
              ui_settings_scene_update(&scene, &registry, viewport),
          "settings dialog: a report step with an unlabelled radio group builds");
    if (!scene.active)
        return;
    const auto& l = scene.layout;
    const auto* status = ui_widget_manager_layout_node(scene.widgets, scene.footer_status);
    const auto* band = status ? ui_widget_manager_layout_node(scene.widgets,
                                    ui_widget_manager_parent(scene.widgets, scene.footer_status))
                              : nullptr;
    const auto* body = ui_widget_manager_layout_node(scene.widgets, scene.body);
    const auto* save = ui_widget_manager_layout_node(scene.widgets, scene.footer_save);
    check(!scene.search_field.generation && ui_rect_height(l.modal) < 600 &&
              close_to(l.modal.top + l.modal.bottom, viewport.bottom) && band && body && save &&
              close_to(band->rectangle.bottom, l.modal.bottom) && close_to(band->rectangle.left, l.modal.left) &&
              close_to(band->rectangle.right, l.modal.right) && close_to(band->rectangle.top, body->rectangle.bottom) &&
              save->rectangle.right <= l.modal.right - kUiSettingsDialogPadding + .01f &&
              save->rectangle.top > band->rectangle.top && save->rectangle.bottom < band->rectangle.bottom,
          "settings dialog: fitted, centred, with its actions in a full-width band at the bottom edge");
    const auto close = ui_settings_scene_hit_test(scene, center(l.close).x, center(l.close).y);
    check(close.kind == UiSettingsTargetKind::close &&
              ui_settings_scene_hit_test(scene, l.modal.left + 4, l.modal.top + 200).kind != UiSettingsTargetKind::page,
          "settings dialog: the close glyph is a target and no sidebar pages exist");
    ui_settings_scene_release(&scene);
}
// Settings > Updates: "Check now" turns into a status row while it works. The focus stays on that slot
// (no ring, not the search box), carries Tab on from it, returns with the button, and a removed row hands
// it to its neighbour.
void test_focus_recovery(Check check) noexcept {
    static UiPage pages[1]{};
    static UiPageRegistry registry{};
    static UiSettingsControl controls[4]{};
    static UiSettingsPageContent content{};
    static UiSettingsNavEntry navigation[1]{};
    UiSettingsScene scene{};
    const UiPageKey key{1, 1};
    const UiRect viewport{0, 0, 1400, 900};
    const UiSettingsTarget automatic{UiSettingsTargetKind::control, key, 1};
    const UiSettingsTarget check_now{UiSettingsTargetKind::control, key, 2};
    const UiSettingsTarget preview{UiSettingsTargetKind::control, key, 3};
    const UiSettingsTarget folder{UiSettingsTargetKind::control, key, 4};
    const auto idle = [&]() noexcept {
        controls[0] = {1, utf8_literal("Automatic updates"), {}, UiSettingsControlKind::toggle, true, true};
        controls[1] = {2, utf8_literal("Check for updates"), {}, UiSettingsControlKind::notice, true, false, {}, 0,
                       utf8_literal("Check now")};
        controls[2] = {3, utf8_literal("Preview channel"), {}, UiSettingsControlKind::toggle, true, false};
        controls[3] = {4, utf8_literal("Open download folder"), {}, UiSettingsControlKind::action, true, false, {}, 0,
                       utf8_literal("Open")};
        content.controls = {controls, 4};
        ++content.revision;
        return ui_settings_scene_update(&scene, &registry, viewport);
    };
    const auto checking = [&]() noexcept {
        controls[1] = {2, utf8_literal("Checking for updates"), {}, UiSettingsControlKind::notice, true, true};
        ++content.revision;
        return ui_settings_scene_update(&scene, &registry, viewport);
    };
    // The focus rests on "Check now", then the click turns it into the status row.
    const auto park = [&]() noexcept {
        return idle() && ui_settings_scene_focus(&scene, check_now) && checking();
    };
    const auto focused_is = [&](const UiSettingsTarget& target) noexcept {
        return ui_settings_target_equal(ui_settings_scene_focused(scene), target);
    };
    content = {utf8_literal("Updates"), {}, {controls, 4}, 1};
    navigation[0] = {UiSettingsNavKind::page, false, UiIcon::palette, utf8_literal("Updates"), key, 0};
    check(ui_page_registry_initialize(&registry, {pages, 1}) &&
              ui_page_registry_register(&registry, {key, utf8_literal("Updates"), &content}).ok() &&
              ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal) &&
              ui_settings_scene_bind_navigation(&scene, {navigation, 1}, 1) && idle() &&
              ui_settings_scene_focused(scene).kind == UiSettingsTargetKind::search,
          "settings focus: the Updates fixture builds with the search box focused");
    if (!scene.active)
        return;
    PaintStream paint{};
    const auto quads = [&](bool visible) noexcept {
        ui_settings_scene_set_focus_visible(&scene, visible);
        return paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                       ui_settings_scene_paint(scene, kDefaultTheme, &paint, 96, nullptr)
                   ? paint.count
                   : 0u;
    };
    // Painted with and without the keyboard ring; the difference is the ring.
    const auto ring = [&]() noexcept {
        const u32 without = quads(false), with = quads(true);
        return with > without;
    };
    check(paint_stream_initialize(&paint) && ui_settings_scene_focus(&scene, check_now) && focused_is(check_now) &&
              scene.widgets.focused.generation && ring(),
          "settings focus: the Check now button takes the focus and draws a keyboard ring");
    check(checking() && focused_is(check_now) && !scene.widgets.focused.generation &&
              ui_settings_scene_focus_zone(scene) == UiSettingsFocusZone::panel && !ring(),
          "settings focus: Check now turning into the status row keeps the focus on its slot, not the search box, "
          "and draws no ring on the status row");
    check(!ui_settings_scene_focus(&scene, check_now) && idle() && focused_is(check_now) &&
              scene.widgets.focused.generation && ring(),
          "settings focus: the button takes the focus and its ring back when the check finishes");
    check(park() && ui_settings_scene_move_focus(&scene, false) && focused_is(preview) && park() &&
              ui_settings_scene_move_focus(&scene, true) && focused_is(automatic),
          "settings focus: Tab from the status row continues from its slot in both directions");
    check(park() && ui_settings_scene_move_within(&scene, false) && focused_is(preview) && park() &&
              ui_settings_scene_move_within(&scene, true) && focused_is(automatic),
          "settings focus: arrows from the status row continue from its slot in both directions");
    ui_settings_scene_set_focus_visible(&scene, false);
    check(park() && focused_is(check_now) && quads(false) == quads(true),
          "settings focus: a pointer press on Check now keeps the focus off the search box without a ring");
    // The row is removed outright: the next focusable row takes the focus, else the previous one.
    check(idle() && ui_settings_scene_focus(&scene, check_now), "settings focus: Check now is focused again");
    controls[1] = controls[2];
    controls[2] = controls[3];
    content.controls.count = 3;
    ++content.revision;
    check(ui_settings_scene_update(&scene, &registry, viewport) && focused_is(preview) &&
              scene.widgets.focused.generation,
          "settings focus: a removed row hands the focus to the row that took its place");
    const bool last = ui_settings_scene_focus(&scene, folder);
    content.controls.count = 2;
    ++content.revision;
    check(last && ui_settings_scene_update(&scene, &registry, viewport) && focused_is(preview),
          "settings focus: a removed last row hands the focus to the row before it");
    content.controls.count = 0;
    ++content.revision;
    check(ui_settings_scene_update(&scene, &registry, viewport) &&
              focused_is({UiSettingsTargetKind::page, key, 0}),
          "settings focus: with no focusable row left the focus goes to the selected page, not the search box");
    // A disabled text editor gives the focus up (the host owns its text input) to the next plain row.
    UiSettingsEditor field{{utf8_literal("draft"), 0, 0, true, false, false}, nullptr, utf8_literal("Name")};
    controls[0] = {5, utf8_literal("Name"), {}, UiSettingsControlKind::edit, true, false, {}, 0, {}, &field};
    controls[1] = {3, utf8_literal("Preview channel"), {}, UiSettingsControlKind::toggle, true, false};
    content.controls.count = 2;
    ++content.revision;
    const bool editing = ui_settings_scene_update(&scene, &registry, viewport) &&
                         ui_settings_scene_focus(&scene, {UiSettingsTargetKind::control, key, 5});
    controls[0].enabled = false;
    ++content.revision;
    check(editing && ui_settings_scene_update(&scene, &registry, viewport) && focused_is(preview) &&
              scene.widgets.focused.generation,
          "settings focus: a disabled editor hands the focus on instead of keeping it parked");
    ui_settings_scene_release(&scene);
}

// A page of every new row: grouped into cards, trailing controls, status, disclosure, markdown and footnote.
struct CardFixture {
    UiPage pages[2]{};
    UiPageRegistry registry{};
    Utf8View sorts[4]{utf8_literal("Every hour"), utf8_literal("Every 4 hours"), utf8_literal("Every 12 hours"),
                      utf8_literal("Once a day")};
    Utf8View channel[2]{utf8_literal("Stable"), utf8_literal("Preview")};
    Utf8View values[11]{utf8_literal("0"), utf8_literal("1"), utf8_literal("2"), utf8_literal("3"), utf8_literal("4"),
                        utf8_literal("5"), utf8_literal("6"), utf8_literal("7"), utf8_literal("8"), utf8_literal("9"),
                        utf8_literal("10")};
    Utf8View chips[2]{utf8_literal("Stable release"), utf8_literal("Signature verified")};
    char notes[1200]{};
    UiSettingsControl controls[10]{};
    UiSettingsControlPresentation presentation[10]{};
    u16 meters[10]{};
    UiSettingsPageContent content{};
    UiSettingsNavEntry navigation[5]{};
};
void build_card_fixture(CardFixture* f) noexcept {
    u32 bytes = 0;
    for (u32 i = 0; i < 14; ++i) {
        const char line[] = "- note about change number ";
        copy_bytes(f->notes + bytes, line, sizeof(line) - 1);
        bytes += sizeof(line) - 1;
        f->notes[bytes++] = static_cast<char>('a' + i);
        f->notes[bytes++] = '\n';
    }
    f->controls[0] = {1, utf8_literal("Update settings"), {}, UiSettingsControlKind::category, true, false};
    f->controls[0].detail = utf8_literal("Synced with your account");
    f->controls[1] = {2, utf8_literal("Notify me"), utf8_literal("Tell me when an update is ready."),
                      UiSettingsControlKind::toggle, true, false};
    f->controls[2] = {3, utf8_literal("Check for updates"), {}, UiSettingsControlKind::choice, true, false, {f->sorts, 4}, 1};
    f->controls[3] = {4, utf8_literal("Volume"), {}, UiSettingsControlKind::slider, true, false, {f->values, 11}, 5};
    f->controls[4] = {5, utf8_literal("Channel"), utf8_literal("Stable or preview builds."), UiSettingsControlKind::segments,
                      true, false, {f->channel, 2}, 0};
    f->controls[5] = {6, utf8_literal("You're up to date"), utf8_literal("MaiCord 0.5.4 \xC2\xB7 Stable channel"),
                      UiSettingsControlKind::status, true, false, {f->chips, 2}, 0, utf8_literal("Check now")};
    f->controls[5].detail = utf8_literal("Takes a few seconds");
    f->controls[6] = {7, utf8_literal("MaiCord 0.5.4"), utf8_literal("Nov 1"), UiSettingsControlKind::disclosure, true, false};
    f->controls[6].detail = utf8_literal("New");
    f->controls[7] = {8, utf8_literal("Fixed a crash on startup"), utf8_literal("Oct 30"), UiSettingsControlKind::disclosure,
                      true, false};
    f->controls[7].detail = utf8_literal("dev.38");
    f->controls[8] = {9, {}, {f->notes, bytes}, UiSettingsControlKind::markdown, true, false};
    f->controls[8].detail = utf8_literal("Show all notes");
    f->controls[9] = {10, utf8_literal("Updates replace the app."), {}, UiSettingsControlKind::footnote, true, false, {}, 0,
                      utf8_literal("Releases")};
    for (auto& p : f->presentation)
        p = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    for (auto& m : f->meters)
        m = kUiSettingsNoMeter;
    f->meters[5] = 500;
    f->presentation[5].chip_tones = ui_settings_chip_positive << 2;
    f->presentation[5].more = ui_settings_more_meta_parts;
    f->presentation[5].icon_plus_one = static_cast<u8>(UiIcon::circle_check) + 1;
    f->presentation[6].variant = UiSettingsVariant::primary;
    f->presentation[7].more = ui_settings_more_plain;
    f->content = {utf8_literal("Updates"), utf8_literal("MaiCord updates itself from signed releases."), {f->controls, 10}, 1};
    f->content.presentation = f->presentation;
    f->content.meters = f->meters;
    f->navigation[0] = {UiSettingsNavKind::header, false, UiIcon::count, utf8_literal("MAICORD"), {}, 0};
    f->navigation[1] = {UiSettingsNavKind::page, false, UiIcon::download, utf8_literal("Updates"), {1, 1}, 0};
    f->navigation[1].badge = kUiSettingsNavDot;
    f->navigation[2] = {UiSettingsNavKind::page, false, UiIcon::puzzle, utf8_literal("Plugins"), {1, 2}, 0};
    f->navigation[3] = {UiSettingsNavKind::page, false, UiIcon::code, utf8_literal("Developer"), {1, 3}, 0};
    f->navigation[3].pinned = true;
    f->navigation[4] = {UiSettingsNavKind::action, true, UiIcon::log_out, utf8_literal("Log Out"), {}, 9};
    f->navigation[4].pinned = true;
}

void test_card_pages(Check check) noexcept {
    static CardFixture f{};
    build_card_fixture(&f);
    static UiSettingsPageContent other{utf8_literal("Plugins"), {}, {}, 1}, developer{utf8_literal("Developer"), {}, {}, 1};
    UiPage pages[3]{};
    UiPageRegistry registry{};
    UiSettingsScene scene{};
    const UiRect viewport{0, 0, 1440, 2400}, host_viewport{0, 0, 1440, 900};
    const UiPageKey page{1, 1};
    check(ui_page_registry_initialize(&registry, {pages, 3}) &&
              ui_page_registry_register(&registry, {page, utf8_literal("Updates"), &f.content}).ok() &&
              ui_page_registry_register(&registry, {{1, 2}, utf8_literal("Plugins"), &other}).ok() &&
              ui_page_registry_register(&registry, {{1, 3}, utf8_literal("Developer"), &developer}).ok() &&
              ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal) &&
              ui_settings_scene_bind_navigation(&scene, {f.navigation, 5}, 1) &&
              ui_settings_scene_update(&scene, &registry, viewport) && scene.active,
          "settings cards: a page of cards, trailing controls and the new row kinds builds");
    if (!scene.active)
        return;
    const UiSettingsThemeTokens tokens(kDefaultTheme);
    const auto& m = tokens.metric;
    const auto target = [&](u64 id) noexcept { return UiSettingsTarget{UiSettingsTargetKind::control, page, id}; };
    UiSettingsControlGeometry toggle{}, dropdown{}, segmented{}, status{}, markdown{};
    check(ui_settings_scene_control_geometry(scene, target(2), &toggle) &&
              ui_settings_scene_control_geometry(scene, target(3), &dropdown) &&
              close_to(toggle.field.right, scene.layout.column.right - m.card_inset) &&
              close_to(dropdown.field.right, scene.layout.column.right - m.card_inset) &&
              ui_rect_width(dropdown.field) >= m.select_width - .01f && close_to(ui_rect_height(dropdown.field), m.control_height) &&
              ui_rect_height(toggle.control) >= m.row_min_height && ui_rect_height(dropdown.control) >= m.row_min_height &&
              close_to((dropdown.field.top + dropdown.field.bottom) * .5f, (dropdown.control.top + dropdown.control.bottom) * .5f),
          "settings cards: switches and dropdowns trail inside the card padding, rows are at least 56 tall with the control centred");
    UiSettingsSliderGeometry slider{};
    check(ui_settings_scene_slider_geometry(scene, target(4), &slider) &&
              close_to(ui_rect_width(slider.control.field), m.slider_width) &&
              slider.control.field.right + 8 + m.value_width <= scene.layout.column.right - m.card_inset + .01f &&
              ui_settings_scene_slider_hit(scene, target(4), center(slider.thumb).x, center(slider.thumb).y),
          "settings cards: a slider without marks trails as a 220 DIP track with its value column");
    UiRect option{};
    UiSettingsActivation activation{};
    check(ui_settings_scene_control_geometry(scene, target(5), &segmented) &&
              close_to(ui_rect_height(segmented.field), m.control_height) && ui_rect_width(segmented.field) >= 2 * 72 + 2 * m.segment_padding - .01f &&
              close_to(segmented.field.right, scene.layout.column.right - m.card_inset) &&
              ui_settings_scene_choice_rectangle(scene, target(5), 1, &option) && option.left >= segmented.field.left &&
              option.right <= segmented.field.right && option.top >= segmented.field.top && option.bottom <= segmented.field.bottom &&
              ui_settings_scene_option_pointer(scene, target(5), center(option).x, center(option).y, &activation) &&
              activation.selected_choice == 1 && activation.kind == UiSettingsControlKind::segments &&
              ui_settings_scene_slider_key(scene, target(5), UiSettingsSliderKey::increase, &activation) &&
              activation.selected_choice == 1,
          "settings cards: segmented controls measure their items, resolve the option under the pointer and step with the arrows");
    check(ui_settings_scene_control_geometry(scene, target(6), &status) && close_to(ui_rect_height(status.field), m.button_medium) &&
              ui_settings_scene_button_hit(scene, target(6), center(status.field).x, center(status.field).y) &&
              !ui_settings_scene_button_hit(scene, target(6), status.control.left + 30, center(status.field).y) &&
              ui_rect_height(status.control) >= 2 * 20 + m.status_icon && !ui_settings_scene_spinner_visible(scene),
          "settings status card: the button is the only target; the card pads 20 around a 48 DIP tile");
    UiSettingsControlGeometry plain{}, expandable{};
    check(ui_settings_scene_control_geometry(scene, target(7), &expandable) &&
              ui_settings_target_equal(ui_settings_scene_hit_test(scene, expandable.control.left + 40, center(expandable.control).y),
                                       target(7)) &&
              ui_settings_scene_activation(scene, target(7), false, &activation) && activation.checked &&
              ui_settings_scene_control_geometry(scene, target(8), &plain) && ui_rect_height(plain.control) >= 48 &&
              !ui_settings_scene_focus(&scene, target(8)) && ui_settings_scene_focus(&scene, target(7)),
          "settings disclosure: the whole row toggles, a plain line is a static, unfocusable row");
    check(ui_settings_scene_control_geometry(scene, target(9), &markdown) && close_to(ui_rect_height(markdown.control), 28 + 180 + 8 + 20) &&
              ui_settings_scene_button_hit(scene, target(9), center(markdown.field).x, center(markdown.field).y) &&
              close_to(ui_rect_height(markdown.field), 20),
          "settings markdown: long notes clip at 180 DIP under a Show all link");
    check(ui_settings_scene_focus(&scene, target(10)) && !ui_settings_scene_focus(&scene, target(1)),
          "settings footnote: the link is a focus stop, the category is not");

    // Painting and text.
    PaintStream paint{};
    UiTextCommand commands[160]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    bool painted = paint_stream_initialize(&paint) && paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                   ui_settings_scene_paint(scene, kDefaultTheme, &paint, 96, nullptr) &&
                   ui_text_command_stream_initialize(&stream, {commands, 160}) &&
                   ui_settings_scene_text_output(scene, kDefaultTheme, &output);
    bool title = false, subtitle = false, hint = false, caption = false, tag = false, link = false, styled = false, eyebrow = false,
         esc = false, heading = false, button_rect = false, button_clip = false, segment_rect = false, lead_bold = false,
         rest_seen = false;
    Color lead_ink{}, rest_ink{};
    float label_left = -1, meta_left = -2;
    u32 bullets = 0;
    for (u32 i = 0; painted && i < stream.count; ++i) {
        const auto& c = commands[i];
        title = title || (utf8_equal(c.text, utf8_literal("Updates")) && (c.style_flags & text_style_heading_1) &&
                          (c.style_flags & text_style_bold));
        subtitle = subtitle || utf8_equal(c.text, utf8_literal("MaiCord updates itself from signed releases."));
        hint = hint || utf8_equal(c.text, utf8_literal("Ctrl F"));
        caption = caption || utf8_equal(c.text, utf8_literal("Takes a few seconds"));
        tag = tag || utf8_equal(c.text, utf8_literal("dev.38"));
        link = link || utf8_equal(c.text, utf8_literal("Show all notes"));
        // Each list item is its own themed command with a bullet of its own hanging in front of it.
        styled = styled || (c.effect.themed_styles && c.styles.count >= 1 && c.text.bytes > 20);
        bullets += c.text.bytes == 3 && utf8_equal(c.text, utf8_literal("\xE2\x80\xA2"));
        eyebrow = eyebrow || utf8_equal(c.text, utf8_literal("MAICORD"));
        esc = esc || utf8_equal(c.text, utf8_literal("ESC"));
        heading = heading || (utf8_equal(c.text, utf8_literal("You're up to date")) && (c.style_flags & text_style_heading_2));
        if (utf8_equal(c.text, utf8_literal("You're up to date")))
            label_left = c.rectangle.left;
        if (utf8_equal(c.text, utf8_literal("Check now"))) {
            // The refresh glyph leads the label: the label starts inside the button and ends at its edge, unclipped.
            button_rect = c.rectangle.left > status.field.left + 16 && close_to(c.rectangle.right, status.field.right);
            button_clip = c.clip.left <= c.rectangle.left + .01f && c.clip.right >= status.field.right - .01f;
        }
        if (utf8_equal(c.text, utf8_literal("MaiCord 0.5.4")) && c.style_flags == kUiKitTextSmallBold) {
            meta_left = c.rectangle.left;
            lead_ink = c.color;
            lead_bold = true;
        }
        if (utf8_equal(c.text, utf8_literal("\xC2\xB7 Stable channel"))) {
            rest_ink = c.color;
            rest_seen = true;
        }
        if (utf8_equal(c.text, utf8_literal("Preview")))
            segment_rect = c.clip.right - c.clip.left + .01f >= ui_kit_text_width(nullptr, utf8_literal("Preview"), kUiKitTextLabelBold);
    }
    check(painted && !paint.rejected && hint && eyebrow && !esc, "settings cards: the page paints; the sidebar's Ctrl F hint and group header emit, no ESC caption");
    check(title && subtitle && heading, "settings cards: the 24/30 title, the subtitle and the status card's heading emit");
    check(caption, "settings cards: the status caption emits");
    check(tag, "settings cards: the disclosure tag emits");
    check(link, "settings cards: the markdown link emits");
    check(styled && bullets >= 3, "settings cards: markdown items are themed text commands, each with its own bullet");
    check(button_rect && button_clip, "settings cards: a status button's glyph leads its label, which reaches the button's edge unclipped");
    check(lead_bold && rest_seen && !bytes_equal(&lead_ink, &rest_ink, sizeof(Color)) && close_to(meta_left, label_left),
          "settings cards: a status meta line leads with its bold, brighter identity where the title starts, then its parts");
    check(segment_rect, "settings cards: a segmented label's clip fits the label");

    // The sidebar's foot: the last two entries stand pinned under the scrolling pages.
    const float foot_x = scene.layout.sidebar.left + 100, foot_y = scene.layout.sidebar.bottom - m.sidebar_top - m.nav_item_gap - m.nav_item_height * .5f;
    check(ui_settings_target_equal(ui_settings_scene_hit_test(scene, foot_x, foot_y), {UiSettingsTargetKind::action, {}, 9}) &&
              ui_settings_target_equal(ui_settings_scene_hit_test(scene, foot_x, foot_y - m.nav_item_height - m.nav_item_gap),
                                       {UiSettingsTargetKind::page, {1, 3}, 0}) &&
              !ui_settings_scene_scrollbar(scene, true).visible && ui_settings_scene_focus(&scene, {UiSettingsTargetKind::page, {1, 3}, 0}),
          "settings sidebar: Developer and Log Out are pinned in the foot, always in view");

    // A running check: the spinner clock and the busy button.
    f.controls[5].checked = true;
    f.controls[5].enabled = false;
    f.meters[5] = kUiSettingsIndeterminateMeter;
    ++f.content.revision;
    check(ui_settings_scene_update(&scene, &registry, viewport) && ui_settings_scene_spinner_visible(scene) &&
              !ui_settings_scene_focus(&scene, target(6)) && ui_settings_scene_paint(scene, kDefaultTheme, &paint, 96, nullptr),
          "settings status card: a busy card keeps its disabled button, needs the spinner clock and paints the moving meter");
    f.controls[8].checked = true;
    ++f.content.revision;
    UiSettingsControlGeometry expanded{};
    check(ui_settings_scene_update(&scene, &registry, viewport) && ui_settings_scene_control_geometry(scene, target(9), &expanded) &&
              ui_rect_height(expanded.control) > ui_rect_height(markdown.control) + 60 && !ui_settings_scene_focus(&scene, target(9)),
          "settings markdown: showing all notes grows the row and drops the link");
    // The page's viewport reaches a focus ring's outset past the column's start edge; rows keep the column.
    const auto* body = ui_widget_manager_layout_node(scene.widgets, scene.body);
    check(body && close_to(body->rectangle.left, scene.layout.column.left - ui_settings_focus_reach(m)) &&
              close_to(body->content_rectangle.left, scene.layout.column.left),
          "settings focus: the body clip reaches a focus ring's outset past the column, whose rows stay put");

    // Layer and dialog pages keep their flat rows.
    UiSettingsScene layer{};
    static CardFixture flat{};
    build_card_fixture(&flat);
    UiPage layer_page{};
    UiPageRegistry layer_registry{};
    UiSettingsControlGeometry flat_toggle{};
    check(ui_page_registry_initialize(&layer_registry, {&layer_page, 1}) &&
              ui_page_registry_register(&layer_registry, {page, utf8_literal("Updates"), &flat.content}).ok() &&
              ui_settings_scene_update(&layer, &layer_registry, {0, 0, 1280, 900}) &&
              ui_settings_scene_control_geometry(layer, target(2), &flat_toggle) &&
              close_to(flat_toggle.field.right, layer.layout.column.right),
          "settings layer: pages outside the modal do not group into cards");
    ui_settings_scene_release(&layer);

    // A host page: no title, the wide column up to the close button, one focus stop.
    static UiSettingsPageContent store{utf8_literal("Plugins"), {}, {}, 1};
    store.host = true;
    store.wide = true;
    UiPage store_pages[1]{};
    UiPageRegistry store_registry{};
    UiSettingsScene host{};
    UiRect host_page{}, host_clip{};
    const UiPageKey store_key{1, 7};
    check(ui_page_registry_initialize(&store_registry, {store_pages, 1}) &&
              ui_page_registry_register(&store_registry, {store_key, utf8_literal("Plugins"), &store}).ok() &&
              ui_settings_scene_set_presentation(&host, UiSettingsPresentation::modal) &&
              ui_settings_scene_update(&host, &store_registry, host_viewport) && ui_settings_scene_host_rect(host, &host_page, &host_clip) &&
              close_to(ui_rect_width(host_page), 792) && close_to(host_page.left, host.layout.column.left) &&
              close_to(host_page.top, host.layout.column.top) && host_clip.bottom <= host.layout.panel.bottom + .01f &&
              ui_settings_scene_focus(&host, {UiSettingsTargetKind::host, store_key, 1}) &&
              ui_settings_scene_focus_zone(host) == UiSettingsFocusZone::panel,
          "settings host page: the wide column is 792 DIP at 1440x900, the page keeps one panel focus stop");
    PaintStream host_paint{};
    UiTextCommand host_commands[64]{};
    UiTextCommandStream host_stream{};
    UiTextOutput host_output{&host_stream, nullptr};
    bool no_heading = paint_stream_initialize(&host_paint) && paint_stream_reset(&host_paint, host_viewport.right, host_viewport.bottom) &&
                      ui_settings_scene_paint(host, kDefaultTheme, &host_paint, 96, nullptr) &&
                      ui_text_command_stream_initialize(&host_stream, {host_commands, 64}) &&
                      ui_settings_scene_text_output(host, kDefaultTheme, &host_output);
    for (u32 i = 0; no_heading && i < host_stream.count; ++i)
        no_heading = !utf8_equal(host_commands[i].text, utf8_literal("Plugins")) || !ui_settings_scene_page_owner(host, host_commands[i].owner);
    check(no_heading, "settings host page: the scene draws no heading of its own");
    paint_stream_release(&host_paint);
    ui_settings_scene_release(&host);
    ui_settings_scene_release(&scene);
    paint_stream_release(&paint);
}
// The sidebar scrolls to the selected entry when a page opens or the selection moves (a link, the keyboard), and
// keeps the user's own scrolling while the page stays.
void test_sidebar_follows_selection(Check check) noexcept {
    constexpr u32 kPages = 30;
    static UiPage pages[kPages];
    static UiSettingsPageContent content{utf8_literal("Page"), {}, {}, 1};
    static UiSettingsNavEntry navigation[kPages];
    UiPageRegistry registry{};
    UiSettingsScene scene{};
    bool built = ui_page_registry_initialize(&registry, {pages, kPages});
    for (u32 i = 0; built && i < kPages; ++i) {
        navigation[i] = {UiSettingsNavKind::page, false, UiIcon::puzzle, utf8_literal("Entry"), {1, i + 1}, 0};
        built = ui_page_registry_register(&registry, {{1, i + 1}, utf8_literal("Entry"), &content}).ok();
    }
    const UiRect viewport{0, 0, 1440, 600};
    const auto select = [&](u64 page) noexcept {
        return ui_page_registry_select(&registry, {1, page}).ok() && ui_settings_scene_update(&scene, &registry, viewport);
    };
    // Opening on the last entry scrolls it into view; the first entry brings the list back to the top.
    check(built && ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal) &&
              ui_settings_scene_bind_navigation(&scene, {navigation, kPages}, 1) && select(kPages) &&
              scene.sidebar_offset > 0,
          "settings sidebar: a page that opens on a far entry scrolls it into view");
    const float last = scene.sidebar_offset;
    ui_settings_scene_scroll(&scene, true, 100000);
    check(close_to(scene.sidebar_offset, last) && select(1) && close_to(scene.sidebar_offset, 0) && select(15) &&
              scene.sidebar_offset > 0 && scene.sidebar_offset < last,
          "settings sidebar: selecting another page by link or keyboard scrolls its entry into view, both ways");
    const float middle = scene.sidebar_offset;
    ui_settings_scene_scroll(&scene, true, 40);
    ++content.revision;
    check(ui_settings_scene_update(&scene, &registry, viewport) && close_to(scene.sidebar_offset, middle + 40),
          "settings sidebar: the same page keeps the user's own scroll position");
    ui_settings_scene_release(&scene);
}
// Segmented rows of 2, 3 and 5 options: the pointer over a segment hovers exactly that segment (index and painted
// plate), clicks report it, and a row never shows the option another row last hovered.
void test_segment_hover(Check check) noexcept {
    static const Utf8View labels[5]{utf8_literal("One"), utf8_literal("Two"), utf8_literal("Three"), utf8_literal("Four"),
                                    utf8_literal("Five")};
    static UiSettingsControl controls[3];
    static UiSettingsPageContent content{};
    static UiPage pages[1];
    UiPageRegistry registry{};
    UiSettingsScene scene{};
    const u32 counts[3]{2, 3, 5};
    for (u32 r = 0; r < 3; ++r)
        controls[r] = {r + 1u, utf8_literal("Row"), {}, UiSettingsControlKind::segments, true, false, {labels, counts[r]}, 0};
    content = {utf8_literal("Segments"), {}, {controls, 3}, 1};
    const UiPageKey page{1, 1};
    const UiRect viewport{0, 0, 1440, 900};
    PaintStream paint{};
    const bool built = ui_page_registry_initialize(&registry, {pages, 1}) &&
                       ui_page_registry_register(&registry, {page, utf8_literal("Segments"), &content}).ok() &&
                       ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal) &&
                       ui_settings_scene_update(&scene, &registry, viewport) && paint_stream_initialize(&paint);
    check(built, "settings segments: a page of 2, 3 and 5 segment rows builds");
    if (!built)
        return;
    const UiSettingsThemeTokens tokens(kDefaultTheme);
    bool hovered_index = true, plate = true, clicked = true, stale = true;
    for (u32 r = 0; r < 3; ++r) {
        const UiSettingsTarget target{UiSettingsTargetKind::control, page, r + 1u};
        for (u32 i = 0; i < counts[r]; ++i) {
            UiRect option{}, others{};
            if (!ui_settings_scene_choice_rectangle(scene, target, i, &option)) {
                hovered_index = false;
                continue;
            }
            const float x = (option.left + option.right) * .5f, y = (option.top + option.bottom) * .5f;
            ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, {x, y});
            // Another row's leftover hover must not decide this row: start from a wrong index.
            scene.palette_hovered_index = (i + 1) % counts[r];
            ui_settings_scene_palette_hover(&scene, target, x, y);
            hovered_index = hovered_index && scene.palette_hovered_index == i;
            UiSettingsActivation activation{};
            clicked = clicked && ui_settings_scene_option_pointer(scene, target, x, y, &activation) &&
                      activation.selected_choice == i && activation.kind == UiSettingsControlKind::segments;
            // The plate: the selected segment (0) has its own look; any other hovered segment gets the hover fill, and
            // no other segment of any row does.
            plate = plate && paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                    ui_settings_scene_paint(scene, kDefaultTheme, &paint, 96, nullptr);
            u32 matches = 0, elsewhere = 0;
            for (u32 q = 0; q < paint.count; ++q) {
                const auto& quad = paint.quads[q];
                if (!bytes_equal(&quad.fill, &tokens.color.mod_subtle, sizeof(Color)))
                    continue;
                for (u32 rr = 0; rr < 3; ++rr)
                    for (u32 j = 0; j < counts[rr]; ++j) {
                        const UiSettingsTarget other{UiSettingsTargetKind::control, page, rr + 1u};
                        if (ui_settings_scene_choice_rectangle(scene, other, j, &others) &&
                            close_to(quad.left, others.left) && close_to(quad.top, others.top) &&
                            close_to(quad.right, others.right) && close_to(quad.bottom, others.bottom)) {
                            if (rr == r && j == i)
                                ++matches;
                            else
                                ++elsewhere;
                        }
                    }
            }
            plate = plate && elsewhere == 0 && matches == (i ? 1u : 0u);
        }
        // Leaving the segments clears the hover: the pointer on the page's corner is no option.
        UiSettingsActivation none{};
        ui_settings_scene_palette_hover(&scene, target, 1, 1);
        stale = stale && !ui_settings_scene_option_pointer(scene, target, 1, 1, &none) &&
                scene.palette_hovered_index == ~u32{0};
    }
    check(hovered_index, "settings segments: hovering any segment of a 2-, 3- and 5-segment row hovers exactly that index");
    check(plate, "settings segments: the hover plate is painted on the hovered segment alone, never on another segment or row");
    check(clicked, "settings segments: a click on any segment reports that segment");
    check(stale && scene.palette_hovered_index == ~u32{0}, "settings segments: moving off the segments clears the hovered option");
    paint_stream_release(&paint);
    ui_settings_scene_release(&scene);
}
// A status meta line wraps between its parts only: a break drops its separator (no line ends with a dot or starts
// with one), every line starts at the title's left edge, and the identity stays bold and brighter than the rest.
void test_status_meta_wrap(Check check) noexcept {
    static UiSettingsControl controls[1];
    static UiSettingsControlPresentation looks[1];
    static UiSettingsPageContent content{};
    static UiPage pages[1];
    controls[0] = {1, utf8_literal("You're up to date"),
                   utf8_literal("MaiCord 0.5.5-dev.9+de4eb0a \xC2\xB7 Stable channel \xC2\xB7 Checked today at 4:34 PM"),
                   UiSettingsControlKind::status, true, false, {}, 0, utf8_literal("Check now")};
    looks[0] = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    looks[0].more = ui_settings_more_meta_parts;
    content = {utf8_literal("Updates"), {}, {controls, 1}, 1};
    content.presentation = looks;
    UiPageRegistry registry{};
    UiSettingsScene scene{};
    const UiPageKey page{1, 1};
    check(ui_page_registry_initialize(&registry, {pages, 1}) &&
              ui_page_registry_register(&registry, {page, utf8_literal("Updates"), &content}).ok() &&
              ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal),
          "settings status meta: a status page builds");
    const UiSettingsThemeTokens tokens(kDefaultTheme);
    const float widths[]{1800, 1500, 1152, 960, 840, 760, 700, 640};
    bool invariants = true, wrapped = false, whole = false;
    for (const float width : widths) {
        ++content.revision;
        UiTextCommand commands[160]{};
        UiTextCommandStream stream{};
        UiTextOutput output{&stream, nullptr};
        invariants = invariants && ui_settings_scene_update(&scene, &registry, {0, 0, width, 900}) &&
                     ui_text_command_stream_initialize(&stream, {commands, 160}) &&
                     ui_settings_scene_text_output(scene, kDefaultTheme, &output);
        const UiTextCommand* parts[3]{};
        float title_left = -1;
        for (u32 i = 0; i < stream.count; ++i) {
            const auto& c = commands[i];
            const auto ends = [&](const char* tail) noexcept {
                const u32 n = c_string_length(tail);
                return c.text.bytes >= n && bytes_equal(c.text.data + c.text.bytes - n, tail, n);
            };
            if (utf8_equal(c.text, utf8_literal("You're up to date")))
                title_left = c.rectangle.left;
            if (c.style_flags == kUiKitTextSmallBold && utf8_equal(c.text, utf8_literal("MaiCord 0.5.5-dev.9+de4eb0a")))
                parts[0] = &c;
            else if (ends("Stable channel"))
                parts[1] = &c;
            else if (ends("Checked today at 4:34 PM"))
                parts[2] = &c;
        }
        if (!parts[0] || !parts[1] || !parts[2]) {
            invariants = false;
            continue;
        }
        const auto dotted = [](const UiTextCommand& c) noexcept {
            return c.text.bytes > 2 && static_cast<u8>(c.text.data[0]) == 0xC2 && static_cast<u8>(c.text.data[1]) == 0xB7;
        };
        bool all_on_one_line = true;
        for (u32 p = 0; p < 3; ++p) {
            const auto& c = *parts[p];
            invariants = invariants && !(c.text.bytes >= 2 && static_cast<u8>(c.text.data[c.text.bytes - 2]) == 0xC2 &&
                                         static_cast<u8>(c.text.data[c.text.bytes - 1]) == 0xB7);
            if (!p) {
                invariants = invariants && close_to(c.rectangle.left, title_left) && !dotted(c);
                continue;
            }
            const auto& before = *parts[p - 1];
            const bool same_line = close_to(c.rectangle.top, before.rectangle.top);
            all_on_one_line = all_on_one_line && same_line;
            // On the line of its predecessor the part carries its separator in front; on a new line it starts the
            // line at the left edge without one.
            invariants = invariants && (same_line ? dotted(c) && c.rectangle.left > before.rectangle.right - .01f
                                                  : !dotted(c) && close_to(c.rectangle.left, title_left) &&
                                                        c.rectangle.top > before.rectangle.top);
        }
        wrapped = wrapped || !all_on_one_line;
        whole = whole || all_on_one_line;
        // The identity is brighter than the other parts, which use the subtle ink.
        invariants = invariants && !bytes_equal(&parts[0]->color, &parts[1]->color, sizeof(Color)) &&
                     bytes_equal(&parts[1]->color, &tokens.color.text_subtle, sizeof(Color));
    }
    check(invariants, "settings status meta: a break drops its separator and every line starts at the left edge, the identity bold and brighter");
    check(wrapped && whole, "settings status meta: the line fits at the wide card and wraps part by part at the narrow ones");
    ui_settings_scene_release(&scene);
}
} // namespace
void test_settings_scene(void (*check)(bool, const char*) noexcept) noexcept {
    test_controls(check);
    test_schema(check);
    test_modal_scene(check);
    test_card_pages(check);
    test_layer_scene(check);
    test_dialog_scene(check);
    test_focus_recovery(check);
    test_segment_hover(check);
    test_status_meta_wrap(check);
    test_sidebar_follows_selection(check);
}
} // namespace maicord
