#include "maicord/ui/thread_header.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/foreground_color.hpp"

namespace maicord {
namespace {
bool enabled(const UiThreadHeaderView& v, UiThreadAction action) noexcept {
    return (v.enabled_actions & ui_thread_action_bit(action)) != 0;
}
bool style_valid(const UiThreadHeaderStyle& s) noexcept {
    const float values[]{s.height, s.feedback_height, s.inset, s.gap, s.control_height,
        s.primary_width, s.overflow_width, s.cancel_width};
    for (float value : values) if (!ui_coordinate_valid(value) || value < 0 || value > 256) return false;
    return s.height >= s.control_height && s.feedback_height >= s.control_height &&
        s.control_height >= 24 && s.primary_width >= 64 && s.overflow_width >= 24 && s.cancel_width >= 48;
}
bool same_view(const UiThreadHeaderView& a, const UiThreadHeaderView& b) noexcept {
    return a.thread_id == b.thread_id && a.source_publication == b.source_publication &&
        a.enabled_actions == b.enabled_actions && a.membership == b.membership && a.operation == b.operation &&
        a.visible == b.visible && a.archive_known == b.archive_known && a.archived == b.archived &&
        a.locked_known == b.locked_known && a.locked == b.locked && a.busy == b.busy &&
        a.status_error == b.status_error && utf8_equal(a.status, b.status);
}
bool view_valid(const UiThreadHeaderView& v) noexcept {
    constexpr u32 mask = (1u << 6) - 1u;
    return v.membership <= UiThreadMembership::not_joined && v.operation < UiThreadAction::menu &&
        !(v.enabled_actions & ~mask) && (!v.visible || (v.thread_id && v.source_publication)) &&
        (!v.status.bytes || v.status.data) && v.status.bytes <= 256 && utf8_validate(v.status).valid;
}
UiWidgetHandle handle(const UiThreadHeaderWidgets& w, u32 i) noexcept {
    switch (i) {
    case 0: return w.root; case 1: return w.status; case 2: return w.primary;
    case 3: return w.overflow; case 4: return w.feedback; default: return w.cancel;
    }
}
UiRect rectangle(const UiThreadHeaderGeometry& g, u32 i) noexcept {
    switch (i) {
    case 0: return g.rectangle; case 1: return g.status; case 2: return g.primary;
    case 3: return g.overflow; case 4: return g.feedback; default: return g.cancel;
    }
}
bool geometry_current(const UiThreadHeader& h, const UiThreadHeaderGeometry& g) noexcept {
    if (!h.manager) return false;
    for (u32 i = 0; i < 6; ++i) {
        const auto* n = ui_widget_manager_layout_node(*h.manager, handle(h.widgets, i));
        if (!n || !ui_rect_equal(n->rectangle, rectangle(g, i)) ||
            n->visible != ui_rect_intersects(rectangle(g, i), g.clip)) return false;
    }
    return true;
}
bool project_nodes(UiThreadHeader* h) noexcept {
    auto& m = *h->manager;
    const auto primary = ui_thread_header_primary_action(h->view);
    for (u32 i = 0; i < 6; ++i) {
        const auto id = handle(h->widgets, i);
        auto* w = ui_widget_manager_resolve(&m, id);
        auto* n = w ? ui_layout_engine_resolve(&m.layout, w->layout) : nullptr;
        if (!n) return false;
        const auto r = rectangle(h->geometry, i);
        const bool visible = ui_rect_intersects(r, h->geometry.clip);
        const bool interactive = visible && (i == 3 ||
            (i == 2 && !h->view.busy && enabled(h->view, primary)) ||
            (i == 5 && enabled(h->view, UiThreadAction::cancel)));
        const bool button = i == 2 || i == 3 || i == 5;
        w->flags = visible && (!button || interactive) ? static_cast<u16>(ui_widget_enabled |
            (interactive ? ui_widget_hit_test | ui_widget_focusable : 0)) : ui_widget_none;
        n->spec.style.width = ui_layout_fixed(ui_rect_width(r));
        n->spec.style.height = ui_layout_fixed(ui_rect_height(r));
        n->spec.style.horizontal_alignment = UiLayoutAlignment::start;
        n->spec.style.vertical_alignment = UiLayoutAlignment::start;
        const auto parent = n->parent < m.layout.count ? m.layout.storage[n->parent].rectangle : UiRect{};
        n->spec.style.margin = visible ? UiInsets{r.left - parent.left, r.top - parent.top, 0, 0} : UiInsets{};
        n->spec.style.flags = static_cast<u16>(ui_layout_node_allow_zero_size | ui_layout_node_allow_overlap |
            (visible ? ui_layout_node_visible | ui_layout_node_clip_children : 0));
        n->rectangle = r;
        n->content_rectangle = r;
        n->clip_rectangle = visible ? ui_rect_intersection(r, h->geometry.clip) : UiRect{};
        n->measured_size = {ui_rect_width(r), ui_rect_height(r)};
        n->visible = visible;
        if (h->shell && id.slot < h->shell->count) {
            auto& snapshot = h->shell->nodes[id.slot];
            snapshot.rectangle = r;
            snapshot.visible = visible;
        }
    }
    ui_widget_manager_reconcile_input(&m);
    return true;
}
Utf8View pending_label(UiThreadAction action) noexcept {
    switch (action) {
    case UiThreadAction::check_status: return utf8_literal("Checking…");
    case UiThreadAction::join: return utf8_literal("Joining…");
    case UiThreadAction::leave: return utf8_literal("Leaving…");
    case UiThreadAction::unarchive: return utf8_literal("Unarchiving…");
    default: return utf8_literal("Waiting…");
    }
}
bool text(const UiThreadHeader& h, UiWidgetHandle owner, Utf8View label, UiRect r, Color color,
    UiTextOutput* out, bool center = false, u32 style_flags = text_style_none) noexcept {
    if (!label.bytes || !ui_rect_has_area(r)) return true;
    UiTextCommand command{label, {h.manager, owner}, r, ui_rect_intersection(r, h.geometry.clip), color,
        style_flags, center ? UiTextAlignment::center : UiTextAlignment::start,
        UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis, UiTextFace::normal, {}};
    return ui_text_output_push(out, command);
}
Color button_fill(const UiThreadHeader& h, UiWidgetHandle w, const Theme& theme, bool accent) noexcept {
    return ui_button_background(*h.manager, w, theme, accent ? theme.tokens.emphasis.fill : theme.surface,
        UiButtonSurface::opaque);
}
bool paint_button(const UiThreadHeader& h, UiWidgetHandle w, UiRect r, const Theme& theme,
    PaintStream* out, bool accent) noexcept {
    if (!ui_rect_has_area(r)) return true;
    const bool focus = ui_widget_handle_equal(h.manager->focused, w) && ui_widget_manager_input_enabled(*h.manager, w);
    const auto fill = button_fill(h, w, theme, accent);
    return ui_background_paint_surface(out, r, theme, fill, focus ? theme.tokens.border.focus : theme.surface_high,
        kUiControlCornerRadius, focus ? 1.5f : .75f);
}
bool accent_action(UiThreadAction action) noexcept {
    return action == UiThreadAction::join || action == UiThreadAction::unarchive || action == UiThreadAction::retry;
}
} // namespace

float ui_thread_header_height(const UiThreadHeaderView& view, const UiThreadHeaderStyle& style) noexcept {
    return view.visible && view.thread_id && style_valid(style)
        ? style.height + (view.status.bytes ? style.feedback_height : 0) : 0;
}
Utf8View ui_thread_header_status(const UiThreadHeaderView& v) noexcept {
    // One line; archive state leads so a narrow header never ellipsizes it away.
    static constexpr Utf8View lines[4][3]{
        {{}, utf8_literal("Joined"), utf8_literal("Not joined")},
        {utf8_literal("Active thread"), utf8_literal("Joined · Active thread"),
         utf8_literal("Not joined · Active thread")},
        {utf8_literal("Archived"), utf8_literal("Archived · Joined"), utf8_literal("Archived · Not joined")},
        {utf8_literal("Archived · Locked"), utf8_literal("Archived · Locked · Joined"),
         utf8_literal("Archived · Locked · Not joined")}};
    const u32 archive = !v.archive_known ? 0 : !v.archived ? 1 : v.locked_known && v.locked ? 3 : 2;
    const u32 membership = v.membership == UiThreadMembership::joined       ? 1
                           : v.membership == UiThreadMembership::not_joined ? 2
                                                                            : 0;
    return lines[archive][membership];
}
UiThreadAction ui_thread_header_primary_action(const UiThreadHeaderView& v) noexcept {
    if (!v.visible) return UiThreadAction::none;
    if (v.busy) return v.operation;
    if (enabled(v, UiThreadAction::retry)) return UiThreadAction::retry;
    if (v.archive_known && v.archived) return enabled(v, UiThreadAction::unarchive)
        ? UiThreadAction::unarchive : UiThreadAction::none;
    if (v.membership == UiThreadMembership::unknown) return UiThreadAction::check_status;
    if (v.archive_known && !v.archived && v.membership == UiThreadMembership::not_joined)
        return UiThreadAction::join;
    return UiThreadAction::none;
}
Utf8View ui_thread_action_label(UiThreadAction action) noexcept {
    switch (action) {
    case UiThreadAction::check_status: return utf8_literal("Refresh");
    case UiThreadAction::join: return utf8_literal("Join thread");
    case UiThreadAction::leave: return utf8_literal("Leave thread");
    case UiThreadAction::unarchive: return utf8_literal("Unarchive");
    case UiThreadAction::retry: return utf8_literal("Retry");
    case UiThreadAction::cancel: return utf8_literal("Cancel");
    case UiThreadAction::menu: return utf8_literal("Thread actions");
    default: return {};
    }
}
bool ui_thread_header_project(const UiThreadHeaderView& v, const UiThreadHeaderStyle& s, UiRect r,
    UiRect clip, UiThreadHeaderGeometry* out) noexcept {
    if (!out || !view_valid(v) || !style_valid(s) || !ui_rect_valid_or_empty(r) ||
        !ui_rect_valid_or_empty(clip)) return false;
    UiThreadHeaderGeometry g{};
    if (!v.visible) { *out = g; return true; }
    const float height = ui_thread_header_height(v, s);
    if (!ui_rect_has_area(r) || ui_rect_width(r) < 2 * s.inset + s.primary_width + s.overflow_width +
        2 * s.gap + 64 || ui_rect_height(r) < height) return false;
    g.rectangle = {r.left, r.top, r.right, r.top + height};
    g.clip = ui_rect_intersection(g.rectangle, clip);
    const float y = r.top + (s.height - s.control_height) * .5f;
    g.overflow = {r.right - s.inset - s.overflow_width, y, r.right - s.inset, y + s.control_height};
    float status_right = g.overflow.left - s.gap;
    if (ui_thread_header_primary_action(v) != UiThreadAction::none) {
        g.primary = {status_right - s.primary_width, y, status_right, y + s.control_height};
        status_right = g.primary.left - s.gap;
    }
    g.status = {r.left + s.inset, r.top, status_right, r.top + s.height};
    if (v.status.bytes) {
        const float top = r.top + s.height;
        float right = r.right - s.inset;
        if (enabled(v, UiThreadAction::cancel)) {
            const float cy = top + (s.feedback_height - s.control_height) * .5f;
            g.cancel = {right - s.cancel_width, cy, right, cy + s.control_height};
            right = g.cancel.left - s.gap;
        }
        g.feedback = {r.left + s.inset, top, right, top + s.feedback_height};
    }
    *out = g;
    return true;
}
bool ui_thread_header_bind(UiThreadHeader* h, UiWidgetManager* manager, UiThreadHeaderWidgets widgets) noexcept {
    if (!h || !manager) return false;
    for (u32 i = 0; i < 6; ++i) {
        const auto* w = ui_widget_manager_resolve(*manager, handle(widgets, i));
        if (!w || !ui_widget_manager_layout_node(*manager, handle(widgets, i))) return false;
        if ((i == 2 || i == 3 || i == 5) && w->kind != UiWidgetKind::button) return false;
        for (u32 j = 0; j < i; ++j) if (ui_widget_handle_equal(handle(widgets, i), handle(widgets, j))) return false;
    }
    h->manager = manager;
    h->shell = nullptr;
    h->widgets = widgets;
    h->active = false;
    h->published = false;
    h->captured_publication = 0;
    return true;
}
bool ui_thread_header_bind_shell(UiThreadHeader* h, UiScene* shell) noexcept {
    if (!shell) return false;
    UiThreadHeaderWidgets w{
        shell->semantics[static_cast<u32>(UiSemantic::thread_header)],
        shell->semantics[static_cast<u32>(UiSemantic::thread_status)],
        shell->semantics[static_cast<u32>(UiSemantic::thread_primary)],
        shell->semantics[static_cast<u32>(UiSemantic::thread_overflow)],
        shell->semantics[static_cast<u32>(UiSemantic::thread_feedback)],
        shell->semantics[static_cast<u32>(UiSemantic::thread_cancel)]};
    if (!ui_thread_header_bind(h, &shell->widgets, w)) return false;
    h->shell = shell;
    return true;
}
bool ui_thread_header_update(UiThreadHeader* h, const UiThreadHeaderView& v, UiRect r, UiRect clip,
    const UiThreadHeaderStyle& style) noexcept {
    if (!h || !h->manager) return false;
    UiThreadHeaderGeometry g{};
    if (!ui_thread_header_project(v, style, r, clip, &g)) return false;
    const bool changed = !h->published || !same_view(h->view, v) || !bytes_equal(&h->style, &style, sizeof(style)) ||
        !ui_rect_equal(h->geometry.rectangle, g.rectangle) || !ui_rect_equal(h->geometry.clip, g.clip);
    const bool project = changed || !geometry_current(*h, g);
    if (!project) return false;
    if (h->view.thread_id != v.thread_id || h->view.source_publication != v.source_publication) {
        if (h->captured_publication || ui_thread_header_widget_target(*h, h->manager->captured).thread_id)
            ui_widget_manager_capture(h->manager, ui_widget_invalid_handle());
        h->captured_publication = 0;
    }
    h->view = v;
    h->style = style;
    h->geometry = g;
    if (!project_nodes(h)) return false;
    h->active = v.visible;
    h->published = true;
    if (changed) saturating_increment(h->revision);
    saturating_increment(h->projections);
    return true;
}
UiThreadHeaderTarget ui_thread_header_widget_target(const UiThreadHeader& h, UiWidgetHandle widget) noexcept {
    UiThreadHeaderTarget target{};
    if (!h.active || !h.manager || !h.view.visible || !h.view.thread_id || !h.view.source_publication ||
        !ui_widget_manager_input_enabled(*h.manager, widget)) return target;
    UiThreadAction action = UiThreadAction::none;
    if (ui_widget_handle_equal(widget, h.widgets.primary) && !h.view.busy) action = ui_thread_header_primary_action(h.view);
    else if (ui_widget_handle_equal(widget, h.widgets.overflow)) action = UiThreadAction::menu;
    else if (ui_widget_handle_equal(widget, h.widgets.cancel)) action = UiThreadAction::cancel;
    if (action == UiThreadAction::none || (action != UiThreadAction::menu && !enabled(h.view, action))) return target;
    const auto* n = ui_widget_manager_layout_node(*h.manager, widget);
    if (!n || !n->visible || !ui_rect_has_area(n->clip_rectangle)) return target;
    return {h.view.thread_id, h.view.source_publication, widget, action};
}
bool ui_thread_header_current(const UiThreadHeader& h, const UiThreadHeaderTarget& target) noexcept {
    const auto current = ui_thread_header_widget_target(h, target.widget);
    return target.thread_id && current.thread_id == target.thread_id &&
        current.source_publication == target.source_publication && current.action == target.action;
}
UiThreadHeaderPointerResult ui_thread_header_pointer(UiThreadHeader* h, UiButtonPointerEvent event,
    UiPoint point) noexcept {
    UiThreadHeaderPointerResult result{};
    if (!h || !h->manager) return result;
    const auto captured = ui_thread_header_widget_target(*h, h->manager->captured);
    const auto hit = ui_widget_manager_hit_test(*h->manager, point.x, point.y);
    const auto target = ui_thread_header_widget_target(*h, hit);
    if (event == UiButtonPointerEvent::cancel) {
        if (!captured.thread_id && !h->captured_publication) return result;
    } else if (!target.thread_id && !captured.thread_id && !h->captured_publication) return result;
    const u64 pressed_publication = h->captured_publication;
    const auto button = ui_button_pointer_target(h->manager, event,
        target.thread_id ? hit : ui_widget_invalid_handle());
    result.handled = button.handled || captured.thread_id || pressed_publication;
    result.changed = button.changed;
    if (event == UiButtonPointerEvent::press && button.handled)
        h->captured_publication = target.source_publication;
    if (event == UiButtonPointerEvent::release || event == UiButtonPointerEvent::cancel) {
        h->captured_publication = 0;
        result.activate = button.activate && target.thread_id &&
            pressed_publication == target.source_publication && ui_thread_header_current(*h, target);
        if (result.activate) result.target = target;
    }
    return result;
}
bool ui_thread_header_paint(const UiThreadHeader& h, const Theme& theme, PaintStream* out) noexcept {
    if (!out || !h.manager) return false;
    if (!h.active || !ui_rect_has_area(h.geometry.clip)) return true;
    const auto& g = h.geometry;
    if (!paint_stream_push_clip(out, g.clip)) return false;
    bool ok = ui_background_paint_canvas(out, g.rectangle, theme, theme.base_lower, UiBackgroundTier::lower);
    const auto primary = ui_thread_header_primary_action(h.view);
    ok = ok && paint_button(h, h.widgets.primary, g.primary, theme, out, accent_action(primary)) &&
        paint_button(h, h.widgets.cancel, g.cancel, theme, out, false);
    // The overflow menu is an icon button like the channel header's toggles.
    if (ui_rect_has_area(g.overflow))
        ok = ok && ui_button_icon_paint(out, *h.manager, h.widgets.overflow, UiIcon::ellipsis_vertical, theme,
            false, true, UiButtonSurface::palette);
    if (h.view.status.bytes) ok = ok && paint_stream_push(out, g.rectangle.left,
        g.rectangle.top + h.style.height, g.rectangle.right,
        g.rectangle.top + h.style.height + 1.f, theme.surface_high);
    ok = ok && paint_stream_push(out, g.rectangle.left, g.rectangle.bottom - 1.f,
        g.rectangle.right, g.rectangle.bottom, theme.surface_high);
    return paint_stream_pop_clip(out) && ok;
}
bool ui_thread_header_text(const UiThreadHeader& h, const Theme& theme, UiTextOutput* out) noexcept {
    if (!ui_text_output_valid(out) || !h.manager) return false;
    if (!h.active || !ui_rect_has_area(h.geometry.clip)) return true;
    const auto primary = ui_thread_header_primary_action(h.view);
    const auto primary_text = h.view.busy ? pending_label(h.view.operation) : ui_thread_action_label(primary);
    const auto color = ui_surface_foreground(theme, button_fill(h, h.widgets.primary, theme, accent_action(primary)));
    UiRect primary_rect = h.geometry.primary;
    if (ui_rect_has_area(primary_rect)) { primary_rect.left += 6.f; primary_rect.right -= 6.f; }
    return text(h, h.widgets.status, ui_thread_header_status(h.view), h.geometry.status,
               ui_foreground_color(theme, theme.text_secondary), out, false, text_style_subtext) &&
        text(h, h.widgets.primary, primary_text, primary_rect, color, out, true, text_style_subtext) &&
        text(h, h.widgets.feedback, h.view.status, h.geometry.feedback,
             ui_foreground_color(theme, h.view.status_error ? theme.notification : theme.text_secondary), out,
             false, text_style_subtext) &&
        text(h, h.widgets.cancel, ui_thread_action_label(UiThreadAction::cancel), h.geometry.cancel,
             ui_foreground_color(theme, theme.text_primary), out, true);
}
u32 ui_thread_menu_build(const UiThreadHeaderView& v, UiThreadMenuActions actions,
    Slice<PopupItem> out) noexcept {
    if (!view_valid(v) || !v.visible || !out.data) return 0;
    const u32 ids[]{actions.check_status, actions.join, actions.leave, actions.unarchive, actions.retry, actions.cancel};
    for (u32 i = 0; i < 6; ++i) for (u32 j = 0; ids[i] && j < i; ++j) if (ids[i] == ids[j]) return 0;
    UiThreadAction choices[6]{};
    u32 count = 0;
    if (actions.check_status) choices[count++] = UiThreadAction::check_status;
    if (v.archive_known && v.archived && actions.unarchive) choices[count++] = UiThreadAction::unarchive;
    else if (v.archive_known && !v.archived) {
        if (v.membership == UiThreadMembership::joined && actions.leave) choices[count++] = UiThreadAction::leave;
        if (v.membership == UiThreadMembership::not_joined && actions.join) choices[count++] = UiThreadAction::join;
    }
    if (enabled(v, UiThreadAction::retry) && actions.retry) choices[count++] = UiThreadAction::retry;
    if (enabled(v, UiThreadAction::cancel) && actions.cancel) choices[count++] = UiThreadAction::cancel;
    if (!count || out.count < count) return 0;
    for (u32 i = 0; i < count; ++i) {
        const auto action = choices[i];
        const u32 index = static_cast<u32>(action) - 1;
        const bool available = enabled(v, action) && (!v.busy || action == UiThreadAction::cancel);
        auto& item = out[i];
        item = {ui_thread_action_label(action), ids[index], static_cast<u16>(available ? popup_item_enabled : 0), 0};
        UiIcon icon = UiIcon::members;
        if (action == UiThreadAction::check_status) icon = UiIcon::recent;
        else if (action == UiThreadAction::unarchive) icon = UiIcon::undo;
        else if (action == UiThreadAction::retry) icon = UiIcon::retry;
        else if (action == UiThreadAction::leave) icon = UiIcon::log_out;
        else if (action == UiThreadAction::cancel) icon = UiIcon::close;
        if (!available) {
            if (action == UiThreadAction::unarchive) item.detail = v.locked_known && v.locked
                ? utf8_literal("This thread is locked.") : Utf8View{};
            else if (action == UiThreadAction::join) item.detail = utf8_literal("You can't join this thread.");
            else if (v.busy) item.detail = utf8_literal("A thread action is pending.");
            else reset_value<Utf8View>(&item.detail);
        }
        item.presentation = ui_menu_row_presentation(icon, item.detail.bytes != 0);
    }
    return count;
}
} // namespace maicord
