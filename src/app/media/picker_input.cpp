#include "picker_internal.hpp"

namespace maicord::app_detail {

MessageComposer* application_media_picker_edit_model() noexcept {
    return g_app.media_picker ? &g_app.media_picker->edit : nullptr;
}
bool application_media_picker_edit_focused() noexcept {
    return application_media_picker_active() && g_app.media_picker->focused;
}
UiRect application_media_picker_edit_bounds() noexcept {
    return application_media_picker_edit_focused() ? g_app.media_picker->scene.text : UiRect{};
}
bool application_media_picker_edit_changed(bool content) noexcept {
    auto* p = g_app.media_picker;
    if (!p)
        return false;
    p->caret_visible = p->focused;
    p->blink_at =
        p->focused ? deadline_after(clock_monotonic_milliseconds(), kDefaultCaretBlinkMilliseconds) : 0;
    if (content) {
        revoke_capture(*p);
        if (p->kind == UiMediaPickerKind::gif && p->favorites_view) {
            if (!projection(*p))
                return false;
            ++p->source.revision;
            p->scene.scroll_dips = 0;
        } else if (p->kind == UiMediaPickerKind::gif)
            request(*p,
                    p->edit.bytes ? MediaCatalogOperation::gif_search
                                  : MediaCatalogOperation::gif_categories,
                    true);
        else if (p->kind == UiMediaPickerKind::emoji) {
            if (!p->emoji.storage.base || !emoji_projection(*p))
                emoji_unavailable(*p);
            p->scene.scroll_dips = 0;
        } else {
            if (!projection(*p))
                return false;
            ++p->source.revision;
            p->scene.scroll_dips = 0;
        }
    }
    return repaint();
}
u64 application_media_picker_next_wake() noexcept {
    const auto* p = g_app.media_picker;
    return p && p->active && p->wanted && !p->submitted_id && !p->admission_busy &&
                   g_app.message_history_request_owner == MessageHistoryRequestOwner::none
               ? p->due_at
               : 0;
}
u64 application_media_picker_caret_wake() noexcept {
    const auto* p = g_app.media_picker;
    const u64 paging = p && ui_media_picker_scene_paging(p->scene) ? p->page_at : 0;
    return earliest_deadline(
        application_media_picker_active() ? earliest_deadline(p->focused ? p->blink_at : 0, p->hover_at) : 0,
        paging);
}
void application_media_picker_tick(u64 now) noexcept {
    auto* p = g_app.media_picker;
    if (p && application_media_picker_edit_focused() && deadline_due(now, p->blink_at)) {
        p->caret_visible = !p->caret_visible;
        p->blink_at = deadline_after(now, kDefaultCaretBlinkMilliseconds);
        repaint();
    }
    if (p && application_media_picker_active() && deadline_due(now, p->hover_at)) {
        p->hover_at = 0;
        repaint();
    }
    if (p && ui_media_picker_scene_paging(p->scene) && deadline_due(now, p->page_at)) {
        if (ui_media_picker_scene_page_tick(&p->scene, now))
            repaint(true);
        p->page_at = ui_media_picker_scene_paging(p->scene) ? deadline_after(now, kUiTransitionFrameMs) : 0;
    }
}
bool application_media_picker_preview_request(u32 cell, AccountMediaCatalogPreviewRequest* out) noexcept {
    const auto* p = g_app.media_picker;
    if (!out || !shown(p) || p->kind == UiMediaPickerKind::emoji)
        return false;
    const CatalogBank* bank = nullptr;
    u32 local = 0;
    MediaCatalogItem temporary{};
    const MediaCatalogItem* item = nullptr;
    if (cell >= p->scene.cell_count) {
        // A sticker pack's rail icon: its cover sticker's preview.
        UiMediaPickerRailMediaSlot rail{};
        if (p->kind != UiMediaPickerKind::sticker || !p->packs ||
            !ui_media_picker_scene_rail_media(p->scene, cell - p->scene.cell_count, &rail) || !rail.asset_id)
            return false;
        for (u32 i = 0; !item && i < p->section_count; ++i)
            if (p->sections[i].asset_id == rail.asset_id && p->section_cover[i] < p->packs->count) {
                bank = p->packs;
                local = p->section_cover[i];
                item = &bank->items[local];
            }
    } else {
        UiMediaPickerMediaSlot slot{};
        if (!ui_media_picker_scene_media(p->scene, cell, &slot) || slot.target.source_index >= p->source.count)
            return false;
        item = item_at(*p, slot.target.source_index, &bank, &local, &temporary);
    }
    if (!item || !preview_admitted(*bank, local))
        return false;
    reset_value(out);
    out->url = item->preview_url;
    out->key = catalog_key(bank->first_id + local, bank->generation, *item);
    out->catalog_scope = p->scope;
    out->catalog_generation = bank->generation;
    // Fit within 512 px keeping the aspect ratio the proxy sizes from.
    u64 width = item->width ? item->width : 256, height = item->height ? item->height : 256;
    const u64 longest = maximum(width, height);
    if (longest > 512) {
        width = maximum<u64>(1, width * 512 / longest);
        height = maximum<u64>(1, height * 512 / longest);
    }
    out->width = static_cast<u16>(width);
    out->height = static_cast<u16>(height);
    out->format = format(*item);
    return true;
}
bool application_media_picker_preview_current(MediaAssetKey key, u64 scope, u32 generation) noexcept {
    const auto* p = g_app.media_picker;
    if (!p || p->scope != scope || key.variant != generation || !shown(p))
        return false;
    return application_media_picker_preview_visible(key);
}
bool application_media_picker_preview_visible(MediaAssetKey key) noexcept {
    const auto* p = g_app.media_picker;
    if (!shown(p))
        return false;
    for (u32 i = 0; i < p->scene.cell_count + p->scene.rail_count; ++i) {
        AccountMediaCatalogPreviewRequest request{};
        if (application_media_picker_preview_request(i, &request) &&
            media_asset_key_equal(request.key, key))
            return true;
    }
    return false;
}

bool application_media_picker_preview_owner_current(MediaAssetKey key, u64 scope, u32 generation) noexcept {
    const auto* p = g_app.media_picker;
    if (!shown(p) || p->kind == UiMediaPickerKind::emoji ||
        p->scope != scope || !key.id || !generation || key.variant != generation)
        return false;
    const CatalogBank* banks[]{p->kind == UiMediaPickerKind::gif ? (p->favorites_view ? &p->saved : gif_bank(*p))
                                                                 : p->guild,
                               p->kind == UiMediaPickerKind::sticker
                                   ? p->packs
                                   : (landing(*p) && p->favorites_enabled ? &p->favorites_tile : nullptr)};
    for (const auto* bank : banks) {
        if (!bank || bank->generation != generation || key.id < bank->first_id ||
            key.id - bank->first_id >= bank->count)
            continue;
        const u32 index = static_cast<u32>(key.id - bank->first_id);
        if (!preview_admitted(*bank, index))
            return false;
        MediaCatalogItem temporary{};
        if (bank == &p->saved) {
            const auto saved = application_gif_favorites_view();
            if (index >= saved.records.count)
                return false;
            favorite_item(saved.records[index], &temporary);
        } else if (bank == &p->favorites_tile)
            temporary = p->tile_item;
        const auto& item = bank == &p->saved || bank == &p->favorites_tile ? temporary : bank->items[index];
        return media_asset_key_equal(key, catalog_key(key.id, generation, item));
    }
    return false;
}

bool application_media_picker_preview_rejected(MediaAssetKey key, u64 scope, u32 generation) noexcept {
    auto* p = g_app.media_picker;
    if (!application_media_picker_preview_owner_current(key, scope, generation))
        return false;
    CatalogBank* banks[]{p->gif, p->categories, p->guild, p->packs, &p->saved, &p->favorites_tile};
    for (auto* bank : banks) {
        if (!bank || bank->generation != generation || key.id < bank->first_id ||
            key.id - bank->first_id >= bank->count || !bank->preview_mask)
            continue;
        const u32 index = static_cast<u32>(key.id - bank->first_id);
        bank->preview_mask[index >> 3] &= static_cast<u8>(~(1u << (index & 7)));
        ++p->source.revision;
        application_request_frame();
        return true;
    }
    return false;
}

namespace {

bool picker_edit_ready(const TextInputTarget& target) noexcept {
    const auto* p = g_app.media_picker;
    return p && application_media_picker_edit_focused() && target.context == &p->edit &&
           target.identity == p->edit_identity;
}
using PickerEditInput = ForwardedInput<picker_edit_ready, text_input_single_line_forbidden>;
bool scroll_read(void* opaque, u64 scope, UiScrollRange* output, bool rail) noexcept {
    if (!output || opaque != g_app.media_picker || !application_media_picker_active())
        return false;
    const auto& p = *g_app.media_picker;
    if (p.scope != scope || !ui_media_picker_scene_current(p.scene))
        return false;
    const auto viewport = rail ? p.scene.rail : p.scene.body;
    if (!ui_rect_has_area(viewport))
        return false;
    const auto scrollbar = ui_media_picker_scene_scrollbar(p.scene, rail);
    if (!scrollbar.visible)
        return false;
    *output = {viewport,
               rail ? p.scene.rail_scroll_dips : p.scene.scroll_dips,
               scrollbar.maximum_scroll,
               kUiScrollLineDips,
               1,
               0,
               (static_cast<u64>(p.generation) << 32) | p.source.revision};
    return true;
}
bool body_scroll_read(void* opaque, u64 scope, UiScrollRange* out) noexcept {
    return scroll_read(opaque, scope, out, false);
}
bool rail_scroll_read(void* opaque, u64 scope, UiScrollRange* out) noexcept {
    return scroll_read(opaque, scope, out, true);
}
bool scroll_write(void* opaque, u64 scope, double offset, bool* changed, bool rail) noexcept {
    UiScrollRange range{};
    if (!changed || !scroll_read(opaque, scope, &range, rail))
        return false;
    auto& p = *g_app.media_picker;
    *changed = ui_media_picker_scene_scroll(&p.scene, static_cast<float>(offset - range.offset), rail);
    if (*changed) {
        p.hover_at = 0;
        if (p.focused)
            ui_widget_manager_focus(&p.scene.widgets, p.scene.edit);
        application_composer_text_input_notify_layout();
        application_request_frame();
    }
    return true;
}
bool body_scroll_write(void* opaque, u64 scope, double offset, bool* changed) noexcept {
    return scroll_write(opaque, scope, offset, changed, false);
}
bool rail_scroll_write(void* opaque, u64 scope, double offset, bool* changed) noexcept {
    return scroll_write(opaque, scope, offset, changed, true);
}

} // namespace
namespace picker_detail {

void revoke_capture(ApplicationMediaPicker& p) noexcept {
    const bool was_resizing = p.resizing;
    p.captured = p.dragging = p.scrollbar_dragging = p.resizing = false;
    ui_media_picker_scene_set_scrollbar_state(&p.scene, false, UiScrollbarVisualState::normal);
    ui_media_picker_scene_cancel_capture(&p.scene);
    application_release_capture();
    if (was_resizing) {
        const u16 width = ui_splitter_persisted_width(p.preferred_width);
        if (g_app.preferences.expression_picker_width != width) {
            g_app.preferences.expression_picker_width = width;
            application_settings_commit_preferences();
        }
    }
}

} // namespace picker_detail
namespace {

UiMediaPickerTarget focused_target(const ApplicationMediaPicker& p) noexcept {
    const auto* node = ui_widget_manager_layout_node(p.scene.widgets, p.scene.widgets.focused);
    if (!node)
        return {};
    const auto r = node->rectangle;
    return ui_media_picker_scene_hit_test(p.scene, (r.left + r.right) * .5f, (r.top + r.bottom) * .5f);
}
void focus_selected(ApplicationMediaPicker& p) noexcept {
    const auto cells = ui_media_picker_scene_cells(p.scene);
    for (u32 i = 0; i < cells.count; ++i) {
        const auto& cell = cells[i];
        if (cell.source_index != p.scene.selected_index)
            continue;
        ui_widget_manager_focus(&p.scene.widgets, cell.widget);
        break;
    }
    g_app.context_focus = &p.scene.widgets;
}
// Moves keyboard focus from the search field to the first result.
void focus_results(ApplicationMediaPicker& p) noexcept {
    focus(p, false);
    ui_media_picker_scene_edge_selection(&p.scene, false);
    focus_selected(p);
}
bool edit_pointer(ApplicationMediaPicker& p, float x, float y, bool extend) noexcept {
    u32 position = 0, byte = 0;
    if (!text_edit_layout_hit_test(g_app.renderer.media_picker_layout, x, y, &position) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(p.edit), position, &byte))
        return false;
    message_composer_set_selection(&p.edit, extend ? p.edit.anchor : byte, byte);
    return application_media_picker_edit_changed(false);
}
// Scrolls the dragged bar's pane so its thumb follows the pointer at y.
bool scrollbar_follow(ApplicationMediaPicker& p, const UiScrollbarGeometry& geometry, float y) noexcept {
    const float next = ui_scrollbar_scroll_from_pointer(geometry, y, p.scrollbar_grab);
    return ui_media_picker_scene_scroll(&p.scene, next - p.scene.scroll_dips);
}

} // namespace

