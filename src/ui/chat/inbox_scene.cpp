#include "maicord/ui/chat/inbox_scene.hpp"
#include "maicord/ui/layout/rect_edges.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
constexpr float header_height = 52.f, filter_height = 40.f, footer_height = 48.f, inset = 12.f, gap = 8.f;
// Tabs are the shared pills (kUiTab); filter chips always show, outlined while checked.
constexpr UiLook kChip{.fill = UI_TOKEN(base_lower), .hover = UI_TOKEN(surface), .selected = UI_TOKEN(surface_high),
                       .border = UI_TOKEN(surface_high), .focus = UI_TOKEN(tokens.emphasis.outline),
                       .selected_border = UI_TOKEN(tokens.emphasis.outline), .flags = ui_look_pill};
constexpr UiLook kCard{.fill = UI_TOKEN(surface), .hover = UI_TOKEN(surface_high), .border = UI_TOKEN(surface_high)};
constexpr UiLook kChannel{.fill = UI_TOKEN(surface_high), .radius = 4, .flags = ui_look_hot_only};
constexpr UiLook kRowAction{.fill = UI_TOKEN(surface), .flags = ui_look_hot_only | ui_look_radius_sm};
constexpr UiLook kMore{.fill = UI_TOKEN(surface), .hover = UI_TOKEN(surface_high), .border = UI_TOKEN(interactive),
                       .radius = 6};
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
           ui_source_current(s.source, s.source_revision, s.measured_count);
}
// Server headers carry a quiet "Mark all read" text button; channel rows a
// check icon button that only shows while the row is hovered or focused
// (it stays in the focus order, so the keyboard reveals it too).
constexpr float group_action_width = 104.f, channel_action_extent = 28.f, badge_width = 22.f;
UiRect action_rectangle(const UiInboxRow& row, const UiRect& r) noexcept {
    const float middle = (r.top + r.bottom) * .5f;
    if (row.kind == UiInboxRowKind::group)
        return {r.right - group_action_width - 4, middle - 12, r.right - 4, middle + 12};
    const float half = channel_action_extent * .5f;
    return {r.right - channel_action_extent - 4, middle - half, r.right - 4, middle + half};
}
bool rebuild(UiInboxScene& s) noexcept {
    clear_bytes(s.targets, sizeof(s.targets));
    clear_bytes(s.rows, sizeof(s.rows));
    clear_bytes(s.actions, sizeof(s.actions));
    clear_bytes(s.chips, sizeof(s.chips));
    s.body_owner = s.more = {};
    s.first = s.count = 0;
    s.root = ui_scene_root(&s.widgets, utf8_literal("Inbox"));
    if (!s.root.generation)
        return false;
    const auto& c = *s.content;
    const auto p = s.panel;
    s.tabs[0] = ui_scene_add(s, ui_rect_edges(p, ui_start(92), ui_start(12), ui_start(184), ui_start(40)),
                             {UiInboxAction::tab_mentions}, utf8_literal("Mentions"), true, s.root);
    s.tabs[1] = ui_scene_add(s, ui_rect_edges(p, ui_start(188), ui_start(12), ui_start(272), ui_start(40)),
                             {UiInboxAction::tab_unreads}, utf8_literal("Unreads"), true, s.root);
    s.close = ui_scene_add(s, ui_rect_edges(p, ui_end(-44), ui_start(10), ui_end(-12), ui_start(42)),
                           {UiInboxAction::close}, utf8_literal("Close inbox"), true, s.root);
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
            s.chips[i] =
                ui_scene_add(s, {left, top, left + widths[i], bottom}, {actions[i]}, names[i], !c.loading_more, s.root);
            left += widths[i] + 8;
        }
    }
    s.body_owner = ui_scene_add(s, s.body, {}, utf8_literal("Inbox items"), false, s.root);
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
        if (card) {
            s.rows[slot] = ui_scene_add(s, clipped, {UiInboxAction::jump, row.id, row.channel_id},
                                        utf8_literal("Jump to message"), !c.loading_more, s.body_owner);
            continue;
        }
        const bool group = row.kind == UiInboxRowKind::group;
        if (!group)
            s.rows[slot] = ui_scene_add(s, clipped, {UiInboxAction::open_channel, row.channel_id, row.channel_id},
                                        utf8_literal("Open channel"), true, s.body_owner);
        const auto action = ui_rect_intersection(action_rectangle(row, r), s.body);
        if (ui_rect_has_area(action))
            s.actions[slot] = ui_scene_add(s, action,
                                           group ? UiInboxTarget{UiInboxAction::mark_guild, row.guild_id, 0}
                                                 : UiInboxTarget{UiInboxAction::mark_channel, row.channel_id, row.channel_id},
                                           group ? utf8_literal("Mark all as read") : utf8_literal("Mark as read"), true,
                                           s.body_owner);
    }
    if (ui_rect_has_area(s.footer)) {
        const float middle = (s.footer.left + s.footer.right) * .5f;
        const bool retry = c.phase == UiInboxPhase::failed;
        s.more = ui_scene_add(s, {middle - 64, s.footer.top + 8, middle + 64, s.footer.bottom - 8},
                              {retry ? UiInboxAction::retry : UiInboxAction::load_more},
                              retry ? utf8_literal("Try again") : utf8_literal("Load more"), !c.loading_more, s.root);
    }
    return ui_scene_commit(&s.widgets, s.panel, s.targets, s.focused, ui_inbox_target_equal, &s.revision);
}
// A channel row is "hot" while it or its mark-read button is hovered or focused.
bool row_hot(const UiInboxScene& s, u32 slot) noexcept {
    return ui_widget_state(s.widgets, s.rows[slot]) | ui_widget_state(s.widgets, s.actions[slot]);
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
    if (!s->storage.arena.base && !ui_scene_open(&s->storage, &s->widgets, kUiInboxWidgetCapacity))
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
    ui_scene_release(s);
}
bool ui_inbox_scene_contains(const UiInboxScene& s, float x, float y) noexcept {
    return current(s) && ui_rect_contains(s.panel, x, y);
}
UiInboxTarget ui_inbox_scene_hit_test(const UiInboxScene& s, float x, float y) noexcept {
    return current(s) ? ui_scene_target(s, ui_widget_manager_hit_test(s.widgets, x, y)) : UiInboxTarget{};
}
UiInboxTarget ui_inbox_scene_focused_target(const UiInboxScene& s) noexcept {
    return current(s) ? ui_scene_target(s, s.widgets.focused) : UiInboxTarget{};
}
bool ui_inbox_scene_set_hover(UiInboxScene* s, float x, float y, bool* changed) noexcept {
    return s && current(*s) && ui_scene_hover(&s->widgets, &s->revision, x, y, changed);
}
bool ui_inbox_scene_scroll(UiInboxScene* s, float offset) noexcept {
    if (!s || !current(*s) || !ui_coordinate_valid(offset))
        return false;
    return !ui_scroll_offset_store(&s->scroll_dips, offset, ui_inbox_scene_scroll_maximum(*s)) || rebuild(*s);
}
UiScrollbarGeometry ui_inbox_scene_scrollbar(const UiInboxScene& s) noexcept {
    return current(s) ? ui_scrollbar_geometry(s.body, s.scroll_dips, s.offsets[s.measured_count])
                      : UiScrollbarGeometry{};
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
    if (!ui_scene_focus_target(&s->widgets, s->targets, target, ui_inbox_target_equal))
        return false;
    s->focused = target;
    ++s->revision;
    return true;
}
bool ui_inbox_scene_move_focus(UiInboxScene* s, UiInboxFocusMove move) noexcept {
    if (!current(*s))
        return false;
    UiInboxTarget pick{};
    // Tabs, filters, every row's actions, Load more / Retry, then Close.
    return ui_ring_pick(
               [&](auto emit) noexcept {
                   emit({UiInboxAction::tab_mentions});
                   emit({UiInboxAction::tab_unreads});
                   if (s->tab == UiInboxTab::mentions) {
                       if (s->content->this_server_available)
                           emit({UiInboxAction::filter_server});
                       emit({UiInboxAction::filter_everyone});
                       emit({UiInboxAction::filter_roles});
                   }
                   for (u32 i = 0; i < s->measured_count; ++i) {
                       UiInboxRow row{};
                       if (!s->source->resolve(s->source->context, i, &row))
                           return false;
                       if (row.kind == UiInboxRowKind::mention)
                           emit({UiInboxAction::jump, row.id, row.channel_id});
                       else if (row.kind == UiInboxRowKind::group)
                           emit({UiInboxAction::mark_guild, row.guild_id, 0});
                       else {
                           emit({UiInboxAction::open_channel, row.channel_id, row.channel_id});
                           emit({UiInboxAction::mark_channel, row.channel_id, row.channel_id});
                       }
                   }
                   if (s->more.generation)
                       emit(s->targets[s->more.slot]);
                   emit({UiInboxAction::close});
                   return true;
               },
               ui_inbox_target_equal, ui_inbox_scene_focused_target(*s), ui_ring(move), &pick) &&
           ui_inbox_scene_focus(s, pick);
}

