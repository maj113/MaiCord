#include "maicord/ui/chat/pinned_messages_scene.hpp"
#include "maicord/ui/layout/rect_edges.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
constexpr float card_gap = 10.f, card_inset = 12.f;
constexpr UiLook kFooterButton{.fill = UI_TOKEN(surface_high), .border = UI_TOKEN(interactive), .radius = 6};
constexpr UiLook kCard{.fill = UI_TOKEN(surface), .hover = UI_TOKEN(surface_high), .border = UI_TOKEN(surface_high),
                       .focus = UI_TOKEN(tokens.emphasis.outline)};
float excerpt_height(const UiPinnedMessagesRow& row) noexcept {
    return row.body_height ? row.body_height : 48.f;
}
float card_height(const UiPinnedMessagesRow& row) noexcept {
    return 56.f + excerpt_height(row) + (row.detail.bytes ? 24.f : 0.f) + 44.f;
}
bool row_valid(const UiPinnedMessagesRow& row, u64 channel) noexcept {
    // The range test also rejects NaN and infinities.
    return row.channel_id == channel && row.message_id &&
           utf8_validate(row.author).valid && utf8_validate(row.timestamp).valid &&
           utf8_validate(row.body).valid && utf8_validate(row.detail).valid &&
           utf8_validate(row.avatar_hash).valid &&
           (row.body_height == 0 || (row.body_height >= 24 && row.body_height <= 144)) &&
           (row.edited_offset == 0 || row.edited_offset < row.body.bytes);
}

bool current(const UiPinnedMessagesScene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision &&
           s.content->channel_id == s.channel_id && s.content->page_index == s.page_index &&
           ui_source_current(s.source, s.source_revision, s.measured_count);
}
bool rebuild(UiPinnedMessagesScene& s) noexcept {
    clear_bytes(s.targets, sizeof(s.targets));
    clear_bytes(s.rows, sizeof(s.rows));
    clear_bytes(s.unpin, sizeof(s.unpin));
    s.body_owner = s.retry = s.previous = s.next = {};
    s.first = s.count = 0;
    s.root = ui_scene_root(&s.widgets, utf8_literal("Pinned messages"));
    if (!s.root.generation) return false;
    const auto& c = *s.content;
    const auto p = s.panel;
    s.close = ui_scene_add(s, ui_rect_edges(p, ui_end(-44), ui_start(12), ui_end(-12), ui_start(44)),
                           {UiPinnedMessagesAction::close, c.channel_id}, utf8_literal("Close pins"), true, s.root);
    s.refresh = ui_scene_add(s, ui_rect_edges(p, ui_end(-80), ui_start(12), ui_end(-48), ui_start(44)),
                             {UiPinnedMessagesAction::refresh, c.channel_id}, utf8_literal("Refresh pins"),
                             c.phase != UiPinnedMessagesPhase::loading && !c.loading_more, s.root);
    s.body_owner = ui_scene_add(s, s.body, {}, utf8_literal("Pinned messages viewport"), false, s.root);
    if (!s.close.generation || !s.refresh.generation || !s.body_owner.generation) return false;
    while (s.first < s.measured_count && s.offsets[s.first + 1] <= s.scroll_dips)
        ++s.first;
    for (u32 i = s.first; i < s.measured_count && s.count < kUiPinnedMessagesVisibleRows; ++i) {
        const float top = s.body.top + s.offsets[i] - s.scroll_dips + card_gap;
        if (top >= s.body.bottom) break;
        UiPinnedMessagesRow row{};
        if (!s.source->resolve(s.source->context, i, &row) || !row_valid(row, c.channel_id)) return false;
        const UiRect card{s.body.left + card_inset, top, s.body.right - 24, top + card_height(row)};
        if (!ui_rect_intersects(card, s.body)) continue;
        const u32 slot = s.count++;
        s.visible[slot] = row;
        s.body_styles[slot] = row.edited_offset
            ? TextStyleSpan{row.edited_offset, row.body.bytes - row.edited_offset, text_style_subtext, text_size_xs}
            : TextStyleSpan{};
        s.cards[slot] = card;
        // The whole card jumps; the later child target owns only its unpin slot.
        s.rows[slot] = ui_scene_add(s, ui_rect_intersection(card, s.body),
                                    {UiPinnedMessagesAction::jump, row.channel_id, row.message_id},
                                    utf8_literal("Jump to pinned message"), row.jump_enabled, s.body_owner);
        if (!s.rows[slot].generation) return false;
        if (!row.can_unpin) continue;
        const auto control = ui_rect_intersection(ui_rect_edges(card, ui_end(-40), ui_start(8), ui_end(-8), ui_start(40)),
                                                  s.body);
        if (!ui_rect_has_area(control)) continue;
        s.unpin[slot] = ui_scene_add(s, control, {UiPinnedMessagesAction::unpin, row.channel_id, row.message_id},
                                     utf8_literal("Unpin message"), true, s.body_owner);
        if (!s.unpin[slot].generation) return false;
        if (row.unpin_pending)
            s.widgets.storage[s.unpin[slot].slot].flags &= static_cast<u16>(~ui_widget_focusable);
    }
    const auto f = ui_rect_edges(s.footer, ui_start(12), ui_start(10), ui_end(-12), ui_end(-10));
    const bool ready = c.phase == UiPinnedMessagesPhase::ready && !c.loading_more;
    if (c.phase == UiPinnedMessagesPhase::failed) {
        s.retry = ui_scene_add(s, f, {UiPinnedMessagesAction::retry, c.channel_id}, utf8_literal("Retry loading pins"),
                               !c.loading_more, s.root);
    } else if (s.measured_count || (c.phase == UiPinnedMessagesPhase::ready && (c.has_previous || c.has_next))) {
        const float button_width = minimum(88.f, ui_rect_width(s.footer) * .3f);
        s.previous = ui_scene_add(s, {f.left, f.top, f.left + button_width, f.bottom},
                                  {UiPinnedMessagesAction::previous, c.channel_id}, utf8_literal("Newer pins"),
                                  c.has_previous && ready, s.root);
        s.next = ui_scene_add(s, {f.right - button_width, f.top, f.right, f.bottom},
                              {UiPinnedMessagesAction::next, c.channel_id}, utf8_literal("Older pins"),
                              c.has_next && ready, s.root);
    }
    return ui_scene_commit(&s.widgets, s.panel, s.targets, s.focused, ui_pinned_messages_target_equal, &s.revision);
}

} // namespace