TextInputTarget application_media_picker_edit_target() noexcept {
    const auto* p = g_app.media_picker;
    return p ? TextInputTarget{&g_app.media_picker->edit, &PickerEditInput::operations, p->edit_identity}
             : TextInputTarget{};
}
void application_media_picker_edit_cancel_native_group(const TextInputTarget& target) noexcept {
    const auto* p = g_app.media_picker;
    if (!p || target.operations != &PickerEditInput::operations || target.context != &p->edit ||
        target.identity != p->edit_identity)
        return;
    PickerEditInput::abandon(target);
}

bool application_media_picker_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto* p = g_app.media_picker;
    if (p && ui_input_consume_key_tail(&p->held_key, {kind, key, repeated}))
        return true;
    if (!application_media_picker_active())
        return false;
    const bool raw_control = application_control_down();
    const bool alt = application_alt_down();
    const bool control = raw_control && !alt, shift = application_shift_down();
    if (kind == UiInputKind::character) {
        if (!p->focused || control || (alt && !raw_control) || key < 32 || (key >= 127 && key <= 159) ||
            key == 0x2028 || key == 0x2029 || key > 0xffff)
            return true;
        if (!p->edit.history)
            message_composer_enable_history(&p->edit, 2048);
        if (message_composer_insert_utf16(&p->edit, static_cast<u16>(key),
                                          clock_monotonic_milliseconds()) == MessageComposerResult::changed)
            application_media_picker_edit_changed(true);
        return true;
    }
    if (kind == UiInputKind::key_up)
        return true;
    if (kind != UiInputKind::key_down)
        return false;
    if (key == VK_ESCAPE) {
        p->held_key = key;
        application_media_picker_close();
        return true;
    }
    if (key == VK_TAB) {
        p->held_key = key;
        focus(*p, false);
        const auto widget = ui_widget_manager_move_focus(&p->scene.widgets, shift);
        if (widget.generation && widget.slot == p->scene.edit.slot)
            focus(*p, true);
        else
            g_app.context_focus = &p->scene.widgets;
        repaint();
        // Content focus changes keep the geometry generation, but the scene
        // reconciles selection focus. Preserve explicit Tab navigation after
        // that reconciliation, including Shift+Tab back to the tab strip.
        if (!p->focused && widget.generation)
            ui_widget_manager_focus(&p->scene.widgets, widget);
        return true;
    }
    if (key == VK_RETURN || (key == VK_SPACE && !p->focused)) {
        p->held_key = key;
        if (p->favorites_view && p->phase == UiMediaPickerPhase::failed) {
            application_gif_favorites_set_visible(false);
            application_gif_favorites_open();
            repaint();
        } else if (p->phase == UiMediaPickerPhase::failed) {
            request(*p, p->operation);
            repaint();
        } else if (p->focused) {
            if (p->kind == UiMediaPickerKind::gif && !p->favorites_view)
                request(*p, p->edit.bytes ? MediaCatalogOperation::gif_search
                                          : MediaCatalogOperation::gif_categories);
            else
                focus_results(*p);
            repaint();
        } else
            activate(*p, focused_target(*p));
        return true;
    }
    if (!p->focused) {
        if (key == 'F' && !control && !alt) {
            p->held_key = key;
            activate(*p, ui_media_picker_scene_selected_favorite_target(p->scene));
            return true;
        }
        bool changed = false;
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)
            changed = ui_media_picker_scene_move_selection(&p->scene,
                                                           key == VK_LEFT    ? -1
                                                           : key == VK_RIGHT ? 1
                                                                             : 0,
                                                           key == VK_UP     ? -1
                                                           : key == VK_DOWN ? 1
                                                                            : 0);
        else if (key == VK_PRIOR || key == VK_NEXT)
            changed = ui_media_picker_scene_page_selection(&p->scene, key == VK_PRIOR);
        else if (key == VK_HOME || key == VK_END)
            // Home/End reach the row's ends; with Ctrl the grid's.
            changed = control ? ui_media_picker_scene_edge_selection(&p->scene, key == VK_END)
                              : ui_media_picker_scene_row_edge_selection(&p->scene, key == VK_END);
        if (changed) {
            focus_selected(*p);
            repaint();
        }
        return true;
    }
    if (!p->edit.history)
        message_composer_enable_history(&p->edit, 2048);
    const u32 revision = p->edit.text_revision;
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (control && (key == 'C' || key == 'X')) {
        const u32 first = minimum(p->edit.anchor, p->edit.caret),
                  last = maximum(p->edit.anchor, p->edit.caret);
        if (last > first && clipboard_write_utf8(g_app.window, {p->edit.text + first, last - first}) &&
            key == 'X')
            result = message_composer_replace_range(&p->edit, first, last, {});
    } else if (control && key == 'V') {
        char text[kMediaCatalogMaximumQueryBytes];
        const auto read = clipboard_read_utf8(g_app.window, {text, sizeof(text)});
        bool valid = read.ok();
        for (u32 i = 0; valid && i < read.bytes;) {
            const auto scalar = utf8_decode_one(text + i, read.bytes - i);
            valid = scalar.valid && scalar.codepoint >= 32 &&
                    !(scalar.codepoint >= 127 && scalar.codepoint <= 159) && scalar.codepoint != 0x2028 &&
                    scalar.codepoint != 0x2029;
            if (valid)
                i += scalar.width;
        }
        if (valid)
            result = message_composer_insert_utf8(&p->edit, {text, read.bytes});
    } else if (key == VK_DOWN) {
        focus_results(*p);
        repaint();
        return true;
    } else {
        if (key == VK_BACK)
            p->held_key = key;
        message_composer_edit_key(&p->edit, key, control, shift, &result);
    }
    if (result == MessageComposerResult::changed)
        application_media_picker_edit_changed(revision != p->edit.text_revision);
    return true;
}

