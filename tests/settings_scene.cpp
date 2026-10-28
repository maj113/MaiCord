#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/settings_schema.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/theme_styles.hpp"
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
    const auto tokens = ui_settings_tokens(kDefaultTheme);
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
        for (u32 i = 0; i < 49; ++i)
            copy_bytes(packed + i * sizeof(u16), &values[(i + shift) % 7], sizeof(u16));
        const auto resolved = ui_settings_layout_tokens(&varied);
        for (u32 i = 0; i < 49; ++i) {
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
    check(ui_settings_scene_focused(scene).kind == UiSettingsTargetKind::search &&
              ui_settings_scene_focus_zone(scene) == UiSettingsFocusZone::search &&
              ui_settings_scene_control_geometry(scene, toggle, &geometry) && close_to(ui_rect_width(geometry.field), 48) &&
              close_to(geometry.field.right, scene.layout.column.right),
          "settings modal: search takes initial focus; switches sit at the column edge");
    const auto search = ui_widget_manager_layout_node(scene.widgets, scene.search_field);
    UiSettingsControlGeometry search_geometry{};
    check(search && ui_settings_scene_search_geometry(scene, &search_geometry) &&
              ui_settings_target_equal(ui_settings_scene_hit_test(scene, center(search->rectangle).x,
                                                                  center(search->rectangle).y),
                                       {UiSettingsTargetKind::search, {}, 0}) &&
              search_geometry.text.left > search->rectangle.left,
          "settings modal: the sidebar search field is a hit target with editor geometry");
    UiSettingsEditor query{{utf8_literal("emb"), 3, 3, true, true, true}, nullptr, utf8_literal("Search")};
    const auto clear = search ? ui_settings_search_clear_rect(ui_settings_tokens(kDefaultTheme), search->rectangle)
                              : UiRect{};
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
    const auto tokens = ui_settings_tokens(kDefaultTheme);
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
    check(switch_ok && hits(toggle, knob.x, knob.y) && misses(switch_geometry.control.left + 8, knob.y) &&
              misses(switch_geometry.field.left - 24, knob.y) &&
              misses(knob.x, switch_geometry.control.bottom - 1),
          "settings hit-test: a toggle row answers only on its switch");
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
    // Hover follows the same region: the label leaves the row unhovered and
    // the switch hovers it, so the switch alone paints its hover state.
    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, {switch_geometry.control.left + 8, knob.y});
    const bool label_cold = !scene.widgets.hovered.generation;
    const auto hovered = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::move, knob);
    check(label_cold && hovered.target.generation && scene.widgets.hovered.slot == switch_geometry.owner.handle.slot,
          "settings hit-test: hovering a row label does not hover the control; the switch does");
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
    const auto swatch_tokens = ui_settings_tokens(kDefaultTheme);
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
        escape = escape || utf8_equal(commands[i].text, utf8_literal("ESC"));
        marks = marks || utf8_equal(commands[i].text, utf8_literal("18px"));
        logout = logout || utf8_equal(commands[i].text, utf8_literal("Log out"));
    }
    check(painted && page_started && contiguous && header && escape && marks && logout,
          "settings modal: chrome text precedes contiguous page text; headers, ESC and marks render");

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
    const auto tokens = ui_settings_tokens(kDefaultTheme);
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
                                                               scene.widgets.storage[scene.footer_status.slot].parent)
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
} // namespace

void test_settings_scene(void (*check)(bool, const char*) noexcept) noexcept {
    test_controls(check);
    test_schema(check);
    test_modal_scene(check);
    test_layer_scene(check);
    test_dialog_scene(check);
}
} // namespace maicord
