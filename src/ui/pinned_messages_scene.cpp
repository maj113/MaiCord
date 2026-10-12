#include "maicord/ui/pinned_messages_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
constexpr float card_gap = 10.f, card_inset = 12.f;
float excerpt_height(const UiPinnedMessagesRow& row) noexcept {
    return row.body_height ? row.body_height : 48.f;
}
float card_height(const UiPinnedMessagesRow& row) noexcept {
    return 56.f + excerpt_height(row) + (row.detail.bytes ? 24.f : 0.f) + 44.f;
}
bool row_valid(const UiPinnedMessagesRow& row, u64 channel) noexcept {
    u32 height_bits = 0;
    copy_bytes(&height_bits, &row.body_height, sizeof(height_bits));
    return row.channel_id == channel && row.message_id &&
           (height_bits & 0x7f800000u) != 0x7f800000u &&
           utf8_validate(row.author).valid && utf8_validate(row.timestamp).valid &&
           utf8_validate(row.body).valid && utf8_validate(row.detail).valid &&
           utf8_validate(row.avatar_hash).valid && ui_coordinate_valid(row.body_height) &&
           (row.body_height == 0 || (row.body_height >= 24 && row.body_height <= 144));
}
UiRect bounds(const UiPinnedMessagesScene& s, UiWidgetHandle handle) noexcept {
    const auto* node = ui_widget_manager_layout_node(s.widgets, handle);
    return node ? node->rectangle : UiRect{};
}
bool current(const UiPinnedMessagesScene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision &&
           s.content->channel_id == s.channel_id && s.content->page_index == s.page_index &&
           (!s.source || (s.source->revision == s.source_revision &&
                          s.source->count == s.measured_count &&
                          (!s.source->current || s.source->current(s.source->context))));
}
UiWidgetHandle add(UiPinnedMessagesScene& s, UiRect r, UiPinnedMessagesTarget target,
                   Utf8View name, bool enabled, UiWidgetHandle parent) noexcept {
    if (!ui_rect_has_area(r)) return {};
    auto style = ui_layout_style();
    style.width = ui_layout_fixed(ui_rect_width(r));
    style.height = ui_layout_fixed(ui_rect_height(r));
    // During tree construction the parent has not been laid out yet.
    const auto parent_origin = s.body_owner.generation && parent.slot == s.body_owner.slot ? s.body : s.panel;
    style.margin.left = r.left - parent_origin.left;
    style.margin.top = r.top - parent_origin.top;
    style.flags |= ui_layout_node_allow_overlap | ui_layout_node_clip_children;
    const u32 id = s.widgets.count + 1;
    const auto result = ui_widget_manager_add_prevalidated(&s.widgets, parent,
        {id, target.action == UiPinnedMessagesAction::none ? UiWidgetKind::container : UiWidgetKind::button,
         static_cast<u16>(enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : 0),
         {name, style, {}, 0, id}});
    if (!result.ok()) return {};
    s.targets[result.handle.slot] = target;
    return result.handle;
}
bool rebuild(UiPinnedMessagesScene& s) noexcept {
    ui_widget_manager_reset(&s.widgets);
    clear_bytes(s.targets, sizeof(s.targets));
    clear_bytes(s.rows, sizeof(s.rows));
    clear_bytes(s.unpin, sizeof(s.unpin));
    s.body_owner = {};
    s.retry = {};
    s.previous = s.next = {};
    s.first = s.count = 0;
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::overlay;
    style.width = style.height = ui_layout_fill();
    style.flags |= ui_layout_node_allow_overlap | ui_layout_node_clip_children;
    const auto root = ui_widget_manager_add_prevalidated(&s.widgets, ui_widget_invalid_handle(),
        {1, UiWidgetKind::container, ui_widget_none, {utf8_literal("Pinned messages"), style}});
    if (!root.ok()) return false;
    s.root = root.handle;
    s.close = add(s, {s.panel.right - 44, s.panel.top + 12, s.panel.right - 12, s.panel.top + 44},
                  {UiPinnedMessagesAction::close, s.content->channel_id}, utf8_literal("Close pins"), true, s.root);
    s.refresh = add(s, {s.panel.right - 80, s.panel.top + 12, s.panel.right - 48, s.panel.top + 44},
                    {UiPinnedMessagesAction::refresh, s.content->channel_id}, utf8_literal("Refresh pins"),
                    s.content->phase != UiPinnedMessagesPhase::loading && !s.content->loading_more, s.root);
    s.body_owner = add(s, s.body, {}, utf8_literal("Pinned messages viewport"), false, s.root);
    if (!s.close.generation || !s.refresh.generation || !s.body_owner.generation) return false;
    while (s.first < s.measured_count && s.offsets[s.first + 1] <= s.scroll_dips)
        ++s.first;
    for (u32 i = s.first; i < s.measured_count && s.count < kUiPinnedMessagesVisibleRows; ++i) {
        const float top = s.body.top + s.offsets[i] - s.scroll_dips + card_gap;
        if (top >= s.body.bottom) break;
        UiPinnedMessagesRow row{};
        if (!s.source->resolve(s.source->context, i, &row) || !row_valid(row, s.content->channel_id)) return false;
        const UiRect card{s.body.left + card_inset, top, s.body.right - 24,
                          top + card_height(row)};
        if (!ui_rect_has_area(ui_rect_intersection(card, s.body))) continue;
        const u32 slot = s.count++;
        s.visible[slot] = row;
        s.cards[slot] = card;
        // The whole card jumps; the later child target owns only its unpin slot.
        s.rows[slot] = add(s, ui_rect_intersection(card, s.body), {UiPinnedMessagesAction::jump, row.channel_id, row.message_id},
                           utf8_literal("Jump to pinned message"), row.jump_enabled, s.body_owner);
        if (!s.rows[slot].generation) return false;
        if (row.can_unpin) {
            const auto control = ui_rect_intersection({card.right - 40, card.top + 8, card.right - 8, card.top + 40}, s.body);
            if (!ui_rect_has_area(control)) continue;
            s.unpin[slot] = add(s, control,
                                {UiPinnedMessagesAction::unpin, row.channel_id, row.message_id},
                                utf8_literal("Unpin message"), true, s.body_owner);
            if (!s.unpin[slot].generation) return false;
            if (row.unpin_pending)
                s.widgets.storage[s.unpin[slot].slot].flags &= static_cast<u16>(~ui_widget_focusable);
        }
    }
    if (s.content->phase == UiPinnedMessagesPhase::failed) {
        const UiRect r{s.footer.left + 12, s.footer.top + 10,
                       s.footer.right - 12, s.footer.bottom - 10};
        s.retry = add(s, r, {UiPinnedMessagesAction::retry, s.content->channel_id},
                      utf8_literal("Retry loading pins"), !s.content->loading_more, s.root);
    } else if (s.measured_count || (s.content->phase == UiPinnedMessagesPhase::ready &&
                                  (s.content->has_previous || s.content->has_next))) {
        const float button_width = minimum(88.f, ui_rect_width(s.footer) * .3f);
        s.previous = add(s, {s.footer.left + 12, s.footer.top + 10,
                            s.footer.left + 12 + button_width, s.footer.bottom - 10},
                         {UiPinnedMessagesAction::previous, s.content->channel_id},
                         utf8_literal("Newer pins"), s.content->has_previous &&
                         s.content->phase == UiPinnedMessagesPhase::ready && !s.content->loading_more, s.root);
        s.next = add(s, {s.footer.right - 12 - button_width, s.footer.top + 10,
                        s.footer.right - 12, s.footer.bottom - 10},
                     {UiPinnedMessagesAction::next, s.content->channel_id},
                     utf8_literal("Older pins"), s.content->has_next &&
                     s.content->phase == UiPinnedMessagesPhase::ready && !s.content->loading_more, s.root);
    }
    if (!ui_widget_manager_layout_rectangle(&s.widgets, s.panel).ok()) return false;
    for (u32 i = 1; i < s.widgets.count; ++i)
        if (ui_pinned_messages_target_equal(s.targets[i], s.focused))
            ui_widget_manager_focus(&s.widgets, {static_cast<u16>(i), s.widgets.generation});
    ++s.revision;
    return true;
}
bool text(UiTextOutput* out, const UiPinnedMessagesScene& s, UiWidgetHandle owner,
          Utf8View value, UiRect r, Color color, u32 flags = 0, bool wrap = false,
          bool row = false, UiTextAlignment alignment = UiTextAlignment::start) noexcept {
    const auto clip = ui_rect_intersection(r, row ? s.body : s.panel);
    return !value.bytes || !ui_rect_has_area(clip) || ui_text_output_push(out,
        {value, {&s.widgets, owner}, r, clip, color, flags, alignment,
         wrap ? UiTextAlignment::start : UiTextAlignment::center,
         wrap ? UiTextWrapping::wrap : UiTextWrapping::no_wrap, UiTextOverflow::ellipsis,
         UiTextFace::normal});
}
bool surface(PaintStream* out, UiRect r, const Theme& theme, Color fill, Color border, float radius = 8) noexcept {
    return ui_background_paint_surface(out, r, theme, fill, border, radius, 1, .9f,
        UiBackgroundSurfaceMode::gradient, UiBackgroundTier::high);
}
bool focused(const UiPinnedMessagesScene& s, UiWidgetHandle handle) noexcept {
    return handle.generation && s.widgets.focused.generation == handle.generation &&
           s.widgets.focused.slot == handle.slot;
}
bool hovered(const UiPinnedMessagesScene& s, UiWidgetHandle handle) noexcept {
    return handle.generation && s.widgets.hovered.generation == handle.generation &&
           s.widgets.hovered.slot == handle.slot;
}
} // namespace

