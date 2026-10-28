#include "maicord/ui/inbox_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/rect_edges.hpp"
#include "maicord/ui/scene_text.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
namespace {
constexpr float header_height = 52.f, filter_height = 40.f, footer_height = 48.f, inset = 12.f, gap = 8.f;
// Mention cards: avatar in the left gutter beside the author and message text.
constexpr float mention_text_left = 12.f + kUiInboxAvatarExtent + 10.f;
constexpr float mention_body_top = 48.f, mention_body_line = 22.f, mention_bottom = 10.f;
constexpr u32 mention_body_lines_maximum = 3;
Utf8View mention_body(const UiInboxRow& row) noexcept {
    return row.body.bytes ? row.body : utf8_literal("Message has no text");
}
// Cards fit their message: one to three estimated lines (the estimate errs
// wide so a wrapped line is never clipped). panel_width is the inbox panel.
float row_height(const UiInboxRow& row, float panel_width) noexcept {
    if (row.kind != UiInboxRowKind::mention)
        return row.kind == UiInboxRowKind::group ? kUiInboxGroupHeight : kUiInboxChannelHeight;
    const float text_width = panel_width - inset * 2 - 4 - mention_text_left - 12;
    const u32 lines = minimum(mention_body_lines_maximum,
                              maximum(1u, ui_settings_text_lines(mention_body(row), 15.f * 1.08f, text_width)));
    return gap + mention_body_top + lines * mention_body_line + mention_bottom;
}
bool row_valid(const UiInboxRow& row) noexcept {
    return row.kind <= UiInboxRowKind::channel && row.id && utf8_validate(row.title).valid &&
           utf8_validate(row.detail).valid && utf8_validate(row.body).valid && utf8_validate(row.timestamp).valid;
}
UiRect mention_avatar(UiRect card) noexcept {
    return {card.left + 12, card.top + 30, card.left + 12 + kUiInboxAvatarExtent,
            card.top + 30 + kUiInboxAvatarExtent};
}
bool current(const UiInboxScene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision && s.content->tab == s.tab &&
           (!s.source || (s.source->revision == s.source_revision && s.source->count == s.measured_count &&
                          (!s.source->current || s.source->current(s.source->context))));
}
UiRect bounds(const UiInboxScene& s, UiWidgetHandle handle) noexcept {
    const auto* node = ui_widget_manager_layout_node(s.widgets, handle);
    return node ? node->rectangle : UiRect{};
}
bool focused(const UiInboxScene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_handle_matches(s.widgets.focused, handle);
}
bool hovered(const UiInboxScene& s, UiWidgetHandle handle) noexcept {
    return ui_widget_handle_matches(s.widgets.hovered, handle);
}
UiWidgetHandle add(UiInboxScene& s, const UiRect& r, const UiInboxTarget& target, const Utf8View& name, bool enabled,
                   UiWidgetHandle parent) noexcept {
    if (!ui_rect_has_area(r))
        return {};
    const auto origin = s.body_owner.generation && parent.slot == s.body_owner.slot ? s.body : s.panel;
    const auto result = ui_widget_manager_add_placed(
        &s.widgets, parent, target.action == UiInboxAction::none ? UiWidgetKind::container : UiWidgetKind::button,
        static_cast<u16>(enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : 0), name, r,
        origin);
    if (!result.ok())
        return {};
    s.targets[result.handle.slot] = target;
    return result.handle;
}
// Server headers carry a quiet "Mark all read" text button; channel rows a
// check icon button that only shows while the row is hovered or focused
// (it stays in the focus order, so the keyboard reveals it too).
constexpr float group_action_width = 104.f, channel_action_extent = 28.f, badge_width = 22.f;
UiRect action_rectangle(const UiInboxRow& row, UiRect r) noexcept {
    const float middle = (r.top + r.bottom) * .5f;
    if (row.kind == UiInboxRowKind::group)
        return {r.right - group_action_width - 4, middle - 12, r.right - 4, middle + 12};
    const float half = channel_action_extent * .5f;
    return {r.right - channel_action_extent - 4, middle - half, r.right - 4, middle + half};
}
bool rebuild(UiInboxScene& s) noexcept {
    ui_widget_manager_reset(&s.widgets);
    clear_bytes(s.targets, sizeof(s.targets));
    clear_bytes(s.rows, sizeof(s.rows));
    clear_bytes(s.actions, sizeof(s.actions));
    s.body_owner = s.more = {};
    clear_bytes(s.chips, sizeof(s.chips));
    s.first = s.count = 0;
    auto style = ui_layout_fill_style(ui_layout_node_visible | ui_layout_node_allow_overlap |
                                      ui_layout_node_clip_children);
    const auto root = ui_widget_manager_add_prevalidated(&s.widgets, ui_widget_invalid_handle(),
                                                         {1, UiWidgetKind::container, ui_widget_none,
                                                          {utf8_literal("Inbox"), style}});
    if (!root.ok())
        return false;
    s.root = root.handle;
    const auto& c = *s.content;
    const auto p = s.panel;
    s.tabs[0] = add(s, ui_rect_edges(p, ui_start(92), ui_start(12), ui_start(184), ui_start(40)), {UiInboxAction::tab_mentions},
                    utf8_literal("Mentions"), true, s.root);
    s.tabs[1] = add(s, ui_rect_edges(p, ui_start(188), ui_start(12), ui_start(272), ui_start(40)), {UiInboxAction::tab_unreads},
                    utf8_literal("Unreads"), true, s.root);
    s.close = add(s, ui_rect_edges(p, ui_end(-44), ui_start(10), ui_end(-12), ui_start(42)), {UiInboxAction::close},
                  utf8_literal("Close inbox"), true, s.root);
    if (!s.tabs[0].generation || !s.tabs[1].generation || !s.close.generation)
        return false;
    if (s.tab == UiInboxTab::mentions) {
        float left = s.filters.left + inset;
        const float top = s.filters.top + 6, bottom = s.filters.bottom - 6;
        const float widths[]{c.this_server_available ? 104.f : 0.f, 96.f, 72.f};
        const UiInboxAction actions[]{UiInboxAction::filter_server, UiInboxAction::filter_everyone,
                                      UiInboxAction::filter_roles};
        static constexpr Utf8View names[]{utf8_literal("Server filter"), utf8_literal("Include @everyone mentions"),
                                   utf8_literal("Include @role mentions")};
        for (u32 i = 0; i < 3; ++i) {
            if (!widths[i])
                continue;
            s.chips[i] = add(s, {left, top, left + widths[i], bottom}, {actions[i]}, names[i], !c.loading_more, s.root);
            left += widths[i] + 8;
        }
    }
    s.body_owner = add(s, s.body, {}, utf8_literal("Inbox items"), false, s.root);
    if (!s.body_owner.generation)
        return false;
    while (s.first < s.measured_count && s.offsets[s.first + 1] <= s.scroll_dips)
        ++s.first;
    for (u32 i = s.first; i < s.measured_count && s.count < kUiInboxVisibleRows; ++i) {
        const float top = s.body.top + s.offsets[i] - s.scroll_dips;
        if (top >= s.body.bottom)
            break;
        UiInboxRow row{};
        if (!s.source->resolve(s.source->context, i, &row) || !row_valid(row))
            return false;
        const float height = row_height(row, ui_rect_width(s.panel));
        const bool card = row.kind == UiInboxRowKind::mention;
        const UiRect r{s.body.left + inset, top + (card ? gap : 0), s.body.right - inset - 4, top + height};
        const u32 slot = s.count++;
        s.visible[slot] = row;
        s.row_rectangles[slot] = r;
        const auto clipped = ui_rect_intersection(r, s.body);
        if (row.kind == UiInboxRowKind::mention)
            s.rows[slot] = add(s, clipped, {UiInboxAction::jump, row.id, row.channel_id},
                               utf8_literal("Jump to message"), !c.loading_more, s.body_owner);
        else if (row.kind == UiInboxRowKind::channel)
            s.rows[slot] = add(s, clipped, {UiInboxAction::open_channel, row.channel_id, row.channel_id},
                               utf8_literal("Open channel"), true, s.body_owner);
        if (row.kind != UiInboxRowKind::mention) {
            const auto action = ui_rect_intersection(action_rectangle(row, r), s.body);
            if (ui_rect_has_area(action))
                s.actions[slot] = add(s, action,
                                      row.kind == UiInboxRowKind::group
                                          ? UiInboxTarget{UiInboxAction::mark_guild, row.guild_id, 0}
                                          : UiInboxTarget{UiInboxAction::mark_channel, row.channel_id, row.channel_id},
                                      row.kind == UiInboxRowKind::group ? utf8_literal("Mark all as read")
                                                                        : utf8_literal("Mark as read"),
                                      true, s.body_owner);
        }
    }
    if (ui_rect_has_area(s.footer)) {
        const float middle = (s.footer.left + s.footer.right) * .5f;
        const bool retry = c.phase == UiInboxPhase::failed;
        s.more = add(s, {middle - 64, s.footer.top + 8, middle + 64, s.footer.bottom - 8},
                     {retry ? UiInboxAction::retry : UiInboxAction::load_more},
                     retry ? utf8_literal("Try again") : utf8_literal("Load more"), !c.loading_more, s.root);
    }
    if (!ui_widget_manager_layout_rectangle(&s.widgets, s.panel).ok())
        return false;
    for (u32 i = 1; i < s.widgets.count; ++i)
        if (ui_inbox_target_equal(s.targets[i], s.focused))
            ui_widget_manager_focus(&s.widgets, {static_cast<u16>(i), s.widgets.generation});
    ++s.revision;
    return true;
}
bool text(UiTextOutput* out, const UiInboxScene& s, UiWidgetHandle owner, const Utf8View& value, const UiRect& r, const Color& color,
          u32 flags = 0, u32 mode = 0) noexcept {
    return ui_scene_text(out, s.widgets, owner, value, r, (mode & ui_text_in_body) ? s.body : s.panel, color, flags, mode);
}
bool surface(PaintStream* out, const UiRect& r, const Theme& theme, const Color& fill, const Color& border, float radius = 8) noexcept {
    return ui_background_paint_surface(out, r, theme, fill, border, radius, 1, .9f, UiBackgroundSurfaceMode::gradient,
                                       UiBackgroundTier::high);
}
bool pill(PaintStream* out, const UiInboxScene& s, UiWidgetHandle handle, const Theme& theme, bool selected) noexcept {
    if (!handle.generation)
        return true;
    const auto r = bounds(s, handle);
    const bool hot = hovered(s, handle) || focused(s, handle);
    if (!selected && !hot)
        return true;
    return surface(out, r, theme, selected ? theme.surface_high : theme.surface,
                   focused(s, handle) ? theme.tokens.border.focus : theme.surface_high, ui_rect_height(r) * .5f);
}
// A channel row is "hot" while it or its mark-read button is hovered or focused.
bool row_hot(const UiInboxScene& s, u32 slot) noexcept {
    return hovered(s, s.rows[slot]) || focused(s, s.rows[slot]) || hovered(s, s.actions[slot]) ||
           focused(s, s.actions[slot]);
}
// Mention badge: at the trailing edge, stepping left of the revealed button.
UiRect badge_rectangle(const UiInboxScene& s, u32 slot) noexcept {
    const auto r = s.row_rectangles[slot];
    const float right = row_hot(s, slot) && s.actions[slot].generation
                            ? action_rectangle(s.visible[slot], r).left - 6
                            : r.right - 10;
    return {right - badge_width, r.top + 10, right, r.bottom - 10};
}
} // namespace