bool ui_inbox_scene_spinner_visible(const UiInboxScene& s) noexcept {
    return current(s) && !s.measured_count && s.content->phase == UiInboxPhase::loading;
}
bool ui_inbox_scene_paint(const UiInboxScene& s, const Theme& theme, PaintStream* out) noexcept {
    if (!current(s))
        return true;
    const auto& c = *s.content;
    UiPaint p{out, theme};
    p.popover(s.panel, theme.scale.radius_md)
        .rule_below(s.header)
        .icon(UiIcon::inbox, ui_rect_edges(s.panel, ui_start(14), ui_start(16), ui_start(34), ui_start(36)),
              theme.text_secondary)
        .control<kUiTab>(s.widgets, s.tabs[0], s.tab == UiInboxTab::mentions)
        .control<kUiTab>(s.widgets, s.tabs[1], s.tab == UiInboxTab::unreads)
        .icon_control<kUiGhostButton>(s.widgets, s.close, UiIcon::close, 7, theme.text_secondary);
    if (s.tab == UiInboxTab::mentions)
        p.control<kChip>(s.widgets, s.chips[0], c.this_server)
            .control<kChip>(s.widgets, s.chips[1], c.everyone)
            .control<kChip>(s.widgets, s.chips[2], c.roles)
            .rule_below(s.filters);
    p.clip(s.body);
    for (u32 i = 0; p && i < s.count; ++i) {
        const auto& row = s.visible[i];
        const auto r = s.row_rectangles[i];
        if (row.kind == UiInboxRowKind::mention) {
            // Placeholder under the author's avatar media.
            p.look<kCard>(r, ui_widget_state(s.widgets, s.rows[i]))
                .rounded(mention_avatar(r), theme.avatar_placeholder, kUiInboxAvatarExtent * .5f);
        } else if (row.kind == UiInboxRowKind::channel) {
            p.look<kChannel>(r, ui_widget_state(s.widgets, s.rows[i]))
                .icon(row.guild_id ? UiIcon::channel : UiIcon::direct_message,
                      ui_rect_edges(r, ui_start(8), ui_start(9), ui_start(26), ui_start(27)),
                      row.muted ? theme.interactive : theme.text_secondary);
            if (row.mention_count) {
                const auto badge = badge_rectangle(s, i);
                p.rounded(badge, theme.notification, ui_rect_height(badge) * .5f);
            }
            if (ui_inbox_scene_channel_action_visible(s, i)) {
                const auto a = ui_bounds(s, s.actions[i]);
                const float icon = ui_icon_button_extent(ui_rect_width(a) - 8), x = (a.left + a.right - icon) * .5f,
                            y = (a.top + a.bottom - icon) * .5f;
                p.control<kRowAction>(s.widgets, s.actions[i])
                    .icon(UiIcon::check, {x, y, x + icon, y + icon},
                          ui_widget_state(s.widgets, s.actions[i]) ? theme.text_primary : theme.text_secondary);
            }
        } else {
            p.control<kRowAction>(s.widgets, s.actions[i]);
        }
    }
    if (!s.measured_count) {
        const auto r = s.body;
        const float x = (r.left + r.right) * .5f, y = r.top + minimum(56.f, ui_rect_height(r) * .25f);
        if (c.phase == UiInboxPhase::loading)
            p.spinner({x - 12, y - 12, x + 12, y + 12}, theme.tokens.emphasis.outline, s.spinner_phase);
        else
            p.icon(c.phase == UiInboxPhase::failed ? UiIcon::warning
                   : s.tab == UiInboxTab::mentions ? UiIcon::mention
                                                   : UiIcon::check,
                   {x - 16, y - 16, x + 16, y + 16}, theme.text_secondary);
    }
    p.unclip();
    if (s.more.generation)
        p.control<kMore>(s.widgets, s.more).rule_above(s.footer);
    return p.scrollbar(ui_inbox_scene_scrollbar(s)).end();
}

