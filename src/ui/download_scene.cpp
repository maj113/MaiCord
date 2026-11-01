#include "maicord/ui/download_scene.hpp"
#include "maicord/ui/rect_edges.hpp"
#include "maicord/ui/scene_kit.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
constexpr UiLook kClose{.fill = UI_TOKEN(surface_high), .border = UI_TOKEN(interactive), .focus = UI_TOKEN(interactive),
                        .radius = 6, .flags = ui_look_hot_only | ui_look_dialog};
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
bool valid_view(const Utf8View& text, u32 maximum_bytes) noexcept {
    return text.bytes <= maximum_bytes && utf8_validate(text).valid;
}
bool valid_row(const UiDownloadRow& r) noexcept {
    return r.id && r.filename.bytes && valid_view(r.filename, 1024) && valid_view(r.source, 512) &&
        valid_view(r.status, 1024) && r.phase <= UiDownloadPhase::cancelled;
}
bool initialize(UiDownloadScene& s) noexcept {
    if (s.storage.arena.base) return true;
    if (!ui_scene_open(&s.storage, &s.widgets, kUiDownloadWidgetCapacity, sizeof(UiDownloadRow) * kUiDownloadMaximumRows,
                       alignof(UiDownloadRow))) return false;
    s.accepted = ui_fixed_widget_storage_sidecar<UiDownloadRow>(s.storage);
    s.root = ui_scene_part_root(&s.widgets, utf8_literal("Downloads"));
    if (!s.root.generation) return false;
    const auto add = [&](Utf8View name) noexcept { return ui_scene_part(&s.widgets, s.root, UiWidgetKind::button, name); };
    s.close = add(utf8_literal("Hide downloads"));
    s.previous = add(utf8_literal("Previous downloads"));
    s.next = add(utf8_literal("Next downloads"));
    s.confirm = add(utf8_literal("Save file anyway"));
    s.cancel = add(utf8_literal("Cancel file save"));
    if (!s.close.generation || !s.previous.generation || !s.next.generation || !s.confirm.generation ||
        !s.cancel.generation) return false;
    for (u32 i = 0; i < kUiDownloadVisibleRows; ++i) {
        s.primary[i] = add(utf8_literal("Download action"));
        s.secondary[i] = add(utf8_literal("Dismiss download"));
        if (!s.primary[i].generation || !s.secondary[i].generation) return false;
    }
    return true;
}
bool rectangle(UiDownloadScene& s, UiWidgetHandle handle, UiRect r, bool visible, bool enabled) noexcept {
    return ui_widget_manager_place_part(&s.widgets, handle, r, s.panel, visible, enabled);
}
void progress_label(UiDownloadScene& s, u32 slot, const UiDownloadRow& r) noexcept {
    UiLabelWriter label{s.progress[slot], &s.progress_bytes[slot]};
    label << r.bytes;
    if (r.total) label << utf8_literal(" / ") << r.total;
    label << utf8_literal(" bytes");
    if (!r.indeterminate && r.total)
        label << utf8_literal(" • ")
              << static_cast<u32>(minimum(100., static_cast<double>(r.bytes) * 100. / static_cast<double>(r.total)))
              << utf8_literal("%");
}
bool layout(UiDownloadScene& s, bool force = false) noexcept {
    bool dirty = force;
    const auto p = s.panel;
    dirty |= rectangle(s, s.close, ui_rect_edges(p, ui_end(-44), ui_start(8), ui_end(-12), ui_start(40)), true, true);
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
    UiLabelWriter page{s.page_label, &s.page_label_bytes};
    if (s.accepted_count) {
        page << s.first + 1;
        if (s.count > 1) page << utf8_literal("–") << s.first + s.count;
        page << utf8_literal(" of ") << s.accepted_count;
    }
    if (dirty && !ui_widget_manager_layout_rectangle(&s.widgets, s.panel).ok()) return false;
    ui_widget_manager_reconcile_input(&s.widgets); return true;
}
// Disabled pager buttons (Previous on the first page, Next on the last)
// fade to half opacity.
constexpr float kDisabledOpacity = .5f;
void button(UiPaint& p, const UiDownloadScene& s, UiWidgetHandle handle, const UiRect& r, bool enabled,
    bool filled = false) noexcept {
    const auto& t = p.theme();
    const bool hover = enabled && ui_hovered(s, handle), focus = enabled && ui_focused(s, handle);
    const float opacity = enabled ? 1.f : kDisabledOpacity;
    p.surface(r, ui_theme_color_alpha(filled ? t.tokens.emphasis.fill : hover ? t.surface_high : t.base_low, opacity),
        ui_theme_color_alpha(focus ? ui_foreground_color(t, t.tokens.border.focus) : filled ? t.tokens.emphasis.fill
                                                                                         : t.interactive, opacity),
        6, true);
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
        if (!retain_capture) ui_widget_manager_release_pointer(&s->widgets);
        copy_bytes(s->accepted,accepted,sizeof(UiDownloadRow) * count);
        s->accepted_count = count;
    } else if (!initialize(*s)) return false;
    const auto old_panel = s->panel;
    const bool entering_confirmation = content->mode == UiDownloadMode::confirmation &&
        (!s->active || s->mode != content->mode || s->confirmation_id != content->confirmation_id);
    if (s->mode != content->mode || s->confirmation_id != content->confirmation_id) ui_widget_manager_release_pointer(&s->widgets);
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
    if (!ui_rect_equal(old_panel,s->panel)) ui_widget_manager_release_pointer(&s->widgets);
    if (!layout(*s,!ui_rect_equal(old_panel,s->panel))) { s->active = false; return false; }
    if (entering_confirmation) ui_widget_manager_focus(&s->widgets,s->cancel);
    ++s->revision; return true;
}
void ui_download_scene_release(UiDownloadScene* s) noexcept { ui_scene_release(s); }
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
    if (!current(s) || !ui_rect_contains(s.viewport,x,y)) return {};
    if (!ui_rect_contains(s.panel,x,y)) return {UiDownloadAction::close};
    return ui_download_scene_target(s,ui_widget_manager_hit_test(s.widgets,x,y));
}
bool ui_download_scene_page(UiDownloadScene* s, u32 page) noexcept {
    if (!s || !current(*s) || s->mode != UiDownloadMode::receipts || page >
        (s->accepted_count ? (s->accepted_count - 1) / s->page_size : 0)) return false;
    if (page == s->page) return true;
    ui_widget_manager_release_pointer(&s->widgets); s->page = page;
    if (!layout(*s)) return false;
    ++s->revision; return true;
}
bool ui_download_scene_set_hover(UiDownloadScene* s, float x, float y, bool* changed) noexcept {
    if (changed) *changed = false;
    return s && current(*s) && ui_scene_hover(&s->widgets, &s->revision, x, y, changed);
}
bool ui_download_scene_paint(const UiDownloadScene& s, const Theme& t, PaintStream* out) noexcept {
    if (!current(s)) return true;
    const auto panel = s.panel;
    UiPaint p{out, t};
    p.dialog(s.viewport, panel)
        .icon(s.mode == UiDownloadMode::confirmation ? UiIcon::warning : UiIcon::download,
              ui_rect_edges(panel, ui_start(20), ui_start(16), ui_start(40), ui_start(36)),
              ui_foreground_color(t, t.tokens.emphasis.outline))
        .look<kClose>(ui_rect_edges(panel, ui_end(-44), ui_start(8), ui_end(-12), ui_start(40)),
                      ui_widget_state(s.widgets, s.close))
        .icon(UiIcon::close, ui_rect_edges(panel, ui_end(-38), ui_start(14), ui_end(-18), ui_start(34)), t.text_secondary);
    if (s.mode == UiDownloadMode::confirmation) {
        p.surface(s.confirmation_filename, t.base_lowest, t.surface_high, 6, true);
        button(p, s, s.cancel, s.cancel_rectangle, true);
        button(p, s, s.confirm, s.confirm_rectangle, true, true);
        return p.end();
    }
    for (u32 i = 0; p && i < s.count; ++i) {
        const auto& r = s.accepted[s.first + i];
        const auto card = s.cards[i];
        const auto color = phase_color(t, r.phase);
        const UiRect track = ui_rect_edges(card, ui_start(12), ui_end(-42), ui_end(-12), ui_end(-38));
        p.surface(card, t.base_lowest, t.surface_high, 9, true)
            .icon(r.phase == UiDownloadPhase::failed ? UiIcon::warning : UiIcon::download,
                  ui_rect_edges(card, ui_start(12), ui_start(12), ui_start(32), ui_start(32)), color)
            .rounded(track, t.interactive, t.scale.radius_xs);
        // A row still waiting (choosing a location, refreshing the link)
        // shows the bare track: there is no progress to draw yet.
        if ((terminal(r.phase) || !r.indeterminate) && r.total && r.bytes) {
            const float ratio = static_cast<float>(minimum(1., static_cast<double>(r.bytes) / static_cast<double>(r.total)));
            p.rounded({track.left, track.top, track.left + ui_rect_width(track) * ratio, track.bottom}, color,
                      t.scale.radius_xs);
        }
        if (primary_action(r) != UiDownloadAction::none)
            button(p, s, s.primary[i], s.primary_rectangles[i], true, r.phase == UiDownloadPhase::failed);
        if (!terminal(r.phase) || !r.dismiss) continue;
        button(p, s, s.secondary[i], s.secondary_rectangles[i], true);
        if (ui_rect_width(card) < 350)
            p.icon(UiIcon::close, ui_rect_edges(s.secondary_rectangles[i], ui_start(6), ui_start(5), ui_end(-6), ui_end(-5)),
                   t.text_secondary);
    }
    if (s.accepted_count) {
        button(p, s, s.previous, s.previous_rectangle, s.page != 0);
        button(p, s, s.next, s.next_rectangle, (s.page + 1) * s.page_size < s.accepted_count);
    } else {
        const float mid = (s.body.top + s.body.bottom) * .5f;
        p.icon(UiIcon::download, {s.body.left + ui_rect_width(s.body) * .5f - 22, mid - 62,
                                  s.body.left + ui_rect_width(s.body) * .5f + 22, mid - 18},
               ui_foreground_color(t, t.tokens.emphasis.outline));
    }
    return p.end();
}
bool ui_download_scene_text_output(const UiDownloadScene& s, const Theme& t, UiTextOutput* out) noexcept {
    if (!current(s)) return true;
    UiTextPen<UiTextClip::region> text{out, s.widgets, s.panel};
    const auto& c = *s.content;
    text(s.root, c.heading.bytes ? c.heading
                 : s.mode == UiDownloadMode::confirmation ? utf8_literal("Save this file?") : utf8_literal("Downloads"),
         ui_rect_edges(s.panel, ui_start(48), ui_start(10), ui_end(-48), ui_start(40)), t.text_primary, text_style_bold);
    if (s.mode == UiDownloadMode::confirmation) {
        text(s.root, c.filename, ui_rect_edges(s.confirmation_filename, ui_start(10), ui_start(0), ui_end(-10), ui_end(0)),
             t.text_primary, text_style_bold | text_style_subtext);
        text(s.root, c.warning, s.warning, t.text_primary, 0, ui_text_wrap | ui_text_middle);
        text(s.cancel, utf8_literal("Cancel"), s.cancel_rectangle, t.text_primary, 0, ui_text_center);
        return text(s.confirm, utf8_literal("Save anyway"), s.confirm_rectangle,
                    ui_surface_foreground(t, t.tokens.emphasis.fill), text_style_bold | text_style_subtext,
                    ui_text_center).ok();
    }
    text(s.root, c.status, s.hint, t.text_secondary, text_style_subtext);
    for (u32 i = 0; text.ok() && i < s.count; ++i) {
        const auto& r = s.accepted[s.first + i];
        const auto card = s.cards[i];
        const bool short_card = ui_rect_height(card) < 144;
        const float source_top = card.top + 34;
        const float status_top = card.top + (short_card ? 50.f : 58.f);
        const float bytes_top = card.bottom - 66;
        text(s.root, r.filename, ui_rect_edges(card, ui_start(42), ui_start(8), ui_end(-12), ui_start(34)), t.text_primary,
             text_style_bold);
        text(s.root, r.source, {card.left + 42, source_top, card.right - 12, source_top + (short_card ? 16.f : 18.f)},
             t.text_secondary, text_style_subtext);
        text(s.root, r.status.bytes ? r.status : phase_label(r.phase),
             {card.left + 12, status_top, card.right - 12, status_top + (short_card ? 18.f : 20.f)}, phase_color(t, r.phase),
             text_style_subtext);
        text(s.root, {s.progress[i], s.progress_bytes[i]}, {card.left + 12, bytes_top, card.right - 12, bytes_top + 20},
             t.text_secondary, text_style_subtext);
        if (primary_action(r) != UiDownloadAction::none)
            text(s.primary[i], primary_label(r), s.primary_rectangles[i],
                 r.phase == UiDownloadPhase::failed ? ui_surface_foreground(t, t.tokens.emphasis.fill) : t.text_primary,
                 text_style_subtext, ui_text_center);
        if (terminal(r.phase) && r.dismiss && ui_rect_width(card) >= 350)
            text(s.secondary[i], utf8_literal("Dismiss"), s.secondary_rectangles[i], t.text_primary, text_style_subtext,
                 ui_text_center);
    }
    if (!s.accepted_count) {
        const float mid = (s.body.top + s.body.bottom) * .5f;
        text(s.root, utf8_literal("No downloads yet"), {s.body.left, mid - 8, s.body.right, mid + 18}, t.text_primary,
             text_style_bold, ui_text_center);
        return text(s.root, utf8_literal("Save an attachment to see its progress here."),
                    {s.body.left, mid + 20, s.body.right, mid + 60}, t.text_secondary, text_style_subtext,
                    ui_text_center | ui_text_wrap | ui_text_middle).ok();
    }
    const Color disabled = ui_theme_color_alpha(t.text_secondary, kDisabledOpacity);
    text(s.previous, utf8_literal("Previous"), s.previous_rectangle, s.page ? t.text_primary : disabled, text_style_subtext,
         ui_text_center);
    text(s.next, utf8_literal("Next"), s.next_rectangle,
         (s.page + 1) * s.page_size < s.accepted_count ? t.text_primary : disabled, text_style_subtext, ui_text_center);
    return text(s.root, {s.page_label, s.page_label_bytes},
                {s.previous_rectangle.right + 6, s.footer.top, s.next_rectangle.left - 6, s.footer.bottom},
                t.text_secondary, text_style_subtext, ui_text_center).ok();
}
} // namespace maicord
