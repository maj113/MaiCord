#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/ui/text_form.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/widget_scroll.hpp"
#include "maicord/ui/widget_builder.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
u32 index(TextFormElement element) noexcept {
    return static_cast<u32>(element);
}
Utf8View label(const TextFormContent& content, TextFormElement element) noexcept {
    switch (element) {
    case TextFormElement::title:
        return content.title;
    case TextFormElement::context:
        return content.context;
    case TextFormElement::label:
        return content.label;
    case TextFormElement::status:
        return content.status;
    case TextFormElement::option:
        return content.option;
    case TextFormElement::cancel:
        return content.cancel;
    case TextFormElement::accept:
        return content.accept;
    case TextFormElement::edit:
        return content.text;
    default:
        return {};
    }
}
bool control(TextFormElement element) noexcept {
    return element == TextFormElement::edit || element == TextFormElement::cancel ||
           element == TextFormElement::accept || element == TextFormElement::option;
}
// The checkbox square and the gap before its label.
constexpr float kOptionBox = 20.0f;
constexpr float kOptionGap = 8.0f;
bool action(TextFormElement element) noexcept {
    return element == TextFormElement::cancel || element == TextFormElement::accept;
}
u32 text_style(TextFormElement element) noexcept {
    // Modal heading, then a compact bold field label above the editor.
    return element == TextFormElement::title   ? text_style_heading_2 | text_style_bold
           : element == TextFormElement::label ? text_style_subtext | text_style_bold
                                               : text_style_none;
}
bool measure_text(TextFormScene* scene, TextFormElement element, UiSize constraint,
                  UiTextMeasureValue* value) noexcept {
    const auto text = label(scene->content, element);
    const u64 signature = hash64_counted_bytes(kHash64Seed, text.data, text.bytes);
    const TextStyleSpan style{0, text.bytes, text_style(element)};
    return ui_text_measure_cache_measure(
               scene->measure,
               {{0x54455854464F524Dull, signature, 1, static_cast<u16>(element), scene->measure_revision},
                text,
                {&style, style.flags ? 1u : 0u},
                constraint,
                action(element)},
               value)
        .ok();
}
bool measure_label(void* opaque, const UiLayoutMeasureRequest& request, UiSize* size,
                   float* baseline) noexcept {
    auto* scene = static_cast<TextFormScene*>(opaque);
    if (!scene || !scene->measure || request.key == 0 || request.key > kTextFormElements)
        return false;
    const auto element = static_cast<TextFormElement>(request.key - 1);
    const auto text = label(scene->content, element);
    if (!text.bytes) {
        *size = {};
        *baseline = 0;
        return true;
    }
    UiTextMeasureValue value{};
    if (!measure_text(scene, element, request.constraint, &value))
        return false;
    *size = value.size;
    // Action widths are reserved from an unconstrained single-line measurement
    // before layout. Very narrow viewports clip/ellipsize the label instead.
    if (action(element))
        size->width = minimum(size->width, request.constraint.width);
    *baseline = value.baseline;
    return true;
}
bool build(TextFormScene* scene) noexcept {
    if (!scene->storage.arena.base) {
        if (!ui_fixed_widget_storage_initialize(&scene->storage, kTextFormElements) ||
            !ui_widget_manager_initialize(&scene->widgets, scene->storage.widgets,
                                          scene->storage.layout_nodes))
            return false;
    } else
        ui_widget_manager_reset(&scene->widgets);
    for (auto& element : scene->elements)
        element = ui_widget_invalid_handle();
    if (!ui_widget_manager_set_measure_callback(&scene->widgets, measure_label, scene))
        return false;
    constexpr TextFormElement parents[] = {
        TextFormElement::count,   TextFormElement::root,   TextFormElement::panel, TextFormElement::panel,
        TextFormElement::panel,   TextFormElement::panel,  TextFormElement::panel, TextFormElement::panel,
        TextFormElement::panel,   TextFormElement::actions, TextFormElement::actions};
    constexpr Utf8View names[] = {utf8_literal("TextForm"), utf8_literal("Panel"),  utf8_literal("Title"),
                                  utf8_literal("Context"),  utf8_literal("Label"),  utf8_literal("Edit"),
                                  utf8_literal("Status"),   utf8_literal("Option"), utf8_literal("Actions"),
                                  utf8_literal("Cancel"),   utf8_literal("Accept")};
    static_assert(sizeof(parents) / sizeof(parents[0]) == kTextFormElements &&
                  sizeof(names) / sizeof(names[0]) == kTextFormElements);
    UiWidgetBuilder widgets(scene->widgets);
    for (u32 i = 0; i < kTextFormElements; ++i) {
        const auto element = static_cast<TextFormElement>(i);
        if ((scene->content.confirmation_only &&
             (element == TextFormElement::label || element == TextFormElement::edit)) ||
            (element == TextFormElement::option && !scene->content.option.bytes))
            continue;
        UiLayoutStyle style = ui_layout_style();
        style.width = ui_layout_fill();
        style.height = ui_layout_content();
        UiWidgetKind kind = UiWidgetKind::text;
        u16 flags = ui_widget_none;
        if (element == TextFormElement::root) {
            kind = UiWidgetKind::container;
            style.height = ui_layout_fill();
            style.flags |= ui_layout_node_clip_children;
        } else if (element == TextFormElement::panel) {
            kind = UiWidgetKind::scroll_view;
            style.flow = UiLayoutFlow::vertical;
            style.gap = ui_theme_metrics(scene->theme_styles).form_group_gap * .5f;
            const float padding = ui_theme_metrics(scene->theme_styles).form_field_padding + 8;
            style.padding = {padding, padding, padding, padding};
            style.horizontal_alignment = UiLayoutAlignment::center;
            style.vertical_alignment = UiLayoutAlignment::center;
            style.flags |= ui_layout_node_clip_children | ui_layout_node_scroll_vertical;
        } else if (element == TextFormElement::actions) {
            kind = UiWidgetKind::container;
            style.flow = UiLayoutFlow::horizontal_wrap;
            style.justification = UiLayoutJustification::end;
            style.gap = ui_theme_metrics(scene->theme_styles).form_group_gap * .5f;
            style.margin.top = style.gap;
        } else if (element == TextFormElement::edit) {
            kind = UiWidgetKind::text_field;
            style.height = ui_layout_fixed(
                scene->content.single_line ? ui_theme_metrics(scene->theme_styles).form_single_height + 4
                                           : ui_theme_metrics(scene->theme_styles).form_multiline_height);
            const float padding = ui_theme_metrics(scene->theme_styles).form_field_padding;
            style.padding = {padding, maximum(0.f, padding - 2), padding, maximum(0.f, padding - 2)};
            style.flags |= ui_layout_node_clip_children;
        } else if (element == TextFormElement::option) {
            // A full-width checkbox row: the square, then the wrapped label.
            kind = UiWidgetKind::button;
            style.flags |= ui_layout_node_text_content;
            style.padding = {kOptionBox + kOptionGap, 0, 0, 0};
            style.minimum_size = {0, kOptionBox};
            style.margin.top = 4;
        } else {
            style.flags |= ui_layout_node_text_content | ui_layout_node_allow_zero_size;
            if (control(element)) {
                kind = UiWidgetKind::button;
                style.width = ui_layout_content();
                style.minimum_size = {80, 36};
                style.padding = {16, 8, 16, 8};
            }
        }
        if (control(element))
            flags = ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable;
        const UiWidgetHandle parent = parents[i] == TextFormElement::count
                                          ? ui_widget_invalid_handle()
                                          : scene->elements[index(parents[i])];
        const UiWidgetResult added = widgets.widget(parent, {i + 1, names[i]}, kind, flags, style);
        if (!added.ok()) {
            return false;
        }
        scene->elements[i] = added.handle;
    }
    return true;
}
Color shade(Color color, float amount) noexcept {
    return {color.red * amount, color.green * amount, color.blue * amount, color.alpha};
}
bool button_enabled(const TextFormScene& scene, TextFormElement element) noexcept {
    return element == TextFormElement::accept ? scene.content.can_accept : scene.content.can_cancel;
}
bool widget_is(UiWidgetHandle a, UiWidgetHandle b) noexcept {
    return a.generation && a.slot == b.slot && a.generation == b.generation;
}
Color button_fill(const TextFormScene& scene, TextFormElement element, const Theme& theme) noexcept {
    const bool accept = element == TextFormElement::accept;
    const Color fill = accept ? (scene.content.danger ? theme.tokens.accent.danger : theme.tokens.emphasis.fill)
                              : theme.interactive;
    if (!button_enabled(scene, element))
        return ui_theme_color_alpha(fill, .5f);
    const auto handle = scene.elements[index(element)];
    const bool hovered = widget_is(scene.widgets.hovered, handle);
    const bool pressed = hovered && widget_is(scene.widgets.captured, handle);
    if (!accept)
        return pressed || hovered ? theme.interactive_hover : fill;
    return pressed ? shade(fill, .82f) : hovered ? shade(fill, .9f) : fill;
}
UiRect intersect(UiRect a, UiRect b) noexcept {
    return {maximum(a.left, b.left), maximum(a.top, b.top), minimum(a.right, b.right),
            minimum(a.bottom, b.bottom)};
}
bool fill(PaintStream* stream, UiRect rectangle, Color color) noexcept {
    return !ui_rect_has_area(rectangle) || paint_stream_push(stream, rectangle.left, rectangle.top,
                                                             rectangle.right, rectangle.bottom, color);
}
} // namespace

