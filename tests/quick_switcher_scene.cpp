#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
namespace {
struct QuickFixture { u32 calls{}; u64 base{1000}; bool current{true}, duplicate{}, mixed{}; };
bool quick_current(void* opaque) noexcept { return static_cast<QuickFixture*>(opaque)->current; }
bool quick_row(void* opaque, u32 index, UiQuickSwitcherRow* row) noexcept {
    auto& f = *static_cast<QuickFixture*>(opaque); ++f.calls;
    const u64 key = f.duplicate && index == 1 ? f.base : f.base + index;
    *row = {key, 71, 100 + index, 0, f.mixed && index == 3 ? UiQuickSwitcherKind::guild : UiQuickSwitcherKind::text_channel,
        utf8_literal("release-planning with an intentionally long channel name"),
        utf8_literal("Original server • a useful destination label"), utf8_literal("Channel"), {}, index != 1};
    return true;
}
// People and DMs with status: a phone (online on mobile only), an idle DM, a
// channel (never a dot) and a stranger (unknown, no dot).
bool quick_person(void*, u32 index, UiQuickSwitcherRow* row) noexcept {
    constexpr UiQuickSwitcherKind kinds[]{UiQuickSwitcherKind::user, UiQuickSwitcherKind::private_channel,
                                          UiQuickSwitcherKind::text_channel, UiQuickSwitcherKind::user};
    constexpr UiPresence presences[]{UiPresence::online, UiPresence::idle, UiPresence::online, UiPresence::unknown};
    *row = {500 + index, 0, 600 + index, 700 + index, kinds[index], utf8_literal("Person"), {}, {}, {}, true,
            presences[index], index == 0};
    return true;
}
} // namespace
void test_quick_switcher_scene(void (*check)(bool, const char*) noexcept) noexcept {
    UiQuickSwitcherScene scene{};
    QuickFixture fixture{};
    UiQuickSwitcherSource source{&fixture, 100, 1, quick_row, quick_current};
    UiQuickSwitcherContent content{};
    content.phase = UiQuickSwitcherPhase::ready;
    content.revision = 1;
    content.edit = {utf8_literal("release"), 7, 7, true, true, true};
    const UiRect viewport{0, 0, 1280, 800};
    check(!scene.storage.arena.base && ui_quick_switcher_scene_update(&scene, &source, &content, viewport) &&
        scene.accepted_count == 100 && scene.count <= kUiQuickSwitcherVisibleRows && fixture.calls == 100 &&
        ui_quick_switcher_scene_selected(scene).key == 1000 &&
        scene.widgets.count == kUiQuickSwitcherWidgetCapacity,
        "quick switcher: lazy bounded receipt selects the first available route without eager widgets");
    {
        // Rows align with the search field; only a scrollable list keeps a bar gutter.
        UiQuickSwitcherScene few{};
        QuickFixture few_fixture{};
        UiQuickSwitcherSource few_source{&few_fixture, 3, 1, quick_row, quick_current};
        const bool short_list = ui_quick_switcher_scene_update(&few, &few_source, &content, viewport) &&
                                few.count == 3 && ui_quick_switcher_scene_scroll_maximum(few) == 0;
        const auto row = few.row_rectangles[0], field = few.field, gutter = scene.row_rectangles[0];
        check(short_list && row.left == field.left && row.right == field.right && gutter.left == field.left &&
                  gutter.right < scene.field.right,
              "quick switcher: result rows share the field edges and reserve a gutter only while scrollable");
        ui_quick_switcher_scene_release(&few);
    }
    const auto edit = scene.edit;
    const u32 resolves = fixture.calls, layout = scene.widgets.layout.revision, revision = scene.revision;
    bool retained = true;
    for (u32 i = 0; i < 4; ++i)
        retained &= ui_quick_switcher_scene_update(&scene, &source, &content, viewport);
    check(retained && fixture.calls == resolves && scene.widgets.layout.revision == layout && scene.revision == revision,
        "quick switcher: unchanged receipt performs no row resolution or layout work");
    for (u32 i = 0; i < 4; ++i) {
        content.edit.caret_visible = !content.edit.caret_visible; ++content.revision;
        retained &= ui_quick_switcher_scene_update(&scene, &source, &content, viewport);
    }
    check(retained && fixture.calls == resolves && scene.widgets.layout.revision == layout &&
        scene.edit.slot == edit.slot && scene.edit.generation == edit.generation,
        "quick switcher: caret and content updates preserve native editor identity without resolving results");
    check(ui_quick_switcher_scene_move(&scene, UiQuickSwitcherMove::next) &&
        ui_quick_switcher_scene_selected(scene).index == 2 &&
        ui_quick_switcher_scene_move(&scene, UiQuickSwitcherMove::previous) &&
        ui_quick_switcher_scene_selected(scene).index == 0 &&
        ui_quick_switcher_scene_move(&scene, UiQuickSwitcherMove::page_next) && scene.selected >= 2 &&
        ui_quick_switcher_scene_move(&scene, UiQuickSwitcherMove::last) && scene.selected == 99 &&
        scene.scroll_dips == ui_quick_switcher_scene_scroll_maximum(scene) && fixture.calls == resolves,
        "quick switcher: logical arrows pages and End reveal every accepted result and skip unavailable routes");
    bool last_visible = false;
    for (u32 i = 0; i < scene.count; ++i) {
        if (scene.first + i != 99) continue;
        const auto r = scene.row_rectangles[i];
        const auto hit = ui_quick_switcher_scene_hit_test(scene, r.left + 50, (r.top + r.bottom) * .5f);
        last_visible = hit.key == 1099 && hit.channel_id == 199 && hit.source_revision == source.revision;
    }
    check(last_visible && ui_quick_switcher_scene_contains(scene, 0, 0) &&
        ui_quick_switcher_scene_hit_test(scene, 0, 0).action == UiQuickSwitcherAction::close &&
        !ui_quick_switcher_scene_contains(scene, -1, 0),
        "quick switcher: virtual pointer targets keep exact route identity and modal backdrop shields covered UI");
    ui_quick_switcher_scene_move(&scene, UiQuickSwitcherMove::first);
    const auto first = scene.row_rectangles[0];
    const UiPoint point{first.left + 50, first.top + 28};
    const auto press = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::press, point);
    fixture.base = 5000; ++source.revision; ++content.revision;
    const bool receipt = ui_quick_switcher_scene_update(&scene, &source, &content, viewport);
    const auto release = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::release, point);
    check(press.handled && receipt && !scene.widgets.captured.generation && !release.activate &&
        scene.edit.slot == edit.slot && scene.edit.generation == edit.generation &&
        ui_quick_switcher_scene_selected(scene).key == 5000,
        "quick switcher: replacement result bank cancels row capture before a slot can activate another destination");
    ui_quick_switcher_scene_select(&scene, 3);
    ++source.revision; ++content.revision;
    check(ui_quick_switcher_scene_update(&scene, &source, &content, viewport) && scene.selected == 3,
        "quick switcher: authoritative republish retains selection by destination identity");
    fixture.mixed = true; ++source.revision; ++content.revision;
    check(ui_quick_switcher_scene_update(&scene, &source, &content, viewport) && scene.selected == 0,
        "quick switcher: reused numeric destination ID of another route kind cannot retain selection");

    PaintStream paint{};
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    bool painted = paint_stream_initialize(&paint) && ui_text_command_stream_initialize(&stream, {commands, 128});
    const Theme themes[]{kDefaultTheme, kLightTheme, ui_theme_amoled()};
    const UiRect viewports[]{viewport, {0, 0, 320, 360}, {0, 0, 1920, 1080}};
    for (u32 i = 0; i < 3; ++i) {
        painted &= ui_quick_switcher_scene_update(&scene, &source, &content, viewports[i]);
        paint_stream_reset(&paint, viewports[i].right, viewports[i].bottom);
        ui_text_command_stream_reset(&stream);
        painted &= ui_quick_switcher_scene_paint(scene, themes[i], &paint, nullptr, i == 1 ? 144 : 96) &&
            ui_quick_switcher_scene_text_output(scene, themes[i], &output) && !paint.rejected && !stream.rejected &&
            paint.clip_depth == 1 && ui_rect_width(scene.text) >= 180.f;
        for (u32 c = 0; c < stream.count; ++c)
            painted &= commands[c].clip.left >= scene.panel.left && commands[c].clip.right <= scene.panel.right &&
                commands[c].clip.top >= scene.panel.top && commands[c].clip.bottom <= scene.panel.bottom;
    }
    check(painted, "quick switcher: minimum default wide DPI and theme layouts keep query and result text clipped coherently");
    {
        UiQuickSwitcherScene people{};
        QuickFixture people_fixture{};
        UiQuickSwitcherSource people_source{&people_fixture, 4, 1, quick_person, quick_current};
        bool dots = ui_quick_switcher_scene_update(&people, &people_source, &content, viewport) && people.count == 4;
        // The 24-DIP avatar at (10, 16) in the row gets the DM header's 7.5-DIP dot.
        const auto row = people.row_rectangles[0];
        const auto dot = ui_quick_switcher_scene_status_rectangle(people, 0);
        dots = dots && dot.left == row.left + 26.5f && dot.top == row.top + 32.5f && dot.right == row.left + 34 &&
               dot.bottom == row.top + 40 && ui_rect_has_area(ui_quick_switcher_scene_status_rectangle(people, 1)) &&
               !ui_rect_has_area(ui_quick_switcher_scene_status_rectangle(people, 2)) &&
               !ui_rect_has_area(ui_quick_switcher_scene_status_rectangle(people, 3));
        paint_stream_reset(&paint, 1280, 800);
        dots = dots && ui_quick_switcher_scene_presence_paint(people, kDefaultTheme, &paint) && !paint.rejected &&
               paint.clip_depth == 1;
        // The phone keeps the dot's width and corner and stands 1.5 dots tall.
        const auto phone = ui_presence_mobile_geometry(dot, 2.f);
        bool body = false, idle = false;
        for (u32 q = 0; q < paint.count; ++q) {
            const auto& quad = paint.quads[q];
            body = body || (quad.left == phone.body.left && quad.top == phone.body.top &&
                            quad.right == phone.body.right && quad.bottom == phone.body.bottom &&
                            quad.fill.green == kDefaultTheme.tokens.status.online.green);
            idle = idle || (quad.fill.red == kDefaultTheme.tokens.status.idle.red &&
                            quad.fill.green == kDefaultTheme.tokens.status.idle.green &&
                            quad.bottom == people.row_rectangles[1].top + 42);
        }
        check(dots && body && idle && phone.body.top == row.top + 28.75f,
              "quick switcher: people and DM rows draw status over the avatar, the phone for mobile-only");
        ui_quick_switcher_scene_release(&people);
    }
    source.count = 0; ++source.revision; ++content.revision;
    content.phase = UiQuickSwitcherPhase::no_matches;
    check(ui_quick_switcher_scene_update(&scene, &source, &content, viewport) && !scene.count &&
        ui_quick_switcher_scene_selected(scene).action == UiQuickSwitcherAction::none &&
        !ui_quick_switcher_scene_move(&scene, UiQuickSwitcherMove::next),
        "quick switcher: no matches cannot retain or activate a retired selection");
    fixture.current = false;
    paint_stream_reset(&paint, 1280, 800); ui_text_command_stream_reset(&stream);
    check(!ui_quick_switcher_scene_current(scene) &&
        ui_quick_switcher_scene_hit_test(scene, scene.field.left + 8, scene.field.top + 8).action == UiQuickSwitcherAction::none &&
        ui_quick_switcher_scene_paint(scene, kDefaultTheme, &paint) && !paint.count &&
        ui_quick_switcher_scene_text_output(scene, kDefaultTheme, &output) && !stream.count,
        "quick switcher: retired account or query authority exposes no stale editor routes paint or text");
    fixture.current = true; fixture.duplicate = true;
    source.count = 2; ++source.revision; ++content.revision;
    check(!ui_quick_switcher_scene_update(&scene, &source, &content, viewport) && !ui_quick_switcher_scene_current(scene),
        "quick switcher: duplicate route identity fails before publishing a partial bank");
    paint_stream_release(&paint); ui_quick_switcher_scene_release(&scene);
    check(!scene.active && !scene.storage.arena.base, "quick switcher: close releases the lazy native bank");
}
} // namespace maicord