bool ui_inbox_scene_channel_action_visible(const UiInboxScene& s, u32 slot) noexcept {
    return current(s) && slot < s.count && s.visible[slot].kind == UiInboxRowKind::channel &&
           s.actions[slot].generation && row_hot(s, slot);
}

bool ui_inbox_scene_current(const UiInboxScene& s) noexcept {
    return current(s);
}
bool ui_inbox_scene_avatar(const UiInboxScene& s, u32 slot, UiRect* rectangle, UiRect* clip) noexcept {
    if (!rectangle || !clip || !current(s) || slot >= s.count ||
        s.visible[slot].kind != UiInboxRowKind::mention)
        return false;
    *rectangle = mention_avatar(s.row_rectangles[slot]);
    *clip = s.body;
    return ui_rect_intersects(*rectangle, *clip);
}
float ui_inbox_scene_scroll_maximum(const UiInboxScene& s) noexcept {
    return maximum(0.f, s.offsets[s.measured_count] - ui_rect_height(s.body));
}
bool ui_inbox_scene_update(UiInboxScene* s, const UiInboxSource* source, const UiInboxContent* content,
                           UiRect viewport) noexcept {
    if (!s || !content || content->tab > UiInboxTab::unreads || content->phase > UiInboxPhase::failed ||
        !utf8_validate(content->status).valid || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 240 ||
        ui_rect_height(viewport) < 200 ||
        (source && (!source->resolve || source->count > kUiInboxMaximumRows ||
                    (source->current && !source->current(source->context)))))
        return false;
    if (current(*s) && s->source == source && s->content == content && ui_rect_equal(s->viewport, viewport))
        return true;
    float offsets[kUiInboxMaximumRows + 1]{};
    const u32 count = source ? source->count : 0;
    const float width = minimum(kUiInboxPreferredWidth, ui_rect_width(viewport) - 16);
    for (u32 i = 0; i < count; ++i) {
        UiInboxRow row{};
        if (!source->resolve(source->context, i, &row) || !row_valid(row))
            return false;
        offsets[i + 1] = offsets[i] + row_height(row, width);
    }
    if (s->active && s->tab != content->tab) {
        s->scroll_dips = 0;
        s->focused = {content->tab == UiInboxTab::mentions ? UiInboxAction::tab_mentions : UiInboxAction::tab_unreads};
    }
    if (!s->storage.arena.base && (!ui_fixed_widget_storage_initialize(&s->storage, kUiInboxWidgetCapacity) ||
                                   !ui_widget_manager_initialize(&s->widgets, s->storage.widgets,
                                                                 s->storage.layout_nodes)))
        return false;
    s->viewport = viewport;
    const float height = minimum(kUiInboxPreferredHeight, ui_rect_height(viewport) - 8);
    s->panel = {viewport.right - 8 - width, viewport.top + 4, viewport.right - 8, viewport.top + 4 + height};
    s->header = {s->panel.left, s->panel.top, s->panel.right, s->panel.top + header_height};
    const bool mentions = content->tab == UiInboxTab::mentions;
    s->filters = mentions ? UiRect{s->panel.left, s->header.bottom, s->panel.right, s->header.bottom + filter_height}
                          : UiRect{};
    const bool footer = mentions && count && (content->has_more || content->loading_more ||
                                              content->phase == UiInboxPhase::failed);
    s->footer = footer ? UiRect{s->panel.left, s->panel.bottom - footer_height, s->panel.right, s->panel.bottom}
                       : UiRect{};
    s->body = {s->panel.left, mentions ? s->filters.bottom : s->header.bottom, s->panel.right,
               footer ? s->footer.top : s->panel.bottom - 4};
    if (!ui_rect_has_area(s->body))
        return false;
    s->source = source;
    s->content = content;
    s->source_revision = source ? source->revision : 0;
    s->content_revision = content->revision;
    s->measured_count = count;
    s->tab = content->tab;
    copy_bytes(s->offsets, offsets, sizeof(offsets));
    // Mention cards keep a bottom gap under the last card.
    if (mentions && count)
        s->offsets[count] += gap;
    s->scroll_dips = minimum(maximum(0.f, s->scroll_dips), ui_inbox_scene_scroll_maximum(*s));
    s->active = true;
    if (!rebuild(*s)) {
        s->active = false;
        return false;
    }
    return true;
}
void ui_inbox_scene_release(UiInboxScene* s) noexcept {
    if (!s)
        return;
    ui_fixed_widget_storage_release(&s->storage);
    reset_value(s);
}
bool ui_inbox_scene_contains(const UiInboxScene& s, float x, float y) noexcept {
    return current(s) && ui_rect_contains(s.panel, x, y);
}
UiInboxTarget ui_inbox_scene_hit_test(const UiInboxScene& s, float x, float y) noexcept {
    if (!current(s))
        return {};
    const auto handle = ui_widget_manager_hit_test(s.widgets, x, y);
    return handle.slot < kUiInboxWidgetCapacity && ui_widget_manager_input_enabled(s.widgets, handle)
               ? s.targets[handle.slot]
               : UiInboxTarget{};
}
UiInboxTarget ui_inbox_scene_focused_target(const UiInboxScene& s) noexcept {
    const auto handle = s.widgets.focused;
    return current(s) && handle.slot < kUiInboxWidgetCapacity && ui_widget_manager_input_enabled(s.widgets, handle)
               ? s.targets[handle.slot]
               : UiInboxTarget{};
}
bool ui_inbox_scene_set_hover(UiInboxScene* s, float x, float y, bool* changed) noexcept {
    if (!s || !changed || !current(*s))
        return false;
    const auto handle = ui_widget_manager_hit_test(s->widgets, x, y);
    *changed = handle.slot != s->widgets.hovered.slot || handle.generation != s->widgets.hovered.generation;
    if (!ui_widget_manager_hover(&s->widgets, handle))
        return false;
    if (*changed)
        ++s->revision;
    return true;
}
bool ui_inbox_scene_scroll(UiInboxScene* s, float offset) noexcept {
    if (!s || !current(*s) || !ui_coordinate_valid(offset))
        return false;
    const float next = minimum(maximum(0.f, offset), ui_inbox_scene_scroll_maximum(*s));
    if (next == s->scroll_dips)
        return true;
    s->scroll_dips = next;
    return rebuild(*s);
}
UiScrollbarGeometry ui_inbox_scene_scrollbar(const UiInboxScene& s) noexcept {
    return current(s) ? ui_scrollbar_geometry(s.body, s.scroll_dips, s.offsets[s.measured_count])
                      : UiScrollbarGeometry{};
}
bool ui_inbox_scene_set_scrollbar_state(UiInboxScene* s, UiScrollbarVisualState state) noexcept {
    if (!s || s->scrollbar_state == state)
        return false;
    s->scrollbar_state = state;
    ++s->revision;
    return true;
}
bool ui_inbox_scene_focus(UiInboxScene* s, UiInboxTarget target) noexcept {
    if (!s || !current(*s) || target.action == UiInboxAction::none)
        return false;
    if (target.action == UiInboxAction::jump || target.action == UiInboxAction::open_channel ||
        target.action == UiInboxAction::mark_channel || target.action == UiInboxAction::mark_guild) {
        for (u32 i = 0; i < s->measured_count; ++i) {
            UiInboxRow row{};
            if (!s->source->resolve(s->source->context, i, &row))
                return false;
            const u64 key = target.action == UiInboxAction::jump         ? row.id
                            : target.action == UiInboxAction::mark_guild ? row.guild_id
                                                                         : row.channel_id;
            const bool kind = target.action == UiInboxAction::jump         ? row.kind == UiInboxRowKind::mention
                              : target.action == UiInboxAction::mark_guild ? row.kind == UiInboxRowKind::group
                                                                           : row.kind == UiInboxRowKind::channel;
            if (!kind || key != target.id)
                continue;
            const float first = s->offsets[i], last = s->offsets[i + 1], height = ui_rect_height(s->body);
            const float offset = first < s->scroll_dips ? first
                                 : last > s->scroll_dips + height ? last - height
                                                                  : s->scroll_dips;
            s->focused = target;
            if (!ui_inbox_scene_scroll(s, offset))
                return false;
            break;
        }
    }
    for (u32 i = 1; i < s->widgets.count; ++i) {
        const UiWidgetHandle handle{static_cast<u16>(i), s->widgets.generation};
        if (ui_inbox_target_equal(s->targets[i], target) && ui_widget_manager_focus(&s->widgets, handle)) {
            s->focused = target;
            ++s->revision;
            return true;
        }
    }
    return false;
}
bool ui_inbox_scene_move_focus(UiInboxScene* s, UiInboxFocusMove move) noexcept {
    if (!s || !current(*s))
        return false;
    UiInboxTarget candidates[2 * kUiInboxMaximumRows + 8]{};
    u32 count = 0;
    candidates[count++] = {UiInboxAction::tab_mentions};
    candidates[count++] = {UiInboxAction::tab_unreads};
    if (s->tab == UiInboxTab::mentions) {
        if (s->content->this_server_available)
            candidates[count++] = {UiInboxAction::filter_server};
        candidates[count++] = {UiInboxAction::filter_everyone};
        candidates[count++] = {UiInboxAction::filter_roles};
    }
    for (u32 i = 0; i < s->measured_count; ++i) {
        UiInboxRow row{};
        if (!s->source->resolve(s->source->context, i, &row))
            return false;
        if (row.kind == UiInboxRowKind::mention)
            candidates[count++] = {UiInboxAction::jump, row.id, row.channel_id};
        else if (row.kind == UiInboxRowKind::group)
            candidates[count++] = {UiInboxAction::mark_guild, row.guild_id, 0};
        else {
            candidates[count++] = {UiInboxAction::open_channel, row.channel_id, row.channel_id};
            candidates[count++] = {UiInboxAction::mark_channel, row.channel_id, row.channel_id};
        }
    }
    if (s->more.generation)
        candidates[count++] = s->targets[s->more.slot];
    candidates[count++] = {UiInboxAction::close};
    u32 index = ~0u;
    const auto selected = ui_inbox_scene_focused_target(*s);
    for (u32 i = 0; i < count; ++i)
        if (ui_inbox_target_equal(candidates[i], selected))
            index = i;
    if (move == UiInboxFocusMove::first)
        index = 0;
    else if (move == UiInboxFocusMove::last)
        index = count - 1;
    else if (move == UiInboxFocusMove::previous)
        index = index >= count || index == 0 ? count - 1 : index - 1;
    else
        index = index >= count || index + 1 == count ? 0 : index + 1;
    return ui_inbox_scene_focus(s, candidates[index]);
}