const UiLayoutNode* text_form_node(const TextFormScene& scene, TextFormElement element) noexcept {
    return index(element) < kTextFormElements
               ? ui_widget_manager_layout_node(scene.widgets, scene.elements[index(element)])
               : nullptr;
}

UiRect text_form_option_box(const TextFormScene& scene) noexcept {
    const auto* node = scene.content.option.bytes ? text_form_node(scene, TextFormElement::option) : nullptr;
    if (!node)
        return {};
    // Centred on the first label line (20 DIP for one line of body text).
    const float line = minimum(ui_rect_height(node->rectangle), 20.0f);
    const float top = node->rectangle.top + (line - kOptionBox) * .5f;
    return {node->rectangle.left, top, node->rectangle.left + kOptionBox, top + kOptionBox};
}

bool text_form_bind_measure_revision(TextFormScene* scene, u16 revision) noexcept {
    if (!scene || !revision || scene->measure_revision == revision)
        return false;
    scene->measure_revision = revision;
    scene->layout_signature = 0;
    return true;
}

bool text_form_bind_theme_styles(TextFormScene* scene, const UiResolvedThemeStyles* styles) noexcept {
    if (!scene)
        return false;
    const u32 revision = styles && styles->ready ? styles->geometry_revision : 0;
    if (scene->theme_styles == styles && scene->theme_geometry_revision == revision)
        return false;
    scene->theme_styles = styles;
    scene->theme_geometry_revision = revision;
    scene->layout_signature = 0;
    return true;
}

