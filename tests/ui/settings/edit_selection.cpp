// A focused multi-line settings editor with its whole text selected (Ctrl+A in the profile Bio): the edit layout
// reports a selected empty line as a zero-width rectangle. The page takes that geometry, lays out, paints and outputs
// its text, and the empty rectangle paints nothing; a rectangle with reversed edges is still refused.
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/text/edit_field.hpp"
#include "maicord/ui/text/presentation.hpp"
#include "maicord/ui/core/paint.hpp"
#include "../../support/test_output.hpp"

namespace maicord::settings_edit_selection_test {
namespace {
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
bool close_to(float a, float b) noexcept {
    return a - b < .01f && b - a < .01f;
}
bool painted(const PaintStream& paint, const UiRect& r) noexcept {
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        if (close_to(q.left, r.left) && close_to(q.top, r.top) && close_to(q.right, r.right) &&
            close_to(q.bottom, r.bottom))
            return true;
    }
    return false;
}

void test_selected_empty_line() noexcept {
    UiSettingsScene scene{};
    UiPage page{};
    UiPageRegistry registry{};
    UiEditGeometry geometry{};
    const auto bio = utf8_literal("First line\n\nThird line");
    UiSettingsEditor editor{{bio, 0, bio.bytes, true, true, true}, &geometry, {}};
    UiSettingsControl controls[]{
        {1, utf8_literal("Bio"), {}, UiSettingsControlKind::multiline, true, false, {}, 0, {}, &editor}};
    UiSettingsPageContent content{utf8_literal("Profiles"), {}, {controls, 1}, 1, kUiSettingsPageFormat, nullptr,
                                  nullptr};
    const UiPageKey key{9, 1};
    const UiRect viewport{0, 0, 1280, 900};
    const UiSettingsTarget target{UiSettingsTargetKind::control, key, 1};
    UiSettingsControlGeometry field{};
    const bool ready = ui_page_registry_initialize(&registry, {&page, 1}) &&
                       ui_page_registry_register(&registry, {key, utf8_literal("Profiles"), &content}).ok() &&
                       ui_settings_scene_update(&scene, &registry, viewport) && ui_settings_scene_focus(&scene, target) &&
                       ui_settings_scene_control_geometry(scene, target, &field) && field.multiline;
    check(ready, "settings editor: a focused multi-line field");
    // What the edit layout publishes for the three lines selected: the blank line's rectangle has no width.
    const UiRect& t = field.text;
    const float line = ui_rect_height(t) / 3;
    geometry.text = t;
    geometry.caret = {t.left + 70, t.top + 2 * line, t.left + 71, t.top + 3 * line};
    geometry.selections[0] = {t.left, t.top, t.left + 64, t.top + line};
    geometry.selections[1] = {t.left, t.top + line, t.left, t.top + 2 * line};
    geometry.selections[2] = {t.left, t.top + 2 * line, t.left + 70, t.top + 3 * line};
    geometry.selection_count = 3;
    // The next projection (a caret blink, the next key) lays the page out with that geometry.
    ++content.revision;
    check(ready && ui_settings_scene_update(&scene, &registry, viewport),
          "settings editor: a selection over an empty line lays out");
    PaintStream paint{};
    UiTextCommand commands[64]{};
    UiTextCommandStream text{};
    UiTextOutput output{&text, nullptr};
    const bool drawn = paint_stream_initialize(&paint) && paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                       ui_settings_scene_paint(scene, kDefaultTheme, &paint, 96, nullptr) &&
                       ui_text_command_stream_initialize(&text, {commands, 64}) &&
                       ui_settings_scene_text_output(scene, kDefaultTheme, &output);
    check(drawn, "settings editor: a selection over an empty line paints and outputs its text");
    u32 empty = 0;
    for (u32 i = 0; i < paint.count; ++i)
        empty += !ui_rect_has_area({paint.quads[i].left, paint.quads[i].top, paint.quads[i].right,
                                    paint.quads[i].bottom});
    check(drawn && painted(paint, geometry.selections[0]) && painted(paint, geometry.selections[2]) && !empty,
          "settings editor: the text lines' selection paints, the empty line's paints nothing");
    geometry.selections[1] = {t.left + 4, t.top + line, t.left, t.top + 2 * line};
    ++content.revision;
    check(!ui_settings_scene_update(&scene, &registry, viewport),
          "settings editor: a selection rectangle with reversed edges is still refused");
    paint_stream_release(&paint);
    ui_settings_scene_release(&scene);
}
} // namespace
} // namespace maicord::settings_edit_selection_test

extern "C" __declspec(noreturn) void WINAPI maicord_settings_edit_selection_entry() noexcept {
    using namespace maicord::settings_edit_selection_test;
    test_selected_empty_line();
    maicord::test_exit(failures, "settings_edit_selection");
}
