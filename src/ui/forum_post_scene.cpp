#include "maicord/ui/forum_post_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/widget_builder.hpp"
#include "maicord/ui/widget_scroll.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/style_lengths.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/text_measure_cache.hpp"

namespace maicord {
namespace {
constexpr u32 attachment_key_base = 4096;
u32 idx(ForumPostElement e) noexcept {
    return static_cast<u32>(e);
}
bool edit(ForumPostElement e) noexcept {
    return e == ForumPostElement::title || e == ForumPostElement::body;
}
bool button(ForumPostElement e) noexcept {
    return e == ForumPostElement::cancel || e == ForumPostElement::inspect ||
           e == ForumPostElement::accept || e == ForumPostElement::risk || e == ForumPostElement::tags ||
           e == ForumPostElement::add_files;
}
u32 field_index(ForumPostElement e) noexcept {
    return e == ForumPostElement::body ? 1u : 0u;
}
Utf8View label(const ForumPostContent& c, u32 key) noexcept {
    if (key >= kForumPostElements)
        return key - kForumPostElements < c.tags.count ? c.tags[key - kForumPostElements].name : Utf8View{};
    // Byte offset of each element's label inside the content; elements without one hold 0xffff.
    constexpr u16 none = 0xffff;
    static constexpr u16 members[kForumPostElements]{
        none,
        none,
        __builtin_offsetof(ForumPostContent, heading),
        __builtin_offsetof(ForumPostContent, context),
        __builtin_offsetof(ForumPostContent, title_label),
        __builtin_offsetof(ForumPostContent, fields),
        __builtin_offsetof(ForumPostContent, body_label),
        __builtin_offsetof(ForumPostContent, fields) + sizeof(ForumPostFieldView),
        __builtin_offsetof(ForumPostContent, hint),
        __builtin_offsetof(ForumPostContent, tags_hint),
        none,
        __builtin_offsetof(ForumPostContent, status),
        __builtin_offsetof(ForumPostContent, risk),
        none,
        __builtin_offsetof(ForumPostContent, cancel),
        __builtin_offsetof(ForumPostContent, inspect),
        __builtin_offsetof(ForumPostContent, accept),
        none,
        none,
        __builtin_offsetof(ForumPostContent, add_files),
        __builtin_offsetof(ForumPostContent, attachment_status),
        none};
    static_assert(__builtin_offsetof(ForumPostFieldView, text) == 0);
    return members[key] == none ? Utf8View{}
                                : *reinterpret_cast<const Utf8View*>(reinterpret_cast<const u8*>(&c) + members[key]);
}
bool natural(ForumPostScene* s, u32 key, UiSize constraint, UiTextMeasureValue* value) noexcept {
    auto text = key >= attachment_key_base ? ui_attachment_draft_list_label(s->attachment_list, key)
                                           : label(s->content, key);
    return ui_text_measure_cache_measure(
               s->measure,
               {{0x464F52554D504F53ull, hash64_counted_bytes(kHash64Seed, text.data, text.bytes), 1,
                 static_cast<u16>(key), s->content.measure_revision},
                text,
                {},
                constraint,
                key >= attachment_key_base
                    ? ui_attachment_draft_list_single_line(s->attachment_list, key)
                    : (key >= kForumPostElements || button(static_cast<ForumPostElement>(key)))},
               value)
        .ok();
}
bool measure(void* context, const UiLayoutMeasureRequest& r, UiSize* size, float* baseline) noexcept {
    auto* s = static_cast<ForumPostScene*>(context);
    if (!s || !r.key)
        return false;
    const u32 key = r.key >= attachment_key_base ? r.key : r.key - 1;
    if (!(key >= attachment_key_base ? ui_attachment_draft_list_label(s->attachment_list, key)
                                     : label(s->content, key))
             .bytes) {
        reset_value(size);
        *baseline = 0;
        return true;
    }
    UiTextMeasureValue value{};
    if (!natural(s, key, r.constraint, &value))
        return false;
    *size = value.size;
    *baseline = value.baseline;
    if (key >= kForumPostElements || button(static_cast<ForumPostElement>(key)))
        size->width = minimum(size->width, r.constraint.width);
    return true;
}
UiWidgetHandle target_handle(const ForumPostScene& s, ForumPostTarget t) noexcept {
    if (t.element == ForumPostElement::attachments) {
        const auto handle =
            ui_attachment_draft_list_handle(s.attachment_list, t.attachment_id, t.attachment_action);
        if (t.attachment_widget.generation && (handle.slot != t.attachment_widget.slot ||
                                               handle.generation != t.attachment_widget.generation))
            return ui_widget_invalid_handle();
        return handle;
    }
    if (t.element == ForumPostElement::tags)
        return t.tag < s.content.tags.count ? s.tags[t.tag] : ui_widget_invalid_handle();
    return idx(t.element) < kForumPostElements ? s.elements[idx(t.element)] : ui_widget_invalid_handle();
}
// The element, tag or attachment control that owns a widget; count when none does.
ForumPostTarget target_of(const ForumPostScene& s, UiWidgetHandle h) noexcept {
    const auto attachment = ui_attachment_draft_list_target(s.attachment_list, h);
    if (attachment.id)
        return {ForumPostElement::attachments, 0, attachment.id, attachment.action, attachment.widget};
    for (u32 i = 0; i < s.content.tags.count; ++i)
        if (ui_widget_handle_matches(h, s.tags[i]))
            return {ForumPostElement::tags, i};
    for (u32 i = 0; i < kForumPostElements; ++i)
        if (ui_widget_handle_matches(h, s.elements[i]))
            return {static_cast<ForumPostElement>(i), 0};
    return {ForumPostElement::count, 0};
}
bool build(ForumPostScene* s) noexcept {
    if (!s->storage.arena.base) {
        if (!ui_fixed_widget_storage_initialize(&s->storage,
                                                kForumPostElements + kForumPostTagCapacity +
                                                    kUiAttachmentDraftMaximum * kUiAttachmentDraftParts) ||
            !ui_widget_manager_initialize(&s->widgets, s->storage.widgets, s->storage.layout_nodes))
            return false;
    } else
        ui_widget_manager_reset(&s->widgets);
    for (auto& h : s->elements)
        h = ui_widget_invalid_handle();
    for (auto& h : s->tags)
        h = ui_widget_invalid_handle();
    if (!ui_widget_manager_set_measure_callback(&s->widgets, measure, s))
        return false;
    UiWidgetBuilder b(s->widgets);
    const ForumPostElement order[] = {ForumPostElement::root,
                                      ForumPostElement::panel,
                                      ForumPostElement::heading,
                                      ForumPostElement::context,
                                      ForumPostElement::content,
                                      ForumPostElement::title_label,
                                      ForumPostElement::title,
                                      ForumPostElement::body_label,
                                      ForumPostElement::body,
                                      ForumPostElement::hint,
                                      ForumPostElement::attachment_toolbar,
                                      ForumPostElement::add_files,
                                      ForumPostElement::attachment_status,
                                      ForumPostElement::attachments,
                                      ForumPostElement::tags_hint,
                                      ForumPostElement::tags,
                                      ForumPostElement::status,
                                      ForumPostElement::risk,
                                      ForumPostElement::actions,
                                      ForumPostElement::cancel,
                                      ForumPostElement::inspect,
                                      ForumPostElement::accept};
    for (u32 ordinal = 0; ordinal < kForumPostElements + s->content.tags.count; ++ordinal) {
        const bool tag = ordinal >= kForumPostElements;
        const u32 key = tag ? ordinal : idx(order[ordinal]);
        const auto e = tag ? ForumPostElement::tags : order[ordinal];
        if (!tag &&
            ((e == ForumPostElement::risk || e == ForumPostElement::inspect) && !s->content.uncertain))
            continue;
        UiLayoutStyle style = ui_layout_style();
        style.width = ui_layout_fill();
        style.height = ui_layout_content();
        UiWidgetKind kind = UiWidgetKind::text;
        u16 flags = ui_widget_none;
        UiWidgetHandle parent = s->elements[idx(ForumPostElement::content)];
        if (e == ForumPostElement::root) {
            parent = ui_widget_invalid_handle();
            kind = UiWidgetKind::container;
            style.height = ui_layout_fill();
            style.flags |= ui_layout_node_clip_children;
        } else if (e == ForumPostElement::panel) {
            parent = s->elements[idx(ForumPostElement::root)];
            kind = UiWidgetKind::container;
            style.flow = UiLayoutFlow::vertical;
            style.gap = s->content.style.gap;
            style.padding = {s->content.style.padding, s->content.style.padding, s->content.style.padding,
                             s->content.style.padding};
            style.horizontal_alignment = UiLayoutAlignment::center;
            style.vertical_alignment = UiLayoutAlignment::center;
            style.flags |= ui_layout_node_clip_children;
        } else if (e == ForumPostElement::content) {
            parent = s->elements[idx(ForumPostElement::panel)];
            kind = UiWidgetKind::scroll_view;
            style.flow = UiLayoutFlow::vertical;
            style.height = ui_layout_fill();
            style.gap = s->content.style.gap;
            style.flags |= ui_layout_node_clip_children | ui_layout_node_scroll_vertical;
        } else if (e == ForumPostElement::heading || e == ForumPostElement::context) {
            parent = s->elements[idx(ForumPostElement::panel)];
            style.flags |= ui_layout_node_text_content | ui_layout_node_allow_zero_size;
        } else if ((e == ForumPostElement::tags && !tag) || e == ForumPostElement::actions ||
                   e == ForumPostElement::attachments || e == ForumPostElement::attachment_toolbar) {
            kind = UiWidgetKind::container;
            style.flow = UiLayoutFlow::horizontal_wrap;
            // Optional groups have no geometry when their inventory is empty.
            style.flags |= ui_layout_node_allow_zero_size;
            style.gap = e == ForumPostElement::attachments ? s->content.attachment_style.gap : 8;
            if (e == ForumPostElement::actions)
                parent = s->elements[idx(ForumPostElement::panel)];
        } else if (edit(e)) {
            kind = UiWidgetKind::text_field;
            style.height = ui_layout_fixed(e == ForumPostElement::title ? s->content.style.title_height
                                                                        : s->content.style.body_height);
            style.padding = {12, 10, 12, 10};
            style.flags |= ui_layout_node_clip_children;
            flags = ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable;
        } else {
            style.flags |= ui_layout_node_text_content | ui_layout_node_allow_zero_size;
            if (button(e)) {
                kind = UiWidgetKind::button;
                style.width = ui_layout_content();
                style.padding = {12, 8, 12, 8};
                style.minimum_size = {0, 36};
                flags = ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable;
                if (tag)
                    parent = s->elements[idx(ForumPostElement::tags)];
                else if (e == ForumPostElement::add_files)
                    parent = s->elements[idx(ForumPostElement::attachment_toolbar)];
                else if (e != ForumPostElement::risk)
                    parent = s->elements[idx(ForumPostElement::actions)];
            } else if (e == ForumPostElement::attachment_status)
                parent = s->elements[idx(ForumPostElement::attachment_toolbar)];
        }
        const auto added = b.widget(parent, {key + 1, utf8_literal("ForumPost")}, kind, flags, style);
        if (!added.ok())
            return false;
        if (tag)
            s->tags[key - kForumPostElements] = added.handle;
        else
            s->elements[key] = added.handle;
    }
    if (!ui_attachment_draft_list_build(
            &s->attachment_list, &s->widgets, s->elements[idx(ForumPostElement::attachments)],
            s->content.attachments, attachment_key_base, s->content.attachment_style))
        return false;
    return true;
}
bool fill(PaintStream* p, UiRect r, Color c) noexcept {
    return !ui_rect_has_area(r) || paint_stream_push(p, r, c);
}
} // namespace
const UiLayoutNode* forum_post_node(const ForumPostScene& s, ForumPostElement e) noexcept {
    return idx(e) < kForumPostElements ? ui_widget_manager_layout_node(s.widgets, s.elements[idx(e)])
                                       : nullptr;
}
bool forum_post_update(ForumPostScene* s, const ForumPostContent& c, UiRect viewport,
                       UiTextMeasureCache* cache) noexcept {
    if (!s || !cache || !cache->backend || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 96 ||
        ui_rect_height(viewport) < 64 || !c.heading.bytes || !c.title_label.bytes || !c.body_label.bytes ||
        !c.cancel.bytes || !c.accept.bytes || c.tags.count > kForumPostTagCapacity ||
        (!c.tags.data && c.tags.count) || c.attachments.count > kUiAttachmentDraftMaximum ||
        (!c.attachments.data && c.attachments.count))
        return false;
    const auto& style = c.style;
    // Each style's lengths lead it, in declaration order.
    static_assert(__builtin_offsetof(ForumPostStyle, corner_radius) == 6 * sizeof(float));
    static_assert(sizeof(UiAttachmentDraftStyle) == 8 * sizeof(float));
    const auto& attachment_style = c.attachment_style;
    if (!ui_lengths_valid(&style.maximum_width, 7) || !ui_lengths_valid(&attachment_style.minimum_card_width, 8))
        return false;
    if (attachment_style.minimum_card_width < 96 || attachment_style.preview_extent < 24 ||
        attachment_style.control_height < 24)
        return false;
    if (style.maximum_width < 96 || style.title_height < 24 || style.body_height < 24)
        return false;
    u64 layout = hash64_bytes(kHash64Seed, &viewport, sizeof(viewport));
    auto geometry_style = c.style;
    geometry_style.corner_radius = 0;
    layout = hash64_bytes(layout, &geometry_style, sizeof(geometry_style));
    layout = hash64_mix(layout, c.measure_revision);
    u64 presentation = kHash64Seed, tree = hash64_mix(kHash64Seed, c.uncertain);
    for (u32 i = 0; i < kForumPostElements + c.tags.count; ++i) {
        const auto text = label(c, i);
        if (!utf8_validate(text).valid)
            return false;
        if (i != idx(ForumPostElement::title) && i != idx(ForumPostElement::body))
            layout = hash64_counted_bytes(layout, text.data, text.bytes);
    }
    for (u32 i = 0; i < c.tags.count; ++i) {
        const auto& tag = c.tags[i];
        if (!tag.id || !tag.name.bytes)
            return false;
        for (u32 j = 0; j < i; ++j)
            if (c.tags[j].id == tag.id)
                return false;
        tree = hash64_mix(tree, tag.id);
        const u64 tag_flags = static_cast<u64>(tag.selected) | (static_cast<u64>(tag.enabled) << 1);
        presentation = hash64_mix(presentation, tag_flags);
    }
    tree = hash64_mix(tree, c.tags.count);
    tree = hash64_mix(tree, c.attachments.count);
    layout = hash64_bytes(layout, &c.attachment_style, sizeof(c.attachment_style));
    for (u32 i = 0; i < c.attachments.count; ++i) {
        const auto& row = c.attachments[i];
        if (!row.id || !row.filename.bytes || !utf8_validate(row.filename).valid ||
            !utf8_validate(row.status).valid || !utf8_validate(row.description).valid)
            return false;
        for (u32 j = 0; j < i; ++j)
            if (c.attachments[j].id == row.id)
                return false;
        tree = hash64_mix(tree, row.id);
        layout = hash64_counted_bytes(layout, row.filename.data, row.filename.bytes);
        layout = hash64_counted_bytes(layout, row.status.data, row.status.bytes);
        layout = hash64_mix(layout, row.description.bytes != 0);
        layout = hash64_mix(layout, row.can_retry);
        layout = hash64_mix(layout, ui_attachment_draft_progress_visible(row));
        presentation = hash64_mix(presentation, row.bytes);
        presentation = hash64_mix(presentation, row.uploaded_bytes);
        presentation = hash64_bytes(presentation, &row.preview, sizeof(row.preview));
        presentation = hash64_mix(presentation, static_cast<u64>(row.phase));
        presentation = hash64_mix(presentation, static_cast<u64>(row.spoiler) |
                                                    (static_cast<u64>(row.can_remove) << 1) |
                                                    (static_cast<u64>(row.can_describe) << 2) |
                                                    (static_cast<u64>(row.preview_ready) << 3));
    }
    presentation = hash64_mix(presentation, c.can_add_files);
    layout = hash64_mix(layout, tree);
    layout = hash64_mix(layout, cache->generation);
    layout = hash64_mix(layout, reinterpret_cast<usize>(cache));
    u64 text_signatures[2]{};
    for (u32 f = 0; f < 2; ++f) {
        const auto& field = c.fields[f];
        u32 offset = 0;
        if (!utf8_byte_offset_for_utf16_position(field.text, field.anchor_utf16, &offset) ||
            !utf8_byte_offset_for_utf16_position(field.text, field.caret_utf16, &offset) ||
            field.composition.count > kTextCompositionSpanCapacity ||
            (!field.composition.data && field.composition.count))
            return false;
        for (u32 i = 0; i < field.composition.count; ++i) {
            const auto& span = field.composition[i];
            if (span.start >= span.end || span.line_style > TextCompositionLineStyle::squiggle ||
                span.attribute > TextCompositionAttribute::other || span.color_flags > 7 ||
                !utf8_byte_offset_for_utf16_position(field.text, span.start, &offset) ||
                !utf8_byte_offset_for_utf16_position(field.text, span.end, &offset))
                return false;
            presentation = hash64_bytes(presentation, &span, sizeof(span));
        }
        text_signatures[f] = hash64_counted_bytes(kHash64Seed, field.text.data, field.text.bytes);
        presentation = hash64_mix(presentation, text_signatures[f]);
        presentation = hash64_mix(presentation, field.anchor_utf16);
        presentation = hash64_mix(presentation, field.caret_utf16);
        presentation = hash64_mix(presentation, field.caret_visible);
    }
    presentation = hash64_bytes(presentation, &c.style.corner_radius, sizeof(c.style.corner_radius));
    presentation = hash64_mix(presentation, c.owner_serial);
    presentation = hash64_mix(presentation, c.editable);
    presentation = hash64_mix(presentation, c.can_accept);
    presentation = hash64_mix(presentation, c.focus_visible);
    presentation = hash64_mix(presentation, c.risk_acknowledged);
    const bool was = s->active,
               changed = !was || layout != s->layout_signature || presentation != s->presentation_signature;
    auto prior = forum_post_focused(*s);
    reset_value(&prior.attachment_widget);
    copy_value(&s->content, c);
    s->measure = cache;
    const bool rebuilt = !s->widgets.count || tree != s->tree_signature;
    if (rebuilt && !build(s)) {
        forum_post_release(s);
        return false;
    }
    auto enable = [s](UiWidgetHandle h, bool enabled) noexcept {
        if (auto* widget = ui_widget_manager_resolve(&s->widgets, h))
            widget->flags =
                enabled ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : ui_widget_none;
    };
    s->attachment_list.rows = c.attachments;
    s->attachment_list.style = c.attachment_style;
    enable(s->elements[idx(ForumPostElement::add_files)], c.editable && c.can_add_files);
    enable(s->elements[idx(ForumPostElement::title)], c.editable);
    enable(s->elements[idx(ForumPostElement::body)], c.editable);
    enable(s->elements[idx(ForumPostElement::accept)],
           c.can_accept && (!c.uncertain || c.risk_acknowledged));
    u32 selected_count = 0;
    for (u32 i = 0; i < c.tags.count; ++i)
        if (c.tags[i].selected)
            ++selected_count;
    for (u32 i = 0; i < c.tags.count; ++i)
        enable(s->tags[i], c.editable && c.tags[i].enabled &&
                               (c.tags[i].selected || selected_count < kForumPostSelectedTagMaximum));
    const float inset =
        minimum(style.inset, 8.f + minimum(ui_rect_width(viewport), ui_rect_height(viewport)) * .1f);
    const float pad = ui_rect_width(viewport) < 240 ? 8.f : style.padding;
    const float panel_width = minimum(style.maximum_width, ui_rect_width(viewport) - inset);
    if (!was || rebuilt || layout != s->layout_signature) {
        auto* panel = ui_layout_engine_resolve(
            &s->widgets.layout, s->widgets.storage[s->elements[idx(ForumPostElement::panel)].slot].layout);
        panel->spec.style.width = ui_layout_fixed(panel_width);
        const float panel_height =
            minimum(760.f, ui_rect_height(viewport) - minimum(inset, ui_rect_height(viewport) * .25f));
        panel->spec.style.height = ui_layout_fixed(panel_height);
        auto* content = ui_layout_engine_resolve(
            &s->widgets.layout,
            s->widgets.storage[s->elements[idx(ForumPostElement::content)].slot].layout);
        const bool compact = ui_rect_height(viewport) < 240;
        panel->spec.style.flags = ui_layout_node_visible | ui_layout_node_clip_children |
                                  (compact ? ui_layout_node_scroll_vertical : 0);
        content->spec.style.height = compact ? ui_layout_content() : ui_layout_fill();
        content->spec.style.gap = style.gap;
        static constexpr ForumPostElement fields[]{ForumPostElement::title, ForumPostElement::body};
        for (const auto field : fields) {
            auto* node = ui_layout_engine_resolve(&s->widgets.layout,
                                                  s->widgets.storage[s->elements[idx(field)].slot].layout);
            node->spec.style.height =
                ui_layout_fixed(field == ForumPostElement::title ? style.title_height : style.body_height);
        }
        content->spec.style.flags = ui_layout_node_visible | ui_layout_node_clip_children |
                                    (compact ? 0 : ui_layout_node_scroll_vertical);
        panel->spec.style.padding = {pad, pad, pad, pad};
        panel->spec.style.gap = style.gap;
        const float available = panel->spec.style.width.value - pad * 2;
        for (u32 i = 0; i < kForumPostElements + c.tags.count; ++i) {
            const bool tag = i >= kForumPostElements;
            const auto e = tag ? ForumPostElement::tags : static_cast<ForumPostElement>(i);
            if ((!tag && !button(e)) || (!tag && e == ForumPostElement::tags))
                continue;
            const auto handle = tag ? s->tags[i - kForumPostElements] : s->elements[i];
            if (!handle.generation)
                continue;
            UiTextMeasureValue value{};
            if (!natural(s, i, {kMaximumUiCoordinate, kMaximumUiCoordinate}, &value))
                return false;
            auto* node =
                ui_layout_engine_resolve(&s->widgets.layout, s->widgets.storage[handle.slot].layout);
            node->spec.style.minimum_size.width =
                minimum(maximum(tag ? 40.f : 80.f, value.size.width + 24), available);
            node->spec.style.maximum_size.width = available;
            node->spec.style.height = ui_layout_fixed(maximum(36.f, value.size.height + 16));
        }
        if (!ui_attachment_draft_list_layout(&s->attachment_list, available, c.editable))
            return false;
        // Reserve complete control labels before the card's wrapping flow
        // assigns rows. Ellipsis is reserved for names and exceptional widths.
        for (u32 row = 0; row < c.attachments.count; ++row) {
            for (u32 part = 7; part < kUiAttachmentDraftParts; ++part) {
                const u32 key = attachment_key_base + row * kUiAttachmentDraftParts + part;
                UiTextMeasureValue value{};
                if (!natural(s, key, {kMaximumUiCoordinate, kMaximumUiCoordinate}, &value))
                    return false;
                const auto handle = s->attachment_list.parts[row][part];
                auto* node =
                    ui_layout_engine_resolve(&s->widgets.layout, s->widgets.storage[handle.slot].layout);
                node->spec.style.minimum_size.width =
                    minimum(maximum(40.f, value.size.width + c.attachment_style.control_padding * 2),
                            node->spec.style.maximum_size.width);
            }
        }
        if (!ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok())
            return false;
    }
    if (!ui_attachment_draft_list_layout(&s->attachment_list, maximum(1.f, panel_width - pad * 2), c.editable))
        return false;
    s->viewport = viewport;
    s->layout_signature = layout;
    s->tree_signature = tree;
    s->presentation_signature = presentation;
    s->text_signature[0] = text_signatures[0];
    s->text_signature[1] = text_signatures[1];
    s->active = true;
    ui_widget_manager_reconcile_input(&s->widgets);
    if (changed)
        ++s->revision;
    if (rebuilt && prior.element != ForumPostElement::count)
        forum_post_focus(s, prior);
    if (!was || forum_post_focused(*s).element == ForumPostElement::count)
        return forum_post_focus(s, {c.editable ? ForumPostElement::title : ForumPostElement::cancel, 0});
    return true;
}
void forum_post_release(ForumPostScene* s) noexcept {
    if (s) {
        ui_fixed_widget_storage_release(&s->storage);
        reset_value(s);
    }
}
ForumPostTarget forum_post_hit_test(const ForumPostScene& s, float x, float y) noexcept {
    return s.active ? target_of(s, ui_widget_manager_hit_test(s.widgets, x, y))
                    : ForumPostTarget{ForumPostElement::count, 0};
}
ForumPostTarget forum_post_focused(const ForumPostScene& s) noexcept {
    return s.active ? target_of(s, s.widgets.focused) : ForumPostTarget{ForumPostElement::count, 0};
}
bool forum_post_focus(ForumPostScene* s, ForumPostTarget t) noexcept {
    if (!s || !s->active ||
        (!edit(t.element) && !button(t.element) && t.element != ForumPostElement::attachments))
        return false;
    const auto h = target_handle(*s, t);
    if (!ui_widget_manager_focus(&s->widgets, h))
        return false;
    const auto revision = s->widgets.layout.revision;
    const bool ok = ui_widget_reveal(&s->widgets, h).ok();
    if (s->widgets.layout.revision != revision)
        ++s->revision;
    return ok;
}
bool forum_post_move_focus(ForumPostScene* s, bool reverse) noexcept {
    if (!s || !s->active)
        return false;
    // Visual order is independent of retained storage insertion order.
    ForumPostTarget order[kForumPostTagCapacity + 7 + kUiAttachmentDraftMaximum * 4]{};
    u32 count = 0;
    order[count++] = {ForumPostElement::title, 0};
    order[count++] = {ForumPostElement::body, 0};
    order[count++] = {ForumPostElement::add_files, 0};
    const UiAttachmentDraftAction attachment_actions[] = {
        UiAttachmentDraftAction::remove, UiAttachmentDraftAction::retry, UiAttachmentDraftAction::spoiler,
        UiAttachmentDraftAction::describe};
    for (u32 r = 0; r < s->content.attachments.count; ++r)
        for (auto action : attachment_actions)
            order[count++] = {ForumPostElement::attachments, 0, s->content.attachments[r].id, action, {}};
    for (u32 i = 0; i < s->content.tags.count; ++i)
        order[count++] = {ForumPostElement::tags, i};
    if (s->content.uncertain)
        order[count++] = {ForumPostElement::risk, 0};
    order[count++] = {ForumPostElement::cancel, 0};
    if (s->content.uncertain)
        order[count++] = {ForumPostElement::inspect, 0};
    order[count++] = {ForumPostElement::accept, 0};
    const auto current = forum_post_focused(*s);
    u32 position = count;
    for (u32 i = 0; i < count; ++i)
        if (order[i].element == current.element && order[i].tag == current.tag &&
            order[i].attachment_id == current.attachment_id &&
            order[i].attachment_action == current.attachment_action) {
            position = i;
            break;
        }
    for (u32 step = 0; step < count; ++step) {
        position = reverse ? (position == 0 || position == count ? count - 1 : position - 1)
                           : (position == count || position + 1 == count ? 0 : position + 1);
        if (forum_post_focus(s, order[position]))
            return true;
    }
    return false;
}

UiScrollbarGeometry forum_post_scrollbar(const ForumPostScene& s) noexcept {
    if (!s.active)
        return {};
    UiWidgetScrollGeometry geometry{};
    const auto owner = s.elements[idx(ui_rect_height(s.viewport) < 240 ? ForumPostElement::panel
                                                                       : ForumPostElement::content)];
    const auto* panel = forum_post_node(s, ForumPostElement::panel);
    if (!panel || !ui_widget_scroll_geometry(s.widgets, owner, &geometry))
        return {};
    auto viewport = geometry.viewport;
    // Place the shared overlay thumb in the modal's existing themed padding.
    viewport.right = maximum(viewport.right, minimum(panel->rectangle.right - s.content.style.corner_radius,
                                                     viewport.right + s.content.style.padding));
    return ui_scrollbar_geometry(viewport, geometry.offset.y,
                                 ui_rect_height(geometry.viewport) + geometry.maximum.y);
}
bool forum_post_scroll(ForumPostScene* s, float delta) noexcept {
    if (!s || !s->active || !ui_coordinate_valid(delta))
        return false;
    UiWidgetScrollGeometry geometry{};
    const auto h = s->elements[idx(ui_rect_height(s->viewport) < 240 ? ForumPostElement::panel
                                                                     : ForumPostElement::content)];
    if (!ui_widget_scroll_geometry(s->widgets, h, &geometry))
        return false;
    const float offset = minimum(maximum(0.f, geometry.offset.y + delta), geometry.maximum.y);
    if (offset == geometry.offset.y)
        return true;
    const bool ok =
        ui_layout_engine_set_scroll(&s->widgets.layout, s->widgets.storage[h.slot].layout, {0, offset}) &&
        ui_widget_manager_layout_rectangle(&s->widgets, s->viewport).ok();
    if (ok)
        ++s->revision;
    return ok;
}
bool forum_post_paint(const ForumPostScene& s, const TextFormEditGeometry (&geometry)[2],
                      const Theme& theme, PaintStream* p) noexcept {
    if (!s.active)
        return true;
    const auto* panel = forum_post_node(s, ForumPostElement::panel);
    if (!p || !panel ||
        !paint_stream_push_clip(p, s.viewport))
        return false;
    bool ok =
        fill(p, s.viewport, theme.tokens.overlay.scrim) &&
        ui_surface_paint(p, panel->rectangle, theme, UiElevation::dialog,
                         s.content.style.corner_radius + panel->spec.style.padding.left, nullptr, &theme.interactive);
    const auto focused = forum_post_focused(s);
    for (u32 i = 0; ok && i < kForumPostElements + s.content.tags.count; ++i) {
        const bool tag = i >= kForumPostElements;
        const auto e = tag ? ForumPostElement::tags : static_cast<ForumPostElement>(i);
        if ((!tag && !edit(e) && !button(e)) || (!tag && e == ForumPostElement::tags))
            continue;
        const auto h = tag ? s.tags[i - kForumPostElements] : s.elements[i];
        if (!h.generation)
            continue;
        const auto* node = ui_widget_manager_layout_node(s.widgets, h);
        if (!node)
            return false;
        if (!paint_stream_push_clip(p, node->clip_rectangle)) {
            ok = false;
            break;
        }
        const bool has_focus = focused.element == e && (!tag || focused.tag == i - kForumPostElements);
        const bool selected = tag ? s.content.tags[i - kForumPostElements].selected
                                  : (e == ForumPostElement::risk && s.content.risk_acknowledged);
        const bool enabled = ui_widget_manager_input_enabled(s.widgets, h);
        const Color background =
            edit(e) ? theme.base_lowest
                    : (selected || (e == ForumPostElement::accept && enabled) ? theme.tokens.emphasis.fill
                                                                              : theme.surface_high);
        ok = paint_stream_push_rounded_border(p, node->rectangle, background, has_focus && s.content.focus_visible ? theme.tokens.border.focus : theme.interactive, s.content.style.corner_radius, has_focus && s.content.focus_visible ? 2.f : 1.f);
        if (edit(e)) {
            const auto& g = geometry[field_index(e)];
            const auto clip = ui_rect_overlap(node->content_rectangle, node->clip_rectangle);
            for (u32 j = 0; ok && j < g.selections.count; ++j)
                ok = fill(p, ui_rect_overlap(g.selections[j], clip), theme.interactive_hover);
            for (u32 j = 0; ok && j < g.composition.count; ++j)
                ok = text_composition_paint_mark(p, g.composition[j], clip, theme.accent);
            if (ok && has_focus && s.content.fields[field_index(e)].caret_visible)
                ok = fill(p, ui_rect_overlap(g.caret, clip), theme.text_primary);
        }
        ok = paint_stream_pop_clip(p) && ok;
    }
    ok = ok && ui_attachment_draft_list_paint(s.attachment_list, theme, p, s.content.focus_visible);
    // The covered shell's hover scrollbar filter must not hide this owner's
    // overflow cue. Restore it before returning to the underlying renderer.
    const bool filtered = p->filter_scrollbars;
    p->filter_scrollbars = false;
    ok = ok && ui_scrollbar_paint(p, forum_post_scrollbar(s), theme);
    p->filter_scrollbars = filtered;
    return paint_stream_pop_clip(p) && ok;
}
bool forum_post_text_output(const ForumPostScene& s, const TextFormEditGeometry (&geometry)[2],
                            const Theme& theme, UiTextOutput* output) noexcept {
    if (!s.active)
        return true;
    for (u32 i = 0; i < kForumPostElements + s.content.tags.count; ++i) {
        const auto text = label(s.content, i);
        if (!text.bytes)
            continue;
        const bool tag = i >= kForumPostElements;
        const auto e = tag ? ForumPostElement::tags : static_cast<ForumPostElement>(i);
        const auto h = tag ? s.tags[i - kForumPostElements] : s.elements[i];
        if (!h.generation)
            continue;
        const auto* node = ui_widget_manager_layout_node(s.widgets, h);
        if (!node)
            return false;
        const auto clip = ui_rect_overlap(node->content_rectangle, node->clip_rectangle);
        if (!ui_rect_has_area(clip))
            continue;
        const bool action = tag || button(e),
                   enabled = !action || ui_widget_manager_input_enabled(s.widgets, h);
        const bool secondary = e == ForumPostElement::hint || e == ForumPostElement::tags_hint ||
                               e == ForumPostElement::context;
        if (!ui_text_output_push(
                output,
                {text,
                 {&s.widgets, h},
                 edit(e) ? geometry[field_index(e)].text_rectangle : node->content_rectangle,
                 clip,
                 !enabled || secondary ? theme.text_secondary : theme.text_primary,
                 text_style_none,
                 action ? UiTextAlignment::center : UiTextAlignment::start,
                 UiTextAlignment::start,
                 action || e == ForumPostElement::title ? TextWrapping::no_wrap : TextWrapping::wrap,
                 action ? TextOverflow::ellipsis : TextOverflow::clip}))
            return false;
    }
    return ui_attachment_draft_list_text(s.attachment_list, theme, output);
}
} // namespace maicord