namespace {

// Inline results take the pointer only over their panel (the composer keeps
// hover highlights a result and drops the keyboard selection, a click sends it
// or pages the strip, and the wheel scrolls the GIF strip sideways (vertical
// wheel motion becomes horizontal scrolling).
bool inline_pointer(ApplicationMediaPicker& p, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!shown(&p) || !ui_media_picker_scene_current(p.scene))
        return false;
    if (message == WM_MOUSELEAVE) {
        if (ui_widget_manager_hover(&p.scene.widgets, ui_widget_invalid_handle()))
            repaint(true);
        return false;
    }
    if (!application_pointer_message(message))
        return false;
    UiPoint point{};
    if (message == WM_SETCURSOR) {
        if (!application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point))
            return false;
    } else if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        if (!application_wheel_point(lparam, &point))
            return false;
    } else
        point = {client_dip_x(lparam), client_dip_y(lparam)};
    const float x = point.x, y = point.y;
    const bool inside = ui_rect_contains(ui_rect_intersection(p.scene.panel, p.scene.viewport), x, y);
    if (message == WM_SETCURSOR) {
        if (!inside)
            return false;
        application_set_cursor(ui_media_picker_scene_cursor(p.scene, x, y));
        return true;
    }
    if (message == WM_MOUSEMOVE) {
        if (!inside) {
            if (ui_widget_manager_hover(&p.scene.widgets, ui_widget_invalid_handle()))
                repaint(true);
            return p.captured;
        }
        if (ui_media_picker_scene_hover(&p.scene, x, y, clock_monotonic_milliseconds())) {
            if (ui_media_picker_scene_hit_test(p.scene, x, y).action == UiMediaPickerAction::item)
                ui_media_picker_scene_clear_selection(&p.scene);
            repaint(true);
        }
        return true;
    }
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
        if (!inside)
            return false;
        revoke_capture(p);
        ui_media_picker_scene_pointer_down(&p.scene, x, y);
        p.captured = true;
        application_capture_pointer();
        application_request_frame();
        return true;
    }
    if (message == WM_LBUTTONUP) {
        if (!p.captured && !inside)
            return false;
        const auto selected = inside ? ui_media_picker_scene_pointer_up(&p.scene, x, y) : UiMediaPickerTarget{};
        revoke_capture(p);
        if (selected.action != UiMediaPickerAction::none)
            activate(p, selected);
        else
            application_request_frame();
        return true;
    }
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        if (!inside)
            return false;
        const u32 lines = application_wheel_scroll_lines();
        const float notches = application_wheel_notches(wparam);
        // Wheel up scrolls back; tilting right scrolls forward.
        const float delta = (message == WM_MOUSEWHEEL ? -notches : notches) *
                            (lines == WHEEL_PAGESCROLL ? ui_rect_width(p.scene.body) : lines * kUiScrollLineDips);
        if (p.kind == UiMediaPickerKind::gif && ui_media_picker_scene_scroll(&p.scene, delta))
            repaint(true);
        return true;
    }
    return inside && (message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
                      message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
                      message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
                      message == WM_XBUTTONDBLCLK);
}

} // namespace

