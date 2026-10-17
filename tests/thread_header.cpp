#include "maicord/ui/thread_header.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/foreground_color.hpp"

namespace maicord {
namespace {
bool color_same(Color a, Color b) noexcept {
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}
bool disjoint(UiRect a, UiRect b) noexcept {
    return !ui_rect_has_area(a) || !ui_rect_has_area(b) || !ui_rect_has_area(ui_rect_intersection(a, b));
}
}
void test_thread_header(void (*check)(bool, const char*) noexcept) noexcept {
    UiScene shell{};
    UiThreadHeader header{};
    const auto style = ui_thread_header_style();
    const UiRect strip{312, 48, 632, 128}, clip{0, 0, 632, 480};
    check(ui_scene_initialize_bootstrap(&shell) && ui_scene_layout_with_metrics(&shell, UiShellView::guild_channel, kGuildChannelShellLayoutMetrics, 632, 480) &&
        ui_thread_header_bind_shell(&header, &shell), "thread header: borrows six actual shell slots without another widget owner");
    if (!header.manager) { ui_scene_release(&shell); return; }
    const u32 shell_count = shell.widgets.count;
    const auto* message = ui_scene_resolve(shell, ui_scene_find(shell, UiSemantic::message_view));
    const UiRect original_message = message ? message->rectangle : UiRect{};
    UiThreadHeaderView view{};
    view.thread_id = 55; view.source_publication = 1; view.visible = true;
    view.archive_known = true; view.enabled_actions = ui_thread_action_bit(UiThreadAction::check_status);
    check(ui_thread_header_update(&header, view, strip, clip, style) && header.active &&
        ui_thread_header_height(view, style) == 44 && shell.widgets.count == shell_count &&
        shell.nodes[header.widgets.root.slot].visible &&
        ui_rect_equal(shell.nodes[header.widgets.root.slot].rectangle, header.geometry.rectangle) &&
        disjoint(header.geometry.status, header.geometry.primary) && disjoint(header.geometry.primary, header.geometry.overflow),
        "thread header: minimum content width projects distinct status and native controls into synchronized shell snapshots");
    check(ui_thread_header_primary_action(view) == UiThreadAction::check_status,
        "thread header: an unknown current-user membership checks its status");
    const auto unknown = ui_thread_header_widget_target(header, header.widgets.primary);
    check(unknown.action == UiThreadAction::check_status && ui_thread_header_current(header, unknown),
        "thread header: Unknown uses a current native Refresh target");
    const UiPoint point{(header.geometry.primary.left + header.geometry.primary.right) * .5f, 70};
    check(ui_thread_header_pointer(&header, UiButtonPointerEvent::press, point).handled,
        "thread header: pointer press captures the native enabled current action");
    ++view.source_publication;
    ui_thread_header_update(&header, view, strip, clip, style);
    const auto retired = ui_thread_header_pointer(&header, UiButtonPointerEvent::release, point);
    check(!retired.activate && !ui_thread_header_current(header, unknown),
        "thread header: a publication change during pointer capture cannot activate its replacement");
    const auto revision = header.revision, projections = header.projections, layout = shell.widgets.layout.revision;
    bool warm = true;
    for (u32 i = 0; i < 64; ++i) warm = warm && !ui_thread_header_update(&header, view, strip, clip, style);
    check(warm && header.revision == revision && header.projections == projections &&
        shell.widgets.layout.revision == layout && shell.widgets.count == shell_count,
        "thread header: 64 equal authoritative receipts perform no projection, layout or widget growth");
    view.membership = UiThreadMembership::not_joined;
    view.enabled_actions |= ui_thread_action_bit(UiThreadAction::join);
    ++view.source_publication;
    ui_thread_header_update(&header, view, strip, clip, style);
    check(ui_thread_header_widget_target(header, header.widgets.primary).action == UiThreadAction::join,
        "thread header: a known active nonmember gets the explicit Join action");
    view.enabled_actions &= ~ui_thread_action_bit(UiThreadAction::join);
    ++view.source_publication;
    ui_thread_header_update(&header, view, strip, clip, style);
    check(!ui_thread_header_widget_target(header, header.widgets.primary).thread_id &&
        !ui_widget_manager_input_enabled(shell.widgets, header.widgets.primary),
        "thread header: permission refusal cannot leave a keyboard-enabled Join button");
    view.membership = UiThreadMembership::joined;
    view.enabled_actions |= ui_thread_action_bit(UiThreadAction::leave);
    ++view.source_publication;
    ui_thread_header_update(&header, view, strip, clip, style);
    check(!ui_rect_has_area(header.geometry.primary) &&
        ui_thread_header_widget_target(header, header.widgets.overflow).action == UiThreadAction::menu,
        "thread header: Joined preserves readable status and keeps Leave in deliberate overflow");
    const UiThreadMenuActions actions{701, 702, 703, 704, 705, 706};
    PopupItem items[6]{};
    u32 count = ui_thread_menu_build(view, actions, {items, 6});
    check(count == 2 && items[1].action == actions.leave && (items[1].flags & popup_item_enabled),
        "thread header: Joined active thread menu exposes Leave, never Join");
    view.archived = true; view.locked_known = true; view.locked = true;
    view.enabled_actions &= ~ui_thread_action_bit(UiThreadAction::leave);
    count = ui_thread_menu_build(view, actions, {items, 6});
    check(count == 2 && items[1].action == actions.unarchive && !(items[1].flags & popup_item_enabled) &&
        utf8_equal(items[1].detail, utf8_literal("This thread is locked.")),
        "thread header: archived locked status exposes a clear disabled Unarchive reason without an active-only Leave");
    view.enabled_actions |= ui_thread_action_bit(UiThreadAction::unarchive);
    view.status = utf8_literal("Unarchive queued."); view.busy = true; view.operation = UiThreadAction::unarchive;
    view.enabled_actions |= ui_thread_action_bit(UiThreadAction::cancel);
    ++view.source_publication;
    ui_thread_header_update(&header, view, strip, clip, style);
    check(ui_thread_header_height(view, style) == 80 && !ui_thread_header_widget_target(header, header.widgets.primary).thread_id &&
        ui_thread_header_widget_target(header, header.widgets.cancel).action == UiThreadAction::cancel &&
        disjoint(header.geometry.feedback, header.geometry.cancel) && header.geometry.rectangle.bottom == 128,
        "thread header: queued mutation reserves a clipped feedback row and exact Cancel without duplicate submission");
    count = ui_thread_menu_build(view, actions, {items, 6});
    check(count == 3 && !(items[1].flags & popup_item_enabled) && items[2].action == actions.cancel &&
        (items[2].flags & popup_item_enabled), "thread header: pending menu disables mutations while retaining Cancel");
    UiTextCommand commands[5]{}; UiTextCommandStream stream{};
    check(ui_text_command_stream_initialize(&stream, {commands, 5}), "thread header: native text stream admits bounded commands");
    UiTextOutput output{&stream, nullptr};
    view.busy = false; view.status_error = true; view.status = utf8_literal("Unable to unarchive. Retry or cancel.");
    view.enabled_actions |= ui_thread_action_bit(UiThreadAction::retry);
    ++view.source_publication;
    ui_thread_header_update(&header, view, strip, clip, style);
    const Theme themes[]{kDefaultTheme, kLightTheme, kAmoledTheme};
    bool readable = true;
    for (const auto& theme : themes) {
        ui_text_command_stream_reset(&stream);
        readable = readable && ui_thread_header_text(header, theme, &output) && stream.count == 5 &&
            color_same(commands[0].color, ui_foreground_color(theme, theme.text_primary)) &&
            color_same(commands[1].color, ui_foreground_color(theme, theme.text_secondary)) &&
            color_same(commands[3].color, ui_foreground_color(theme, theme.notification)) &&
            utf8_equal(commands[2].text, utf8_literal("Retry")) && commands[3].owner.manager == &shell.widgets &&
            ui_rect_equal(commands[3].clip, header.geometry.feedback) &&
            utf8_equal(commands[1].text, utf8_literal("Archived · Locked")) &&
            commands[0].rectangle.bottom <= commands[1].rectangle.top;
    }
    check(readable, "thread header: Dark Light AMOLED use readable semantic status/error text with exact native feedback clipping");
    items[0].action = 999;
    auto collision = actions; collision.leave = collision.check_status;
    check(!ui_thread_menu_build(view, actions, {items, 1}) && items[0].action == 999 &&
        !ui_thread_menu_build(view, collision, {items, 6}) && items[0].action == 999,
        "thread header: capacity or colliding menu actions cannot partially replace copied descriptors");
    const UiRect short_clip{312, 48, 632, 103};
    check(ui_thread_header_update(&header, view, strip, short_clip, style) &&
        ui_rect_has_area(ui_rect_intersection(header.geometry.cancel, short_clip)) &&
        header.geometry.clip.bottom == 103,
        "thread header: a short viewport clips the feedback and controls to the same native surface");
    view = {};
    ui_thread_header_update(&header, view, strip, clip, style);
    const auto inactive_revision = header.revision, inactive_projections = header.projections;
    warm = true;
    for (u32 i = 0; i < 64; ++i) warm = warm && !ui_thread_header_update(&header, view, strip, clip, style);
    check(!header.active && warm && header.revision == inactive_revision && header.projections == inactive_projections &&
        !shell.nodes[header.widgets.root.slot].visible && !ui_thread_header_current(header, unknown) &&
        ui_thread_header_height(view, style) == 0 && message && ui_rect_equal(message->rectangle, original_message),
        "thread header: scope retirement clears status and all controls, ordinary message geometry and inactive warm frames remain unchanged");
    ui_scene_release(&shell);
}
} // namespace maicord