bool ui_pinned_messages_scene_current(const UiPinnedMessagesScene& s) noexcept { return current(s); }
float ui_pinned_messages_scene_scroll_maximum(const UiPinnedMessagesScene& s) noexcept {
    return maximum(0.f, s.offsets[s.measured_count] - ui_rect_height(s.body));
}
bool ui_pinned_messages_scene_update(UiPinnedMessagesScene* s, const UiPinnedMessagesSource* source,
                                    const UiPinnedMessagesContent* content, const UiRect& viewport) noexcept {
    if (!s || !content || !content->channel_id || content->phase > UiPinnedMessagesPhase::failed ||
        !utf8_validate(content->channel).valid || !utf8_validate(content->status).valid ||
        !ui_rect_valid(viewport) || ui_rect_width(viewport) < 180 || ui_rect_height(viewport) < 128 ||
        (source && (!source->resolve || source->count > kUiPinnedMessagesPageCapacity ||
                    (source->current && !source->current(source->context))))) return false;
    if (current(*s) && s->source == source && s->content == content &&
        ui_rect_equal(s->viewport, viewport)) return true;
    float offsets[kUiPinnedMessagesPageCapacity + 1]{};
    const u32 count = source ? source->count : 0;
    u64 first_message = 0;
    for (u32 i = 0; i < count; ++i) {
        UiPinnedMessagesRow row{};
        if (!source->resolve(source->context, i, &row) || !row_valid(row, content->channel_id)) return false;
        if (!i) first_message = row.message_id;
        offsets[i + 1] = offsets[i] + card_height(row) + card_gap;
    }
    const bool same_page = s->active && s->channel_id == content->channel_id &&
        s->page_index == content->page_index && s->page_first_message_id == first_message;
    // A page publication starts at its beginning. Resize and state updates
    // preserve the current offset and semantic keyboard target.
    if (s->active && !same_page) {
        s->scroll_dips = 0;
        if (s->focused.channel_id != content->channel_id)
            reset_value(&s->focused);
        else if (s->focused.message_id)
            s->focused = first_message
                ? UiPinnedMessagesTarget{UiPinnedMessagesAction::jump, content->channel_id, first_message}
                : UiPinnedMessagesTarget{UiPinnedMessagesAction::close, content->channel_id};
    }
    if (!s->storage.arena.base && !ui_scene_open(&s->storage, &s->widgets, kUiPinnedMessagesWidgetCapacity))
        return false;
    s->viewport = viewport;
    const float width = minimum(kUiPinnedMessagesPreferredWidth, ui_rect_width(viewport));
    s->panel = {viewport.right - width, viewport.top, viewport.right, viewport.bottom};
    s->header = {s->panel.left, s->panel.top, s->panel.right, s->panel.top + kUiPinnedMessagesHeaderHeight};
    s->footer = {s->panel.left, s->panel.bottom - kUiPinnedMessagesFooterHeight, s->panel.right, s->panel.bottom};
    const bool banner = content->mutation_failed || (count && content->phase == UiPinnedMessagesPhase::failed);
    s->status_rectangle = banner ? UiRect{s->panel.left, s->header.bottom, s->panel.right,
        minimum(s->footer.top, s->header.bottom + 36)} : UiRect{};
    s->body = {s->panel.left, banner ? s->status_rectangle.bottom : s->header.bottom,
               s->panel.right, s->footer.top};
    if (!ui_rect_has_area(s->body)) return false;
    s->source = source;
    s->content = content;
    s->source_revision = source ? source->revision : 0;
    s->content_revision = content->revision;
    s->measured_count = count;
    s->channel_id = content->channel_id;
    s->page_index = content->page_index;
    s->page_first_message_id = first_message;
    UiLabelWriter{s->page_text, &s->page_bytes} << utf8_literal("Page ") << static_cast<u64>(content->page_index) + 1;
    copy_bytes(s->offsets, offsets, sizeof(offsets));
    s->scroll_dips = minimum(maximum(0.f, s->scroll_dips), ui_pinned_messages_scene_scroll_maximum(*s));
    s->overlay = ui_rect_width(viewport) < 720;
    s->active = true;
    if (!rebuild(*s)) { s->active = false; return false; }
    return true;
}
void ui_pinned_messages_scene_release(UiPinnedMessagesScene* s) noexcept { ui_scene_release(s); }
bool ui_pinned_messages_scene_contains(const UiPinnedMessagesScene& s, float x, float y) noexcept {
    return current(s) && ui_rect_contains(s.panel, x, y);
}
UiPinnedMessagesTarget ui_pinned_messages_scene_target(const UiPinnedMessagesScene& s, UiWidgetHandle handle) noexcept {
    if (!current(s) || handle.slot >= kUiPinnedMessagesWidgetCapacity ||
        !ui_widget_manager_input_enabled(s.widgets, handle)) return {};
    const auto target = s.targets[handle.slot];
    if (target.action == UiPinnedMessagesAction::unpin)
        for (u32 i = 0; i < s.count; ++i)
            if (ui_widget_handle_equal(s.unpin[i], handle) && s.visible[i].unpin_pending)
                return {};
    return target;
}
UiPinnedMessagesTarget ui_pinned_messages_scene_hit_test(const UiPinnedMessagesScene& s, float x, float y) noexcept {
    return ui_pinned_messages_scene_target(s, ui_widget_manager_hit_test(s.widgets, x, y));
}
UiPinnedMessagesTarget ui_pinned_messages_scene_focused_target(const UiPinnedMessagesScene& s) noexcept {
    return ui_pinned_messages_scene_target(s, s.widgets.focused);
}
bool ui_pinned_messages_scene_set_hover(UiPinnedMessagesScene* s, float x, float y, bool* changed) noexcept {
    return s && current(*s) && ui_scene_hover(&s->widgets, &s->revision, x, y, changed);
}
bool ui_pinned_messages_scene_scroll(UiPinnedMessagesScene* s, float offset) noexcept {
    if (!s || !current(*s) || !ui_coordinate_valid(offset)) return false;
    return !ui_scroll_offset_store(&s->scroll_dips, offset, ui_pinned_messages_scene_scroll_maximum(*s)) ||
           rebuild(*s);
}
bool ui_pinned_messages_scene_focus(UiPinnedMessagesScene* s, UiPinnedMessagesTarget target) noexcept {
    if (!s || !current(*s) || target.channel_id != s->content->channel_id || target.action == UiPinnedMessagesAction::none)
        return false;
    if (target.action == UiPinnedMessagesAction::jump || target.action == UiPinnedMessagesAction::unpin) {
        u32 i = 0;
        for (; i < s->measured_count; ++i) {
            UiPinnedMessagesRow row{};
            if (!s->source->resolve(s->source->context, i, &row)) return false;
            if (row.message_id != target.message_id) continue;
            if (target.action == UiPinnedMessagesAction::jump ? !row.jump_enabled : !row.can_unpin || row.unpin_pending)
                return false;
            const float first = s->offsets[i], last = s->offsets[i + 1], height = ui_rect_height(s->body);
            const float offset = first < s->scroll_dips ? first
                : last > s->scroll_dips + height ? (last - first > height ? first : last - height) : s->scroll_dips;
            s->focused = target;
            if (!ui_pinned_messages_scene_scroll(s, offset)) return false;
            break;
        }
        if (i == s->measured_count) return false;
    }
    if (!ui_scene_focus_target(&s->widgets, s->targets, target, ui_pinned_messages_target_equal)) return false;
    s->focused = target;
    ++s->revision;
    return true;
}
bool ui_pinned_messages_scene_move_focus(UiPinnedMessagesScene* s, UiPinnedMessagesFocusMove move) noexcept {
    if (!current(*s) || move > UiPinnedMessagesFocusMove::page_next) return false;
    if (move == UiPinnedMessagesFocusMove::page_previous || move == UiPinnedMessagesFocusMove::page_next) {
        const float direction = move == UiPinnedMessagesFocusMove::page_previous ? -1.f : 1.f;
        if (!ui_pinned_messages_scene_scroll(s, s->scroll_dips + direction * ui_rect_height(s->body))) return false;
        for (u32 i = 0; i < s->count; ++i)
            if (s->visible[i].jump_enabled)
                return ui_pinned_messages_scene_focus(s, {UiPinnedMessagesAction::jump,
                        s->visible[i].channel_id, s->visible[i].message_id});
        return true;
    }
    const auto& content = *s->content;
    const bool idle = !content.loading_more;
    UiPinnedMessagesTarget pick{};
    // Close, Refresh, each row's Jump and Unpin, then the footer buttons.
    return ui_ring_pick(
               [&](auto emit) noexcept {
                   emit({UiPinnedMessagesAction::close, content.channel_id});
                   if (content.phase != UiPinnedMessagesPhase::loading && idle)
                       emit({UiPinnedMessagesAction::refresh, content.channel_id});
                   for (u32 i = 0; i < s->measured_count; ++i) {
                       UiPinnedMessagesRow row{};
                       if (!s->source->resolve(s->source->context, i, &row)) return false;
                       if (row.jump_enabled) emit({UiPinnedMessagesAction::jump, row.channel_id, row.message_id});
                       if (row.can_unpin && !row.unpin_pending)
                           emit({UiPinnedMessagesAction::unpin, row.channel_id, row.message_id});
                   }
                   if (content.phase == UiPinnedMessagesPhase::failed && idle)
                       emit({UiPinnedMessagesAction::retry, content.channel_id});
                   else if (content.phase == UiPinnedMessagesPhase::ready && idle) {
                       if (content.has_previous) emit({UiPinnedMessagesAction::previous, content.channel_id});
                       if (content.has_next) emit({UiPinnedMessagesAction::next, content.channel_id});
                   }
                   return true;
               },
               ui_pinned_messages_target_equal, ui_pinned_messages_scene_focused_target(*s), ui_ring(move), &pick) &&
           ui_pinned_messages_scene_focus(s, pick);
}
bool ui_pinned_messages_scene_avatar(const UiPinnedMessagesScene& s, u32 slot, UiRect* rectangle, UiRect* clip) noexcept {
    if (!rectangle || !clip || !current(s) || slot >= s.count) return false;
    *rectangle = ui_rect_edges(s.cards[slot], ui_start(12), ui_start(12), ui_start(40), ui_start(40));
    *clip = ui_rect_intersection(*rectangle, s.body);
    return ui_rect_has_area(*clip);
}