bool text_form_update(TextFormScene* scene, const TextFormContent& content, UiRect viewport,
                      UiTextMeasureCache* measure) noexcept {
    if (!scene || !measure || !measure->backend || !ui_rect_valid(viewport) ||
        ui_rect_width(viewport) < 96 || ui_rect_height(viewport) < 64 || !content.title.bytes ||
        (!content.confirmation_only && !content.label.bytes) || !content.cancel.bytes ||
        !content.accept.bytes)
        return false;
    u64 signature = hash64_bytes(kHash64Seed, &viewport, sizeof(viewport));
    signature = hash64_mix(signature, scene->theme_geometry_revision);
    signature = hash64_mix(signature, scene->measure_revision);
    signature = hash64_mix(signature, content.confirmation_only);
    signature = hash64_mix(signature, content.single_line);
    signature = hash64_mix(signature, content.option.bytes != 0);
    for (u32 i = 0; i < kTextFormElements; ++i) {
        if (i == index(TextFormElement::edit) ||
            (content.confirmation_only && i == index(TextFormElement::label)))
            continue;
        const auto text = label(content, static_cast<TextFormElement>(i));
        if (!utf8_validate(text).valid)
            return false;
        signature = hash64_counted_bytes(signature, text.data, text.bytes);
    }
    if (!content.confirmation_only && !utf8_validate(content.text).valid)
        return false;
    if (!content.confirmation_only && ((!content.composition.data && content.composition.count) ||
                                       content.composition.count > kTextCompositionSpanCapacity))
        return false;
    u64 composition_signature = kHash64Seed;
    for (u32 i = 0; !content.confirmation_only && i < content.composition.count; ++i) {
        const auto& span = content.composition[i];
        u32 first = 0, last = 0;
        if (span.start >= span.end || span.line_style > TextCompositionLineStyle::squiggle ||
            span.attribute > TextCompositionAttribute::other || span.color_flags > 7 ||
            !utf8_byte_offset_for_utf16_position(content.text, span.start, &first) ||
            !utf8_byte_offset_for_utf16_position(content.text, span.end, &last))
            return false;
        composition_signature = hash64_bytes(composition_signature, &span, sizeof(span));
    }
    u32 anchor = 0, caret = 0;
    if (!content.confirmation_only &&
        (!utf8_byte_offset_for_utf16_position(content.text, content.anchor_utf16, &anchor) ||
         !utf8_byte_offset_for_utf16_position(content.text, content.caret_utf16, &caret)))
        return false;
    signature = hash64_mix(signature, measure->generation);
    signature = hash64_mix(signature, reinterpret_cast<usize>(measure));
    const bool was_active = scene->active;
    const u64 text_signature =
        content.confirmation_only
            ? 0
            : hash64_counted_bytes(kHash64Seed, content.text.data, content.text.bytes);
    const bool changed =
        !was_active || signature != scene->layout_signature || text_signature != scene->text_signature ||
        composition_signature != scene->composition_signature ||
        (!content.confirmation_only && (content.anchor_utf16 != scene->content.anchor_utf16 ||
                                        content.caret_utf16 != scene->content.caret_utf16)) ||
        content.can_accept != scene->content.can_accept ||
        content.can_cancel != scene->content.can_cancel || content.editable != scene->content.editable ||
        content.danger != scene->content.danger ||
        content.option_checked != scene->content.option_checked ||
        content.caret_visible != scene->content.caret_visible ||
        content.focus_visible != scene->content.focus_visible;
    scene->content = content;
    scene->measure = measure;
    const bool built_confirmation =
        scene->widgets.count && !scene->elements[index(TextFormElement::edit)].generation;
    const bool built_option = scene->elements[index(TextFormElement::option)].generation != 0;
    if ((!scene->widgets.count || content.confirmation_only != built_confirmation ||
         (content.option.bytes != 0) != built_option) &&
        !build(scene)) {
        text_form_release(scene);
        return false;
    }
    auto* accept =
        ui_widget_manager_resolve(&scene->widgets, scene->elements[index(TextFormElement::accept)]);
    accept->flags =
        content.can_accept ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : ui_widget_none;
    auto* cancel =
        ui_widget_manager_resolve(&scene->widgets, scene->elements[index(TextFormElement::cancel)]);
    cancel->flags =
        content.can_cancel ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : ui_widget_none;
    if (!content.confirmation_only) {
        auto* edit =
            ui_widget_manager_resolve(&scene->widgets, scene->elements[index(TextFormElement::edit)]);
        edit->flags = content.editable ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                       : ui_widget_none;
    }
    scene->active = false;
    if (!was_active || signature != scene->layout_signature) {
        const auto& metrics = ui_theme_metrics(scene->theme_styles);
        const float gap = metrics.form_group_gap * .5f;
        if (!content.confirmation_only) {
            auto* edit_layout = ui_layout_engine_resolve(
                &scene->widgets.layout,
                scene->widgets.storage[scene->elements[index(TextFormElement::edit)].slot].layout);
            edit_layout->spec.style.padding = {
                metrics.form_field_padding, maximum(0.f, metrics.form_field_padding - 2),
                metrics.form_field_padding, maximum(0.f, metrics.form_field_padding - 2)};
            edit_layout->spec.style.height = ui_layout_fixed(
                content.single_line ? maximum(metrics.control_height, metrics.form_single_height + 4)
                                    : maximum(metrics.form_multiline_height, metrics.control_height));
        }
        auto* panel = ui_layout_engine_resolve(
            &scene->widgets.layout,
            scene->widgets.storage[scene->elements[index(TextFormElement::panel)].slot].layout);
        panel->spec.style.gap = gap;
        auto* actions = ui_layout_engine_resolve(
            &scene->widgets.layout,
            scene->widgets.storage[scene->elements[index(TextFormElement::actions)].slot].layout);
        actions->spec.style.gap = gap;
        actions->spec.style.margin.top = gap;
        panel->spec.style.width =
            ui_layout_fixed(minimum(maximum(96.f, metrics.form_width - 200), ui_rect_width(viewport) - 16));
        panel->spec.style.maximum_size.height = ui_rect_height(viewport) - 16;
        panel->spec.style.padding =
            ui_rect_width(viewport) < 240
                ? UiInsets{8, 8, 8, 8}
                : UiInsets{metrics.form_field_padding + 8, metrics.form_field_padding + 8,
                           metrics.form_field_padding + 8, metrics.form_field_padding + 8};
        const float button_width = panel->spec.style.width.value - panel->spec.style.padding.left -
                                   panel->spec.style.padding.right;
        constexpr TextFormElement buttons[] = {TextFormElement::cancel, TextFormElement::accept};
        UiTextMeasureValue labels[2]{};
        float button_height = metrics.control_height + 4;
        for (u32 i = 0; i < 2; ++i) {
            if (!measure_text(scene, buttons[i], {kMaximumUiCoordinate, kMaximumUiCoordinate}, &labels[i]))
                return false;
            button_height = maximum(button_height, labels[i].size.height + 16);
        }
        for (u32 i = 0; i < 2; ++i) {
            const auto element = buttons[i];
            auto* button = ui_layout_engine_resolve(
                &scene->widgets.layout,
                scene->widgets.storage[scene->elements[index(element)].slot].layout);
            button->spec.style.minimum_size.width =
                minimum(maximum(80.0f, labels[i].size.width + 32), button_width);
            button->spec.style.maximum_size.width = button_width;
            button->spec.style.height = ui_layout_fixed(button_height);
        }
        if (!ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok())
            return false;
        scene->layout_signature = signature;
    }
    scene->viewport = viewport;
    scene->text_signature = text_signature;
    scene->composition_signature = composition_signature;
    scene->active = true;
    ui_widget_manager_reconcile_input(&scene->widgets);
    if (changed)
        ++scene->revision;
    if (!was_active || text_form_focused(*scene) == TextFormElement::count) {
        if (!content.confirmation_only && content.editable)
            return text_form_focus(scene, TextFormElement::edit);
        if (content.can_cancel)
            return text_form_focus(scene, TextFormElement::cancel);
        if (content.can_accept)
            return text_form_focus(scene, TextFormElement::accept);
    }
    return true;
}