bool ui_inbox_scene_spinner_visible(const UiInboxScene& s) noexcept {
    return current(s) && !s.measured_count && s.content->phase == UiInboxPhase::loading;
}
bool ui_inbox_scene_paint(const UiInboxScene& s, const Theme& theme, PaintStream* out) noexcept {
    if (!current(s))
        return true;
    if (!out ||
        !ui_surface_shadow_paint_isolated(out, s.panel, theme, UiElevation::popover, theme.scale.radius_md) ||
        !paint_stream_push_clip(out, s.panel))
        return false;
    const auto& c = *s.content;
    bool ok = ui_surface_paint_plate(out, s.panel, theme, UiElevation::popover, theme.scale.radius_md, nullptr,
                                     &theme.surface_high) &&
              paint_stream_push(out, ui_rect_edges(s.header, ui_start(0), ui_end(-1), ui_end(0), ui_end(0)),
                                theme.surface_high) &&
              ui_icon_paint(out, UiIcon::inbox, ui_rect_edges(s.panel, ui_start(14), ui_start(16), ui_start(34), ui_start(36)),
                            theme.text_secondary) &&
              pill(out, s, s.tabs[0], theme, s.tab == UiInboxTab::mentions) &&
              pill(out, s, s.tabs[1], theme, s.tab == UiInboxTab::unreads);
    const auto close = bounds(s, s.close);
    if (ok && (hovered(s, s.close) || focused(s, s.close)))
        ok = surface(out, close, theme, theme.surface_high, focused(s, s.close) ? theme.tokens.border.focus : theme.surface_high, 6);
    ok = ok && ui_icon_paint(out, UiIcon::close, ui_rect_shrink(close, 7),
                             theme.text_secondary);
    if (ok && s.tab == UiInboxTab::mentions) {
        const bool checked[]{c.this_server, c.everyone, c.roles};
        for (u32 i = 0; ok && i < 3; ++i) {
            if (!s.chips[i].generation)
                continue;
            const auto r = bounds(s, s.chips[i]);
            const bool hot = hovered(s, s.chips[i]);
            ok = surface(out, r, theme, checked[i] ? theme.surface_high : hot ? theme.surface : theme.base_lower,
                         focused(s, s.chips[i]) || checked[i] ? theme.tokens.emphasis.outline : theme.surface_high, ui_rect_height(r) * .5f);
        }
        ok = ok && paint_stream_push(out, ui_rect_edges(s.filters, ui_start(0), ui_end(-1), ui_end(0), ui_end(0)),
                                     theme.surface_high);
    }
    if (ok && !paint_stream_push_clip(out, s.body))
        ok = false;
    if (ok) {
        for (u32 i = 0; ok && i < s.count; ++i) {
            const auto& row = s.visible[i];
            const auto r = s.row_rectangles[i];
            if (row.kind == UiInboxRowKind::mention) {
                ok = surface(out, r, theme, hovered(s, s.rows[i]) ? theme.surface_high : theme.surface,
                             focused(s, s.rows[i]) ? theme.tokens.border.focus : theme.surface_high);
                // Placeholder under the author's avatar media.
                const auto avatar = mention_avatar(r);
                ok = ok && paint_stream_push_rounded(out, avatar, theme.avatar_placeholder, kUiInboxAvatarExtent * .5f);
            } else if (row.kind == UiInboxRowKind::channel) {
                if (hovered(s, s.rows[i]) || focused(s, s.rows[i]))
                    ok = surface(out, r, theme, theme.surface_high,
                                 focused(s, s.rows[i]) ? theme.tokens.border.focus : theme.surface_high, 4);
                ok = ok && ui_icon_paint(out, row.guild_id ? UiIcon::channel : UiIcon::direct_message,
                                         ui_rect_edges(r, ui_start(8), ui_start(9), ui_start(26), ui_start(27)),
                                         row.muted ? theme.interactive : theme.text_secondary);
                if (ok && row.mention_count) {
                    const auto badge = badge_rectangle(s, i);
                    ok = paint_stream_push_rounded(out, badge, theme.notification, ui_rect_height(badge) * .5f);
                }
                if (ok && ui_inbox_scene_channel_action_visible(s, i)) {
                    const auto a = bounds(s, s.actions[i]);
                    const bool hot = hovered(s, s.actions[i]) || focused(s, s.actions[i]);
                    if (hot)
                        ok = surface(out, a, theme, theme.surface,
                                     focused(s, s.actions[i]) ? theme.tokens.border.focus : theme.surface, theme.scale.radius_sm);
                    const float icon = ui_icon_button_extent(ui_rect_width(a) - 8), x = (a.left + a.right - icon) * .5f,
                                y = (a.top + a.bottom - icon) * .5f;
                    ok = ok && ui_icon_paint(out, UiIcon::check, {x, y, x + icon, y + icon},
                                             hot ? theme.text_primary : theme.text_secondary);
                }
            } else if (row.kind == UiInboxRowKind::group && s.actions[i].generation &&
                       (hovered(s, s.actions[i]) || focused(s, s.actions[i]))) {
                const auto a = bounds(s, s.actions[i]);
                ok = surface(out, a, theme, theme.surface,
                             focused(s, s.actions[i]) ? theme.tokens.border.focus : theme.surface, theme.scale.radius_sm);
            }
        }
        if (!s.measured_count) {
            const auto r = s.body;
            const float x = (r.left + r.right) * .5f, y = r.top + minimum(56.f, ui_rect_height(r) * .25f);
            ok = ok && (c.phase == UiInboxPhase::loading
                            ? ui_spinner_paint(out, {x - 12, y - 12, x + 12, y + 12}, theme.tokens.emphasis.outline, s.spinner_phase)
                            : ui_icon_paint(out,
                                            c.phase == UiInboxPhase::failed ? UiIcon::warning
                                            : s.tab == UiInboxTab::mentions ? UiIcon::mention
                                                                            : UiIcon::check,
                                            {x - 16, y - 16, x + 16, y + 16}, theme.text_secondary));
        }
        ok = paint_stream_pop_clip(out) && ok;
    }
    if (ok && s.more.generation)
        ok = surface(out, bounds(s, s.more), theme, hovered(s, s.more) ? theme.surface_high : theme.surface,
                     focused(s, s.more) ? theme.tokens.border.focus : theme.interactive, 6) &&
             paint_stream_push(out, ui_rect_edges(s.footer, ui_start(0), ui_start(0), ui_end(0), ui_start(1)), theme.surface_high);
    if (ok)
        ok = ui_scrollbar_paint(out, ui_inbox_scene_scrollbar(s), theme, s.scrollbar_state);
    return paint_stream_pop_clip(out) && ok;
}