bool ui_pinned_messages_scene_spinner_visible(const UiPinnedMessagesScene& s) noexcept {
    if (!current(s)) return false;
    if (!s.measured_count && s.content->phase == UiPinnedMessagesPhase::loading) return true;
    for (u32 i = 0; i < s.count; ++i)
        if (s.visible[i].can_unpin && s.visible[i].unpin_pending &&
            ui_rect_intersects(s.cards[i], s.body)) return true;
    return false;
}
bool ui_pinned_messages_scene_paint(const UiPinnedMessagesScene& s, const Theme& theme, PaintStream* out) noexcept {
    if (!current(s)) return true;
    UiPaint p{out, theme};
    p.popover(s.panel, 0).rule_below(s.header).rule_above(s.footer)
        .icon(UiIcon::pin, ui_rect_edges(s.panel, ui_start(12), ui_start(16), ui_start(36), ui_start(40)), theme.text_secondary)
        .icon_control<kUiGhostButton>(s.widgets, s.refresh, UiIcon::refresh, 6, theme.text_secondary)
        .icon_control<kUiGhostButton>(s.widgets, s.close, UiIcon::close, 6, theme.text_secondary)
        .clip(s.body);
    for (u32 i = 0; p && i < s.count; ++i) {
        const auto r = s.cards[i];
        const auto& row = s.visible[i];
        const bool selected = ui_focused(s, s.rows[i]) || ui_focused(s, s.unpin[i]);
        p.look<kCard>(r, static_cast<u8>((ui_hovered(s, s.rows[i]) ? ui_state_hovered : 0) |
                                         (selected ? ui_state_focused : 0)));
        UiRect avatar{}, clip{};
        if (ui_pinned_messages_scene_avatar(s, i, &avatar, &clip))
            p.rounded(avatar, theme.avatar_placeholder, theme.scale.radius_full)
                .rounded(ui_rect_edges(avatar, ui_start(10), ui_start(6), ui_end(-10), ui_start(14)), theme.text_secondary,
                         theme.scale.radius_sm)
                .rounded(ui_rect_edges(avatar, ui_start(6), ui_start(16), ui_end(-6), ui_end(-4)), theme.text_secondary, 6);
        const float action_top = r.bottom - 38;
        p.separator({r.left + 12, action_top - 3, r.right - 12, action_top - 2})
            .icon(UiIcon::reply, {r.left + 12, action_top + 5, r.left + 30, action_top + 23},
                  row.jump_enabled ? theme.tokens.emphasis.outline : theme.interactive);
        if (!row.can_unpin) continue;
        const auto unpin = ui_rect_edges(r, ui_end(-40), ui_start(8), ui_end(-8), ui_start(40));
        p.look<kUiGhostButton>(unpin, ui_widget_state(s.widgets, s.unpin[i]));
        if (row.unpin_pending)
            p.spinner(ui_rect_shrink(unpin, 8), theme.text_secondary, s.spinner_phase);
        else
            p.icon(UiIcon::unpin, ui_rect_shrink(unpin, 6), ui_hovered(s, s.unpin[i]) ? theme.notification : theme.text_secondary);
    }
    if (!s.measured_count) {
        const auto r = s.body;
        const float x = (r.left + r.right) * .5f, y = r.top + minimum(40.f, ui_rect_height(r) * .2f);
        if (s.content->phase == UiPinnedMessagesPhase::loading)
            p.spinner({x - 12, y - 12, x + 12, y + 12}, theme.tokens.emphasis.outline, s.spinner_phase);
        else
            p.icon(s.content->phase == UiPinnedMessagesPhase::failed ? UiIcon::warning : UiIcon::pin,
                   {x - 16, y - 16, x + 16, y + 16}, theme.text_secondary);
    }
    p.unclip().control<kFooterButton>(s.widgets, s.retry).control<kFooterButton>(s.widgets, s.previous)
        .control<kFooterButton>(s.widgets, s.next);
    if (ui_rect_has_area(s.status_rectangle)) p.fill(s.status_rectangle, theme.base_lowest);
    return p.scrollbar(ui_scrollbar_geometry(s.body, s.scroll_dips, s.offsets[s.measured_count])).end();
}
bool ui_pinned_messages_scene_text_output(const UiPinnedMessagesScene& s, const Theme& theme, UiTextOutput* out) noexcept {
    if (!current(s)) return true;
    UiTextPen text{out, s.widgets, s.panel, s.body};
    const auto& c = *s.content;
    const auto p = s.panel;
    text(s.root, utf8_literal("Pins"), ui_rect_edges(p, ui_start(44), ui_start(10), ui_end(-84), ui_start(36)),
         theme.text_primary, text_style_bold);
    text(s.root, c.channel, ui_rect_edges(p, ui_start(16), ui_start(38), ui_end(-16), ui_start(58)), theme.text_secondary,
         text_style_subtext);
    for (u32 i = 0; i < s.count; ++i) {
        const auto& row = s.visible[i];
        const auto r = s.cards[i];
        const auto card = s.rows[i];
        const float body_bottom = r.top + 56 + excerpt_height(row);
        text(card, row.author.bytes ? row.author : utf8_literal("Unknown author"),
             {r.left + 50, r.top + 8, r.right - (row.can_unpin ? 44 : 12), r.top + 30}, theme.text_primary,
             text_style_bold, ui_text_in_body);
        text(card, row.timestamp, ui_rect_edges(r, ui_start(50), ui_start(30), ui_end(-12), ui_start(48)),
             theme.text_secondary, text_style_subtext, ui_text_in_body);
        const Utf8View body = row.body.bytes ? row.body : utf8_literal("Message has no text");
        const UiRect body_rectangle{r.left + 12, r.top + 56, r.right - 12, body_bottom};
        if (row.edited_offset) {
            // The "(edited)" label: small muted type after the text, so the command carries its span.
            const UiRect clip = ui_rect_intersection(body_rectangle, s.body);
            UiTextCommand command{body, {&s.widgets, card}, body_rectangle, clip, theme.text_primary, 0,
                                  UiTextAlignment::start, UiTextAlignment::start, TextWrapping::wrap,
                                  TextOverflow::ellipsis, UiTextFace::normal};
            command.effect.themed_styles = true;
            command.styles = {&s.body_styles[i], 1};
            if (ui_rect_has_area(clip))
                text.emit(command);
        } else {
            text(card, body, body_rectangle, theme.text_primary, 0, ui_text_wrap | ui_text_in_body);
        }
        text(card, row.detail, {r.left + 12, body_bottom + 2, r.right - 12, body_bottom + 22}, theme.text_secondary,
             text_style_subtext, ui_text_in_body);
        text(card, utf8_literal("Jump to message"), ui_rect_edges(r, ui_start(36), ui_end(-33), ui_end(-12), ui_end(-9)),
             row.jump_enabled ? theme.tokens.emphasis.outline : theme.interactive, text_style_subtext | text_style_bold,
             ui_text_in_body);
    }
    if (!s.measured_count) {
        const auto r = s.body;
        const float top = r.top + minimum(68.f, ui_rect_height(r) * .35f);
        text(s.root, c.phase == UiPinnedMessagesPhase::loading ? utf8_literal("Loading pins…")
                     : c.phase == UiPinnedMessagesPhase::failed ? utf8_literal("Couldn't load pins")
                                                                 : utf8_literal("No pinned messages"),
             {r.left + 20, top, r.right - 20, top + 28}, theme.text_primary, text_style_bold,
             ui_text_in_body | ui_text_center);
        if (c.phase != UiPinnedMessagesPhase::loading)
            text(s.root, c.status.bytes ? c.status
                         : c.phase == UiPinnedMessagesPhase::failed ? Utf8View{}
                                                                     : utf8_literal("Pinned messages will appear here."),
                 {r.left + 24, top + 36, r.right - 24, top + 108}, theme.text_secondary, 0,
                 ui_text_wrap | ui_text_in_body | ui_text_center);
    }
    if (ui_rect_has_area(s.status_rectangle))
        text(s.root, c.status.bytes ? c.status : utf8_literal("Couldn't update this pin. Try again."),
             ui_rect_edges(s.status_rectangle, ui_start(12), ui_start(4), ui_end(-12), ui_end(-4)), theme.notification,
             text_style_subtext);
    if (s.retry.generation)
        return text(s.retry, utf8_literal("Retry"), ui_bounds(s, s.retry), theme.text_primary, text_style_bold,
                    ui_text_center).ok();
    if (s.previous.generation || s.next.generation) {
        const bool ready = c.phase == UiPinnedMessagesPhase::ready && !c.loading_more;
        text(s.previous, utf8_literal("Newer"), ui_bounds(s, s.previous),
             c.has_previous && ready ? theme.text_primary : theme.interactive, text_style_bold, ui_text_center);
        text(s.next, utf8_literal("Older"), ui_bounds(s, s.next), c.has_next && ready ? theme.text_primary : theme.interactive,
             text_style_bold, ui_text_center);
        return text(s.root, c.loading_more ? utf8_literal("Loading…") : Utf8View{s.page_text, s.page_bytes},
                    {ui_bounds(s, s.previous).right + 4, s.footer.top + 10, ui_bounds(s, s.next).left - 4,
                     s.footer.bottom - 10}, theme.text_secondary, text_style_subtext, ui_text_center).ok();
    }
    if (c.phase == UiPinnedMessagesPhase::loading && !s.measured_count) return text.ok();
    return text(s.root, c.status.bytes ? c.status : utf8_literal("Pinned in this channel"),
                ui_rect_edges(s.footer, ui_start(16), ui_start(10), ui_end(-16), ui_end(-10)),
                c.mutation_failed ? theme.notification : theme.text_secondary, text_style_subtext).ok();
}
} // namespace maicord