void text_form_close(TextFormScene* scene) noexcept {
    if (!scene)
        return;
    scene->active = false;
    scene->content = {};
    scene->measure = nullptr;
    ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
    ui_widget_manager_capture(&scene->widgets, ui_widget_invalid_handle());
    ++scene->revision;
}
void text_form_release(TextFormScene* scene) noexcept {
    if (!scene)
        return;
    ui_fixed_widget_storage_release(&scene->storage);
    *scene = {};
}
TextFormElement text_form_hit_test(const TextFormScene& scene, float x, float y) noexcept {
    if (!scene.active)
        return TextFormElement::count;
    const auto target = ui_widget_manager_hit_test(scene.widgets, x, y);
    for (u32 i = 0; i < kTextFormElements; ++i)
        if (target.generation && target.slot == scene.elements[i].slot &&
            target.generation == scene.elements[i].generation)
            return static_cast<TextFormElement>(i);
    return TextFormElement::count;
}
TextFormElement text_form_focused(const TextFormScene& scene) noexcept {
    if (!scene.active)
        return TextFormElement::count;
    for (u32 i = 0; i < kTextFormElements; ++i)
        if (scene.widgets.focused.generation && scene.widgets.focused.slot == scene.elements[i].slot &&
            scene.widgets.focused.generation == scene.elements[i].generation)
            return static_cast<TextFormElement>(i);
    return TextFormElement::count;
}
bool text_form_focus(TextFormScene* scene, TextFormElement element) noexcept {
    if (!scene || !scene->active || !control(element) ||
        (scene->content.confirmation_only && element == TextFormElement::edit) ||
        !ui_widget_manager_focus(&scene->widgets, scene->elements[index(element)]))
        return false;
    const auto revision = scene->widgets.layout.revision;
    const bool valid = ui_widget_reveal(&scene->widgets, scene->elements[index(element)]).ok();
    if (scene->widgets.layout.revision != revision)
        ++scene->revision;
    return valid;
}
bool text_form_move_focus(TextFormScene* scene, bool reverse) noexcept {
    if (!scene || !scene->active)
        return false;
    const auto target = ui_widget_manager_move_focus(&scene->widgets, reverse);
    const auto revision = scene->widgets.layout.revision;
    const bool valid = target.generation && ui_widget_reveal(&scene->widgets, target).ok();
    if (scene->widgets.layout.revision != revision)
        ++scene->revision;
    return valid;
}
bool text_form_scroll(TextFormScene* scene, float delta) noexcept {
    if (!scene || !scene->active || !ui_coordinate_valid(delta))
        return false;
    UiWidgetScrollGeometry geometry{};
    const auto handle = scene->elements[index(TextFormElement::panel)];
    if (!ui_widget_scroll_geometry(scene->widgets, handle, &geometry))
        return false;
    const float offset = minimum(maximum(0.0f, geometry.offset.y + delta), geometry.maximum.y);
    if (offset == geometry.offset.y)
        return true;
    const bool valid =
        ui_layout_engine_set_scroll(&scene->widgets.layout, scene->widgets.storage[handle.slot].layout,
                                    {0, offset}) &&
        ui_widget_manager_layout_rectangle(&scene->widgets, scene->viewport).ok();
    if (valid)
        ++scene->revision;
    return valid;
}

