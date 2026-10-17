#include "maicord/ui/download_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool current(const UiDownloadScene& s) noexcept {
    return s.active && s.content && s.content->revision == s.content_revision &&
        (!s.source || (s.source->revision == s.source_revision && s.source->count == s.accepted_count &&
        (!s.source->current || s.source->current(s.source->context))));
}
bool terminal(UiDownloadPhase phase) noexcept {
    return phase == UiDownloadPhase::saved || phase == UiDownloadPhase::failed || phase == UiDownloadPhase::cancelled;
}
UiDownloadAction primary_action(const UiDownloadRow& r) noexcept {
    if (!terminal(r.phase) && r.cancel) return UiDownloadAction::cancel;
    if ((r.phase == UiDownloadPhase::failed || r.phase == UiDownloadPhase::cancelled) && r.retry)
        return UiDownloadAction::retry;
    if (r.phase == UiDownloadPhase::saved && r.reveal) return UiDownloadAction::reveal;
    return UiDownloadAction::none;
}
Utf8View primary_label(const UiDownloadRow& r) noexcept {
    switch (primary_action(r)) {
    case UiDownloadAction::cancel: return utf8_literal("Cancel");
    case UiDownloadAction::retry: return utf8_literal("Retry");
    case UiDownloadAction::reveal: return utf8_literal("Show in folder");
    default: return {};
    }
}
Utf8View phase_label(UiDownloadPhase phase) noexcept {
    switch (phase) {
    case UiDownloadPhase::queued: return utf8_literal("Queued");
    case UiDownloadPhase::preparing: return utf8_literal("Preparing download…");
    case UiDownloadPhase::downloading: return utf8_literal("Saving…");
    case UiDownloadPhase::committing: return utf8_literal("Finishing save…");
    case UiDownloadPhase::saved: return utf8_literal("Saved");
    case UiDownloadPhase::failed: return utf8_literal("Could not save this file");
    case UiDownloadPhase::cancelled: return utf8_literal("Cancelled");
    }
    return {};
}
Color phase_color(const Theme& t, UiDownloadPhase phase) noexcept {
    return ui_foreground_color(t, phase == UiDownloadPhase::failed ? t.notification :
        phase == UiDownloadPhase::saved ? t.syntax_string :
        phase == UiDownloadPhase::cancelled ? t.text_secondary : t.tokens.emphasis.outline);
}
bool valid_view(Utf8View text, u32 maximum_bytes) noexcept {
    return text.bytes <= maximum_bytes && utf8_validate(text).valid;
}
bool valid_row(const UiDownloadRow& r) noexcept {
    return r.id && r.filename.bytes && valid_view(r.filename, 1024) && valid_view(r.source, 512) &&
        valid_view(r.status, 1024) && r.phase <= UiDownloadPhase::cancelled;
}
bool initialize(UiDownloadScene& s) noexcept {
    if (s.storage.arena.base) return true;
    if (!ui_fixed_widget_storage_initialize(&s.storage, kUiDownloadWidgetCapacity,
        sizeof(UiDownloadRow) * kUiDownloadMaximumRows, alignof(UiDownloadRow)) ||
        !ui_widget_manager_initialize(&s.widgets, s.storage.widgets, s.storage.layout_nodes)) return false;
    s.accepted = ui_fixed_widget_storage_sidecar<UiDownloadRow>(s.storage);
    const auto add = [&](UiWidgetHandle parent, UiWidgetKind kind, Utf8View name) noexcept {
        auto style = ui_layout_style();
        style.flags |= ui_layout_node_clip_children | ui_layout_node_allow_overlap | ui_layout_node_allow_zero_size;
        return ui_widget_manager_add_prevalidated(&s.widgets, parent,
            {s.widgets.count + 1, kind, ui_widget_none, {name, style}});
    };
    const auto root = add(ui_widget_invalid_handle(), UiWidgetKind::container, utf8_literal("Downloads"));
    if (!root.ok()) return false;
    s.root = root.handle;
    auto* node = ui_layout_engine_resolve(&s.widgets.layout, s.widgets.storage[s.root.slot].layout);
    node->spec.style.width = node->spec.style.height = ui_layout_fill();
    const auto close = add(s.root, UiWidgetKind::button, utf8_literal("Hide downloads"));
    const auto previous = add(s.root, UiWidgetKind::button, utf8_literal("Previous downloads"));
    const auto next = add(s.root, UiWidgetKind::button, utf8_literal("Next downloads"));
    const auto confirm = add(s.root, UiWidgetKind::button, utf8_literal("Save file anyway"));
    const auto cancel = add(s.root, UiWidgetKind::button, utf8_literal("Cancel file save"));
    if (!close.ok() || !previous.ok() || !next.ok() || !confirm.ok() || !cancel.ok()) return false;
    s.close = close.handle; s.previous = previous.handle; s.next = next.handle;
    s.confirm = confirm.handle; s.cancel = cancel.handle;
    for (u32 i = 0; i < kUiDownloadVisibleRows; ++i) {
        const auto primary = add(s.root, UiWidgetKind::button, utf8_literal("Download action"));
        const auto secondary = add(s.root, UiWidgetKind::button, utf8_literal("Dismiss download"));
        if (!primary.ok() || !secondary.ok()) return false;
        s.primary[i] = primary.handle; s.secondary[i] = secondary.handle;
    }
    return true;
}
bool rectangle(UiDownloadScene& s, UiWidgetHandle handle, UiRect r, bool visible, bool enabled) noexcept {
    auto* widget = ui_widget_manager_resolve(&s.widgets, handle);
    auto* node = widget ? ui_layout_engine_resolve(&s.widgets.layout, widget->layout) : nullptr;
    if (!node) return false;
    auto& style = node->spec.style;
    const float width = visible ? ui_rect_width(r) : 0, height = visible ? ui_rect_height(r) : 0;
    const float left = visible ? r.left - s.panel.left : 0, top = visible ? r.top - s.panel.top : 0;
    const u16 flags = static_cast<u16>(ui_layout_node_clip_children | ui_layout_node_allow_overlap |
        ui_layout_node_allow_zero_size | (visible ? ui_layout_node_visible : 0));
    const bool changed = style.width.mode != UiLayoutLengthMode::fixed || style.width.value != width ||
        style.height.mode != UiLayoutLengthMode::fixed || style.height.value != height ||
        style.margin.left != left || style.margin.top != top || style.flags != flags;
    style.width = ui_layout_fixed(width); style.height = ui_layout_fixed(height);
    style.margin = {left, top, 0, 0}; style.flags = flags;
    widget->flags = static_cast<u16>(visible && enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : 0);
    return changed;
}
void cancel_capture(UiDownloadScene& s) noexcept {
    ui_widget_manager_capture(&s.widgets, ui_widget_invalid_handle());
    ui_widget_manager_hover(&s.widgets, ui_widget_invalid_handle());
}
void append(char* out, u8& used, const char* value) noexcept {
    while (*value) out[used++] = *value++;
}
void append_number(char* out, u8& used, u64 number) noexcept {
    char reverse[20]{}; u8 count = 0;
    do { reverse[count++] = static_cast<char>('0' + number % 10); number /= 10; } while (number);
    while (count) out[used++] = reverse[--count];
}
void progress_label(UiDownloadScene& s, u32 slot, const UiDownloadRow& r) noexcept {
    auto& used = s.progress_bytes[slot]; used = 0;
    auto* out = s.progress[slot]; append_number(out, used, r.bytes);
    if (r.total) { append(out, used, " / "); append_number(out, used, r.total); }
    append(out, used, " bytes");
    if (r.indeterminate || !r.total) {}
    else {
        append(out, used, " • ");
        const auto percentage = static_cast<u32>(minimum(100., static_cast<double>(r.bytes) * 100. / static_cast<double>(r.total)));
        append_number(out, used, percentage); append(out, used, "%");
    }
}
bool layout(UiDownloadScene& s, bool force = false) noexcept {
    bool dirty = force;
    const auto p = s.panel;
    dirty |= rectangle(s, s.close, {p.right - 44,p.top + 8,p.right - 12,p.top + 40}, true, true);
    const bool confirmation = s.mode == UiDownloadMode::confirmation;
    const u32 pages = s.accepted_count ? (s.accepted_count + s.page_size - 1) / s.page_size : 1;
    dirty |= rectangle(s, s.previous, s.previous_rectangle, !confirmation && s.accepted_count != 0, s.page != 0);
    dirty |= rectangle(s, s.next, s.next_rectangle, !confirmation && s.accepted_count != 0, s.page + 1 < pages);
    dirty |= rectangle(s, s.confirm, s.confirm_rectangle, confirmation, confirmation);
    dirty |= rectangle(s, s.cancel, s.cancel_rectangle, confirmation, confirmation);
    s.first = s.page * s.page_size;
    s.count = confirmation ? 0 : minimum(s.page_size, s.accepted_count - minimum(s.first, s.accepted_count));
    const float card_height = minimum(152.f, (ui_rect_height(s.body) - (s.page_size - 1) * 10.f) / s.page_size);
    for (u32 i = 0; i < kUiDownloadVisibleRows; ++i) {
        const bool visible = i < s.count;
        UiRect card{}, primary{}, secondary{};
        if (visible) {
            const auto& row = s.accepted[s.first + i];
            const float top = s.body.top + i * (card_height + 10);
            card = {s.body.left, top, s.body.right, top + card_height};
            const bool compact = ui_rect_width(card) < 350;
            const float secondary_width = compact ? 28.f : 72.f;
            secondary = {card.right - 12 - secondary_width,card.bottom - 34,card.right - 12,card.bottom - 8};
            const bool dismiss = terminal(row.phase) && row.dismiss;
            const float primary_right = dismiss ? secondary.left - 8 : card.right - 12;
            const float width = primary_action(row) == UiDownloadAction::reveal ? 112.f : 80.f;
            primary = {primary_right - width,card.bottom - 34,primary_right,card.bottom - 8};
            progress_label(s, i, row);
        } else s.progress_bytes[i] = 0;
        s.cards[i] = card; s.primary_rectangles[i] = primary; s.secondary_rectangles[i] = secondary;
        const auto* row = visible ? &s.accepted[s.first + i] : nullptr;
        dirty |= rectangle(s, s.primary[i], primary, visible && primary_action(*row) != UiDownloadAction::none,
            visible && primary_action(*row) != UiDownloadAction::none);
        dirty |= rectangle(s, s.secondary[i], secondary, visible && terminal(row->phase) && row->dismiss,
            visible && terminal(row->phase) && row->dismiss);
    }
    s.page_label_bytes = 0;
    if (s.accepted_count) {
        append_number(s.page_label, s.page_label_bytes, s.first + 1);
        if (s.count > 1) { append(s.page_label, s.page_label_bytes, "–"); append_number(s.page_label, s.page_label_bytes, s.first + s.count); }
        append(s.page_label, s.page_label_bytes, " of "); append_number(s.page_label, s.page_label_bytes, s.accepted_count);
    }
    if (dirty && !ui_widget_manager_layout_rectangle(&s.widgets, s.panel).ok()) return false;
    ui_widget_manager_reconcile_input(&s.widgets); return true;
}
bool surface(PaintStream* out, UiRect r, const Theme& t, Color fill, Color border, float radius = 8) noexcept {
    return ui_background_paint_surface(out,r,t,fill,border,radius,1,1,
        UiBackgroundSurfaceMode::gradient,UiBackgroundTier::highest);
}
// Disabled pager buttons (Previous on the first page, Next on the last)
// fade to half opacity.
constexpr float kDisabledOpacity = .5f;
bool button(PaintStream* out, const UiDownloadScene& s, UiWidgetHandle handle, UiRect r,
    const Theme& t, bool enabled, bool filled = false) noexcept {
    const bool hover = enabled && ui_widget_handle_matches(s.widgets.hovered, handle);
    const bool focus = enabled && ui_widget_handle_matches(s.widgets.focused, handle);
    const float opacity = enabled ? 1.f : kDisabledOpacity;
    return surface(out,r,t,ui_theme_color_alpha(filled ? t.tokens.emphasis.fill : hover ? t.surface_high : t.base_low, opacity),
        ui_theme_color_alpha(focus ? ui_foreground_color(t,t.tokens.border.focus) : filled ? t.tokens.emphasis.fill : t.interactive, opacity),6);
}
bool emit(UiTextOutput* out, const UiDownloadScene& s, UiWidgetHandle owner, Utf8View value,
    UiRect r, Color color, u32 flags = 0, UiTextAlignment alignment = UiTextAlignment::start,
    UiTextWrapping wrapping = UiTextWrapping::no_wrap) noexcept {
    if (!value.bytes || !ui_rect_has_area(r)) return true;
    return ui_text_output_push(out,{value,{&s.widgets,owner},r,s.panel,color,flags,alignment,
        UiTextAlignment::center,wrapping,UiTextOverflow::ellipsis,UiTextFace::normal});
}
} // namespace

