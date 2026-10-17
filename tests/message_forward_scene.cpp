#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
namespace {
struct ForwardFixture { u32 calls{}; u64 base{1000}; bool current{true}, duplicate{}, invalid{}; };
bool forward_current(void* opaque) noexcept { return static_cast<ForwardFixture*>(opaque)->current; }
bool forward_row(void* opaque, u32 index, UiMessageForwardRow* row) noexcept {
    auto& f = *static_cast<ForwardFixture*>(opaque); ++f.calls;
    const u64 id = f.duplicate && index == 1 ? f.base : f.base + index;
    *row = {id, 71, id, 0, f.invalid ? UiQuickSwitcherKind::voice_channel : index % 3 == 0 ?
        UiQuickSwitcherKind::private_channel : UiQuickSwitcherKind::text_channel,
        utf8_literal("release-planning with a deliberately long destination name"),
        utf8_literal("Original server • Text channel"), {}, {}, index != 1};
    return true;
}
}
void test_message_forward_scene(void (*check)(bool, const char*) noexcept) noexcept {
    UiMessageForwardScene s{};
    ForwardFixture fixture{};
    UiMessageForwardSource source{&fixture, 100, 1, forward_row, forward_current};
    UiMessageForwardContent c{};
    c.revision = 1; c.author = utf8_literal("Avery");
    c.preview = utf8_literal("The prototype is ready to share. Native rendering keeps the conversation clear.");
    c.media_summary = utf8_literal("1 image • prototype.png");
    c.edit = {utf8_literal("release"), 7, 7, true, true, true};
    c.caption = {utf8_literal("For our next review"), 19, 19, true, false, false};
    const UiRect viewport{0, 0, 1280, 800};
    check(!s.storage.arena.base && ui_message_forward_scene_update(&s, &source, &c, viewport) &&
        fixture.calls == 100 && s.accepted_count == 100 && s.widgets.count == kUiMessageForwardWidgetCapacity &&
        s.count <= kUiMessageForwardVisibleRows && ui_message_forward_scene_selected(s).channel_id == 1000,
        "forward scene: lazy bounded destination bank keeps two stable native editors");
    const auto edit = s.edit, caption = s.caption;
    const u32 resolves = fixture.calls, revision = s.revision, layout = s.widgets.layout.revision;
    bool warm = true;
    for (u32 i = 0; i < 64; ++i) warm &= ui_message_forward_scene_update(&s, &source, &c, viewport);
    check(warm && fixture.calls == resolves && s.revision == revision && s.widgets.layout.revision == layout,
        "forward scene: 64 unchanged updates allocate resolve and relayout no destinations");
    c.edit.caret_visible = false; ++c.revision;
    check(ui_message_forward_scene_update(&s, &source, &c, viewport) && fixture.calls == resolves &&
        s.widgets.layout.revision == layout && s.edit.slot == edit.slot && s.caption.slot == caption.slot &&
        s.edit.generation == edit.generation && s.caption.generation == caption.generation,
        "forward scene: text-only updates retain editor identities and destination layout");
    bool keyboard = ui_message_forward_scene_move(&s, UiQuickSwitcherMove::next) && s.selected == 2 &&
        ui_message_forward_scene_move(&s, UiQuickSwitcherMove::previous) && s.selected == 0 &&
        ui_message_forward_scene_move(&s, UiQuickSwitcherMove::last) && s.selected == 99 &&
        s.scroll_dips == ui_message_forward_scene_scroll_maximum(s);
    const auto last = s.row_rectangles[s.count - 1];
    keyboard &= ui_message_forward_scene_hit_test(s, last.left + 60, (last.top + last.bottom) * .5f).channel_id == 1099;
    check(keyboard, "forward scene: keyboard reaches all100 routes and skips unavailable rows");
    ui_message_forward_scene_move(&s, UiQuickSwitcherMove::first);
    const auto first = s.row_rectangles[0];
    const UiPoint point{first.left + 60, first.top + 26};
    const auto press = ui_button_pointer(&s.widgets, UiButtonPointerEvent::press, point);
    fixture.base = 5000; ++source.revision; ++c.revision;
    const bool accepted = ui_message_forward_scene_update(&s, &source, &c, viewport);
    const auto release = ui_button_pointer(&s.widgets, UiButtonPointerEvent::release, point);
    check(press.handled && accepted && !release.activate && !s.widgets.captured.generation &&
        ui_message_forward_scene_selected(s).channel_id == 5000,
        "forward scene: bank replacement cancels pressed rows before another destination can inherit capture");
    c.selected_count = 5; c.can_send = true;
    for (u32 i = 0; i < 5; ++i) c.selected[i] = {5000 + i, utf8_literal("release-planning"), {}, UiMessageForwardState::ready};
    ++c.revision;
    check(ui_message_forward_scene_update(&s, &source, &c, viewport) &&
        ui_message_forward_scene_target(s, s.targets[0]).action == UiMessageForwardAction::remove &&
        ui_message_forward_scene_target(s, s.rows[0]).action == UiMessageForwardAction::toggle &&
        ui_message_forward_scene_target(s, s.rows[5]).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_target(s, s.send).action == UiMessageForwardAction::send &&
        ui_message_forward_scene_hit_test(s, 0, 0).action == UiMessageForwardAction::close,
        "forward scene: five selected receipts disable extra selections and modal backdrop shields covered controls");
    c.selected[2].state = UiMessageForwardState::failed; c.can_send = false; ++c.revision;
    check(ui_message_forward_scene_update(&s, &source, &c, viewport) &&
        ui_message_forward_scene_target(s, s.targets[2]).action == UiMessageForwardAction::remove &&
        ui_message_forward_scene_target(s, s.rows[2]).action == UiMessageForwardAction::toggle &&
        ui_message_forward_scene_target(s, s.send).action == UiMessageForwardAction::none,
        "forward scene: a destination lost before commit can be removed without entering the retry workflow");
    c.selected[0].state = UiMessageForwardState::sent; c.selected[2].state = UiMessageForwardState::failed;
    c.can_send = false; c.committed = true; ++c.revision;
    check(ui_message_forward_scene_update(&s, &source, &c, viewport) &&
        ui_message_forward_scene_target(s, s.targets[0]).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_target(s, s.rows[0]).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_target(s, s.targets[2]).action == UiMessageForwardAction::retry &&
        ui_message_forward_scene_target(s, s.rows[2]).action == UiMessageForwardAction::retry &&
        ui_message_forward_scene_target(s, s.send).action == UiMessageForwardAction::retry &&
        !ui_message_forward_scene_target(s, s.send).channel_id &&
        ui_message_forward_scene_target(s, s.rows[3]).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_target(s, s.targets[3]).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_target(s, s.caption).action == UiMessageForwardAction::none,
        "forward scene: partial success exposes failed retry without an activation route for confirmed destinations");
    c.sending = true; ++c.revision;
    check(ui_message_forward_scene_update(&s, &source, &c, viewport) &&
        ui_message_forward_scene_target(s, s.edit).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_target(s, s.caption).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_target(s, s.targets[2]).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_target(s, s.cancel).action == UiMessageForwardAction::cancel_pending &&
        ui_message_forward_scene_target(s, s.close).action == UiMessageForwardAction::close,
        "forward scene: sending freezes edits, destination changes and retries while retaining cancellation");
    c.sending = false; ++c.revision;
    for (auto& target : c.selected) target.state = UiMessageForwardState::sent;
    check(ui_message_forward_scene_update(&s, &source, &c, viewport) &&
        ui_message_forward_scene_target(s, s.send).action == UiMessageForwardAction::close &&
        !ui_message_forward_scene_target(s, s.rows[0]).channel_id,
        "forward scene: completed commit offers Done without re-enabling cached destination rows");
    c.selected[2].state = UiMessageForwardState::cancelled; ++c.revision;
    check(ui_message_forward_scene_update(&s, &source, &c, viewport) &&
        ui_message_forward_scene_target(s, s.targets[2]).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_target(s, s.send).action == UiMessageForwardAction::close,
        "forward scene: explicitly cancelled destinations expose no retry route");
    c.selected[2].state = UiMessageForwardState::failed; ++c.revision;
    PaintStream paint{};
    UiTextCommand commands[128]{}; UiTextCommandStream stream{}; UiTextOutput output{&stream, nullptr};
    bool visual = paint_stream_initialize(&paint) && ui_text_command_stream_initialize(&stream, {commands, 128});
    const Theme themes[]{kDefaultTheme, kLightTheme, kAmoledTheme};
    const UiRect viewports[]{viewport, {0,0,320,480}, {0,0,240,300}, {0,0,1920,1080}};
    for (u32 t = 0; t < 3; ++t) for (auto v : viewports) {
        visual &= ui_message_forward_scene_update(&s, &source, &c, v);
        paint_stream_reset(&paint, v.right, v.bottom); ui_text_command_stream_reset(&stream);
        visual &= ui_message_forward_scene_paint(s, themes[t], &paint, nullptr, nullptr, t ? 144 : 96) &&
            ui_message_forward_scene_text_output(s, themes[t], &output) && !paint.rejected && !stream.rejected &&
            paint.clip_depth == 1 && ui_rect_has_area(s.body) && ui_rect_width(s.text) >= 150 &&
            s.body.bottom < s.caption_field.top && s.caption_field.bottom < s.footer.top &&
            s.field.bottom <= s.hint.top && s.hint.bottom <= s.body.top;
        for (u32 i = 0; i < stream.count; ++i)
            visual &= commands[i].clip.left >= s.panel.left && commands[i].clip.right <= s.panel.right &&
                commands[i].clip.top >= s.panel.top && commands[i].clip.bottom <= s.panel.bottom;
    }
    check(visual, "forward scene: dark, light, AMOLED, minimum, narrow, wide and 144 DPI output stays bounded without field collisions");
    source.count = 0; ++source.revision; ++c.revision;
    bool short_receipts = ui_message_forward_scene_update(&s, &source, &c, {0,0,360,420}) &&
        s.receipts_visible && !s.count && ui_rect_height(s.body) >= 34;
    for (u32 i = 0; i < c.selected_count; ++i) {
        const auto r = s.target_rectangles[i];
        short_receipts &= ui_rect_has_area(r) && r.top >= s.body.bottom && r.bottom < s.caption_field.top &&
            r.left >= s.panel.left && r.right <= s.panel.right;
    }
    const auto failed = s.target_rectangles[2];
    short_receipts &= ui_message_forward_scene_hit_test(s, (failed.left + failed.right) * .5f,
        (failed.top + failed.bottom) * .5f).action == UiMessageForwardAction::retry;
    check(short_receipts, "forward scene: short viable windows retain all five filtered-out destination receipts and exact retry targets");
    source.count = 100; ++source.revision; ++c.revision;
    c.committed = false; c.can_send = true;
    for (auto& target : c.selected) target.state = UiMessageForwardState::ready;
    Theme bright_light = kLightTheme; bright_light.accent = {.95f,.85f,.4f,1}; bright_light = ui_theme_resolve_defaults(bright_light);
    bool contrast = ui_message_forward_scene_update(&s, &source, &c, viewport);
    paint_stream_reset(&paint, viewport.right, viewport.bottom); ui_text_command_stream_reset(&stream);
    contrast &= ui_message_forward_scene_paint(s, bright_light, &paint) &&
        ui_message_forward_scene_text_output(s, bright_light, &output);
    const auto semantic = ui_foreground_color(bright_light, bright_light.accent);
    const auto equal_color = [](Color a, Color b) noexcept {
        return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
    };
    bool hint_color = false, remove_color = false;
    for (u32 i = 0; i < stream.count; ++i)
        if (commands[i].text.data == s.selection_label) hint_color = equal_color(commands[i].color, semantic);
    const auto chip = s.target_rectangles[0];
    const UiRect remove_icon{chip.right - 22, chip.top + 6, chip.right - 6, chip.bottom - 6};
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        if (equal_color(q.fill, semantic) && ui_rect_has_area(ui_rect_intersection(
            {q.left,q.top,q.right,q.bottom}, remove_icon))) remove_color = true;
    }
    const float ratio = (ui_background_luminance(bright_light.base_low) + .05f) /
        (ui_background_luminance(semantic) + .05f);
    check(contrast && hint_color && remove_color && ratio >= 4.5f,
        "forward scene: light bright accents retain readable selection hints and receipt removal icons on neutral surfaces");
    fixture.current = false;
    paint_stream_reset(&paint, 1280, 800); ui_text_command_stream_reset(&stream);
    check(!ui_message_forward_scene_current(s) && ui_message_forward_scene_selected(s).action == UiMessageForwardAction::none &&
        ui_message_forward_scene_paint(s, kDefaultTheme, &paint) && !paint.count &&
        ui_message_forward_scene_text_output(s, kDefaultTheme, &output) && !stream.count,
        "forward scene: retired account authority suppresses borrowed content paint text and routes");
    fixture.current = true; fixture.duplicate = true; source.count = 2; ++source.revision; ++c.revision;
    check(!ui_message_forward_scene_update(&s, &source, &c, viewport) && !ui_message_forward_scene_current(s),
        "forward scene: duplicate destination rejects partial bank publication");
    fixture.duplicate = false; fixture.invalid = true; ++source.revision;
    check(!ui_message_forward_scene_update(&s, &source, &c, viewport), "forward scene: voice and non-message destinations fail acceptance");
    fixture.invalid = false; source.count = 0; c.selected_count = 0; c.phase = UiMessageForwardPhase::no_matches;
    ++source.revision; ++c.revision;
    const auto valid_edit = c.edit;
    c.edit = {utf8_literal("é"), 1, 2, true, true, true};
    check(!ui_message_forward_scene_update(&s, &source, &c, viewport),
        "forward scene: native edit offsets cannot split a UTF-8 scalar");
    c.edit = valid_edit; ++c.revision;
    check(ui_message_forward_scene_update(&s, &source, &c, viewport) && !s.count &&
        ui_message_forward_scene_selected(s).action == UiMessageForwardAction::none,
        "forward scene: no matches retire every old route");
    paint_stream_release(&paint); ui_message_forward_scene_release(&s);
    check(!s.active && !s.storage.arena.base, "forward scene: close releases the lazy allocation");
}
} // namespace maicord
