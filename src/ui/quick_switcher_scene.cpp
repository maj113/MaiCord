#include "maicord/ui/quick_switcher_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/guild_icon_media.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
namespace {
bool current(const UiQuickSwitcherScene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision &&
        (!s.source || (s.source->revision == s.source_revision && s.source->count == s.accepted_count &&
                      (!s.source->current || s.source->current(s.source->context))));
}
bool row_valid(const UiQuickSwitcherRow& row) noexcept {
    return row.key && row.kind <= UiQuickSwitcherKind::user && row.presence <= UiPresence::offline &&
        row.label.bytes && row.label.bytes <= 256 &&
        row.detail.bytes <= 512 && row.metadata.bytes <= 128 && row.avatar_hash.bytes <= 64 &&
        utf8_validate(row.label).valid && utf8_validate(row.detail).valid &&
        utf8_validate(row.metadata).valid && utf8_validate(row.avatar_hash).valid;
}
UiQuickSwitcherTarget row_target(const UiQuickSwitcherScene& s, u32 index) noexcept {
    if (index >= s.accepted_count || !s.accepted[index].enabled) return {};
    const auto& row = s.accepted[index];
    return {UiQuickSwitcherAction::activate, row.key, row.guild_id, row.channel_id, row.user_id,
            index, s.source_revision, row.kind};
}
bool initialize(UiQuickSwitcherScene& s) noexcept {
    if (s.storage.arena.base) return true;
    if (!ui_fixed_widget_storage_initialize(&s.storage, kUiQuickSwitcherWidgetCapacity,
            sizeof(UiQuickSwitcherRow) * kUiQuickSwitcherMaximumRows, alignof(UiQuickSwitcherRow)) ||
        !ui_widget_manager_initialize(&s.widgets, s.storage.widgets, s.storage.layout_nodes)) return false;
    s.accepted = ui_fixed_widget_storage_sidecar<UiQuickSwitcherRow>(s.storage);
    const auto add = [&](UiWidgetHandle parent, UiWidgetKind kind, Utf8View label) noexcept {
        auto style = ui_layout_style();
        style.flags |= ui_layout_node_clip_children | ui_layout_node_allow_overlap | ui_layout_node_allow_zero_size;
        return ui_widget_manager_add_prevalidated(&s.widgets, parent,
            {s.widgets.count + 1, kind, ui_widget_none, {label, style}});
    };
    const auto root = add(ui_widget_invalid_handle(), UiWidgetKind::container, utf8_literal("Quick switcher"));
    if (!root.ok()) return false;
    s.root = root.handle;
    auto* root_node = ui_layout_engine_resolve(&s.widgets.layout, s.widgets.storage[s.root.slot].layout);
    root_node->spec.style.width = root_node->spec.style.height = ui_layout_fill();
    const auto edit = add(s.root, UiWidgetKind::text_field, utf8_literal("Quick switch query"));
    const auto close = add(s.root, UiWidgetKind::button, utf8_literal("Close quick switcher"));
    const auto confirm = add(s.root, UiWidgetKind::button, utf8_literal("Confirm selection"));
    const auto body = add(s.root, UiWidgetKind::container, utf8_literal("Quick switch results"));
    if (!edit.ok() || !close.ok() || !confirm.ok() || !body.ok()) return false;
    s.edit = edit.handle; s.close = close.handle; s.confirm = confirm.handle; s.body_owner = body.handle;
    for (auto& handle : s.rows) {
        const auto row = add(s.body_owner, UiWidgetKind::button, utf8_literal("Quick switch result"));
        if (!row.ok()) return false;
        handle = row.handle;
    }
    return true;
}
bool rectangle(UiQuickSwitcherScene& s, UiWidgetHandle handle, UiRect r, UiRect parent,
               bool visible, bool enabled, bool focusable = true) noexcept {
    auto* widget = ui_widget_manager_resolve(&s.widgets, handle);
    auto* node = widget ? ui_layout_engine_resolve(&s.widgets.layout, widget->layout) : nullptr;
    if (!node) return false;
    auto& style = node->spec.style;
    const float width = visible ? ui_rect_width(r) : 0.f, height = visible ? ui_rect_height(r) : 0.f;
    const float left = visible ? r.left - parent.left : 0.f, top = visible ? r.top - parent.top : 0.f;
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
void cancel_row_capture(UiQuickSwitcherScene& s) noexcept {
    for (auto row : s.rows) if (ui_widget_handle_matches(row, s.widgets.captured)) {
        ui_widget_manager_capture(&s.widgets, ui_widget_invalid_handle()); break;
    }
    ui_widget_manager_hover(&s.widgets, ui_widget_invalid_handle());
}
bool layout(UiQuickSwitcherScene& s, bool force = false) noexcept {
    bool dirty = force;
    dirty |= rectangle(s, s.edit, s.field, s.panel, true, true);
    dirty |= rectangle(s, s.close, {s.panel.right - 44, s.panel.top + 8,
        s.panel.right - 12, s.panel.top + 40}, s.panel, true, true);
    dirty |= rectangle(s, s.body_owner, s.body, s.panel, true, false, false);
    const bool picker = s.content && s.content->picker;
    dirty |= rectangle(s, s.confirm, picker ? s.confirm_rectangle : UiRect{}, s.panel, picker,
                       picker && s.content->can_confirm);
    const u32 first = static_cast<u32>(s.scroll_dips / kUiQuickSwitcherRowHeight);
    if (s.first != first) cancel_row_capture(s);
    s.first = first; s.count = 0;
    for (u32 i = 0; i < kUiQuickSwitcherVisibleRows; ++i) {
        const u32 index = first + i;
        UiRect row{};
        bool visible = index < s.accepted_count;
        if (visible) {
            const float top = s.body.top + index * kUiQuickSwitcherRowHeight - s.scroll_dips;
            // Rows share the search field's edges; only a scrollable list reserves the bar gutter.
            row = {s.body.left + 4, top, s.body.right - (ui_quick_switcher_scene_scroll_maximum(s) > 0 ? 16 : 4),
                   top + kUiQuickSwitcherRowHeight};
            visible = ui_rect_has_area(ui_rect_intersection(row, s.body));
        }
        if (visible) {
            s.visible[i] = s.accepted[index]; s.row_rectangles[i] = row; ++s.count;
        } else { s.visible[i] = {}; s.row_rectangles[i] = {}; }
        const auto clipped = visible ? ui_rect_intersection(row, s.body) : UiRect{};
        dirty |= rectangle(s, s.rows[i], clipped, s.body, visible,
                           visible && s.accepted[index].enabled, false);
    }
    if (dirty && !ui_widget_manager_layout_rectangle(&s.widgets, s.panel).ok()) return false;
    ui_widget_manager_reconcile_input(&s.widgets);
    return true;
}
bool emit(UiTextOutput* out, const UiQuickSwitcherScene& s, UiWidgetHandle owner,
          Utf8View value, UiRect r, UiRect clip, Color color, u32 flags = 0,
          UiTextAlignment alignment = UiTextAlignment::start) noexcept {
    if (!value.bytes || !ui_rect_has_area(ui_rect_intersection(r, clip))) return true;
    return ui_text_output_push(out, {value, {&s.widgets, owner}, r, clip, color, flags,
        alignment, UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis,
        UiTextFace::normal});
}
bool checked(const UiQuickSwitcherScene& s, u64 key) noexcept {
    for (u32 i = 0; s.content->picker && i < s.content->checked.count; ++i)
        if (s.content->checked[i] == key) return true;
    return false;
}
UiIcon icon(UiQuickSwitcherKind kind) noexcept {
    switch (kind) {
    case UiQuickSwitcherKind::voice_channel: return UiIcon::speaker;
    case UiQuickSwitcherKind::private_channel: return UiIcon::direct_message;
    case UiQuickSwitcherKind::group_private: return UiIcon::members;
    case UiQuickSwitcherKind::user: return UiIcon::user;
    case UiQuickSwitcherKind::guild: return UiIcon::guild;
    default: return UiIcon::channel;
    }
}
bool surface(PaintStream* out, UiRect r, const Theme& theme, Color fill, Color border, float radius) noexcept {
    return ui_background_paint_surface(out, r, theme, fill, border, radius, 1, 1,
        UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest);
}
} // namespace

bool ui_quick_switcher_scene_current(const UiQuickSwitcherScene& s) noexcept { return current(s); }
float ui_quick_switcher_scene_scroll_maximum(const UiQuickSwitcherScene& s) noexcept {
    return maximum(0.f, s.accepted_count * kUiQuickSwitcherRowHeight - ui_rect_height(s.body));
}
bool ui_quick_switcher_scene_update(UiQuickSwitcherScene* s, const UiQuickSwitcherSource* source,
    const UiQuickSwitcherContent* content, UiRect viewport) noexcept {
    if (!s || !content || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 240 ||
        ui_rect_height(viewport) < 224 || content->phase > UiQuickSwitcherPhase::unavailable ||
        !utf8_validate(content->edit.text).valid || content->edit.anchor > content->edit.text.bytes ||
        content->edit.caret > content->edit.text.bytes || !utf8_validate(content->heading).valid ||
        !utf8_validate(content->hint).valid || !utf8_validate(content->status).valid ||
        !utf8_validate(content->action).valid || (content->checked.count && !content->checked.data) ||
        (source && (source->count > kUiQuickSwitcherMaximumRows || !source->resolve ||
                   (source->current && !source->current(source->context))))) return false;
    if (current(*s) && s->source == source && s->content == content && ui_rect_equal(s->viewport, viewport)) return true;
    const bool receipt = !s->active || s->source != source ||
        s->source_revision != (source ? source->revision : 0) || s->accepted_count != (source ? source->count : 0);
    const u32 count = source ? source->count : 0;
    u64 selected_key = s->active && s->selected < s->accepted_count ? s->accepted[s->selected].key : 0;
    const auto selected_kind = s->active && s->selected < s->accepted_count ?
        s->accepted[s->selected].kind : UiQuickSwitcherKind::text_channel;
    if (receipt) {
        UiQuickSwitcherRow accepted[kUiQuickSwitcherMaximumRows]{};
        for (u32 i = 0; i < count; ++i) {
            if (!source->resolve(source->context, i, &accepted[i]) || !row_valid(accepted[i])) return false;
            for (u32 previous = 0; previous < i; ++previous)
                if (accepted[previous].key == accepted[i].key && accepted[previous].kind == accepted[i].kind) return false;
        }
        if (!initialize(*s)) return false;
        cancel_row_capture(*s);
        copy_bytes(s->accepted, accepted, sizeof(UiQuickSwitcherRow) * count);
        s->accepted_count = count; s->selected = ~0u; s->scroll_dips = 0;
        for (u32 i = 0; i < count; ++i)
            if (s->accepted[i].enabled && (s->selected == ~0u ||
                (s->accepted[i].key == selected_key && s->accepted[i].kind == selected_kind))) s->selected = i;
    } else if (!initialize(*s)) return false;
    const auto old_panel = s->panel;
    s->source = source; s->content = content; s->viewport = viewport;
    s->source_revision = source ? source->revision : 0; s->content_revision = content->revision;
    const float inset = ui_rect_width(viewport) < 400 ? 12.f : 24.f;
    const float width = minimum(640.f, ui_rect_width(viewport) - inset * 2);
    const float height = minimum(520.f, ui_rect_height(viewport) - 32.f);
    const float left = viewport.left + (ui_rect_width(viewport) - width) * .5f;
    const float top = viewport.top + (ui_rect_height(viewport) - height) * .5f;
    s->panel = {left, top, left + width, top + height};
    s->field = {left + 16, top + 48, left + width - 16, top + 96};
    s->text = {s->field.left + 40, s->field.top + 6, s->field.right - 12, s->field.bottom - 6};
    s->hint = {left + 20, top + 100, left + width - 20, top + 124};
    s->body = {left + 12, top + 132, left + width - 12, top + height - (content->picker ? 64.f : 40.f)};
    if (!ui_rect_has_area(s->body)) return false;
    s->footer = content->picker ? UiRect{left + 16, s->body.bottom + 12, left + width - 16, s->panel.bottom - 12}
                                : UiRect{left + 16, s->body.bottom + 4, left + width - 16, s->panel.bottom - 4};
    s->confirm_rectangle = {s->footer.right - minimum(176.f, ui_rect_width(s->footer) * .5f), s->footer.top,
                            s->footer.right, s->footer.bottom};
    s->scroll_dips = minimum(s->scroll_dips, ui_quick_switcher_scene_scroll_maximum(*s));
    if ((receipt || !ui_rect_equal(old_panel, s->panel)) && s->selected < s->accepted_count) {
        const float row_top = s->selected * kUiQuickSwitcherRowHeight;
        const float row_bottom = row_top + kUiQuickSwitcherRowHeight;
        if (row_top < s->scroll_dips) s->scroll_dips = row_top;
        else if (row_bottom > s->scroll_dips + ui_rect_height(s->body))
            s->scroll_dips = minimum(ui_quick_switcher_scene_scroll_maximum(*s),
                                    maximum(0.f, row_bottom - ui_rect_height(s->body)));
    }
    s->active = true;
    if (!layout(*s, !ui_rect_equal(old_panel, s->panel))) { s->active = false; return false; }
    if (content->edit.focused) ui_widget_manager_focus(&s->widgets, s->edit);
    ++s->revision;
    return true;
}
void ui_quick_switcher_scene_release(UiQuickSwitcherScene* s) noexcept {
    if (!s) return;
    ui_fixed_widget_storage_release(&s->storage); *s = {};
}
bool ui_quick_switcher_scene_contains(const UiQuickSwitcherScene& s, float x, float y) noexcept {
    return current(s) && ui_rect_contains(s.viewport, x, y);
}
UiQuickSwitcherTarget ui_quick_switcher_scene_target(const UiQuickSwitcherScene& s, UiWidgetHandle handle) noexcept {
    if (!current(s)) return {};
    if (ui_widget_handle_matches(handle, s.edit)) return {UiQuickSwitcherAction::edit};
    if (ui_widget_handle_matches(handle, s.close)) return {UiQuickSwitcherAction::close};
    if (ui_widget_handle_matches(handle, s.confirm))
        return s.content->picker && s.content->can_confirm ? UiQuickSwitcherTarget{UiQuickSwitcherAction::confirm}
                                                           : UiQuickSwitcherTarget{};
    for (u32 i = 0; i < s.count; ++i) if (ui_widget_handle_matches(handle, s.rows[i])) return row_target(s, s.first + i);
    return {};
}
UiQuickSwitcherTarget ui_quick_switcher_scene_hit_test(const UiQuickSwitcherScene& s, float x, float y) noexcept {
    if (!ui_quick_switcher_scene_contains(s, x, y)) return {};
    if (!ui_rect_contains(s.panel, x, y)) return {UiQuickSwitcherAction::close};
    return ui_quick_switcher_scene_target(s, ui_widget_manager_hit_test(s.widgets, x, y));
}
UiQuickSwitcherTarget ui_quick_switcher_scene_selected(const UiQuickSwitcherScene& s) noexcept {
    return current(s) ? row_target(s, s.selected) : UiQuickSwitcherTarget{};
}
bool ui_quick_switcher_scene_scroll(UiQuickSwitcherScene* s, float offset) noexcept {
    if (!s || !current(*s) || !is_finite(offset)) return false;
    offset = minimum(maximum(0.f, offset), ui_quick_switcher_scene_scroll_maximum(*s));
    if (offset == s->scroll_dips) return true;
    cancel_row_capture(*s); s->scroll_dips = offset;
    if (!layout(*s)) return false;
    ++s->revision; return true;
}
bool ui_quick_switcher_scene_select(UiQuickSwitcherScene* s, u32 index, bool reveal) noexcept {
    if (!s || !current(*s) || index >= s->accepted_count || !s->accepted[index].enabled) return false;
    const u32 previous = s->selected; s->selected = index;
    if (reveal) {
        const float top = index * kUiQuickSwitcherRowHeight, bottom = top + kUiQuickSwitcherRowHeight;
        const float height = ui_rect_height(s->body);
        const float offset = top < s->scroll_dips ? top : bottom > s->scroll_dips + height ? bottom - height : s->scroll_dips;
        if (!ui_quick_switcher_scene_scroll(s, offset)) return false;
    }
    if (previous != index) ++s->revision;
    return true;
}
bool ui_quick_switcher_scene_move(UiQuickSwitcherScene* s, UiQuickSwitcherMove move) noexcept {
    if (!s || !current(*s) || move > UiQuickSwitcherMove::page_previous || !s->accepted_count) return false;
    const bool reverse = move == UiQuickSwitcherMove::previous || move == UiQuickSwitcherMove::page_previous || move == UiQuickSwitcherMove::last;
    u32 step = move == UiQuickSwitcherMove::page_next || move == UiQuickSwitcherMove::page_previous
        ? maximum(1u, static_cast<u32>(ui_rect_height(s->body) / kUiQuickSwitcherRowHeight)) : 1u;
    u32 index = s->selected;
    if (move == UiQuickSwitcherMove::first || index >= s->accepted_count) index = 0;
    else if (move == UiQuickSwitcherMove::last) index = s->accepted_count - 1;
    else if (reverse) index = step > index ? 0 : index - step;
    else index = minimum(s->accepted_count - 1, index + step);
    for (u32 i = 0; i < s->accepted_count; ++i) {
        if (s->accepted[index].enabled) return ui_quick_switcher_scene_select(s, index);
        if (reverse) { if (!index) break; --index; }
        else { if (index + 1 == s->accepted_count) break; ++index; }
    }
    return false;
}
bool ui_quick_switcher_scene_set_hover(UiQuickSwitcherScene* s, float x, float y, bool* changed) noexcept {
    if (changed) *changed = false;
    if (!s || !current(*s)) return false;
    const auto hit = ui_widget_manager_hit_test(s->widgets, x, y);
    const auto before = s->widgets.hovered; const u32 selected = s->selected;
    if (!ui_widget_manager_hover(&s->widgets, hit)) return false;
    const auto target = ui_quick_switcher_scene_target(*s, hit);
    if (target.action == UiQuickSwitcherAction::activate) ui_quick_switcher_scene_select(s, target.index, false);
    const bool delta = !ui_widget_handle_matches(before, hit) && (before.generation || hit.generation);
    if (delta) ++s->revision;
    if (changed) *changed = delta || selected != s->selected;
    return true;
}

bool ui_quick_switcher_scene_media(const UiQuickSwitcherScene& s, u32 slot, const ChatStateStore* state,
                                   UiQuickSwitcherMedia* output) noexcept {
    if (!output || !current(s) || slot >= s.count || slot >= kUiQuickSwitcherVisibleRows)
        return false;
    const auto& row = s.visible[slot];
    MediaAssetKey key{};
    Utf8View hash{};
    if ((row.kind == UiQuickSwitcherKind::user || row.kind == UiQuickSwitcherKind::private_channel) &&
        row.user_id && row.avatar_hash.bytes) {
        const u32 variant = user_avatar_variant(row.avatar_hash);
        if (!variant)
            return false;
        hash = row.avatar_hash;
        key = media_asset_user_avatar(row.user_id, variant, asset_hash_animated(hash));
    } else if (row.kind == UiQuickSwitcherKind::guild && row.guild_id && state) {
        if (!guild_icon_media_key(*state, row.guild_id, &key, &hash))
            return false;
    } else
        return false;
    // A 24-DIP circle centred on the 20-DIP kind glyph.
    const auto r = s.row_rectangles[slot];
    const UiRect rectangle{r.left + 10, r.top + 16, r.left + 34, r.top + 40};
    const UiRect clip{maximum(rectangle.left, s.body.left), maximum(rectangle.top, s.body.top),
                      minimum(rectangle.right, s.body.right), minimum(rectangle.bottom, s.body.bottom)};
    if (clip.left >= clip.right || clip.top >= clip.bottom)
        return false;
    *output = {key, hash, rectangle, clip};
    return true;
}

UiRect ui_quick_switcher_scene_status_rectangle(const UiQuickSwitcherScene& s, u32 slot) noexcept {
    if (!current(s) || slot >= s.count || slot >= kUiQuickSwitcherVisibleRows)
        return {};
    const auto& row = s.visible[slot];
    if ((row.kind != UiQuickSwitcherKind::user && row.kind != UiQuickSwitcherKind::private_channel) ||
        row.presence == UiPresence::unknown)
        return {};
    // The 24-DIP avatar of ui_quick_switcher_scene_media, with the DM header's dot.
    const auto r = s.row_rectangles[slot];
    return ui_private_row_status_rectangle({r.left + 10, r.top + 16, r.left + 34, r.top + 40});
}
bool ui_quick_switcher_scene_presence_paint(const UiQuickSwitcherScene& s, const Theme& theme,
                                            PaintStream* out) noexcept {
    if (!current(s))
        return true;
    if (!out || !paint_stream_push_clip(out, s.body.left, s.body.top, s.body.right, s.body.bottom))
        return false;
    bool ok = true;
    for (u32 i = 0; ok && i < s.count; ++i) {
        const auto dot = ui_quick_switcher_scene_status_rectangle(s, i);
        if (!ui_rect_has_area(dot))
            continue;
        // The cutout repeats the row's card behind the avatar.
        const bool card = s.first + i == s.selected || ui_widget_handle_matches(s.widgets.hovered, s.rows[i]);
        ok = ui_presence_paint(out, dot, s.visible[i].presence, card ? theme.surface_high : theme.base_lower, theme,
                               s.visible[i].mobile);
    }
    return paint_stream_pop_clip(out) && ok;
}

bool ui_quick_switcher_scene_paint(const UiQuickSwitcherScene& s, const Theme& theme, PaintStream* out,
    const UiEditGeometry* geometry, u32 dpi) noexcept {
    if (!current(s)) return true;
    if (!out || !paint_stream_push_clip(out, s.viewport.left, s.viewport.top, s.viewport.right, s.viewport.bottom)) return false;
    bool ok = paint_stream_push(out, s.viewport.left, s.viewport.top, s.viewport.right, s.viewport.bottom, theme.tokens.overlay.scrim_light) &&
        ui_surface_paint(out, s.panel, theme, UiElevation::dialog, theme.scale.radius_lg, nullptr,
                         &theme.interactive_hover) &&
        ui_edit_field_paint(out, s.field, s.text, s.content->edit, geometry, theme.base_lowest, theme, dpi, 8) &&
        ui_icon_paint(out, UiIcon::search, {s.field.left + 12, s.field.top + 14, s.field.left + 32, s.field.top + 34}, theme.text_secondary);
    const auto* close = ui_widget_manager_layout_node(s.widgets, s.close);
    if (ok && (ui_widget_handle_matches(s.widgets.hovered, s.close) || ui_widget_handle_matches(s.widgets.focused, s.close)))
        ok = surface(out, close->rectangle, theme, theme.surface_high, theme.tokens.border.focus, 6);
    ok = ok && ui_icon_paint(out, UiIcon::close, {s.panel.right - 38, s.panel.top + 14,
                                               s.panel.right - 18, s.panel.top + 34}, theme.text_secondary);
    bool body_clip = false;
    if (ok) { body_clip = paint_stream_push_clip(out, s.body.left, s.body.top, s.body.right, s.body.bottom); ok = body_clip; }
    for (u32 i = 0; ok && i < s.count; ++i) {
        const auto r = s.row_rectangles[i]; const bool selected = s.first + i == s.selected;
        const UiRect card{r.left, r.top + 2, r.right, r.bottom - 2};
        if (selected || ui_widget_handle_matches(s.widgets.hovered, s.rows[i])) ok = surface(out, card, theme, theme.surface_high,
            selected ? theme.tokens.emphasis.outline : theme.surface_high, 6);
        ok = ok && ui_icon_paint(out, icon(s.visible[i].kind), {r.left + 12, r.top + 18, r.left + 32, r.top + 38},
            selected ? theme.tokens.emphasis.outline : theme.text_secondary);
        if (ok && s.content->picker) {
            const UiRect box{r.right - 32, r.top + 18, r.right - 12, r.top + 38};
            const bool on = checked(s, s.visible[i].key);
            ok = surface(out, box, theme, on ? theme.tokens.emphasis.fill : theme.base_lowest,
                         on ? theme.tokens.emphasis.fill : s.visible[i].enabled ? theme.interactive : theme.surface_high, 5) &&
                 (!on || ui_icon_paint(out, UiIcon::check, {box.left + 3, box.top + 3, box.right - 3, box.bottom - 3},
                                       ui_surface_foreground(theme, theme.tokens.emphasis.fill)));
        } else if (ok && selected)
            ok = ui_icon_paint(out, UiIcon::chevron_right, {r.right - 26, r.top + 20, r.right - 10, r.top + 36}, theme.tokens.emphasis.outline);
    }
    if (body_clip) ok = paint_stream_pop_clip(out) && ok;
    if (ok) ok = ui_scrollbar_paint(out, ui_scrollbar_geometry(s.body, s.scroll_dips,
        s.accepted_count * kUiQuickSwitcherRowHeight), theme) &&
        paint_stream_push(out, s.panel.left + 16, s.footer.top - 4, s.panel.right - 16, s.footer.top - 3, theme.surface_high);
    if (ok && s.content->picker) {
        const bool enabled = s.content->can_confirm;
        const bool emphasis = ui_widget_handle_matches(s.widgets.hovered, s.confirm) || ui_widget_handle_matches(s.widgets.focused, s.confirm);
        ok = surface(out, s.confirm_rectangle, theme, enabled ? theme.tokens.emphasis.fill : theme.surface_high,
                     enabled && emphasis ? theme.text_primary : enabled ? theme.tokens.emphasis.fill : theme.surface_high, 8);
    }
    return paint_stream_pop_clip(out) && ok;
}
bool ui_quick_switcher_scene_text_output(const UiQuickSwitcherScene& s, const Theme& theme, UiTextOutput* out,
    const UiEditGeometry* geometry) noexcept {
    if (!current(s)) return true;
    if (!ui_text_output_valid(out)) return false;
    const auto p = s.panel;
    if (!emit(out, s, s.root, s.content->heading.bytes ? s.content->heading : utf8_literal("Quick switcher"),
        {p.left + 20, p.top + 10, p.right - 52, p.top + 38}, p, theme.text_primary, text_style_bold) ||
        !ui_edit_field_text_output(out, {&s.widgets, s.edit}, s.text, s.content->edit, geometry,
            s.content->picker ? utf8_literal("Type the username of a friend") : utf8_literal("Where would you like to go?"),
            theme) ||
        !emit(out, s, s.root, s.content->hint.bytes ? s.content->hint : utf8_literal("@ people   # channels   * servers"),
            s.hint, p, theme.text_secondary, text_style_subtext)) return false;
    for (u32 i = 0; i < s.count; ++i) {
        const auto r = s.row_rectangles[i]; const bool selected = s.first + i == s.selected;
        const float metadata = s.visible[i].metadata.bytes && ui_rect_width(r) >= 380 ? 92.f : 0.f;
        const float right = r.right - (selected || s.content->picker ? 40.f : 12.f) - metadata;
        if (!emit(out, s, s.rows[i], s.visible[i].label, {r.left + 44, r.top + 7, right, r.top + 29},
            s.body, s.visible[i].enabled ? theme.text_primary : theme.text_secondary,
            selected ? text_style_bold : text_style_none) ||
            !emit(out, s, s.rows[i], s.visible[i].detail, {r.left + 44, r.top + 29, r.right - 40, r.bottom - 5},
                s.body, theme.text_secondary, text_style_subtext)) return false;
        if (metadata && !emit(out, s, s.rows[i], s.visible[i].metadata,
            {right + 8, r.top + 7, r.right - (selected ? 36.f : 12.f), r.top + 29},
            s.body, theme.text_secondary, text_style_subtext, UiTextAlignment::end)) return false;
    }
    if (!s.accepted_count) {
        const float middle = s.body.top + ui_rect_height(s.body) * .4f;
        const auto heading = s.content->phase == UiQuickSwitcherPhase::no_matches ? utf8_literal("No matches") :
            s.content->phase == UiQuickSwitcherPhase::unavailable ? utf8_literal("Nothing to switch to") : utf8_literal("Start typing to jump");
        const auto detail = s.content->status.bytes ? s.content->status :
            s.content->phase == UiQuickSwitcherPhase::no_matches ? utf8_literal("Try another name or a search prefix.") : utf8_literal("Search your channels, servers and people.");
        if (!emit(out, s, s.body_owner, heading, {s.body.left + 12, middle - 22, s.body.right - 12, middle + 2},
                s.body, theme.text_primary, text_style_bold, UiTextAlignment::center) ||
            !emit(out, s, s.body_owner, detail, {s.body.left + 12, middle + 4, s.body.right - 12, middle + 28},
                s.body, theme.text_secondary, text_style_subtext, UiTextAlignment::center)) return false;
    }
    if (s.content->picker)
        return emit(out, s, s.root, s.content->status, {s.footer.left, s.footer.top, s.confirm_rectangle.left - 12,
                    s.footer.bottom}, p, theme.text_secondary, text_style_subtext) &&
               emit(out, s, s.confirm, s.content->action, {s.confirm_rectangle.left + 8, s.confirm_rectangle.top,
                    s.confirm_rectangle.right - 8, s.confirm_rectangle.bottom}, p,
                    s.content->can_confirm ? ui_surface_foreground(theme, theme.tokens.emphasis.fill) : theme.text_secondary,
                    text_style_bold, UiTextAlignment::center);
    const float third = ui_rect_width(s.footer) / 3.f;
    const Utf8View hints[]{utf8_literal("↑↓ Select"), utf8_literal("Enter Open"), utf8_literal("Esc Close")};
    for (u32 i = 0; i < 3; ++i)
        if (!emit(out, s, s.root, hints[i], {s.footer.left + third * i, s.footer.top,
            s.footer.left + third * (i + 1), s.footer.bottom}, p, theme.text_secondary,
            text_style_subtext, UiTextAlignment::center)) return false;
    return true;
}
} // namespace maicord