bool text_form_paint(const TextFormScene& scene, const TextFormEditGeometry& edit, const Theme& theme,
                     PaintStream* paint) noexcept {
    if (!scene.active)
        return true;
    const auto* panel = text_form_node(scene, TextFormElement::panel);
    if (!paint || !panel ||
        !paint_stream_push_clip(paint, scene.viewport.left, scene.viewport.top, scene.viewport.right,
                                scene.viewport.bottom))
        return false;
    bool valid =
        fill(paint, scene.viewport, theme.tokens.overlay.scrim) &&
        ui_surface_paint(paint, panel->rectangle, theme, UiElevation::dialog, theme.scale.radius_lg) &&
        paint_stream_record_content(paint, {&scene.widgets, scene.elements[index(TextFormElement::panel)]},
                                    panel->rectangle, UiLayoutContentKind::control,
                                    ui_layout_content_bounds_allow_overlap);
    constexpr TextFormElement controls[] = {TextFormElement::edit, TextFormElement::option,
                                            TextFormElement::cancel, TextFormElement::accept};
    for (const auto element : controls) {
        if ((scene.content.confirmation_only && element == TextFormElement::edit) ||
            (element == TextFormElement::option && !scene.content.option.bytes))
            continue;
        const auto* node = text_form_node(scene, element);
        if (!valid || !node) {
            valid = false;
            break;
        }
        if (!paint_stream_push_clip(paint, node->clip_rectangle.left, node->clip_rectangle.top,
                                    node->clip_rectangle.right, node->clip_rectangle.bottom)) {
            valid = false;
            break;
        }
        const bool focused = text_form_focused(scene) == element;
        const bool outlined = focused && scene.content.focus_visible;
        const auto& metrics = ui_theme_metrics(scene.theme_styles);
        if (element == TextFormElement::edit) {
            valid = paint_stream_push_rounded_border(
                paint, node->rectangle.left, node->rectangle.top, node->rectangle.right,
                node->rectangle.bottom, theme.base_lowest, outlined ? theme.tokens.border.focus : theme.interactive,
                metrics.compact_radius, outlined ? 2.0f : 1.0f);
        } else if (element == TextFormElement::option) {
            // The checkbox: an outlined square, filled with a check when set.
            const auto r = text_form_option_box(scene);
            const bool checked = scene.content.option_checked;
            const Color fill = checked ? theme.tokens.emphasis.fill : theme.base_lowest;
            valid = paint_stream_push_rounded_border(paint, r.left, r.top, r.right, r.bottom, fill,
                                                     outlined ? theme.tokens.border.focus
                                                     : checked ? fill
                                                               : theme.text_secondary,
                                                     metrics.compact_radius, outlined ? 2.0f : 1.0f);
            if (valid && checked) {
                const float s = ui_rect_width(r) / 20.0f;
                const Color mark = theme.tokens.emphasis.text;
                valid = paint_stream_push_line(paint, {r.left + 5 * s, r.top + 10 * s}, {r.left + 8.5f * s, r.top + 13.5f * s},
                                               2 * s, mark) &&
                        paint_stream_push_line(paint, {r.left + 8.5f * s, r.top + 13.5f * s},
                                               {r.left + 15 * s, r.top + 6.5f * s}, 2 * s, mark);
            }
        } else {
            // Modal footer: a quiet secondary Cancel and a filled primary (or
            // destructive) action. Keyboard focus draws an offset ring.
            const auto r = node->rectangle;
            const Color fill = button_fill(scene, element, theme);
            const float ring = theme.scale.space_2 * .75f;
            valid = paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, fill,
                                              metrics.control_radius) &&
                    (!outlined || paint_stream_push_rounded_border(
                                      paint, r.left - ring, r.top - ring, r.right + ring, r.bottom + ring,
                                      ui_theme_color_alpha(fill, 0), theme.tokens.border.focus,
                                      metrics.control_radius + ring, 2.0f));
        }
        valid = valid && paint_stream_record_content(
                             paint, {&scene.widgets, scene.elements[index(element)]}, node->rectangle,
                             UiLayoutContentKind::control, ui_layout_content_bounds_allow_overlap);
        if (element == TextFormElement::edit) {
            const auto clip = intersect(node->content_rectangle, node->clip_rectangle);
            for (u32 i = 0; valid && i < edit.selections.count; ++i)
                valid = fill(paint, intersect(edit.selections[i], clip), theme.interactive_hover);
            for (u32 i = 0; valid && i < edit.composition.count; ++i) {
                const auto& mark = edit.composition[i];
                const bool targeted =
                    mark.attributes.attribute == TextCompositionAttribute::target_converted ||
                    mark.attributes.attribute == TextCompositionAttribute::target_not_converted;
                valid = text_composition_paint_mark(paint, mark, clip,
                                                    targeted ? theme.accent : theme.text_primary);
            }
            if (valid && focused && scene.content.caret_visible)
                valid = fill(paint, intersect(edit.caret, clip), theme.text_primary);
        }
        valid = paint_stream_pop_clip(paint) && valid;
    }
    return paint_stream_pop_clip(paint) && valid;
}
bool text_form_text_output(const TextFormScene& scene, const TextFormEditGeometry& edit, const Theme& theme,
                           UiTextOutput* stream) noexcept {
    if (!scene.active)
        return true;
    for (u32 i = 0; i < kTextFormElements; ++i) {
        const auto element = static_cast<TextFormElement>(i);
        if ((scene.content.confirmation_only &&
             (element == TextFormElement::label || element == TextFormElement::edit)) ||
            (element == TextFormElement::option && !scene.content.option.bytes))
            continue;
        const auto text = label(scene.content, element);
        if (!text.bytes)
            continue;
        const auto* node = text_form_node(scene, element);
        if (!node)
            return false;
        const auto clip = intersect(node->content_rectangle, node->clip_rectangle);
        if (!ui_rect_has_area(clip))
            continue;
        const bool button = action(element);
        Color color = element == TextFormElement::label ? theme.text_secondary : theme.text_primary;
        if (button) {
            Color fill = button_fill(scene, element, theme);
            fill.alpha = 1;
            color = element == TextFormElement::accept && !scene.content.danger &&
                            ui_theme_contrast_ratio(theme.tokens.emphasis.text, fill) >= 4.5f
                        ? theme.tokens.emphasis.text
                        : ui_surface_foreground(theme, fill);
            if (!button_enabled(scene, element))
                color.alpha *= .5f;
        }
        if (!ui_text_output_push(
                stream, {text,
                         {&scene.widgets, scene.elements[i]},
                         element == TextFormElement::edit ? edit.text_rectangle : node->content_rectangle,
                         clip,
                         color,
                         text_style(element),
                         button ? UiTextAlignment::center : UiTextAlignment::start,
                         UiTextAlignment::start,
                         button || (element == TextFormElement::edit && scene.content.single_line)
                             ? UiTextWrapping::no_wrap
                             : UiTextWrapping::wrap,
                         button ? UiTextOverflow::ellipsis : UiTextOverflow::clip}))
            return false;
    }
    return true;
}
} // namespace maicord

namespace maicord {
bool text_form_text(const TextFormScene& scene, const TextFormEditGeometry& edit, const Theme& theme,
                    UiTextCommandStream* stream) noexcept {
    UiTextOutput output{stream, nullptr};
    return text_form_text_output(scene, edit, theme, &output);
}
} // namespace maicord