bool ui_pinned_messages_scene_current(const UiPinnedMessagesScene& s) noexcept { return current(s); }
float ui_pinned_messages_scene_scroll_maximum(const UiPinnedMessagesScene& s) noexcept {
    return maximum(0.f, s.offsets[s.measured_count] - ui_rect_height(s.body));
}
bool ui_pinned_messages_scene_update(UiPinnedMessagesScene* s, const UiPinnedMessagesSource* source,
                                    const UiPinnedMessagesContent* content, UiRect viewport) noexcept {
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
            s->focused = {};
        else if (s->focused.message_id)
            s->focused = first_message
                ? UiPinnedMessagesTarget{UiPinnedMessagesAction::jump, content->channel_id, first_message}
                : UiPinnedMessagesTarget{UiPinnedMessagesAction::close, content->channel_id};
    }
    if (!s->storage.arena.base && (!ui_fixed_widget_storage_initialize(&s->storage, kUiPinnedMessagesWidgetCapacity) ||
        !ui_widget_manager_initialize(&s->widgets, s->storage.widgets, s->storage.layout_nodes))) return false;
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
    s->state_rectangle = s->body;
    s->source = source;
    s->content = content;
    s->source_revision = source ? source->revision : 0;
    s->content_revision = content->revision;
    s->measured_count = count;
    s->channel_id = content->channel_id;
    s->page_index = content->page_index;
    s->page_first_message_id = first_message;
    constexpr auto page_prefix = utf8_literal("Page ");
    copy_bytes(s->page_text, page_prefix.data, page_prefix.bytes);
    s->page_bytes = static_cast<u8>(page_prefix.bytes);
    char digits[10]{};
    u32 digit_count = 0;
    u64 page_number = static_cast<u64>(content->page_index) + 1;
    do { digits[digit_count++] = static_cast<char>('0' + page_number % 10); page_number /= 10; } while (page_number);
    while (digit_count) s->page_text[s->page_bytes++] = digits[--digit_count];
    copy_bytes(s->offsets, offsets, sizeof(offsets));
    s->scroll_dips = minimum(maximum(0.f, s->scroll_dips), ui_pinned_messages_scene_scroll_maximum(*s));
    s->overlay = ui_rect_width(viewport) < 720;
    s->active = true;
    if (!rebuild(*s)) { s->active = false; return false; }
    return true;
}
void ui_pinned_messages_scene_release(UiPinnedMessagesScene* s) noexcept {
    if (!s) return;
    ui_fixed_widget_storage_release(&s->storage);
    *s = {};
}
bool ui_pinned_messages_scene_contains(const UiPinnedMessagesScene& s, float x, float y) noexcept {
    return current(s) && ui_rect_contains(s.panel, x, y);
}
UiPinnedMessagesTarget ui_pinned_messages_scene_target(const UiPinnedMessagesScene& s, UiWidgetHandle handle) noexcept {
    if (!current(s) || handle.slot >= kUiPinnedMessagesWidgetCapacity ||
        !ui_widget_manager_input_enabled(s.widgets, handle)) return {};
    const auto target = s.targets[handle.slot];
    if (target.action == UiPinnedMessagesAction::unpin)
        for (u32 i = 0; i < s.count; ++i)
            if (s.unpin[i].generation == handle.generation && s.unpin[i].slot == handle.slot && s.visible[i].unpin_pending)
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
    if (!s || !changed || !current(*s)) return false;
    const auto handle = ui_widget_manager_hit_test(s->widgets, x, y);
    *changed = handle.slot != s->widgets.hovered.slot || handle.generation != s->widgets.hovered.generation;
    if (!ui_widget_manager_hover(&s->widgets, handle)) return false;
    if (*changed) ++s->revision;
    return true;
}
bool ui_pinned_messages_scene_scroll(UiPinnedMessagesScene* s, float offset) noexcept {
    if (!s || !current(*s) || !ui_coordinate_valid(offset)) return false;
    const float next = minimum(maximum(0.f, offset), ui_pinned_messages_scene_scroll_maximum(*s));
    if (next == s->scroll_dips) return true;
    s->scroll_dips = next;
    return rebuild(*s);
}
bool ui_pinned_messages_scene_focus(UiPinnedMessagesScene* s, UiPinnedMessagesTarget target) noexcept {
    if (!s || !current(*s) || target.channel_id != s->content->channel_id || target.action == UiPinnedMessagesAction::none)
        return false;
    if (target.action == UiPinnedMessagesAction::jump || target.action == UiPinnedMessagesAction::unpin) {
        bool found = false;
        for (u32 i = 0; i < s->measured_count; ++i) {
            UiPinnedMessagesRow row{};
            if (!s->source->resolve(s->source->context, i, &row)) return false;
            if (row.message_id != target.message_id) continue;
            if (target.action == UiPinnedMessagesAction::jump && !row.jump_enabled) return false;
            if (target.action == UiPinnedMessagesAction::unpin && (!row.can_unpin || row.unpin_pending)) return false;
            const float first = s->offsets[i], last = s->offsets[i + 1];
            const float height = ui_rect_height(s->body);
            const float offset = first < s->scroll_dips ? first
                : last > s->scroll_dips + height ? (last - first > height ? first : last - height) : s->scroll_dips;
            s->focused = target;
            if (!ui_pinned_messages_scene_scroll(s, offset)) return false;
            found = true;
            break;
        }
        if (!found) return false;
    }
    for (u32 i = 1; i < s->widgets.count; ++i) {
        const auto handle = UiWidgetHandle{static_cast<u16>(i), s->widgets.generation};
        if (ui_pinned_messages_target_equal(s->targets[i], target) && ui_widget_manager_focus(&s->widgets, handle)) {
            s->focused = target;
            ++s->revision;
            return true;
        }
    }
    return false;
}
bool ui_pinned_messages_scene_move_focus(UiPinnedMessagesScene* s, UiPinnedMessagesFocusMove move) noexcept {
    if (!s || !current(*s) || move > UiPinnedMessagesFocusMove::page_next) return false;
    UiPinnedMessagesTarget candidates[2 * kUiPinnedMessagesPageCapacity + 4]{};
    u32 count = 0;
    candidates[count++] = {UiPinnedMessagesAction::close, s->content->channel_id};
    if (s->content->phase != UiPinnedMessagesPhase::loading && !s->content->loading_more)
        candidates[count++] = {UiPinnedMessagesAction::refresh, s->content->channel_id};
    for (u32 i = 0; i < s->measured_count; ++i) {
        UiPinnedMessagesRow row{};
        if (!s->source->resolve(s->source->context, i, &row)) return false;
        if (row.jump_enabled) candidates[count++] = {UiPinnedMessagesAction::jump, row.channel_id, row.message_id};
        if (row.can_unpin && !row.unpin_pending)
            candidates[count++] = {UiPinnedMessagesAction::unpin, row.channel_id, row.message_id};
    }
    if (s->content->phase == UiPinnedMessagesPhase::failed && !s->content->loading_more)
        candidates[count++] = {UiPinnedMessagesAction::retry, s->content->channel_id};
    else if (s->content->phase == UiPinnedMessagesPhase::ready && !s->content->loading_more) {
        if (s->content->has_previous) candidates[count++] = {UiPinnedMessagesAction::previous, s->content->channel_id};
        if (s->content->has_next) candidates[count++] = {UiPinnedMessagesAction::next, s->content->channel_id};
    }
    if (move == UiPinnedMessagesFocusMove::page_previous || move == UiPinnedMessagesFocusMove::page_next) {
        const float direction = move == UiPinnedMessagesFocusMove::page_previous ? -1.f : 1.f;
        if (!ui_pinned_messages_scene_scroll(s, s->scroll_dips + direction * ui_rect_height(s->body))) return false;
        for (u32 i = 0; i < s->count; ++i)
            if (s->visible[i].jump_enabled)
                return ui_pinned_messages_scene_focus(s, {UiPinnedMessagesAction::jump,
                        s->visible[i].channel_id, s->visible[i].message_id});
        return true;
    }
    u32 index = ~0u;
    const auto selected = ui_pinned_messages_scene_focused_target(*s);
    for (u32 i = 0; i < count; ++i)
        if (ui_pinned_messages_target_equal(candidates[i], selected)) index = i;
    if (move == UiPinnedMessagesFocusMove::first) index = 0;
    else if (move == UiPinnedMessagesFocusMove::last) index = count - 1;
    else if (move == UiPinnedMessagesFocusMove::previous) index = index >= count || index == 0 ? count - 1 : index - 1;
    else index = index >= count || index + 1 == count ? 0 : index + 1;
    return ui_pinned_messages_scene_focus(s, candidates[index]);
}
bool ui_pinned_messages_scene_avatar(const UiPinnedMessagesScene& s, u32 slot, UiRect* rectangle, UiRect* clip) noexcept {
    if (!rectangle || !clip || !current(s) || slot >= s.count) return false;
    const auto r = s.cards[slot];
    *rectangle = {r.left + 12, r.top + 12, r.left + 40, r.top + 40};
    *clip = ui_rect_intersection(*rectangle, s.body);
    return ui_rect_has_area(*clip);
}

