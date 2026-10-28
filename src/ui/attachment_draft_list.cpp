#include "maicord/ui/attachment_draft_list.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/text_style.hpp"
namespace maicord {
namespace {
enum Part : u32 {
    card,
    header,
    preview,
    meta,
    name,
    status,
    actions,
    remove,
    retry,
    spoiler,
    describe
};
u32 part(UiAttachmentDraftAction a) noexcept {
    return a == UiAttachmentDraftAction::remove     ? remove
           : a == UiAttachmentDraftAction::retry    ? retry
           : a == UiAttachmentDraftAction::spoiler  ? spoiler
           : a == UiAttachmentDraftAction::describe ? describe
                                                    : kUiAttachmentDraftParts;
}
const UiLayoutNode* node(const UiAttachmentDraftList& list, u32 row, u32 p) noexcept {
    return list.manager && row < list.rows.count
               ? ui_widget_manager_layout_node(*list.manager, list.parts[row][p])
               : nullptr;
}
} // namespace
bool ui_attachment_draft_list_build(UiAttachmentDraftList* list, UiWidgetManager* manager,
                                    UiWidgetHandle parent, ConstSlice<UiAttachmentDraftView> rows,
                                    u32 key_base, const UiAttachmentDraftStyle& style) noexcept {
    if (!list || !manager || rows.count > kUiAttachmentDraftMaximum || (!rows.data && rows.count))
        return false;
    const float lengths[] = {
        style.minimum_card_width, style.preview_extent,  style.gap,    style.padding,
        style.control_height,     style.control_padding, style.radius, style.progress_height};
    for (float length : lengths)
        if (!ui_coordinate_valid(length) || length < 0)
            return false;
    if (style.minimum_card_width < 96 || style.preview_extent < 24 || style.control_height < 24)
        return false;
    reset_value(list);
    list->manager = manager;
    list->rows = rows;
    list->key_base = key_base;
    list->style = style;
    UiWidgetBuilder builder(*manager);
    for (u32 r = 0; r < rows.count; ++r) {
        if (!rows[r].id || !rows[r].filename.bytes || !utf8_validate(rows[r].filename).valid ||
            !utf8_validate(rows[r].status).valid || !utf8_validate(rows[r].description).valid)
            return false;
        for (u32 p = 0; p < kUiAttachmentDraftParts; ++p) {
            auto layout = ui_layout_style();
            layout.width = ui_layout_fill();
            layout.height = ui_layout_content();
            UiWidgetKind kind = UiWidgetKind::container;
            u16 flags = ui_widget_none;
            UiWidgetHandle owner = parent;
            if (p == card) {
                layout.flow = UiLayoutFlow::vertical;
                layout.padding = {style.padding, style.padding, style.padding, style.padding};
                layout.gap = style.gap;
            } else if (p == header) {
                owner = list->parts[r][card];
                layout.flow = UiLayoutFlow::horizontal;
                layout.gap = style.gap;
            } else if (p == preview) {
                owner = list->parts[r][header];
                kind = UiWidgetKind::image;
                layout.width = ui_layout_fixed(style.preview_extent);
                layout.height = ui_layout_fixed(style.preview_extent);
            } else if (p == meta) {
                owner = list->parts[r][header];
                layout.flow = UiLayoutFlow::vertical;
                layout.gap = 4;
            } else if (p == name || p == status) {
                owner = list->parts[r][meta];
                kind = UiWidgetKind::text;
                layout.flags |= ui_layout_node_text_content | ui_layout_node_allow_zero_size;
            } else if (p == actions) {
                owner = list->parts[r][card];
                layout.flow = UiLayoutFlow::horizontal_wrap;
                layout.gap = 6;
            } else {
                owner = list->parts[r][actions];
                kind = UiWidgetKind::button;
                layout.width = ui_layout_content();
                layout.height = ui_layout_fixed(style.control_height);
                layout.padding = {style.control_padding, 6, style.control_padding, 6};
                layout.flags |= ui_layout_node_text_content | ui_layout_node_allow_zero_size;
                flags = ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable;
            }
            const auto added = builder.widget(
                owner, {key_base + r * kUiAttachmentDraftParts + p, utf8_literal("DraftAttachment")}, kind,
                flags, layout);
            if (!added.ok())
                return false;
            list->parts[r][p] = added.handle;
        }
    }
    return true;
}
Utf8View ui_attachment_draft_list_label(const UiAttachmentDraftList& list, u32 key) noexcept {
    if (key < list.key_base)
        return {};
    const u32 offset = key - list.key_base, row = offset / kUiAttachmentDraftParts,
              p = offset % kUiAttachmentDraftParts;
    if (row >= list.rows.count)
        return {};
    const auto& v = list.rows[row];
    switch (p) {
    case name:
        return v.filename;
    case status:
        return v.status;
    case remove:
        return utf8_literal("Remove");
    case retry:
        return v.can_retry ? utf8_literal("Retry") : Utf8View{};
    case spoiler:
        return utf8_literal("Spoiler");
    case describe:
        return v.description.bytes ? utf8_literal("Edit details") : utf8_literal("Details");
    default:
        return {};
    }
}
bool ui_attachment_draft_list_single_line(const UiAttachmentDraftList& list, u32 key) noexcept {
    return key >= list.key_base && (key - list.key_base) % kUiAttachmentDraftParts != status;
}
bool ui_attachment_draft_list_layout(UiAttachmentDraftList* list, float width, bool editable) noexcept {
    if (!list || !list->manager || !ui_coordinate_valid(width) || width <= 0)
        return false;
    const auto& style = list->style;
    const u32 columns = width >= style.minimum_card_width * 2 + style.gap ? 2u : 1u;
    const float card_width = (width - style.gap * (columns - 1)) / columns;
    for (u32 r = 0; r < list->rows.count; ++r) {
        const auto& view = list->rows[r];
        for (u32 p = 0; p < kUiAttachmentDraftParts; ++p) {
            auto* widget = ui_widget_manager_resolve(list->manager, list->parts[r][p]);
            if (!widget)
                return false;
            auto* layout = ui_layout_engine_resolve(&list->manager->layout, widget->layout);
            if (!layout)
                return false;
            if (p == card) {
                layout->spec.style.width = ui_layout_fixed(card_width);
                layout->spec.style.padding = {style.padding, style.padding, style.padding, style.padding};
                layout->spec.style.gap = style.gap;
            }
            if (p == header) {
                layout->spec.style.gap = style.gap;
                layout->spec.style.margin.bottom =
                    ui_attachment_draft_progress_visible(view) ? style.gap + style.progress_height : 0;
            }
            if (p == preview) {
                const float extent =
                    minimum(style.preview_extent, maximum(24.f, card_width - style.padding * 2 - 24.f));
                layout->spec.style.width = layout->spec.style.height = ui_layout_fixed(extent);
            }
            if (p >= remove) {
                layout->spec.style.height = ui_layout_fixed(style.control_height);
                layout->spec.style.padding = {style.control_padding, 6, style.control_padding, 6};
                const bool shown = p != retry || view.can_retry;
                const bool enabled = editable && shown &&
                                     (p == remove  ? view.can_remove
                                      : p == retry ? view.can_retry
                                                   : view.can_describe);
                widget->flags =
                    enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : ui_widget_none;
                if (shown)
                    layout->spec.style.flags |= ui_layout_node_visible;
                else
                    layout->spec.style.flags = static_cast<u16>(layout->spec.style.flags &
                                                                ~static_cast<u16>(ui_layout_node_visible));
                layout->spec.style.maximum_size.width = maximum(24.f, card_width - style.padding * 2);
            }
        }
    }
    return true;
}
UiAttachmentDraftTarget ui_attachment_draft_list_target(const UiAttachmentDraftList& list,
                                                        UiWidgetHandle h) noexcept {
    for (u32 r = 0; r < list.rows.count; ++r)
        for (u32 p = remove; p < kUiAttachmentDraftParts; ++p)
            if (ui_widget_handle_matches(h, list.parts[r][p]))
                return {list.rows[r].id,
                        p == remove    ? UiAttachmentDraftAction::remove
                        : p == retry   ? UiAttachmentDraftAction::retry
                        : p == spoiler ? UiAttachmentDraftAction::spoiler
                                       : UiAttachmentDraftAction::describe,
                        h};
    return {};
}
UiWidgetHandle ui_attachment_draft_list_handle(const UiAttachmentDraftList& list, u64 id,
                                               UiAttachmentDraftAction action) noexcept {
    const u32 p = part(action);
    if (p < kUiAttachmentDraftParts)
        for (u32 r = 0; r < list.rows.count; ++r)
            if (list.rows[r].id == id)
                return list.parts[r][p];
    return ui_widget_invalid_handle();
}
UiRect ui_attachment_draft_list_preview(const UiAttachmentDraftList& list, u32 row, UiRect* clip) noexcept {
    const auto* n = node(list, row, preview);
    if (!n) {
        if (clip)
            reset_value(clip);
        return {};
    }
    if (clip)
        *clip = ui_rect_intersection(n->content_rectangle, n->clip_rectangle);
    return n->content_rectangle;
}
UiRect ui_attachment_draft_list_progress(const UiAttachmentDraftList& list, u32 row) noexcept {
    const auto* content = node(list, row, card);
    const auto* metadata = node(list, row, header);
    if (!content || !metadata || !ui_attachment_draft_progress_visible(list.rows[row]))
        return {};
    const float top = metadata->rectangle.bottom + list.style.gap;
    return {content->content_rectangle.left, top, content->content_rectangle.right,
            top + list.style.progress_height};
}
bool ui_attachment_draft_list_paint(const UiAttachmentDraftList& list, const Theme& theme,
                                    PaintStream* paint, bool focus_visible) noexcept {
    if (!list.manager || !paint)
        return false;
    for (u32 r = 0; r < list.rows.count; ++r) {
        const auto& view = list.rows[r];
        const auto* n = node(list, r, card);
        if (!n)
            return false;
        if (!paint_stream_push_clip(paint, n->clip_rectangle))
            return false;
        bool ok = ui_background_paint_surface(paint, n->rectangle, theme, theme.surface, theme.interactive,
                                              list.style.radius, 1);
        const auto picture = ui_attachment_draft_list_preview(list, r);
        ok = ok && ui_background_paint_surface(paint, picture, theme, theme.base_lowest, theme.interactive,
                                               list.style.radius, 1);
        if (ok && (!view.preview_ready || view.spoiler)) {
            const float extent = minimum(24.f, minimum(ui_rect_width(picture), ui_rect_height(picture)));
            const float x = (picture.left + picture.right - extent) * .5f,
                        y = (picture.top + picture.bottom - extent) * .5f;
            ok = ui_icon_paint(paint, view.spoiler ? UiIcon::eye_off : UiIcon::attachment,
                               {x, y, x + extent, y + extent}, theme.text_secondary);
        }
        if (ok && ui_attachment_draft_progress_visible(view)) {
            const float fraction =
                view.bytes
                    ? minimum(1.f,
                              static_cast<float>(static_cast<double>(view.uploaded_bytes) / view.bytes))
                    : 0;
            const auto bar = ui_attachment_draft_list_progress(list, r);
            ok = paint_stream_push(paint, bar, theme.interactive);
            if (ok && fraction > 0)
                ok = paint_stream_push(paint, bar.left, bar.top, bar.left + ui_rect_width(bar) * fraction,
                                       bar.bottom, theme.tokens.emphasis.outline);
        }
        for (u32 p = remove; ok && p < kUiAttachmentDraftParts; ++p) {
            const auto* b = node(list, r, p);
            if (!b)
                return false;
            if (!b->visible)
                continue;
            const bool focused = ui_widget_handle_matches(list.manager->focused, list.parts[r][p]) && focus_visible;
            ok = ui_background_paint_surface(paint, b->rectangle, theme, theme.surface_high,
                                             focused || (p == spoiler && view.spoiler) ? theme.tokens.border.focus
                                                                                       : theme.interactive,
                                             list.style.radius, focused ? 2.f : 1.f);
        }
        if (!paint_stream_pop_clip(paint) || !ok)
            return false;
    }
    return true;
}
bool ui_attachment_draft_list_text(const UiAttachmentDraftList& list, const Theme& theme,
                                   UiTextOutput* output) noexcept {
    for (u32 r = 0; r < list.rows.count; ++r)
        for (u32 p = name; p < kUiAttachmentDraftParts; ++p) {
            const auto text =
                ui_attachment_draft_list_label(list, list.key_base + r * kUiAttachmentDraftParts + p);
            if (!text.bytes)
                continue;
            const auto* n = node(list, r, p);
            if (!n || !n->visible)
                continue;
            const auto clip = ui_rect_intersection(n->content_rectangle, n->clip_rectangle);
            if (!ui_rect_has_area(clip))
                continue;
            const bool button = p >= remove,
                       enabled =
                           !button || ui_widget_manager_input_enabled(*list.manager, list.parts[r][p]);
            if (!ui_text_output_push(output,
                                     {text,
                                      {list.manager, list.parts[r][p]},
                                      n->content_rectangle,
                                      clip,
                                      !enabled || p == status ? theme.text_secondary : theme.text_primary,
                                      text_style_none,
                                      button ? UiTextAlignment::center : UiTextAlignment::start,
                                      UiTextAlignment::start,
                                      p == status ? UiTextWrapping::wrap : UiTextWrapping::no_wrap,
                                      p == status ? UiTextOverflow::clip : UiTextOverflow::ellipsis}))
                return false;
        }
    return true;
}
} // namespace maicord