bool ui_inbox_scene_text_output(const UiInboxScene& s, const Theme& theme, UiTextOutput* out) noexcept {
    if (!current(s))
        return true;
    if (!ui_text_output_valid(out))
        return false;
    const auto& c = *s.content;
    const auto p = s.panel;
    if (!text(out, s, s.root, utf8_literal("Inbox"), ui_rect_edges(p, ui_start(40), ui_start(12), ui_start(88), ui_start(40)),
              theme.text_primary, text_style_bold) ||
        !text(out, s, s.tabs[0], utf8_literal("Mentions"), bounds(s, s.tabs[0]),
              s.tab == UiInboxTab::mentions ? theme.text_primary : theme.text_secondary, text_style_bold,
              ui_text_center) ||
        !text(out, s, s.tabs[1], utf8_literal("Unreads"), bounds(s, s.tabs[1]),
              s.tab == UiInboxTab::unreads ? theme.text_primary : theme.text_secondary, text_style_bold,
              ui_text_center))
        return false;
    if (s.tab == UiInboxTab::mentions) {
        const Utf8View labels[]{c.this_server ? utf8_literal("This server") : utf8_literal("All servers"),
                                utf8_literal("@everyone"), utf8_literal("@roles")};
        const bool checked[]{c.this_server, c.everyone, c.roles};
        for (u32 i = 0; i < 3; ++i)
            if (s.chips[i].generation &&
                !text(out, s, s.chips[i], labels[i], bounds(s, s.chips[i]),
                      checked[i] ? theme.text_primary : theme.text_secondary, text_style_subtext | text_style_bold,
                      ui_text_center))
                return false;
    }
    for (u32 i = 0; i < s.count; ++i) {
        const auto& row = s.visible[i];
        const auto r = s.row_rectangles[i];
        if (row.kind == UiInboxRowKind::mention) {
            if (!text(out, s, s.rows[i], row.detail, ui_rect_edges(r, ui_start(12), ui_start(8), ui_end(-12), ui_start(26)),
                      theme.text_secondary, text_style_subtext, ui_text_in_body) ||
                !text(out, s, s.rows[i], row.title,
                      ui_rect_edges(r, ui_start(static_cast<int>(mention_text_left)), ui_start(26), ui_end(-140), ui_start(46)),
                      theme.text_primary, text_style_bold, ui_text_in_body) ||
                !text(out, s, s.rows[i], row.timestamp, ui_rect_edges(r, ui_end(-140), ui_start(26), ui_end(-12), ui_start(46)),
                      theme.text_secondary, text_style_subtext, ui_text_in_body | ui_text_end) ||
                !text(out, s, s.rows[i], mention_body(row),
                      ui_rect_edges(r, ui_start(static_cast<int>(mention_text_left)), ui_start(static_cast<int>(mention_body_top)),
                                    ui_end(-12), ui_end(-8)),
                      theme.text_primary, 0, ui_text_wrap | ui_text_in_body))
                return false;
            continue;
        }
        const auto action = action_rectangle(row, r);
        const float left = row.kind == UiInboxRowKind::channel ? r.left + 32 : r.left + 4;
        // Channel names stop before the revealed button and badge, so hovering never reflows them.
        const float right = action.left - (row.mention_count ? badge_width + 14 : 8);
        if (!text(out, s, row.kind == UiInboxRowKind::channel ? s.rows[i] : s.root, row.title,
                  {left, r.top, right, r.bottom},
                  row.kind == UiInboxRowKind::group ? theme.text_secondary
                  : row.muted                      ? theme.interactive
                                                   : theme.text_primary,
                  row.kind == UiInboxRowKind::group ? text_style_subtext | text_style_bold : text_style_bold,
                  ui_text_in_body))
            return false;
        if (row.mention_count && row.kind == UiInboxRowKind::channel) {
            char digits[4];
            u32 bytes = 0;
            const u32 value = minimum(row.mention_count, 99u);
            if (value >= 10)
                digits[bytes++] = static_cast<char>('0' + value / 10);
            digits[bytes++] = static_cast<char>('0' + value % 10);
            if (!text(out, s, s.rows[i], {digits, bytes}, badge_rectangle(s, i), theme.tokens.text.on_accent,
                      text_style_subtext | text_style_bold, ui_text_in_body | ui_text_center))
                return false;
        }
        const bool hot = s.actions[i].generation && (hovered(s, s.actions[i]) || focused(s, s.actions[i]));
        if (row.kind == UiInboxRowKind::group && s.actions[i].generation &&
            !text(out, s, s.actions[i], utf8_literal("Mark all read"), bounds(s, s.actions[i]),
                  hot ? theme.text_primary : theme.tokens.text.link, text_style_subtext | text_style_bold,
                  ui_text_in_body | ui_text_center))
            return false;
    }
    if (!s.measured_count) {
        const auto r = s.body;
        const float top = r.top + minimum(84.f, ui_rect_height(r) * .4f);
        const bool mentions = s.tab == UiInboxTab::mentions;
        const auto title = c.phase == UiInboxPhase::loading  ? utf8_literal("Loading…")
                           : c.phase == UiInboxPhase::failed ? utf8_literal("Couldn't load your inbox")
                           : mentions                        ? utf8_literal("No recent mentions")
                                                             : utf8_literal("You're all caught up");
        const auto detail = c.status.bytes ? c.status
                            : c.phase != UiInboxPhase::ready
                                ? Utf8View{}
                                : mentions ? utf8_literal("Mentions of you will show up here.")
                                           : utf8_literal("Channels with new messages will show up here.");
        if (!text(out, s, s.root, title, {r.left + 20, top, r.right - 20, top + 28}, theme.text_primary,
                  text_style_bold, ui_text_in_body | ui_text_center) ||
            !text(out, s, s.root, detail, {r.left + 24, top + 32, r.right - 24, top + 80}, theme.text_secondary, 0,
                  ui_text_wrap | ui_text_in_body | ui_text_center))
            return false;
    }
    if (s.more.generation)
        return text(out, s, s.more,
                    c.loading_more                    ? utf8_literal("Loading…")
                    : c.phase == UiInboxPhase::failed ? utf8_literal("Try again")
                                                      : utf8_literal("Load more"),
                    bounds(s, s.more), c.loading_more ? theme.text_secondary : theme.text_primary, text_style_bold,
                    ui_text_center);
    return true;
}
} // namespace maicord
