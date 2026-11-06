#include "maicord/ui/chat/messages/forward_scene.hpp"
#include "maicord/ui/layout/rect_edges.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/theme/foreground_color.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/guild/icon_media.hpp"
#include "maicord/ui/media/avatar_media.hpp"

namespace maicord {
namespace {
bool current(const UiMessageForwardScene& s) noexcept {
    return ui_row_scene_current(s);
}
// The 32-DIP destination tile leading each row; identity images cover it.
constexpr float kDestinationTileRadius = 8.f;
// The close button's hover plate: dialog material, no focus outline.
constexpr UiLook kClose{.fill = UI_TOKEN(surface_high), .focus = UI_TOKEN(surface_high), .radius = 6,
                        .flags = ui_look_hot_only | ui_look_dialog};
UiRect destination_tile(const UiRect& row) noexcept {
    return ui_rect_edges(row, ui_start(10), ui_start(10), ui_start(42), ui_start(42));
}
bool view_valid(UiEditView view, u32 maximum_bytes) noexcept {
    if (view.text.bytes > maximum_bytes || view.anchor > view.text.bytes || view.caret > view.text.bytes ||
        !utf8_validate(view.text).valid) return false;
    const auto boundary = [&](u32 offset) noexcept {
        return !offset || offset == view.text.bytes || (static_cast<u8>(view.text.data[offset]) & 0xc0u) != 0x80u;
    };
    return boundary(view.anchor) && boundary(view.caret);
}
bool row_valid(const UiMessageForwardRow& r) noexcept {
    return r.key && r.channel_id && (r.kind == UiQuickSwitcherKind::text_channel ||
        r.kind == UiQuickSwitcherKind::private_channel || r.kind == UiQuickSwitcherKind::group_private) &&
        r.label.bytes && r.label.bytes <= 256 && r.detail.bytes <= 512 && utf8_validate(r.label).valid &&
        utf8_validate(r.detail).valid;
}
const UiMessageForwardSelection* find_selection(const UiMessageForwardScene& s, u64 channel_id) noexcept {
    for (u32 i = 0; i < s.content->selected_count; ++i)
        if (s.content->selected[i].channel_id == channel_id) return &s.content->selected[i];
    return nullptr;
}
bool has_failed(const UiMessageForwardScene& s) noexcept {
    for (u32 i = 0; i < s.content->selected_count; ++i)
        if (s.content->selected[i].state == UiMessageForwardState::failed) return true;
    return false;
}
UiMessageForwardAction footer_action(const UiMessageForwardScene& s) noexcept {
    if (s.content->sending) return UiMessageForwardAction::none;
    if (s.content->committed) return has_failed(s) ? UiMessageForwardAction::retry : UiMessageForwardAction::close;
    if (!s.content->can_send) return UiMessageForwardAction::none;
    for (u32 i = 0; i < s.content->selected_count; ++i)
        if (s.content->selected[i].state == UiMessageForwardState::ready) return UiMessageForwardAction::send;
    return UiMessageForwardAction::none;
}
bool toggle_enabled(const UiMessageForwardScene& s, const UiMessageForwardRow& r) noexcept {
    const auto* picked = find_selection(s, r.channel_id);
    return !s.content->committed && s.content->edit.active && !s.content->sending && r.enabled && (picked ? picked->state == UiMessageForwardState::ready :
        s.content->selected_count < kUiMessageForwardMaximumTargets);
}
UiMessageForwardTarget row_target(const UiMessageForwardScene& s, u32 index) noexcept {
    if (index >= s.accepted_count) return {};
    const auto& r = s.accepted[index];
    const auto* picked = find_selection(s, r.channel_id);
    if (picked && !s.content->committed && !s.content->sending &&
        picked->state != UiMessageForwardState::sent && picked->state != UiMessageForwardState::sending)
        return {UiMessageForwardAction::toggle, r.channel_id, index, s.source_revision};
    if (picked && s.content->committed && picked->state == UiMessageForwardState::failed && r.enabled && !s.content->sending)
        return {UiMessageForwardAction::retry, r.channel_id, index, s.source_revision};
    return toggle_enabled(s, r) ? UiMessageForwardTarget{UiMessageForwardAction::toggle, r.channel_id, index,
        s.source_revision} : UiMessageForwardTarget{};
}
bool initialize(UiMessageForwardScene& s) noexcept {
    if (s.storage.arena.base) return true;
    if (!ui_scene_open(&s.storage, &s.widgets, kUiMessageForwardWidgetCapacity,
                       sizeof(UiMessageForwardRow) * kUiMessageForwardMaximumRows, alignof(UiMessageForwardRow)))
        return false;
    s.accepted = ui_fixed_widget_storage_sidecar<UiMessageForwardRow>(s.storage);
    s.root = ui_scene_part_root(&s.widgets, utf8_literal("Forward message"));
    if (!s.root.generation) return false;
    const auto add = [&](UiWidgetHandle parent, UiWidgetKind kind, Utf8View name) noexcept {
        return ui_scene_part(&s.widgets, parent, kind, name);
    };
    s.edit = add(s.root, UiWidgetKind::text_field, utf8_literal("Find destinations"));
    s.caption = add(s.root, UiWidgetKind::text_field, utf8_literal("Optional forward caption"));
    s.close = add(s.root, UiWidgetKind::button, utf8_literal("Close forwarding"));
    s.send = add(s.root, UiWidgetKind::button, utf8_literal("Send selected forwards"));
    s.cancel = add(s.root, UiWidgetKind::button, utf8_literal("Cancel forwarding"));
    s.body_owner = add(s.root, UiWidgetKind::container, utf8_literal("Forward destinations"));
    if (!s.edit.generation || !s.caption.generation || !s.close.generation || !s.send.generation ||
        !s.cancel.generation || !s.body_owner.generation) return false;
    for (auto& handle : s.rows)
        if (!(handle = add(s.body_owner, UiWidgetKind::button, utf8_literal("Forward destination"))).generation)
            return false;
    for (auto& handle : s.targets)
        if (!(handle = add(s.root, UiWidgetKind::button, utf8_literal("Selected forward destination"))).generation)
            return false;
    return true;
}
bool rectangle(UiMessageForwardScene& s, UiWidgetHandle handle, const UiRect& r, const UiRect& parent,
    bool visible, bool enabled, bool focusable = true) noexcept {
    return ui_widget_manager_place_part(&s.widgets, handle, r, parent, visible, enabled, focusable);
}
bool layout(UiMessageForwardScene& s, bool force = false) noexcept {
    bool dirty = force;
    dirty |= rectangle(s, s.edit, s.field, s.panel, true, !s.content->committed && s.content->edit.active && !s.content->sending);
    dirty |= rectangle(s, s.caption, s.caption_field, s.panel, true, !s.content->committed && s.content->caption.active && !s.content->sending);
    dirty |= rectangle(s, s.close, ui_rect_edges(s.panel, ui_end(-44), ui_start(8), ui_end(-12), ui_start(40)),
        s.panel, true, true);
    dirty |= rectangle(s, s.send, s.send_rectangle, s.panel, true, footer_action(s) != UiMessageForwardAction::none);
    dirty |= rectangle(s, s.cancel, s.cancel_rectangle, s.panel, true, true);
    dirty |= rectangle(s, s.body_owner, s.body, s.panel, true, false, false);
    const u32 first = static_cast<u32>(s.scroll_dips / kUiMessageForwardRowHeight);
    if (s.first != first) ui_widget_manager_release_pointer(&s.widgets);
    s.first = first; s.count = 0;
    for (u32 i = 0; i < kUiMessageForwardVisibleRows; ++i) {
        const u32 index = first + i;
        UiRect r{};
        bool visible = index < s.accepted_count;
        if (visible) {
            const float top = s.body.top + index * kUiMessageForwardRowHeight - s.scroll_dips;
            r = {s.body.left + 2, top, s.body.right - 14, top + kUiMessageForwardRowHeight};
            visible = ui_rect_intersects(r, s.body);
        }
        if (visible) { s.visible[i] = s.accepted[index]; s.row_rectangles[i] = r; ++s.count; }
        else { reset_value(&s.visible[i]); reset_value(&s.row_rectangles[i]); }
        dirty |= rectangle(s, s.rows[i], visible ? ui_rect_intersection(r, s.body) : UiRect{}, s.body,
            visible, visible && row_target(s, index).action != UiMessageForwardAction::none, false);
    }
    const u32 columns = ui_rect_width(s.receipts) >= 430 ? 3 : 2;
    const float width = (ui_rect_width(s.receipts) - (columns - 1) * 6.f) / columns;
    for (u32 i = 0; i < kUiMessageForwardMaximumTargets; ++i) {
        const bool visible = s.receipts_visible && i < s.content->selected_count;
        UiRect r{};
        if (visible) {
            const float left = s.receipts.left + (i % columns) * (width + 6);
            const float top = s.receipts.top + (i / columns) * 32.f;
            r = {left, top, left + width, top + 28};
        }
        s.target_rectangles[i] = r;
        const bool enabled = visible && !s.content->sending &&
            ((!s.content->committed && s.content->selected[i].state != UiMessageForwardState::sent &&
                s.content->selected[i].state != UiMessageForwardState::sending) ||
             (s.content->committed && s.content->selected[i].state == UiMessageForwardState::failed));
        dirty |= rectangle(s, s.targets[i], r, s.panel, visible, enabled);
    }
    if (dirty && !ui_widget_manager_layout_rectangle(&s.widgets, s.panel).ok()) return false;
    ui_widget_manager_reconcile_input(&s.widgets);
    return true;
}
Color tint(Color value, float alpha) noexcept { value.alpha = alpha; return value; }
UiEditView presentation_view(UiEditView view, bool sending) noexcept {
    if (!view.active || sending) { view.focused = false; view.caret_visible = false; }
    view.active = true;
    return view;
}
UiIcon destination_icon(UiQuickSwitcherKind kind) noexcept {
    return kind == UiQuickSwitcherKind::text_channel ? UiIcon::channel :
        kind == UiQuickSwitcherKind::group_private ? UiIcon::members : UiIcon::direct_message;
}
void labels(UiMessageForwardScene& s) noexcept {
    u32 send_count = 0;
    for (u32 i = 0; i < s.content->selected_count; ++i)
        if (s.content->selected[i].state == UiMessageForwardState::ready) ++send_count;
    const auto write = [](char* to, const char* from) noexcept { u8 n = 0; while (*from) to[n++] = *from++; return n; };
    s.send_label_bytes = write(s.send_label, s.content->sending ? "Sending…" :
        s.content->committed ? has_failed(s) ? "Retry failed" : "Done" : "Send (0)");
    if (!s.content->sending && !s.content->committed) s.send_label[6] = static_cast<char>('0' + send_count);
    s.selection_label_bytes = write(s.selection_label, "0 of 5 selected");
    s.selection_label[0] = static_cast<char>('0' + s.content->selected_count);
}
} // namespace

bool ui_message_forward_scene_current(const UiMessageForwardScene& s) noexcept { return current(s); }
float ui_message_forward_scene_scroll_maximum(const UiMessageForwardScene& s) noexcept {
    return maximum(0.f, s.accepted_count * kUiMessageForwardRowHeight - ui_rect_height(s.body));
}
bool ui_message_forward_scene_update(UiMessageForwardScene* s, const UiMessageForwardSource* source,
    const UiMessageForwardContent* content, UiRect viewport) noexcept {
    if (!s || !content || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 240 || ui_rect_height(viewport) < 300) return false;
    if (current(*s) && s->source == source && s->content == content && ui_rect_equal(s->viewport, viewport)) return true;
    if (content->phase > UiMessageForwardPhase::unavailable || content->selected_count > kUiMessageForwardMaximumTargets ||
        !view_valid(content->edit, 512) || !view_valid(content->caption, 8000) ||
        content->author.bytes > 256 || content->preview.bytes > 2048 || content->media_summary.bytes > 256 ||
        content->status.bytes > 512 || !utf8_validate(content->author).valid || !utf8_validate(content->preview).valid ||
        !utf8_validate(content->media_summary).valid || !utf8_validate(content->status).valid ||
        (source && (source->count > kUiMessageForwardMaximumRows || !source->resolve ||
        (source->current && !source->current(source->context))))) return false;
    for (u32 i = 0; i < content->selected_count; ++i) {
        const auto& r = content->selected[i];
        if (!r.channel_id || r.state > UiMessageForwardState::cancelled || !r.label.bytes || r.label.bytes > 256 ||
            r.detail.bytes > 512 || !utf8_validate(r.label).valid || !utf8_validate(r.detail).valid) return false;
        for (u32 j = 0; j < i; ++j) if (content->selected[j].channel_id == r.channel_id) return false;
    }
    const bool republished = !s->active || s->source != source || s->source_revision != (source ? source->revision : 0) ||
        s->accepted_count != (source ? source->count : 0);
    const u32 count = source ? source->count : 0;
    const u32 source_revision = source ? source->revision : 0;
    const u64 selected_id = s->active && s->selected < s->accepted_count ? s->accepted[s->selected].channel_id : 0;
    if (republished) {
        // Rows resolve once, straight into the scene's bank (only ids are kept on the side), so a
        // rejected source retires the scene instead of keeping a half-replaced bank.
        if (!initialize(*s)) return false;
        u64 ids[kUiMessageForwardMaximumRows];
        u32 selected = ~0u;
        for (u32 i = 0; i < count; ++i) {
            auto& row = s->accepted[i];
            bool ok = source->resolve(source->context, i, &row) && row_valid(row);
            for (u32 j = 0; ok && j < i; ++j) ok = ids[j] != row.channel_id;
            if (!ok) { s->active = false; return false; }
            ids[i] = row.channel_id;
            if (row.enabled && (selected == ~0u || row.channel_id == selected_id)) selected = i;
        }
        if (source && (source->revision != source_revision || source->count != count ||
            (source->current && !source->current(source->context)))) { s->active = false; return false; }
        ui_widget_manager_release_pointer(&s->widgets);
        s->accepted_count = count; s->selected = selected; s->scroll_dips = 0;
    } else if (!initialize(*s)) return false;
    const auto old_panel = s->panel;
    // Cancel pressed state/buttons when completion changes their semantic action.
    if (s->content_revision != content->revision) ui_widget_manager_release_pointer(&s->widgets);
    s->source = source; s->content = content; s->viewport = viewport;
    s->source_revision = source ? source->revision : 0; s->content_revision = content->revision;
    const float width = minimum(620.f, ui_rect_width(viewport) - (ui_rect_width(viewport) < 400 ? 16.f : 48.f));
    const float height = minimum(700.f, ui_rect_height(viewport) - 24.f);
    const float left = viewport.left + (ui_rect_width(viewport) - width) * .5f;
    const float top = viewport.top + (ui_rect_height(viewport) - height) * .5f;
    s->panel = {left, top, left + width, top + height}; s->compact = height < 520;
    const float inset = width < 360 ? 12.f : 20.f;
    const float right = s->panel.right - inset;
    const bool short_window = height < 420;
    const float field_height = short_window ? 36.f : 40.f;
    float y = top + (short_window ? 44.f : 48.f);
    const float preview_height = short_window ? 0.f : s->compact ? 32.f : 96.f;
    s->preview = {left + inset, y, right, y + preview_height}; y += preview_height + (short_window ? 4.f : 12.f);
    s->field = {left + inset, y, right, y + field_height};
    s->text = {s->field.left + 36, y + 6, right - 10, y + field_height - 6}; y += field_height + 4;
    s->hint = {left + inset, y, right, y + 20}; y += 24;
    s->footer = {left + inset, s->panel.bottom - (short_window ? 44.f : 52.f), right,
        s->panel.bottom - (short_window ? 8.f : 12.f)};
    const float send_width = content->committed && has_failed(*s) && width >= 320 ? 144.f : 118.f;
    s->send_rectangle = {right - send_width, s->footer.top, right, s->footer.bottom};
    const float cancel_right = s->send_rectangle.left - 8;
    s->cancel_rectangle = {maximum(left + inset, cancel_right - (width >= 400 ? 126.f : 86.f)),
        s->footer.top, cancel_right, s->footer.bottom};
    s->caption_field = {left + inset, s->footer.top - field_height - 10, right, s->footer.top - 10};
    s->caption_text = {s->caption_field.left + 12, s->caption_field.top + 6, right - 12, s->caption_field.bottom - 6};
    const u32 columns = width - inset * 2 >= 430 ? 3 : 2;
    const u32 receipt_rows = (content->selected_count + columns - 1) / columns;
    const float receipt_height = receipt_rows * 32.f;
    // Keep selected destinations reachable whenever the remaining body can
    // retain one useful result. This also allows shorter high-DPI windows.
    s->receipts_visible = content->selected_count &&
        s->caption_field.top - receipt_height - 16.f - y >= 34.f;
    const float receipts_top = s->caption_field.top - (s->receipts_visible ? receipt_height + 8 : 0);
    s->receipts = {left + inset, receipts_top, right, s->caption_field.top - 8};
    s->body = {left + inset - 2, y, right + 2, receipts_top - 8};
    // Very short windows preserve both editable fields and a useful destination row.
    if (ui_rect_height(s->body) < 34) {
        const float reclaim = 34 - ui_rect_height(s->body);
        s->preview.bottom -= minimum(reclaim, ui_rect_height(s->preview));
        const float shift = preview_height - ui_rect_height(s->preview);
        s->field.top -= shift; s->field.bottom -= shift; s->text.top -= shift; s->text.bottom -= shift;
        s->hint.top -= shift; s->hint.bottom -= shift; s->body.top -= shift;
    }
    if (!ui_rect_has_area(s->body)) { s->active = false; return false; }
    s->scroll_dips = minimum(s->scroll_dips, ui_message_forward_scene_scroll_maximum(*s));
    if ((republished || !ui_rect_equal(old_panel, s->panel)) && s->selected < s->accepted_count) {
        const float row_top = s->selected * kUiMessageForwardRowHeight;
        const float row_bottom = row_top + kUiMessageForwardRowHeight;
        if (row_top < s->scroll_dips) s->scroll_dips = row_top;
        else if (row_bottom > s->scroll_dips + ui_rect_height(s->body))
            s->scroll_dips = minimum(ui_message_forward_scene_scroll_maximum(*s),
                maximum(0.f, row_bottom - ui_rect_height(s->body)));
    }
    s->active = true; labels(*s);
    if (!layout(*s, !ui_rect_equal(old_panel, s->panel))) { s->active = false; return false; }
    if (content->edit.focused && !content->committed && !content->sending) ui_widget_manager_focus(&s->widgets, s->edit);
    else if (content->caption.focused && !content->committed && !content->sending) ui_widget_manager_focus(&s->widgets, s->caption);
    ++s->revision; return true;
}
void ui_message_forward_scene_release(UiMessageForwardScene* s) noexcept { ui_scene_release(s); }
UiMessageForwardTarget ui_message_forward_scene_target(const UiMessageForwardScene& s, UiWidgetHandle handle) noexcept {
    if (!current(s)) return {};
    if (ui_widget_handle_matches(handle, s.close)) return {UiMessageForwardAction::close};
    if (ui_widget_handle_matches(handle, s.cancel)) return {s.content->sending || s.content->committed ?
        UiMessageForwardAction::cancel_pending : UiMessageForwardAction::close};
    if (ui_widget_handle_matches(handle, s.send)) return {footer_action(s)};
    if (ui_widget_handle_matches(handle, s.edit)) return !s.content->committed && s.content->edit.active && !s.content->sending ? UiMessageForwardTarget{UiMessageForwardAction::edit} : UiMessageForwardTarget{};
    if (ui_widget_handle_matches(handle, s.caption)) return !s.content->committed && s.content->caption.active && !s.content->sending ? UiMessageForwardTarget{UiMessageForwardAction::caption} : UiMessageForwardTarget{};
    for (u32 i = 0; i < s.count; ++i) if (ui_widget_handle_matches(handle, s.rows[i])) return row_target(s, s.first + i);
    for (u32 i = 0; i < s.content->selected_count; ++i) if (ui_widget_handle_matches(handle, s.targets[i]) && s.receipts_visible && !s.content->sending) {
        const auto& r = s.content->selected[i];
        if (r.state == UiMessageForwardState::failed && s.content->committed)
            return {UiMessageForwardAction::retry, r.channel_id, i, s.source_revision};
        if (r.state != UiMessageForwardState::sent && r.state != UiMessageForwardState::sending && !s.content->committed)
            return {UiMessageForwardAction::remove, r.channel_id, i, s.source_revision};
    }
    return {};
}
UiMessageForwardTarget ui_message_forward_scene_hit_test(const UiMessageForwardScene& s, float x, float y) noexcept {
    if (!current(s) || !ui_rect_contains(s.viewport, x, y)) return {};
    if (!ui_rect_contains(s.panel, x, y)) return {UiMessageForwardAction::close};
    return ui_message_forward_scene_target(s, ui_widget_manager_hit_test(s.widgets, x, y));
}
UiMessageForwardTarget ui_message_forward_scene_selected(const UiMessageForwardScene& s) noexcept {
    return current(s) ? row_target(s, s.selected) : UiMessageForwardTarget{};
}
bool ui_message_forward_scene_scroll(UiMessageForwardScene* s, float offset) noexcept {
    if (!s || !current(*s) || !is_finite(offset)) return false;
    offset = minimum(maximum(0.f, offset), ui_message_forward_scene_scroll_maximum(*s));
    if (offset == s->scroll_dips) return true;
    ui_widget_manager_release_pointer(&s->widgets); s->scroll_dips = offset;
    if (!layout(*s)) return false;
    ++s->revision; return true;
}
// Callers have checked the scene is current.
static bool select_row(UiMessageForwardScene* s, u32 index, bool reveal = true) noexcept {
    if (index >= s->accepted_count || !s->accepted[index].enabled) return false;
    const auto previous = s->selected; s->selected = index;
    if (reveal) {
        const float top = index * kUiMessageForwardRowHeight, bottom = top + kUiMessageForwardRowHeight;
        const float offset = top < s->scroll_dips ? top : bottom > s->scroll_dips + ui_rect_height(s->body) ?
            bottom - ui_rect_height(s->body) : s->scroll_dips;
        if (!ui_message_forward_scene_scroll(s, offset)) return false;
    }
    if (previous != index) ++s->revision;
    return true;
}
bool ui_message_forward_scene_move(UiMessageForwardScene* s, UiQuickSwitcherMove move) noexcept {
    if (!s || !current(*s) || !s->accepted_count || move > UiQuickSwitcherMove::page_previous) return false;
    const bool reverse = move == UiQuickSwitcherMove::previous || move == UiQuickSwitcherMove::page_previous || move == UiQuickSwitcherMove::last;
    const u32 step = move == UiQuickSwitcherMove::page_next || move == UiQuickSwitcherMove::page_previous ?
        maximum(1u, static_cast<u32>(ui_rect_height(s->body) / kUiMessageForwardRowHeight)) : 1;
    u32 index = s->selected;
    if (move == UiQuickSwitcherMove::first || index >= s->accepted_count) index = 0;
    else if (move == UiQuickSwitcherMove::last) index = s->accepted_count - 1;
    else if (reverse) index = step > index ? 0 : index - step;
    else index = minimum(s->accepted_count - 1, index + step);
    for (u32 i = 0; i < s->accepted_count; ++i) {
        if (s->accepted[index].enabled) return select_row(s, index);
        if (reverse) { if (!index) break; --index; }
        else { if (index + 1 == s->accepted_count) break; ++index; }
    }
    return false;
}
bool ui_message_forward_scene_set_hover(UiMessageForwardScene* s, float x, float y, bool* changed) noexcept {
    if (changed) *changed = false;
    if (!s || !current(*s)) return false;
    const auto selected = s->selected;
    bool delta = false;
    if (!ui_widget_manager_hover_at(&s->widgets, x, y, &delta)) return false;
    for (u32 i = 0; i < s->count; ++i) if (ui_widget_handle_matches(s->widgets.hovered, s->rows[i])) select_row(s, s->first + i, false);
    if (delta) ++s->revision;
    if (changed) *changed = delta || selected != s->selected;
    return true;
}

bool ui_message_forward_scene_media(const UiMessageForwardScene& s, u32 slot, const ChatStateStore* state,
                                    UiMessageForwardMedia* output) noexcept {
    if (!output || !current(s) || slot >= s.count || slot >= kUiMessageForwardVisibleRows)
        return false;
    const auto& row = s.visible[slot];
    MediaAssetKey key{};
    Utf8View hash{};
    bool circle = false;
    if (row.kind == UiQuickSwitcherKind::private_channel && row.user_id) {
        if (!avatar_media_key(row.user_id, row.avatar_hash, &key))
            return false;
        hash = row.avatar_hash;
        circle = true;
    } else if ((row.kind == UiQuickSwitcherKind::text_channel || row.kind == UiQuickSwitcherKind::voice_channel) &&
               row.guild_id && state) {
        if (!guild_icon_media_key(*state, row.guild_id, &key, &hash))
            return false;
    } else
        return false;
    const UiRect rectangle = destination_tile(s.row_rectangles[slot]);
    const UiRect clip = ui_rect_overlap(rectangle, s.body);
    if (!ui_rect_has_area(clip))
        return false;
    *output = {key, hash, rectangle, clip, circle ? 0.f : kDestinationTileRadius, circle};
    return true;
}

bool ui_message_forward_scene_paint(const UiMessageForwardScene& s, const Theme& t, PaintStream* out,
    const UiEditGeometry* query, const UiEditGeometry* caption, u32 dpi) noexcept {
    if (!current(s)) return true;
    UiPaint pen{out, t};
    const auto p = s.panel;
    const auto accent = ui_foreground_color(t, t.tokens.emphasis.outline);
    const auto error = ui_foreground_color(t, t.notification);
    const auto sent = ui_foreground_color(t, t.syntax_string);
    pen.dialog(s.viewport, p).icon(UiIcon::forwarded, ui_rect_edges(p, ui_start(20), ui_start(16), ui_start(40), ui_start(36)), accent);
    if (ui_rect_has_area(s.preview)) pen.surface(s.preview, t.base_lowest, t.surface_high, 8, true);
    if (!s.compact) pen.fill(ui_rect_edges(s.preview, ui_start(12), ui_start(12), ui_start(15), ui_end(-12)), accent);
    const auto query_view = presentation_view(s.content->edit, s.content->sending || s.content->committed);
    const auto caption_view = presentation_view(s.content->caption, s.content->sending || s.content->committed);
    if (pen && !ui_edit_field_paint(out, s.field, s.text, query_view, query, t.base_lowest, t, dpi, 8)) pen.fail();
    pen.icon(UiIcon::search, ui_rect_edges(s.field, ui_start(10), ui_start(10), ui_start(30), ui_start(30)), t.text_secondary);
    if (pen && !ui_edit_field_paint(out, s.caption_field, s.caption_text, caption_view, caption, t.base_lowest, t, dpi, 8))
        pen.fail();
    pen.control<kClose>(s.widgets, s.close)
        .icon(UiIcon::close, ui_rect_edges(p, ui_end(-38), ui_start(14), ui_end(-18), ui_start(34)), t.text_secondary)
        .clip(s.body);
    for (u32 i = 0; pen && i < s.count; ++i) {
        const auto r = s.row_rectangles[i]; const auto* picked = find_selection(s, s.visible[i].channel_id);
        const bool selected = !s.content->committed && s.first + i == s.selected;
        if (selected || picked || ui_hovered(s, s.rows[i]))
            pen.surface(ui_rect_edges(r, ui_start(0), ui_start(2), ui_end(0), ui_end(-2)), picked ? t.surface_high : t.base_low,
                        selected ? accent : t.surface_high, 7, true);
        const UiRect avatar = destination_tile(r);
        pen.surface(avatar, t.base_lowest, t.base_lowest, kDestinationTileRadius, true)
            .icon(destination_icon(s.visible[i].kind), ui_rect_shrink(avatar, 6), t.text_secondary);
        const UiRect box = ui_rect_edges(r, ui_end(-30), ui_start(16), ui_end(-10), ui_start(36));
        if (picked && picked->state == UiMessageForwardState::failed)
            pen.icon(UiIcon::retry, box, error);
        else if (picked && picked->state == UiMessageForwardState::cancelled)
            pen.icon(UiIcon::close, box, t.text_secondary);
        else if (picked && picked->state == UiMessageForwardState::sending)
            pen.icon(UiIcon::sending, box, accent);
        else {
            pen.surface(box, picked ? t.tokens.emphasis.fill : t.base_lowest, picked ? t.tokens.emphasis.fill : t.interactive, 5,
                        true);
            if (picked) pen.icon(UiIcon::check, ui_rect_shrink(box, 2), ui_surface_foreground(t, t.tokens.emphasis.fill));
        }
    }
    pen.unclip().scrollbar(ui_scrollbar_geometry(s.body, s.scroll_dips, s.accepted_count * kUiMessageForwardRowHeight));
    for (u32 i = 0; pen && s.receipts_visible && i < s.content->selected_count; ++i) {
        const auto r = s.target_rectangles[i]; const auto state = s.content->selected[i].state;
        const Color color = state == UiMessageForwardState::cancelled ? t.text_secondary : state == UiMessageForwardState::failed ? error :
            state == UiMessageForwardState::sent ? sent : accent;
        pen.surface(r, t.base_low, tint(color, .55f), 6, true)
            .icon(state == UiMessageForwardState::sent ? UiIcon::check :
                  state == UiMessageForwardState::failed ? UiIcon::retry :
                  state == UiMessageForwardState::sending ? UiIcon::sending : UiIcon::close,
                  ui_rect_edges(r, ui_end(-22), ui_start(6), ui_end(-6), ui_end(-6)), color);
    }
    const bool enabled = footer_action(s) != UiMessageForwardAction::none;
    return pen.surface(s.cancel_rectangle, ui_hovered(s, s.cancel) ? t.surface_high : t.base_low,
                       ui_focused(s, s.cancel) ? accent : t.interactive, 7, true)
        .surface(s.send_rectangle, enabled ? t.tokens.emphasis.fill : t.surface_high,
                 ui_focused(s, s.send) ? t.text_primary : enabled ? t.tokens.emphasis.fill : t.surface_high, 7, true)
        .icon(s.content->committed ? has_failed(s) ? UiIcon::retry : UiIcon::check : UiIcon::forwarded,
              ui_rect_edges(s.send_rectangle, ui_start(12), ui_start(11), ui_start(30), ui_start(29)),
              enabled ? ui_surface_foreground(t, t.tokens.emphasis.fill) : t.text_secondary)
        .end();
}
bool ui_message_forward_scene_text_output(const UiMessageForwardScene& s, const Theme& t, UiTextOutput* out,
    const UiEditGeometry* query, const UiEditGeometry* caption) noexcept {
    if (!current(s)) return true;
    UiTextPen<UiTextClip::label> text{out, s.widgets, s.panel, s.body};
    const auto p = s.panel;
    const auto accent = ui_foreground_color(t, t.tokens.emphasis.outline);
    const auto error = ui_foreground_color(t, t.notification);
    text(s.root, utf8_literal("Forward message"), ui_rect_edges(p, ui_start(48), ui_start(10), ui_end(-48), ui_start(40)),
         t.text_primary, text_style_bold);
    const auto r = s.preview;
    if (s.compact)
        text.in(r)(s.root, utf8_literal("Forwarded"), ui_rect_edges(r, ui_start(12), ui_start(0), ui_start(94), ui_end(0)),
                   t.text_secondary, text_style_subtext)
            (s.root, s.content->preview.bytes ? s.content->preview : s.content->media_summary,
             ui_rect_edges(r, ui_start(98), ui_start(0), ui_end(-12), ui_end(0)), t.text_primary, text_style_subtext);
    else
        text.in(r)(s.root, utf8_literal("Forwarded"), ui_rect_edges(r, ui_start(26), ui_start(7), ui_start(108), ui_start(27)),
                   t.text_secondary, text_style_subtext)
            (s.root, s.content->author, ui_rect_edges(r, ui_start(112), ui_start(7), ui_end(-12), ui_start(27)),
             t.text_primary, text_style_bold | text_style_subtext)
            (s.root, s.content->preview.bytes ? s.content->preview : utf8_literal("Media message"),
             ui_rect_edges(r, ui_start(26), ui_start(29), ui_end(-12), ui_start(63)), t.text_primary, 0,
             ui_text_wrap | ui_text_middle)
            (s.root, s.content->media_summary, ui_rect_edges(r, ui_start(26), ui_start(65), ui_end(-12), ui_end(-7)),
             t.text_secondary, text_style_subtext);
    const auto query_view = presentation_view(s.content->edit, s.content->sending || s.content->committed);
    const auto caption_view = presentation_view(s.content->caption, s.content->sending || s.content->committed);
    if (text.ok() && (!ui_edit_field_text_output(out, {&s.widgets, s.edit}, s.text, query_view, query,
                                                 utf8_literal("Find a channel or conversation"), t) ||
                      !ui_edit_field_text_output(out, {&s.widgets, s.caption}, s.caption_text, caption_view, caption,
                                                 utf8_literal("Add a message (optional)"), t)))
        text.fail();
    const float split = ui_rect_width(s.hint) < 320 ? 94.f : 128.f;
    text(s.root, utf8_literal("Choose destinations"), {s.hint.left, s.hint.top, s.hint.right - split, s.hint.bottom},
         t.text_secondary, text_style_subtext);
    text(s.root, {s.selection_label, s.selection_label_bytes}, {s.hint.right - split, s.hint.top, s.hint.right, s.hint.bottom},
         s.content->selected_count == kUiMessageForwardMaximumTargets ? accent : t.text_secondary, text_style_subtext,
         ui_text_end);
    for (u32 i = 0; text.ok() && i < s.count; ++i) {
        const auto row_rectangle = s.row_rectangles[i]; const auto& row = s.visible[i];
        const auto* picked = find_selection(s, row.channel_id);
        const auto color = row.enabled && (!s.content->committed || picked) ? t.text_primary : t.text_secondary;
        const auto detail = picked && picked->state != UiMessageForwardState::ready && picked->detail.bytes ? picked->detail :
            picked && picked->state == UiMessageForwardState::cancelled ? utf8_literal("Cancelled") :
            picked && picked->state == UiMessageForwardState::sent ? utf8_literal("Sent") :
            picked && picked->state == UiMessageForwardState::sending ? utf8_literal("Sending…") :
            picked && picked->state == UiMessageForwardState::failed ? utf8_literal("Failed • select to retry") : row.detail;
        text(s.rows[i], row.label, ui_rect_edges(row_rectangle, ui_start(52), ui_start(5), ui_end(-38), ui_start(28)), color,
             picked ? text_style_bold : 0, ui_text_in_body);
        text(s.rows[i], detail, ui_rect_edges(row_rectangle, ui_start(52), ui_start(28), ui_end(-38), ui_end(-4)),
             picked && picked->state == UiMessageForwardState::failed ? error : t.text_secondary, text_style_subtext,
             ui_text_in_body);
    }
    if (!s.accepted_count) {
        const auto heading = s.content->phase == UiMessageForwardPhase::unavailable ? utf8_literal("No available destinations") :
            s.content->phase == UiMessageForwardPhase::no_matches ? utf8_literal("No matches") : utf8_literal("Choose where to forward");
        const float mid = (s.body.top + s.body.bottom) * .5f;
        text(s.body_owner, heading, {s.body.left + 8, mid - 22, s.body.right - 8, mid + 2}, t.text_primary, text_style_bold,
             ui_text_in_body | ui_text_center);
        text(s.body_owner, s.content->status.bytes ? s.content->status : utf8_literal("Try a channel or conversation name."),
             {s.body.left + 8, mid + 3, s.body.right - 8, mid + 27}, t.text_secondary, text_style_subtext,
             ui_text_in_body | ui_text_center);
    }
    for (u32 i = 0; text.ok() && s.receipts_visible && i < s.content->selected_count; ++i) {
        const auto& target = s.content->selected[i];
        text(s.targets[i], target.label, ui_rect_edges(s.target_rectangles[i], ui_start(8), ui_start(0), ui_end(-26), ui_end(0)),
             target.state == UiMessageForwardState::failed ? error : t.text_primary, text_style_subtext);
    }
    if (ui_rect_width(p) >= 440)
        text(s.root, s.content->status.bytes ? s.content->status : utf8_literal("Send a snapshot"),
             {s.footer.left, s.footer.top, s.cancel_rectangle.left - 10, s.footer.bottom}, t.text_secondary, text_style_subtext);
    const bool enabled = footer_action(s) != UiMessageForwardAction::none;
    const bool committed = s.content->sending || s.content->committed;
    text(s.cancel, committed ? utf8_literal("Cancel remaining") : utf8_literal("Cancel"), s.cancel_rectangle, t.text_primary,
         committed ? text_style_subtext : 0, ui_text_center);
    return text(s.send, {s.send_label, s.send_label_bytes},
                ui_rect_edges(s.send_rectangle, ui_start(34), ui_start(0), ui_end(-6), ui_end(0)),
                enabled ? ui_surface_foreground(t, t.tokens.emphasis.fill) : t.text_secondary,
                text_style_bold | (s.content->committed ? text_style_subtext : 0), ui_text_center).ok();
}
} // namespace maicord