bool ui_inbox_scene_text_output(const UiInboxScene& s, const Theme& theme, UiTextOutput* out) noexcept {
    if (!current(s))
        return true;
    UiTextPen text{out, s.widgets, s.panel, s.body};
    const auto& c = *s.content;
    const bool mentions = s.tab == UiInboxTab::mentions;
    text(s.root, utf8_literal("Inbox"), ui_rect_edges(s.panel, ui_start(40), ui_start(12), ui_start(88), ui_start(40)),
         theme.text_primary, text_style_bold);
    text(s.tabs[0], utf8_literal("Mentions"), ui_bounds(s, s.tabs[0]), mentions ? theme.text_primary : theme.text_secondary,
         text_style_bold, ui_text_center);
    text(s.tabs[1], utf8_literal("Unreads"), ui_bounds(s, s.tabs[1]), mentions ? theme.text_secondary : theme.text_primary,
         text_style_bold, ui_text_center);
    if (mentions) {
        const Utf8View labels[]{c.this_server ? utf8_literal("This server") : utf8_literal("All servers"),
                                utf8_literal("@everyone"), utf8_literal("@roles")};
        const bool checked[]{c.this_server, c.everyone, c.roles};
        for (u32 i = 0; i < 3; ++i)
            if (s.chips[i].generation)
                text(s.chips[i], labels[i], ui_bounds(s, s.chips[i]), checked[i] ? theme.text_primary : theme.text_secondary,
                     text_style_subtext | text_style_bold, ui_text_center);
    }
    for (u32 i = 0; i < s.count; ++i) {
        const auto& row = s.visible[i];
        const auto r = s.row_rectangles[i];
        if (row.kind == UiInboxRowKind::mention) {
            constexpr auto left = ui_start(static_cast<int>(mention_text_left));
            text(s.rows[i], row.detail, ui_rect_edges(r, ui_start(12), ui_start(8), ui_end(-12), ui_start(26)),
                 theme.text_secondary, text_style_subtext, ui_text_in_body);
            text(s.rows[i], row.title, ui_rect_edges(r, left, ui_start(26), ui_end(-140), ui_start(46)), theme.text_primary,
                 text_style_bold, ui_text_in_body);
            text(s.rows[i], row.timestamp, ui_rect_edges(r, ui_end(-140), ui_start(26), ui_end(-12), ui_start(46)),
                 theme.text_secondary, text_style_subtext, ui_text_in_body | ui_text_end);
            text(s.rows[i], mention_body(row),
                 ui_rect_edges(r, left, ui_start(static_cast<int>(mention_body_top)), ui_end(-12), ui_end(-8)),
                 theme.text_primary, 0, ui_text_wrap | ui_text_in_body);
            continue;
        }
        const bool channel = row.kind == UiInboxRowKind::channel;
        // Channel names stop before the revealed button and badge, so hovering never reflows them.
        const float right = action_rectangle(row, r).left - (row.mention_count ? badge_width + 14 : 8);
        text(channel ? s.rows[i] : s.root, row.title, {channel ? r.left + 32 : r.left + 4, r.top, right, r.bottom},
             !channel ? theme.text_secondary : row.muted ? theme.interactive : theme.text_primary,
             channel ? text_style_bold : text_style_subtext | text_style_bold, ui_text_in_body);
        if (row.mention_count && channel) {
            char digits[2];
            text(s.rows[i], {digits, decimal_write(digits, minimum(row.mention_count, 99u))}, badge_rectangle(s, i),
                 theme.tokens.text.on_accent, text_style_subtext | text_style_bold, ui_text_in_body | ui_text_center);
        }
        if (!channel && s.actions[i].generation)
            text(s.actions[i], utf8_literal("Mark all read"), ui_bounds(s, s.actions[i]),
                 ui_widget_state(s.widgets, s.actions[i]) ? theme.text_primary : theme.tokens.text.link,
                 text_style_subtext | text_style_bold, ui_text_in_body | ui_text_center);
    }
    if (!s.measured_count) {
        const auto r = s.body;
        const float top = r.top + minimum(84.f, ui_rect_height(r) * .4f);
        text(s.root, c.phase == UiInboxPhase::loading  ? utf8_literal("Loading…")
                     : c.phase == UiInboxPhase::failed ? utf8_literal("Couldn't load your inbox")
                     : mentions                        ? utf8_literal("No recent mentions")
                                                       : utf8_literal("You're all caught up"),
             {r.left + 20, top, r.right - 20, top + 28}, theme.text_primary, text_style_bold,
             ui_text_in_body | ui_text_center);
        text(s.root, c.status.bytes                 ? c.status
                     : c.phase != UiInboxPhase::ready ? Utf8View{}
                     : mentions ? utf8_literal("Mentions of you will show up here.")
                                : utf8_literal("Channels with new messages will show up here."),
             {r.left + 24, top + 32, r.right - 24, top + 80}, theme.text_secondary, 0,
             ui_text_wrap | ui_text_in_body | ui_text_center);
    }
    if (s.more.generation)
        text(s.more, c.loading_more ? utf8_literal("Loading…") : c.phase == UiInboxPhase::failed ? utf8_literal("Try again")
                                                                                              : utf8_literal("Load more"),
             ui_bounds(s, s.more), c.loading_more ? theme.text_secondary : theme.text_primary, text_style_bold,
             ui_text_center);
    return text.ok();
}
} // namespace maicord