bool ui_download_scene_current(const UiDownloadScene& s) noexcept { return current(s); }
bool ui_download_scene_update(UiDownloadScene* s, const UiDownloadSource* source,
    const UiDownloadContent* content, UiRect viewport) noexcept {
    if (!s || !content || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 240 || ui_rect_height(viewport) < 300) return false;
    if (current(*s) && s->source == source && s->content == content && ui_rect_equal(s->viewport,viewport)) return true;
    if (content->mode > UiDownloadMode::confirmation || !valid_view(content->heading,128) ||
        !valid_view(content->status,512) || !valid_view(content->filename,1024) || !valid_view(content->warning,2048) ||
        (content->mode == UiDownloadMode::confirmation && (!content->confirmation_id || !content->filename.bytes || !content->warning.bytes)) ||
        (source && (source->count > kUiDownloadMaximumRows || !source->resolve ||
        (source->current && !source->current(source->context))))) return false;
    const u32 count = source ? source->count : 0, revision = source ? source->revision : 0;
    const u32 content_revision = content->revision;
    const bool republished = !s->active || s->source != source || s->source_revision != revision || s->accepted_count != count;
    if (republished) {
        UiDownloadRow accepted[kUiDownloadMaximumRows]{};
        for (u32 i = 0; i < count; ++i) {
            if (!source->resolve(source->context,i,&accepted[i]) || !valid_row(accepted[i])) return false;
            for (u32 j = 0; j < i; ++j) if (accepted[j].id == accepted[i].id) return false;
        }
        if (source && (source->revision != revision || source->count != count ||
            (source->current && !source->current(source->context)))) return false;
        if (content->revision != content_revision || !initialize(*s)) return false;
        bool retain_capture = s->active && s->source == source;
        for (u32 i = 0; i < s->count; ++i) {
            const auto index = s->first + i;
            if (!ui_widget_handle_matches(s->widgets.captured, s->primary[i]) && !ui_widget_handle_matches(s->widgets.captured, s->secondary[i])) continue;
            retain_capture &= index < count && s->accepted[index].id == accepted[index].id;
            if (retain_capture && ui_widget_handle_matches(s->widgets.captured, s->primary[i]))
                retain_capture &= primary_action(s->accepted[index]) == primary_action(accepted[index]);
            if (retain_capture && ui_widget_handle_matches(s->widgets.captured, s->secondary[i]))
                retain_capture &= terminal(accepted[index].phase) && accepted[index].dismiss;
        }
        if (!retain_capture) cancel_capture(*s);
        copy_bytes(s->accepted,accepted,sizeof(UiDownloadRow) * count);
        s->accepted_count = count;
    } else if (!initialize(*s)) return false;
    const auto old_panel = s->panel;
    const bool entering_confirmation = content->mode == UiDownloadMode::confirmation &&
        (!s->active || s->mode != content->mode || s->confirmation_id != content->confirmation_id);
    if (s->mode != content->mode || s->confirmation_id != content->confirmation_id) cancel_capture(*s);
    s->source = source; s->content = content; s->source_revision = revision; s->content_revision = content_revision;
    s->mode = content->mode; s->confirmation_id = content->confirmation_id; s->viewport = viewport;
    const float inset = ui_rect_width(viewport) < 400 ? 8.f : 24.f;
    const float width = minimum(content->mode == UiDownloadMode::confirmation ? 520.f : 680.f,ui_rect_width(viewport) - inset * 2);
    const float height = minimum(content->mode == UiDownloadMode::confirmation ? 380.f : 660.f,ui_rect_height(viewport) - 24);
    const float left = viewport.left + (ui_rect_width(viewport) - width) * .5f;
    const float top = viewport.top + (ui_rect_height(viewport) - height) * .5f;
    s->panel = {left,top,left + width,top + height};
    const float padding = width < 360 ? 12.f : 20.f;
    s->hint = {left + padding,top + 48,s->panel.right - padding,top + 72};
    s->footer = {left + padding,s->panel.bottom - 48,s->panel.right - padding,s->panel.bottom - 12};
    s->body = {left + padding,top + 82,s->panel.right - padding,s->footer.top - 10};
    s->confirmation_filename = {left + padding,top + 60,s->panel.right - padding,top + 96};
    s->warning = {left + padding,top + 104,s->panel.right - padding,s->footer.top - 16};
    s->confirm_rectangle = {s->footer.right - 120,s->footer.top,s->footer.right,s->footer.bottom};
    s->cancel_rectangle = {maximum(s->footer.left,s->confirm_rectangle.left - 104),s->footer.top,
        s->confirm_rectangle.left - 8,s->footer.bottom};
    const float pager_width = width < 360 ? 60.f : 88.f;
    s->previous_rectangle = {s->footer.left,s->footer.top,s->footer.left + pager_width,s->footer.bottom};
    s->next_rectangle = {s->footer.right - pager_width,s->footer.top,s->footer.right,s->footer.bottom};
    const u32 old_size = s->page_size ? s->page_size : 1;
    const u32 first = s->page * old_size;
    s->page_size = ui_rect_height(s->body) >= 440 ? 3 : ui_rect_height(s->body) >= 290 ? 2 : 1;
    s->page = minimum(first / s->page_size,count ? (count - 1) / s->page_size : 0);
    s->active = true;
    if (!ui_rect_equal(old_panel,s->panel)) cancel_capture(*s);
    if (!layout(*s,!ui_rect_equal(old_panel,s->panel))) { s->active = false; return false; }
    if (entering_confirmation) ui_widget_manager_focus(&s->widgets,s->cancel);
    ++s->revision; return true;
}
void ui_download_scene_release(UiDownloadScene* s) noexcept {
    if (!s) return;
    ui_fixed_widget_storage_release(&s->storage); *s = {};
}
bool ui_download_scene_contains(const UiDownloadScene& s, float x, float y) noexcept {
    return current(s) && ui_rect_contains(s.viewport,x,y);
}
UiDownloadTarget ui_download_scene_target(const UiDownloadScene& s, UiWidgetHandle handle) noexcept {
    if (!current(s)) return {};
    if (ui_widget_handle_matches(handle, s.close)) return {UiDownloadAction::close};
    if (s.mode == UiDownloadMode::confirmation) {
        if (ui_widget_handle_matches(handle, s.confirm)) return {UiDownloadAction::confirm,s.confirmation_id,0,s.source_revision};
        if (ui_widget_handle_matches(handle, s.cancel)) return {UiDownloadAction::cancel_confirmation,s.confirmation_id,0,s.source_revision};
        return {};
    }
    if (ui_widget_handle_matches(handle, s.previous) && s.page) return {UiDownloadAction::previous};
    if (ui_widget_handle_matches(handle, s.next) && (s.page + 1) * s.page_size < s.accepted_count) return {UiDownloadAction::next};
    for (u32 i = 0; i < s.count; ++i) {
        const auto& r = s.accepted[s.first + i];
        if (ui_widget_handle_matches(handle, s.primary[i])) return {primary_action(r),r.id,s.first + i,s.source_revision};
        if (ui_widget_handle_matches(handle, s.secondary[i]) && terminal(r.phase) && r.dismiss)
            return {UiDownloadAction::dismiss,r.id,s.first + i,s.source_revision};
    }
    return {};
}
UiDownloadTarget ui_download_scene_hit_test(const UiDownloadScene& s, float x, float y) noexcept {
    if (!ui_download_scene_contains(s,x,y)) return {};
    if (!ui_rect_contains(s.panel,x,y)) return {UiDownloadAction::close};
    return ui_download_scene_target(s,ui_widget_manager_hit_test(s.widgets,x,y));
}
bool ui_download_scene_page(UiDownloadScene* s, u32 page) noexcept {
    if (!s || !current(*s) || s->mode != UiDownloadMode::receipts || page >
        (s->accepted_count ? (s->accepted_count - 1) / s->page_size : 0)) return false;
    if (page == s->page) return true;
    cancel_capture(*s); s->page = page;
    if (!layout(*s)) return false;
    ++s->revision; return true;
}
bool ui_download_scene_set_hover(UiDownloadScene* s, float x, float y, bool* changed) noexcept {
    if (changed) *changed = false;
    if (!s || !current(*s)) return false;
    const auto hit = ui_widget_manager_hit_test(s->widgets,x,y), before = s->widgets.hovered;
    if (!ui_widget_manager_hover(&s->widgets,hit)) return false;
    const bool delta = !ui_widget_handle_matches(before, hit) && (before.generation || hit.generation);
    if (delta) ++s->revision;
    if (changed) *changed = delta;
    return true;
}
bool ui_download_scene_paint(const UiDownloadScene& s, const Theme& t, PaintStream* out, u32) noexcept {
    if (!current(s)) return true;
    if (!out || !paint_stream_push_clip(out,s.viewport.left,s.viewport.top,s.viewport.right,s.viewport.bottom)) return false;
    const auto p = s.panel;
    bool ok = paint_stream_push(out,s.viewport.left,s.viewport.top,s.viewport.right,s.viewport.bottom,t.tokens.overlay.scrim) &&
        ui_surface_paint(out,p,t,UiElevation::dialog,t.scale.radius_lg + t.scale.space_1,nullptr,
                         &t.interactive_hover) &&
        ui_icon_paint(out,s.mode == UiDownloadMode::confirmation ? UiIcon::warning : UiIcon::download,
            {p.left + 20,p.top + 16,p.left + 40,p.top + 36},ui_foreground_color(t,t.tokens.emphasis.outline));
    if (ok && (ui_widget_handle_matches(s.widgets.hovered, s.close) || ui_widget_handle_matches(s.widgets.focused, s.close)))
        ok = surface(out,{p.right - 44,p.top + 8,p.right - 12,p.top + 40},t,t.surface_high,t.interactive,6);
    ok = ok && ui_icon_paint(out,UiIcon::close,{p.right - 38,p.top + 14,p.right - 18,p.top + 34},t.text_secondary);
    if (s.mode == UiDownloadMode::confirmation) {
        ok = ok && surface(out,s.confirmation_filename,t,t.base_lowest,t.surface_high,6) &&
            button(out,s,s.cancel,s.cancel_rectangle,t,true) && button(out,s,s.confirm,s.confirm_rectangle,t,true,true);
    } else {
        for (u32 i = 0; ok && i < s.count; ++i) {
            const auto& r = s.accepted[s.first + i]; const auto card = s.cards[i];
            const auto color = phase_color(t,r.phase);
            ok = surface(out,card,t,t.base_lowest,t.surface_high,9) &&
                ui_icon_paint(out,r.phase == UiDownloadPhase::failed ? UiIcon::warning : UiIcon::download,
                    {card.left + 12,card.top + 12,card.left + 32,card.top + 32},color);
            const UiRect track{card.left + 12,card.bottom - 42,card.right - 12,card.bottom - 38};
            ok = ok && paint_stream_push_rounded(out,track.left,track.top,track.right,track.bottom,t.interactive,t.scale.radius_xs);
            // A row still waiting (choosing a location, refreshing the link)
            // shows the bare track: there is no progress to draw yet.
            if ((terminal(r.phase) || !r.indeterminate) && r.total && r.bytes) {
                const float ratio = static_cast<float>(minimum(1.,static_cast<double>(r.bytes) / static_cast<double>(r.total)));
                ok = ok && paint_stream_push_rounded(out,track.left,track.top,
                    track.left + ui_rect_width(track) * ratio,track.bottom,color,t.scale.radius_xs);
            }
            if (primary_action(r) != UiDownloadAction::none)
                ok = ok && button(out,s,s.primary[i],s.primary_rectangles[i],t,true,r.phase == UiDownloadPhase::failed);
            if (terminal(r.phase) && r.dismiss) {
                ok = ok && button(out,s,s.secondary[i],s.secondary_rectangles[i],t,true);
                if (ui_rect_width(card) < 350) {
                    const auto x = s.secondary_rectangles[i];
                    ok = ok && ui_icon_paint(out,UiIcon::close,{x.left + 6,x.top + 5,x.right - 6,x.bottom - 5},t.text_secondary);
                }
            }
        }
        if (ok && s.accepted_count) ok = button(out,s,s.previous,s.previous_rectangle,t,s.page != 0) &&
            button(out,s,s.next,s.next_rectangle,t,(s.page + 1) * s.page_size < s.accepted_count);
        if (ok && !s.accepted_count) {
            const float mid = (s.body.top + s.body.bottom) * .5f;
            ok = ui_icon_paint(out,UiIcon::download,{s.body.left + ui_rect_width(s.body) * .5f - 22,mid - 62,
                s.body.left + ui_rect_width(s.body) * .5f + 22,mid - 18},ui_foreground_color(t,t.tokens.emphasis.outline));
        }
    }
    return paint_stream_pop_clip(out) && ok;
}
bool ui_download_scene_text_output(const UiDownloadScene& s, const Theme& t, UiTextOutput* out) noexcept {
    if (!current(s)) return true;
    if (!ui_text_output_valid(out)) return false;
    const auto p = s.panel;
    if (!emit(out,s,s.root,s.content->heading.bytes ? s.content->heading :
        s.mode == UiDownloadMode::confirmation ? utf8_literal("Save this file?") : utf8_literal("Downloads"),
        {p.left + 48,p.top + 10,p.right - 48,p.top + 40},t.text_primary,text_style_bold)) return false;
    if (s.mode == UiDownloadMode::confirmation) {
        const auto f = s.confirmation_filename;
        return emit(out,s,s.root,s.content->filename,{f.left + 10,f.top,f.right - 10,f.bottom},t.text_primary,text_style_bold | text_style_subtext) &&
            emit(out,s,s.root,s.content->warning,s.warning,t.text_primary,0,UiTextAlignment::start,UiTextWrapping::wrap) &&
            emit(out,s,s.cancel,utf8_literal("Cancel"),s.cancel_rectangle,t.text_primary,0,UiTextAlignment::center) &&
            emit(out,s,s.confirm,utf8_literal("Save anyway"),s.confirm_rectangle,ui_surface_foreground(t,t.tokens.emphasis.fill),
                text_style_bold | text_style_subtext,UiTextAlignment::center);
    }
    if (!emit(out,s,s.root,s.content->status,
        s.hint,t.text_secondary,text_style_subtext)) return false;
    for (u32 i = 0; i < s.count; ++i) {
        const auto& r = s.accepted[s.first + i]; const auto c = s.cards[i];
        const bool short_card = ui_rect_height(c) < 144;
        const float source_top = c.top + 34;
        const float status_top = c.top + (short_card ? 50.f : 58.f);
        const float bytes_top = c.bottom - 66;
        if (!emit(out,s,s.root,r.filename,{c.left + 42,c.top + 8,c.right - 12,c.top + 34},t.text_primary,text_style_bold) ||
            !emit(out,s,s.root,r.source,{c.left + 42,source_top,c.right - 12,source_top + (short_card ? 16.f : 18.f)},t.text_secondary,text_style_subtext) ||
            !emit(out,s,s.root,r.status.bytes ? r.status : phase_label(r.phase),
                {c.left + 12,status_top,c.right - 12,status_top + (short_card ? 18.f : 20.f)},phase_color(t,r.phase),text_style_subtext) ||
            !emit(out,s,s.root,{s.progress[i],s.progress_bytes[i]},
                {c.left + 12,bytes_top,c.right - 12,bytes_top + 20},t.text_secondary,text_style_subtext)) return false;
        if (primary_action(r) != UiDownloadAction::none && !emit(out,s,s.primary[i],primary_label(r),s.primary_rectangles[i],
            r.phase == UiDownloadPhase::failed ? ui_surface_foreground(t,t.tokens.emphasis.fill) : t.text_primary,
            text_style_subtext,UiTextAlignment::center)) return false;
        if (terminal(r.phase) && r.dismiss && ui_rect_width(c) >= 350 &&
            !emit(out,s,s.secondary[i],utf8_literal("Dismiss"),s.secondary_rectangles[i],t.text_primary,
                text_style_subtext,UiTextAlignment::center)) return false;
    }
    if (!s.accepted_count) {
        const float mid = (s.body.top + s.body.bottom) * .5f;
        return emit(out,s,s.root,utf8_literal("No downloads yet"),{s.body.left,mid - 8,s.body.right,mid + 18},
            t.text_primary,text_style_bold,UiTextAlignment::center) &&
            emit(out,s,s.root,utf8_literal("Save an attachment to see its progress here."),
                {s.body.left,mid + 20,s.body.right,mid + 60},t.text_secondary,text_style_subtext,
                UiTextAlignment::center,UiTextWrapping::wrap);
    }
    const Color disabled = ui_theme_color_alpha(t.text_secondary, kDisabledOpacity);
    return emit(out,s,s.previous,utf8_literal("Previous"),s.previous_rectangle,s.page ? t.text_primary : disabled,
        text_style_subtext,UiTextAlignment::center) &&
        emit(out,s,s.next,utf8_literal("Next"),s.next_rectangle,(s.page + 1) * s.page_size < s.accepted_count ?
            t.text_primary : disabled,text_style_subtext,UiTextAlignment::center) &&
        emit(out,s,s.root,{s.page_label,s.page_label_bytes},
            {s.previous_rectangle.right + 6,s.footer.top,s.next_rectangle.left - 6,s.footer.bottom},
            t.text_secondary,text_style_subtext,UiTextAlignment::center);
}
} // namespace maicord