bool ui_pinned_messages_scene_spinner_visible(const UiPinnedMessagesScene& s) noexcept {
    if (!current(s)) return false;
    if (!s.measured_count && s.content->phase == UiPinnedMessagesPhase::loading) return true;
    for (u32 i = 0; i < s.count; ++i)
        if (s.visible[i].can_unpin && s.visible[i].unpin_pending &&
            ui_rect_has_area(ui_rect_intersection(s.cards[i], s.body))) return true;
    return false;
}
bool ui_pinned_messages_scene_paint(const UiPinnedMessagesScene& s, const Theme& theme, PaintStream* out, u32) noexcept {
    if (!current(s)) return true;
    if (!out || !ui_surface_shadow_paint_isolated(out, s.panel, theme, UiElevation::popover, 0) ||
        !paint_stream_push_clip(out, s.panel.left, s.panel.top, s.panel.right, s.panel.bottom)) return false;
    bool ok = ui_surface_paint_plate(out, s.panel, theme, UiElevation::popover, 0, nullptr, &theme.surface_high) &&
        paint_stream_push(out, s.header.left, s.header.bottom - 1, s.header.right, s.header.bottom, theme.surface_high) &&
        paint_stream_push(out, s.footer.left, s.footer.top, s.footer.right, s.footer.top + 1, theme.surface_high) &&
        ui_icon_paint(out, UiIcon::pin, {s.panel.left + 12, s.panel.top + 16, s.panel.left + 36, s.panel.top + 40}, theme.text_secondary);
    const UiWidgetHandle header_buttons[]{s.refresh, s.close};
    for (u32 i = 0; ok && i < 2; ++i) {
        const auto r = bounds(s, header_buttons[i]);
        if (hovered(s, header_buttons[i]) || focused(s, header_buttons[i]))
            ok = surface(out, r, theme, theme.surface_high, focused(s, header_buttons[i]) ? theme.tokens.border.focus : theme.surface_high, 6);
        ok = ok && ui_icon_paint(out, i ? UiIcon::close : UiIcon::refresh,
            {r.left + 6, r.top + 6, r.right - 6, r.bottom - 6}, theme.text_secondary);
    }
    if (ok && !paint_stream_push_clip(out, s.body.left, s.body.top, s.body.right, s.body.bottom)) ok = false;
    if (ok) {
        for (u32 i = 0; ok && i < s.count; ++i) {
            const auto r = s.cards[i];
            const bool selected = focused(s, s.rows[i]) || focused(s, s.unpin[i]);
            ok = surface(out, r, theme, hovered(s, s.rows[i]) ? theme.surface_high : theme.surface,
                         selected ? theme.tokens.emphasis.outline : theme.surface_high);
            UiRect avatar{}, clip{};
            if (ok && ui_pinned_messages_scene_avatar(s, i, &avatar, &clip))
                ok = paint_stream_push_rounded(out, avatar.left, avatar.top, avatar.right, avatar.bottom,
                    theme.avatar_placeholder, 14) &&
                    paint_stream_push_rounded(out, avatar.left + 10, avatar.top + 6, avatar.right - 10, avatar.top + 14,
                                              theme.text_secondary, 4) &&
                    paint_stream_push_rounded(out, avatar.left + 6, avatar.top + 16, avatar.right - 6, avatar.bottom - 4,
                                              theme.text_secondary, 6);
            const float action_top = r.bottom - 38;
            ok = ok && paint_stream_push(out, r.left + 12, action_top - 3, r.right - 12, action_top - 2, theme.surface_high) &&
                ui_icon_paint(out, UiIcon::reply, {r.left + 12, action_top + 5, r.left + 30, action_top + 23},
                              s.visible[i].jump_enabled ? theme.tokens.emphasis.outline : theme.interactive);
            if (s.visible[i].can_unpin) {
                const auto unpin = UiRect{r.right - 40, r.top + 8, r.right - 8, r.top + 40};
                if (hovered(s, s.unpin[i]) || focused(s, s.unpin[i]))
                    ok = ok && surface(out, unpin, theme, theme.surface_high, focused(s, s.unpin[i]) ? theme.tokens.border.focus : theme.surface_high, 6);
                ok = ok && (s.visible[i].unpin_pending
                    ? ui_spinner_paint(out, {unpin.left + 8, unpin.top + 8, unpin.right - 8, unpin.bottom - 8}, theme.text_secondary, s.spinner_phase)
                    : ui_icon_paint(out, UiIcon::unpin, {unpin.left + 6, unpin.top + 6, unpin.right - 6, unpin.bottom - 6},
                        hovered(s, s.unpin[i]) ? theme.notification : theme.text_secondary));
            }
        }
        if (!s.measured_count) {
            const auto r = s.body;
            const float x = (r.left + r.right) * .5f, y = r.top + minimum(40.f, ui_rect_height(r) * .2f);
            ok = ok && (s.content->phase == UiPinnedMessagesPhase::loading
                ? ui_spinner_paint(out, {x - 12, y - 12, x + 12, y + 12}, theme.tokens.emphasis.outline, s.spinner_phase)
                : ui_icon_paint(out, s.content->phase == UiPinnedMessagesPhase::failed ? UiIcon::warning : UiIcon::pin,
                    {x - 16, y - 16, x + 16, y + 16}, theme.text_secondary));
        }
        ok = paint_stream_pop_clip(out) && ok;
    }
    const UiWidgetHandle footer_buttons[]{s.retry, s.previous, s.next};
    for (const auto button : footer_buttons)
        if (ok && button.generation)
            ok = surface(out, bounds(s, button), theme, theme.surface_high,
                         focused(s, button) ? theme.tokens.border.focus : theme.interactive, 6);
    if (ok && ui_rect_has_area(s.status_rectangle))
        ok = paint_stream_push(out, s.status_rectangle.left, s.status_rectangle.top,
                               s.status_rectangle.right, s.status_rectangle.bottom, theme.base_lowest);
    if (ok) ok = ui_scrollbar_paint(out, ui_scrollbar_geometry(s.body, s.scroll_dips,
        s.offsets[s.measured_count]), theme);
    return paint_stream_pop_clip(out) && ok;
}
bool ui_pinned_messages_scene_text_output(const UiPinnedMessagesScene& s, const Theme& theme, UiTextOutput* out) noexcept {
    if (!current(s)) return true;
    if (!ui_text_output_valid(out)) return false;
    const auto p = s.panel;
    if (!text(out, s, s.root, utf8_literal("Pins"), {p.left + 44, p.top + 10, p.right - 84, p.top + 36},
              theme.text_primary, text_style_bold) ||
        !text(out, s, s.root, s.content->channel, {p.left + 16, p.top + 38, p.right - 16, p.top + 58},
              theme.text_secondary, text_style_subtext)) return false;
    for (u32 i = 0; i < s.count; ++i) {
        const auto& row = s.visible[i];
        const auto r = s.cards[i];
        const float body_bottom = r.top + 56 + excerpt_height(row);
        if (!text(out, s, s.rows[i], row.author.bytes ? row.author : utf8_literal("Unknown author"),
                  {r.left + 50, r.top + 8, r.right - (row.can_unpin ? 44 : 12), r.top + 30},
                  theme.text_primary, text_style_bold, false, true) ||
            !text(out, s, s.rows[i], row.timestamp, {r.left + 50, r.top + 30, r.right - 12, r.top + 48},
                  theme.text_secondary, text_style_subtext, false, true) ||
            !text(out, s, s.rows[i], row.body.bytes ? row.body : utf8_literal("Message has no text"),
                  {r.left + 12, r.top + 56, r.right - 12, body_bottom}, theme.text_primary, 0, true, true) ||
            !text(out, s, s.rows[i], row.detail, {r.left + 12, body_bottom + 2, r.right - 12, body_bottom + 22},
                  theme.text_secondary, text_style_subtext, false, true) ||
            !text(out, s, s.rows[i], utf8_literal("Jump to message"),
                  {r.left + 36, r.bottom - 33, r.right - 12, r.bottom - 9},
                  row.jump_enabled ? theme.tokens.emphasis.outline : theme.interactive,
                  text_style_subtext | text_style_bold, false, true)) return false;
    }
    if (!s.measured_count) {
        const auto r = s.body;
        const float top = r.top + minimum(68.f, ui_rect_height(r) * .35f);
        const auto title = s.content->phase == UiPinnedMessagesPhase::loading ? utf8_literal("Loading pins…")
            : s.content->phase == UiPinnedMessagesPhase::failed ? utf8_literal("Couldn't load pins")
            : utf8_literal("No pinned messages");
        const auto detail = s.content->status.bytes ? s.content->status
            : s.content->phase == UiPinnedMessagesPhase::failed
                ? Utf8View{}
                : utf8_literal("Pinned messages will appear here.");
        if (!text(out, s, s.root, title, {r.left + 20, top, r.right - 20, top + 28}, theme.text_primary,
                  text_style_bold, false, true, UiTextAlignment::center) ||
            (s.content->phase != UiPinnedMessagesPhase::loading &&
             !text(out, s, s.root, detail, {r.left + 24, top + 36, r.right - 24, top + 108},
                   theme.text_secondary, 0, true, true, UiTextAlignment::center))) return false;
    }
    if (ui_rect_has_area(s.status_rectangle) && !text(out, s, s.root, s.content->status.bytes ? s.content->status
        : utf8_literal("Couldn't update this pin. Try again."),
        {s.status_rectangle.left + 12, s.status_rectangle.top + 4, s.status_rectangle.right - 12,
         s.status_rectangle.bottom - 4}, theme.notification, text_style_subtext)) return false;
    if (s.retry.generation)
        return text(out, s, s.retry, utf8_literal("Retry"), bounds(s, s.retry), theme.text_primary,
                    text_style_bold, false, false, UiTextAlignment::center);
    if (s.previous.generation || s.next.generation) {
        const bool ready = s.content->phase == UiPinnedMessagesPhase::ready && !s.content->loading_more;
        const Color previous_color = s.content->has_previous && ready ? theme.text_primary : theme.interactive;
        const Color next_color = s.content->has_next && ready ? theme.text_primary : theme.interactive;
        return text(out, s, s.previous, utf8_literal("Newer"), bounds(s, s.previous), previous_color,
                    text_style_bold, false, false, UiTextAlignment::center) &&
            text(out, s, s.next, utf8_literal("Older"), bounds(s, s.next), next_color,
                 text_style_bold, false, false, UiTextAlignment::center) &&
            text(out, s, s.root, s.content->loading_more ? utf8_literal("Loading…") : Utf8View{s.page_text, s.page_bytes},
                 {bounds(s, s.previous).right + 4, s.footer.top + 10,
                  bounds(s, s.next).left - 4, s.footer.bottom - 10}, theme.text_secondary,
                 text_style_subtext, false, false, UiTextAlignment::center);
    }
    if (s.content->phase == UiPinnedMessagesPhase::loading && !s.measured_count) return true;
    return text(out, s, s.root, s.content->status.bytes ? s.content->status : utf8_literal("Pinned in this channel"),
        {s.footer.left + 16, s.footer.top + 10, s.footer.right - 16, s.footer.bottom - 10},
        s.content->mutation_failed ? theme.notification : theme.text_secondary, text_style_subtext);
}
} // namespace maicord
