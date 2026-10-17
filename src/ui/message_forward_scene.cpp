#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/ui/guild_icon_media.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/scrollbar.hpp"

namespace maicord {
namespace {
bool current(const UiMessageForwardScene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision &&
        (!s.source || (s.source->revision == s.source_revision && s.source->count == s.accepted_count &&
        (!s.source->current || s.source->current(s.source->context))));
}
// The 32-DIP destination tile leading each row; identity images cover it.
constexpr float kDestinationTileRadius = 8.f;
UiRect destination_tile(UiRect row) noexcept {
    return {row.left + 10, row.top + 10, row.left + 42, row.top + 42};
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
    if (!ui_fixed_widget_storage_initialize(&s.storage, kUiMessageForwardWidgetCapacity,
        sizeof(UiMessageForwardRow) * kUiMessageForwardMaximumRows, alignof(UiMessageForwardRow)) ||
        !ui_widget_manager_initialize(&s.widgets, s.storage.widgets, s.storage.layout_nodes)) return false;
    s.accepted = ui_fixed_widget_storage_sidecar<UiMessageForwardRow>(s.storage);
    const auto add = [&](UiWidgetHandle parent, UiWidgetKind kind, Utf8View name) noexcept {
        auto style = ui_layout_style();
        style.flags |= ui_layout_node_clip_children | ui_layout_node_allow_overlap | ui_layout_node_allow_zero_size;
        return ui_widget_manager_add_prevalidated(&s.widgets, parent,
            {s.widgets.count + 1, kind, ui_widget_none, {name, style}});
    };
    auto root = add(ui_widget_invalid_handle(), UiWidgetKind::container, utf8_literal("Forward message"));
    if (!root.ok()) return false;
    s.root = root.handle;
    auto* node = ui_layout_engine_resolve(&s.widgets.layout, s.widgets.storage[s.root.slot].layout);
    node->spec.style.width = node->spec.style.height = ui_layout_fill();
    const auto edit = add(s.root, UiWidgetKind::text_field, utf8_literal("Find destinations"));
    const auto caption = add(s.root, UiWidgetKind::text_field, utf8_literal("Optional forward caption"));
    const auto close = add(s.root, UiWidgetKind::button, utf8_literal("Close forwarding"));
    const auto send = add(s.root, UiWidgetKind::button, utf8_literal("Send selected forwards"));
    const auto cancel = add(s.root, UiWidgetKind::button, utf8_literal("Cancel forwarding"));
    const auto body = add(s.root, UiWidgetKind::container, utf8_literal("Forward destinations"));
    if (!edit.ok() || !caption.ok() || !close.ok() || !send.ok() || !cancel.ok() || !body.ok()) return false;
    s.edit = edit.handle; s.caption = caption.handle; s.close = close.handle; s.send = send.handle;
    s.cancel = cancel.handle; s.body_owner = body.handle;
    for (auto& handle : s.rows) {
        auto row = add(s.body_owner, UiWidgetKind::button, utf8_literal("Forward destination"));
        if (!row.ok()) return false;
        handle = row.handle;
    }
    for (auto& handle : s.targets) {
        auto target = add(s.root, UiWidgetKind::button, utf8_literal("Selected forward destination"));
        if (!target.ok()) return false;
        handle = target.handle;
    }
    return true;
}
bool rectangle(UiMessageForwardScene& s, UiWidgetHandle handle, UiRect r, UiRect parent,
    bool visible, bool enabled, bool focusable = true) noexcept {
    auto* widget = ui_widget_manager_resolve(&s.widgets, handle);
    auto* node = widget ? ui_layout_engine_resolve(&s.widgets.layout, widget->layout) : nullptr;
    if (!node) return false;
    auto& style = node->spec.style;
    const float width = visible ? ui_rect_width(r) : 0, height = visible ? ui_rect_height(r) : 0;
    const float left = visible ? r.left - parent.left : 0, top = visible ? r.top - parent.top : 0;
    const u16 flags = static_cast<u16>(ui_layout_node_clip_children | ui_layout_node_allow_overlap |
        ui_layout_node_allow_zero_size | (visible ? ui_layout_node_visible : 0));
    const bool changed = style.width.mode != UiLayoutLengthMode::fixed || style.width.value != width ||
        style.height.mode != UiLayoutLengthMode::fixed || style.height.value != height ||
        style.margin.left != left || style.margin.top != top || style.flags != flags;
    style.width = ui_layout_fixed(width); style.height = ui_layout_fixed(height);
    style.margin = {left, top, 0, 0}; style.flags = flags;
    widget->flags = static_cast<u16>(visible && enabled ? ui_widget_enabled | ui_widget_hit_test |
        (focusable ? ui_widget_focusable : 0) : 0);
    return changed;
}
void cancel_capture(UiMessageForwardScene& s) noexcept {
    ui_widget_manager_capture(&s.widgets, ui_widget_invalid_handle());
    ui_widget_manager_hover(&s.widgets, ui_widget_invalid_handle());
}
bool layout(UiMessageForwardScene& s, bool force = false) noexcept {
    bool dirty = force;
    dirty |= rectangle(s, s.edit, s.field, s.panel, true, !s.content->committed && s.content->edit.active && !s.content->sending);
    dirty |= rectangle(s, s.caption, s.caption_field, s.panel, true, !s.content->committed && s.content->caption.active && !s.content->sending);
    dirty |= rectangle(s, s.close, {s.panel.right - 44, s.panel.top + 8, s.panel.right - 12, s.panel.top + 40},
        s.panel, true, true);
    dirty |= rectangle(s, s.send, s.send_rectangle, s.panel, true, footer_action(s) != UiMessageForwardAction::none);
    dirty |= rectangle(s, s.cancel, s.cancel_rectangle, s.panel, true, true);
    dirty |= rectangle(s, s.body_owner, s.body, s.panel, true, false, false);
    const u32 first = static_cast<u32>(s.scroll_dips / kUiMessageForwardRowHeight);
    if (s.first != first) cancel_capture(s);
    s.first = first; s.count = 0;
    for (u32 i = 0; i < kUiMessageForwardVisibleRows; ++i) {
        const u32 index = first + i;
        UiRect r{};
        bool visible = index < s.accepted_count;
        if (visible) {
            const float top = s.body.top + index * kUiMessageForwardRowHeight - s.scroll_dips;
            r = {s.body.left + 2, top, s.body.right - 14, top + kUiMessageForwardRowHeight};
            visible = ui_rect_has_area(ui_rect_intersection(r, s.body));
        }
        if (visible) { s.visible[i] = s.accepted[index]; s.row_rectangles[i] = r; ++s.count; }
        else { s.visible[i] = {}; s.row_rectangles[i] = {}; }
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
bool emit(UiTextOutput* out, const UiMessageForwardScene& s, UiWidgetHandle owner, Utf8View value,
    UiRect r, UiRect clip, Color color, u32 flags = 0, UiTextAlignment alignment = UiTextAlignment::start,
    UiTextWrapping wrapping = UiTextWrapping::no_wrap) noexcept {
    if (!value.bytes || !ui_rect_has_area(ui_rect_intersection(r, clip))) return true;
    return ui_text_output_push(out, {value, {&s.widgets, owner}, r, clip, color, flags,
        alignment, UiTextAlignment::center, wrapping, UiTextOverflow::ellipsis, UiTextFace::normal});
}
bool surface(PaintStream* out, UiRect r, const Theme& t, Color fill, Color border, float radius) noexcept {
    return ui_background_paint_surface(out, r, t, fill, border, radius, 1, 1,
        UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest);
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
bool mark(PaintStream* out, UiRect r, Color color) noexcept {
    return ui_icon_paint(out, UiIcon::check, {r.left + 2, r.top + 2, r.right - 2, r.bottom - 2}, color);
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
        UiMessageForwardRow accepted[kUiMessageForwardMaximumRows]{};
        for (u32 i = 0; i < count; ++i) {
            if (!source->resolve(source->context, i, &accepted[i]) || !row_valid(accepted[i])) return false;
            for (u32 j = 0; j < i; ++j) if (accepted[j].channel_id == accepted[i].channel_id) return false;
        }
        if (source && (source->revision != source_revision || source->count != count ||
            (source->current && !source->current(source->context)))) return false;
        if (!initialize(*s)) return false;
        cancel_capture(*s); copy_bytes(s->accepted, accepted, sizeof(UiMessageForwardRow) * count);
        s->accepted_count = count; s->selected = ~0u; s->scroll_dips = 0;
        for (u32 i = 0; i < count; ++i)
            if (accepted[i].enabled && (s->selected == ~0u || accepted[i].channel_id == selected_id)) s->selected = i;
    } else if (!initialize(*s)) return false;
    const auto old_panel = s->panel;
    // Cancel pressed state/buttons when completion changes their semantic action.
    if (s->content_revision != content->revision) cancel_capture(*s);
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
void ui_message_forward_scene_release(UiMessageForwardScene* s) noexcept {
    if (!s) return;
    ui_fixed_widget_storage_release(&s->storage); *s = {};
}
bool ui_message_forward_scene_contains(const UiMessageForwardScene& s, float x, float y) noexcept {
    return current(s) && ui_rect_contains(s.viewport, x, y);
}
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
    if (!ui_message_forward_scene_contains(s, x, y)) return {};
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
    cancel_capture(*s); s->scroll_dips = offset;
    if (!layout(*s)) return false;
    ++s->revision; return true;
}
bool ui_message_forward_scene_select(UiMessageForwardScene* s, u32 index, bool reveal) noexcept {
    if (!s || !current(*s) || index >= s->accepted_count || !s->accepted[index].enabled) return false;
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
        if (s->accepted[index].enabled) return ui_message_forward_scene_select(s, index);
        if (reverse) { if (!index) break; --index; }
        else { if (index + 1 == s->accepted_count) break; ++index; }
    }
    return false;
}
bool ui_message_forward_scene_set_hover(UiMessageForwardScene* s, float x, float y, bool* changed) noexcept {
    if (changed) *changed = false;
    if (!s || !current(*s)) return false;
    const auto hit = ui_widget_manager_hit_test(s->widgets, x, y), before = s->widgets.hovered;
    const auto selected = s->selected;
    if (!ui_widget_manager_hover(&s->widgets, hit)) return false;
    for (u32 i = 0; i < s->count; ++i) if (ui_widget_handle_matches(hit, s->rows[i])) ui_message_forward_scene_select(s, s->first + i, false);
    const bool delta = !ui_widget_handle_matches(before, hit) && (before.generation || hit.generation);
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
    if (row.kind == UiQuickSwitcherKind::private_channel && row.user_id && row.avatar_hash.bytes) {
        const u32 variant = user_avatar_variant(row.avatar_hash);
        if (!variant)
            return false;
        hash = row.avatar_hash;
        key = media_asset_user_avatar(row.user_id, variant, asset_hash_animated(hash));
        circle = true;
    } else if ((row.kind == UiQuickSwitcherKind::text_channel || row.kind == UiQuickSwitcherKind::voice_channel) &&
               row.guild_id && state) {
        if (!guild_icon_media_key(*state, row.guild_id, &key, &hash))
            return false;
    } else
        return false;
    const UiRect rectangle = destination_tile(s.row_rectangles[slot]);
    const UiRect clip{maximum(rectangle.left, s.body.left), maximum(rectangle.top, s.body.top),
                      minimum(rectangle.right, s.body.right), minimum(rectangle.bottom, s.body.bottom)};
    if (clip.left >= clip.right || clip.top >= clip.bottom)
        return false;
    *output = {key, hash, rectangle, clip, circle ? 0.f : kDestinationTileRadius, circle};
    return true;
}

bool ui_message_forward_scene_paint(const UiMessageForwardScene& s, const Theme& t, PaintStream* out,
    const UiEditGeometry* query, const UiEditGeometry* caption, u32 dpi) noexcept {
    if (!current(s)) return true;
    if (!out || !paint_stream_push_clip(out, s.viewport.left, s.viewport.top, s.viewport.right, s.viewport.bottom)) return false;
    const auto p = s.panel;
    const auto accent = ui_foreground_color(t, t.tokens.emphasis.outline);
    const auto error = ui_foreground_color(t, t.notification);
    const auto sent = ui_foreground_color(t, t.syntax_string);
    bool ok = paint_stream_push(out, s.viewport.left, s.viewport.top, s.viewport.right, s.viewport.bottom, t.tokens.overlay.scrim) &&
        ui_surface_paint(out, p, t, UiElevation::dialog, t.scale.radius_lg + t.scale.space_1, nullptr,
                         &t.interactive_hover) &&
        ui_icon_paint(out, UiIcon::forwarded, {p.left + 20, p.top + 16, p.left + 40, p.top + 36}, accent);
    if (ok && ui_rect_has_area(s.preview)) ok = surface(out, s.preview, t, t.base_lowest, t.surface_high, 8);
    if (ok && !s.compact) ok = paint_stream_push(out, s.preview.left + 12, s.preview.top + 12,
        s.preview.left + 15, s.preview.bottom - 12, accent);
    const auto query_view = presentation_view(s.content->edit, s.content->sending || s.content->committed);
    const auto caption_view = presentation_view(s.content->caption, s.content->sending || s.content->committed);
    Theme edit_theme = t;
    edit_theme.accent = accent;
    ok = ok && ui_edit_field_paint(out, s.field, s.text, query_view, query, t.base_lowest, edit_theme, dpi, 8) &&
        ui_icon_paint(out, UiIcon::search, {s.field.left + 10, s.field.top + 10, s.field.left + 30, s.field.top + 30}, t.text_secondary) &&
        ui_edit_field_paint(out, s.caption_field, s.caption_text, caption_view, caption, t.base_lowest, edit_theme, dpi, 8);
    const auto* close = ui_widget_manager_layout_node(s.widgets, s.close);
    if (ok && (ui_widget_handle_matches(s.widgets.hovered, s.close) || ui_widget_handle_matches(s.widgets.focused, s.close)))
        ok = surface(out, close->rectangle, t, t.surface_high, t.surface_high, 6);
    ok = ok && ui_icon_paint(out, UiIcon::close, {p.right - 38, p.top + 14, p.right - 18, p.top + 34}, t.text_secondary);
    bool body_clip = false;
    if (ok) { body_clip = paint_stream_push_clip(out, s.body.left, s.body.top, s.body.right, s.body.bottom); ok = body_clip; }
    for (u32 i = 0; ok && i < s.count; ++i) {
        const auto r = s.row_rectangles[i]; const auto* picked = find_selection(s, s.visible[i].channel_id);
        const bool selected = !s.content->committed && s.first + i == s.selected;
        const auto card = UiRect{r.left, r.top + 2, r.right, r.bottom - 2};
        if (selected || picked || ui_widget_handle_matches(s.widgets.hovered, s.rows[i])) ok = surface(out, card, t,
            picked ? t.surface_high : t.base_low, selected ? accent : t.surface_high, 7);
        const UiRect avatar = destination_tile(r);
        ok = ok && surface(out, avatar, t, t.base_lowest, t.base_lowest, kDestinationTileRadius) &&
            ui_icon_paint(out, destination_icon(s.visible[i].kind),
                {avatar.left + 6, avatar.top + 6, avatar.right - 6, avatar.bottom - 6}, t.text_secondary);
        const UiRect box{r.right - 30, r.top + 16, r.right - 10, r.top + 36};
        if (picked && picked->state == UiMessageForwardState::failed)
            ok = ok && ui_icon_paint(out, UiIcon::retry, box, error);
        else if (picked && picked->state == UiMessageForwardState::cancelled)
            ok = ok && ui_icon_paint(out, UiIcon::close, box, t.text_secondary);
        else if (picked && picked->state == UiMessageForwardState::sending)
            ok = ok && ui_icon_paint(out, UiIcon::sending, box, accent);
        else {
            ok = ok && surface(out, box, t, picked ? t.tokens.emphasis.fill : t.base_lowest,
                picked ? t.tokens.emphasis.fill : t.interactive, 5);
            if (picked) ok = ok && mark(out, box, ui_surface_foreground(t, t.tokens.emphasis.fill));
        }
    }
    if (body_clip) ok = paint_stream_pop_clip(out) && ok;
    if (ok) ok = ui_scrollbar_paint(out, ui_scrollbar_geometry(s.body, s.scroll_dips,
        s.accepted_count * kUiMessageForwardRowHeight), t);
    for (u32 i = 0; ok && s.receipts_visible && i < s.content->selected_count; ++i) {
        const auto r = s.target_rectangles[i]; const auto state = s.content->selected[i].state;
        const Color color = state == UiMessageForwardState::cancelled ? t.text_secondary : state == UiMessageForwardState::failed ? error :
            state == UiMessageForwardState::sent ? sent : accent;
        ok = surface(out, r, t, t.base_low, tint(color, .55f), 6);
        const UiRect icon{r.right - 22, r.top + 6, r.right - 6, r.bottom - 6};
        ok = ok && ui_icon_paint(out, state == UiMessageForwardState::sent ? UiIcon::check :
            state == UiMessageForwardState::failed ? UiIcon::retry :
            state == UiMessageForwardState::sending ? UiIcon::sending : UiIcon::close, icon, color);
    }
    const bool enabled = footer_action(s) != UiMessageForwardAction::none;
    if (ok) ok = surface(out, s.cancel_rectangle, t, ui_widget_handle_matches(s.widgets.hovered, s.cancel) ? t.surface_high : t.base_low,
        ui_widget_handle_matches(s.widgets.focused, s.cancel) ? accent : t.interactive, 7) &&
        surface(out, s.send_rectangle, t, enabled ? t.tokens.emphasis.fill : t.surface_high,
            ui_widget_handle_matches(s.widgets.focused, s.send) ? t.text_primary : enabled ? t.tokens.emphasis.fill : t.surface_high, 7) &&
        ui_icon_paint(out, s.content->committed ? has_failed(s) ? UiIcon::retry : UiIcon::check : UiIcon::forwarded,
            {s.send_rectangle.left + 12, s.send_rectangle.top + 11,
            s.send_rectangle.left + 30, s.send_rectangle.top + 29}, enabled ? ui_surface_foreground(t, t.tokens.emphasis.fill) : t.text_secondary);
    return paint_stream_pop_clip(out) && ok;
}
bool ui_message_forward_scene_text_output(const UiMessageForwardScene& s, const Theme& t, UiTextOutput* out,
    const UiEditGeometry* query, const UiEditGeometry* caption) noexcept {
    if (!current(s)) return true;
    if (!ui_text_output_valid(out)) return false;
    const auto p = s.panel;
    const auto accent = ui_foreground_color(t, t.tokens.emphasis.outline);
    const auto error = ui_foreground_color(t, t.notification);
    if (!emit(out, s, s.root, utf8_literal("Forward message"), {p.left + 48, p.top + 10, p.right - 48, p.top + 40},
        p, t.text_primary, text_style_bold)) return false;
    if (s.compact) {
        if (!emit(out, s, s.root, utf8_literal("Forwarded"),
                {s.preview.left + 12, s.preview.top, s.preview.left + 94, s.preview.bottom}, s.preview,
                t.text_secondary, text_style_subtext) ||
            !emit(out, s, s.root, s.content->preview.bytes ? s.content->preview : s.content->media_summary,
                {s.preview.left + 98, s.preview.top, s.preview.right - 12, s.preview.bottom}, s.preview,
                t.text_primary, text_style_subtext)) return false;
    } else {
        const auto r = s.preview;
        if (!emit(out, s, s.root, utf8_literal("Forwarded"), {r.left + 26, r.top + 7, r.left + 108, r.top + 27}, r,
            t.text_secondary, text_style_subtext) ||
            !emit(out, s, s.root, s.content->author, {r.left + 112, r.top + 7, r.right - 12, r.top + 27}, r,
                t.text_primary, text_style_bold | text_style_subtext) ||
            !emit(out, s, s.root, s.content->preview.bytes ? s.content->preview : utf8_literal("Media message"),
                {r.left + 26, r.top + 29, r.right - 12, r.top + 63}, r, t.text_primary, 0,
                UiTextAlignment::start, UiTextWrapping::wrap) ||
            !emit(out, s, s.root, s.content->media_summary,
                {r.left + 26, r.top + 65, r.right - 12, r.bottom - 7}, r, t.text_secondary, text_style_subtext)) return false;
    }
    const auto query_view = presentation_view(s.content->edit, s.content->sending || s.content->committed);
    const auto caption_view = presentation_view(s.content->caption, s.content->sending || s.content->committed);
    if (!ui_edit_field_text_output(out, {&s.widgets, s.edit}, s.text, query_view, query,
            utf8_literal("Find a channel or conversation"), t) ||
        !ui_edit_field_text_output(out, {&s.widgets, s.caption}, s.caption_text, caption_view, caption,
            utf8_literal("Add a message (optional)"), t)) return false;
    const float split = ui_rect_width(s.hint) < 320 ? 94.f : 128.f;
    if (!emit(out, s, s.root, utf8_literal("Choose destinations"),
            {s.hint.left, s.hint.top, s.hint.right - split, s.hint.bottom}, p, t.text_secondary, text_style_subtext) ||
        !emit(out, s, s.root, {s.selection_label, s.selection_label_bytes},
            {s.hint.right - split, s.hint.top, s.hint.right, s.hint.bottom}, p,
            s.content->selected_count == kUiMessageForwardMaximumTargets ? accent : t.text_secondary,
            text_style_subtext, UiTextAlignment::end)) return false;
    for (u32 i = 0; i < s.count; ++i) {
        const auto r = s.row_rectangles[i]; const auto& row = s.visible[i];
        const auto* picked = find_selection(s, row.channel_id);
        const auto color = row.enabled && (!s.content->committed || picked) ? t.text_primary : t.text_secondary;
        const auto detail = picked && picked->state != UiMessageForwardState::ready && picked->detail.bytes ? picked->detail :
            picked && picked->state == UiMessageForwardState::cancelled ? utf8_literal("Cancelled") :
            picked && picked->state == UiMessageForwardState::sent ? utf8_literal("Sent") :
            picked && picked->state == UiMessageForwardState::sending ? utf8_literal("Sending…") :
            picked && picked->state == UiMessageForwardState::failed ? utf8_literal("Failed • select to retry") : row.detail;
        if (!emit(out, s, s.rows[i], row.label, {r.left + 52, r.top + 5, r.right - 38, r.top + 28},
                s.body, color, picked ? text_style_bold : 0) ||
            !emit(out, s, s.rows[i], detail, {r.left + 52, r.top + 28, r.right - 38, r.bottom - 4}, s.body,
                picked && picked->state == UiMessageForwardState::failed ? error : t.text_secondary,
                text_style_subtext)) return false;
    }
    if (!s.accepted_count) {
        const auto heading = s.content->phase == UiMessageForwardPhase::unavailable ? utf8_literal("No available destinations") :
            s.content->phase == UiMessageForwardPhase::no_matches ? utf8_literal("No matches") : utf8_literal("Choose where to forward");
        const float mid = (s.body.top + s.body.bottom) * .5f;
        if (!emit(out, s, s.body_owner, heading, {s.body.left + 8, mid - 22, s.body.right - 8, mid + 2}, s.body,
                t.text_primary, text_style_bold, UiTextAlignment::center) ||
            !emit(out, s, s.body_owner, s.content->status.bytes ? s.content->status : utf8_literal("Try a channel or conversation name."),
                {s.body.left + 8, mid + 3, s.body.right - 8, mid + 27}, s.body, t.text_secondary,
                text_style_subtext, UiTextAlignment::center)) return false;
    }
    for (u32 i = 0; s.receipts_visible && i < s.content->selected_count; ++i) {
        const auto r = s.target_rectangles[i]; const auto& target = s.content->selected[i];
        const auto color = target.state == UiMessageForwardState::failed ? error : t.text_primary;
        if (!emit(out, s, s.targets[i], target.label,
            {r.left + 8, r.top, r.right - 26, r.bottom}, p, color, text_style_subtext)) return false;
    }
    if (ui_rect_width(p) >= 440 && !emit(out, s, s.root, s.content->status.bytes ? s.content->status : utf8_literal("Send a snapshot"),
        {s.footer.left, s.footer.top, s.cancel_rectangle.left - 10, s.footer.bottom}, p, t.text_secondary, text_style_subtext)) return false;
    const bool enabled = footer_action(s) != UiMessageForwardAction::none;
    const bool committed = s.content->sending || s.content->committed;
    return emit(out, s, s.cancel, committed ? utf8_literal("Cancel remaining") : utf8_literal("Cancel"),
        s.cancel_rectangle, p, t.text_primary, committed ? text_style_subtext : 0, UiTextAlignment::center) &&
        emit(out, s, s.send, {s.send_label, s.send_label_bytes},
            {s.send_rectangle.left + 34, s.send_rectangle.top, s.send_rectangle.right - 6, s.send_rectangle.bottom}, p,
            enabled ? ui_surface_foreground(t, t.tokens.emphasis.fill) : t.text_secondary,
            text_style_bold | (s.content->committed ? text_style_subtext : 0), UiTextAlignment::center);
}
} // namespace maicord