bool application_media_picker_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* p = g_app.media_picker;
    if (!p)
        return false;
    if (message == WM_CANCELMODE) {
        revoke_capture(*p);
        return false;
    }
    if (message == WM_KILLFOCUS) {
        focus(*p, false);
        revoke_capture(*p);
        p->hover_at = 0;
        ui_widget_manager_hover(&p->scene.widgets, ui_widget_invalid_handle());
        application_request_frame();
        return false;
    }
    if (message == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lparam) != g_app.window) {
        revoke_capture(*p);
        return false;
    }
    if (p->inline_mode)
        return inline_pointer(*p, message, wparam, lparam);
    if (!application_media_picker_active())
        return false;
    if (message == WM_MOUSELEAVE) {
        p->hover_at = 0;
        const bool scrollbar_changed =
            !p->scrollbar_dragging &&
            ui_media_picker_scene_set_scrollbar_state(&p->scene, false, UiScrollbarVisualState::normal);
        if (ui_widget_manager_hover(&p->scene.widgets, ui_widget_invalid_handle()) || scrollbar_changed)
            repaint(true);
        return false;
    }
    if (!application_pointer_message(message) && !p->scrollbar_dragging)
        return false;
    UiPoint point{};
    if (message == WM_SETCURSOR) {
        if (!application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point))
            return false;
    } else if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL || message == WM_CONTEXTMENU) {
        if (message == WM_CONTEXTMENU && lparam == -1)
            return true;
        if (!application_wheel_point(lparam, &point))
            return false;
    } else
        point = {client_dip_x(lparam), client_dip_y(lparam)};
    const float scale = g_app.renderer.dpi ? 96.f / g_app.renderer.dpi : 1;
    const float x = point.x, y = point.y;
    const bool inside = ui_rect_contains(ui_rect_intersection(p->scene.panel, p->scene.viewport), x, y);
    const bool resize_hit = !p->reaction_message_id &&
                            ui_rect_contains(p->scene.viewport, x, y) &&
                            ui_splitter_hit(p->scene.panel, UiSplitterEdge::left, x, y);
    if (p->resizing) {
        if (message == WM_SETCURSOR) {
            application_set_cursor(UiCursor::resize_we);
            return true;
        }
        if (message == WM_MOUSEMOVE || message == WM_LBUTTONUP) {
            const float maximum_width = p->resize_fixed_edge - p->scene.viewport.left -
                                        p->scene.style.metrics.viewport_inset;
            const UiSplitterLimits limits{minimum(p->scene.style.metrics.panel_width, maximum_width),
                                           maximum_width};
            p->preferred_width = ui_splitter_drag_width(
                p->resize_fixed_edge, UiSplitterEdge::left, x - p->resize_pointer_offset, limits);
            if (message == WM_LBUTTONUP)
                revoke_capture(*p);
            repaint(true);
            return true;
        }
    }
    if ((message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) && resize_hit) {
        revoke_capture(*p);
        focus(*p, false);
        ui_scroll_host_cancel(&g_app.scroll_host);
        p->resize_fixed_edge = p->scene.panel.right;
        p->resize_pointer_offset = x - p->scene.panel.left;
        p->resizing = p->captured = true;
        p->hover_at = 0;
        application_capture_pointer();
        application_set_cursor(UiCursor::resize_we);
        return true;
    }
    if (message == WM_SETCURSOR && resize_hit) {
        application_set_cursor(UiCursor::resize_we);
        return true;
    }
    const auto hit = inside ? ui_media_picker_scene_hit_test(p->scene, x, y) : UiMediaPickerTarget{};
    const auto geometry = ui_media_picker_scene_scrollbar(p->scene);
    // The category rail remains wheel/keyboard scrollable without an overlay
    // thumb stealing space and hits from its icons. Only the content has a thumb.
    if (p->scrollbar_dragging) {
        const u64 revision = (static_cast<u64>(p->generation) << 32) | p->source.revision;
        if (p->scrollbar_scope != p->scope || p->scrollbar_revision != revision || !geometry.visible) {
            revoke_capture(*p);
            application_request_frame();
            return true;
        }
        if (message == WM_MOUSEMOVE || message == WM_LBUTTONUP) {
            const bool changed = scrollbar_follow(*p, geometry, y);
            if (message == WM_LBUTTONUP)
                revoke_capture(*p);
            if (changed)
                repaint(true);
            else
                application_request_frame();
            return true;
        }
    }
    const bool primary_press = message == WM_LBUTTONDOWN ||
                               (message == WM_LBUTTONDBLCLK && hit.action != UiMediaPickerAction::edit);
    if (primary_press && inside && ui_scrollbar_track_hit(geometry, x, y)) {
        focus(*p, false);
        ui_scroll_host_cancel(&g_app.scroll_host);
        p->scrollbar_grab = ui_scrollbar_thumb_hit(geometry, x, y) ? y - geometry.thumb.top
                                                                   : ui_rect_height(geometry.thumb) * .5f;
        p->scrollbar_scope = p->scope;
        p->scrollbar_revision = (static_cast<u64>(p->generation) << 32) | p->source.revision;
        p->scrollbar_dragging = p->captured = true;
        ui_media_picker_scene_set_scrollbar_state(&p->scene, false, UiScrollbarVisualState::pressed);
        scrollbar_follow(*p, geometry, y);
        application_capture_pointer();
        repaint(true);
        return true;
    }
    if (message == WM_SETCURSOR && inside) {
        application_set_cursor(ui_media_picker_scene_cursor(p->scene, x, y));
        return true;
    }
    if ((message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) && inside) {
        const bool rail = ui_rect_contains(p->scene.rail, x, y);
        if (message == WM_MOUSEWHEEL && (rail || ui_rect_contains(p->scene.body, x, y))) {
            const u32 lines = application_wheel_scroll_lines();
            ui_scroll_host_wheel(&g_app.scroll_host,
                                 {p, p->scope, rail ? rail_scroll_read : body_scroll_read,
                                  rail ? rail_scroll_write : body_scroll_write},
                                 {x, y}, static_cast<short>(HIWORD(wparam)), lines, lines == WHEEL_PAGESCROLL,
                                 application_now_us(), scale);
            application_request_frame();
        }
        return true;
    }
    if (primary_press) {
        if (!inside) {
            const auto control = message_composer_scene_hit_test(g_app.message_composer_scene, x, y);
            // The launcher activates on release. Keep the current picker alive
            // until then so its own same-kind toggle can close it, or a different
            // launcher can switch tabs without a close/reopen cycle.
            if (control == MessageComposerControl::gif || control == MessageComposerControl::sticker ||
                control == MessageComposerControl::emoji)
                return false;
            application_media_picker_close();
            return false;
        }
        application_focus_window();
        revoke_capture(*p);
        if (hit.action == UiMediaPickerAction::edit) {
            focus(*p, true);
            edit_pointer(*p, x, y, (wparam & MK_SHIFT) != 0);
            p->dragging = true;
        } else {
            focus(*p, false);
            if (hit.action != UiMediaPickerAction::none) {
                const auto widget = ui_widget_manager_hit_test(p->scene.widgets, x, y);
                ui_widget_manager_focus(&p->scene.widgets, widget);
                ui_media_picker_scene_pointer_down(&p->scene, x, y);
            }
            g_app.context_focus = &p->scene.widgets;
            application_request_frame();
        }
        p->captured = true;
        application_capture_pointer();
        return true;
    }
    if (message == WM_MOUSEMOVE && p->dragging) {
        edit_pointer(*p, x, y, true);
        return true;
    }
    if (message == WM_MOUSEMOVE) {
        const bool scrollbar_changed = ui_media_picker_scene_set_scrollbar_state(
            &p->scene, false,
            ui_scrollbar_thumb_hit(geometry, x, y) ? UiScrollbarVisualState::hovered
                                                   : UiScrollbarVisualState::normal);
        if (scrollbar_changed)
            application_request_frame();
        if (inside) {
            if (ui_media_picker_scene_hover(&p->scene, x, y, clock_monotonic_milliseconds())) {
                p->hover_at = hit.action == UiMediaPickerAction::item ||
                                      hit.action == UiMediaPickerAction::category ||
                                      hit.action == UiMediaPickerAction::section ||
                                      hit.action == UiMediaPickerAction::section_jump
                                  ? deadline_after(clock_monotonic_milliseconds(), 500)
                                  : 0;
                repaint(true);
            }
        } else {
            p->hover_at = 0;
            if (ui_widget_manager_hover(&p->scene.widgets, ui_widget_invalid_handle()))
                repaint(true);
        }
        return inside || p->captured;
    }
    if (message == WM_LBUTTONUP && (p->captured || inside)) {
        const bool drag = p->dragging;
        const auto selected =
            !drag && inside ? ui_media_picker_scene_pointer_up(&p->scene, x, y) : UiMediaPickerTarget{};
        revoke_capture(*p);
        if (selected.action != UiMediaPickerAction::none)
            activate(*p, selected);
        return true;
    }
    if (message == WM_LBUTTONDBLCLK && hit.action == UiMediaPickerAction::edit) {
        focus(*p, true);
        edit_pointer(*p, x, y, false);
        message_composer_select_word(&p->edit, p->edit.caret);
        application_media_picker_edit_changed(false);
        return true;
    }
    return inside && (message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
                      message == WM_RBUTTONDBLCLK || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP ||
                      message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ||
                      message == WM_XBUTTONDBLCLK || message == WM_LBUTTONDBLCLK);
}

} // namespace maicord::app_detail
