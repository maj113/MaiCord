#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/ui/text/form.hpp"
#include "maicord/ui/theme/foreground_color.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/text/edit_field.hpp"
#include "maicord/ui/theme/surface_elevation.hpp"
#include "maicord/ui/layout/widget_scroll.hpp"
#include "maicord/ui/core/widget_builder.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/text/measure_cache.hpp"

namespace maicord {
namespace {
u32 index(TextFormElement element) noexcept {
    return static_cast<u32>(element);
}
constexpr u32 kChoiceFirst = static_cast<u32>(TextFormElement::choice_0);
constexpr u32 kSelectFirst = static_cast<u32>(TextFormElement::select_0);
Utf8View label(const TextFormContent& content, TextFormElement element) noexcept {
    u32 at = 0;
    if (text_form_choice_index(element, &at))
        return at < content.choices.count ? content.choices[at] : Utf8View{};
    if (text_form_select_index(element, &at))
        return at < content.select_options.count ? content.select_options[at] : Utf8View{};
    switch (element) {
    case TextFormElement::choices_label:
        return content.choices_label;
    case TextFormElement::select_label:
        return content.select_label;
    case TextFormElement::select:
        return content.select_selected < content.select_options.count ? content.select_options[content.select_selected]
                                                                      : Utf8View{};
    case TextFormElement::title:
        return content.title;
    case TextFormElement::context:
        return content.context;
    case TextFormElement::qr_caption:
        return content.qr_caption;
    case TextFormElement::label:
        return content.label;
    case TextFormElement::detail:
        return content.detail;
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
// A radio row: a choice or an open dropdown's option.
bool radio(TextFormElement element) noexcept {
    u32 at = 0;
    return text_form_choice_index(element, &at) || text_form_select_index(element, &at);
}
// The colour picker's interactive parts.
bool picker_part(TextFormElement element) noexcept {
    return element == TextFormElement::color_field || element == TextFormElement::color_hue ||
           element == TextFormElement::eyedropper || element == TextFormElement::color_swatches;
}
bool control(TextFormElement element) noexcept {
    return element == TextFormElement::edit || element == TextFormElement::cancel ||
           element == TextFormElement::accept || element == TextFormElement::option ||
           element == TextFormElement::select || radio(element) || picker_part(element);
}
// Whether a form shows an element at all.
bool present(const TextFormContent& content, TextFormElement element) noexcept {
    u32 at = 0;
    if (text_form_choice_index(element, &at))
        return at < content.choices.count;
    if (text_form_select_index(element, &at))
        return content.select_expanded && at < content.select_options.count;
    switch (element) {
    case TextFormElement::label:
        return content.label.bytes && !content.picker && (!content.confirmation_only || content.detail.bytes);
    case TextFormElement::color_field:
    case TextFormElement::color_hue:
    case TextFormElement::color_row:
        return content.picker;
    case TextFormElement::eyedropper:
        return content.picker && content.color.eyedropper;
    case TextFormElement::color_swatches:
        return content.picker && content.color.suggested.count;
    case TextFormElement::detail:
        return content.confirmation_only && content.detail.bytes;
    case TextFormElement::edit:
        return !content.confirmation_only;
    case TextFormElement::cancel:
        return !content.single_action;
    case TextFormElement::option:
        return content.option.bytes != 0;
    case TextFormElement::qr:
        return content.qr.size != 0;
    case TextFormElement::qr_caption:
        return content.qr.size != 0 && content.qr_caption.bytes;
    case TextFormElement::choices_label:
        return content.choices.count && content.choices_label.bytes;
    case TextFormElement::select_label:
        return content.select_options.count && content.select_label.bytes;
    case TextFormElement::select:
        return content.select_options.count != 0;
    default:
        return true;
    }
}
u64 present_mask(const TextFormContent& content) noexcept {
    u64 mask = 0;
    for (u32 i = 0; i < kTextFormElements; ++i)
        if (present(content, static_cast<TextFormElement>(i)))
            mask |= 1ull << i;
    return mask;
}
bool built(const TextFormScene& scene, TextFormElement element) noexcept {
    return (scene.built >> index(element)) & 1;
}
// `first`, the number `a`, `second`, then (unless b is ~0u) the number `b` and `third`, as a C string.
void format_value(char (&out)[40], const char* first, u32 a, const char* second, u32 b = ~0u,
                  const char* third = "") noexcept {
    u32 at = 0;
    const auto put = [&](const char* text) noexcept {
        while (*text && at + 1 < sizeof(out))
            out[at++] = *text++;
    };
    put(first);
    at += decimal_write(out + at, a);
    put(second);
    if (b != ~0u) {
        at += decimal_write(out + at, b);
        put(third);
    }
    out[at] = 0;
}
// The editor folds away while another choice than its own is chosen.
bool editor_collapsed(const TextFormContent& content) noexcept {
    return !content.confirmation_only && content.choice_editor != ~0u &&
           content.choice_selected != content.choice_editor;
}
// The checkbox square and the gap before its label.
constexpr float kOptionBox = 20.0f;
constexpr float kOptionGap = 8.0f;
// The QR code's light square (its 8 DIP quiet zone included).
constexpr float kQrSide = 192.0f;
// The colour swatch square at the editor's end and the editor's end padding
// that makes room for it.
float swatch_side(const UiThemeMetrics& metrics) noexcept {
    return metrics.control_height * .75f;
}
float edit_end_padding(const TextFormContent& content, const UiThemeMetrics& metrics) noexcept {
    return metrics.form_field_padding + (content.swatch ? swatch_side(metrics) + metrics.form_field_padding * .5f : 0);
}
bool action(TextFormElement element) noexcept {
    return element == TextFormElement::cancel || element == TextFormElement::accept;
}
u32 text_style(TextFormElement element) noexcept {
    // Modal heading, then compact bold field labels above the controls.
    return element == TextFormElement::title ? text_style_heading_2 | text_style_bold
           : element == TextFormElement::label || element == TextFormElement::choices_label ||
                   element == TextFormElement::select_label
               ? text_style_subtext | text_style_bold
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
bool measure_label(void* opaque, const UiLayoutMeasureRequest& request, UiSize* size) noexcept {
    auto* scene = static_cast<TextFormScene*>(opaque);
    if (!scene->measure || request.key == 0 || request.key > kTextFormElements)
        return false;
    const auto element = static_cast<TextFormElement>(request.key - 1);
    const auto text = label(scene->content, element);
    if (!text.bytes) {
        reset_value(size);
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
    static constexpr char names[] =
        "TextForm\0Panel\0Title\0Context\0Qr\0QrCaption\0Choices\0Choice1\0Choice2\0Choice3\0Choice4\0ColorField\0"
        "ColorHue\0Label\0"
        "Detail\0ColorRow\0Edit\0Eyedropper\0Swatches\0SelectLabel\0Select\0Option1\0Option2\0Option3\0Option4\0"
        "Option5\0Option6\0Option7\0Option8\0Status\0Option\0Actions\0Cancel\0Accept\0";
    static_assert(name_list_count(names) == kTextFormElements);
    const bool picker = scene->content.picker;
    const auto parent_of = [picker](TextFormElement element) noexcept {
        return element == TextFormElement::root    ? TextFormElement::count
               : element == TextFormElement::panel ? TextFormElement::root
               : element == TextFormElement::cancel || element == TextFormElement::accept
                   ? TextFormElement::actions
               : picker && (element == TextFormElement::edit || element == TextFormElement::eyedropper)
                   ? TextFormElement::color_row
                   : TextFormElement::panel;
    };
    UiWidgetBuilder widgets(scene->widgets);
    scene->built = present_mask(scene->content);
    for (u32 i = 0; i < kTextFormElements; ++i) {
        const auto element = static_cast<TextFormElement>(i);
        if (!(scene->built >> i & 1))
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
            style.horizontal_alignment = UiLayoutAlignment::center;
            style.vertical_alignment = UiLayoutAlignment::center;
            style.flags |= ui_layout_node_clip_children | ui_layout_node_scroll_vertical;
        } else if (element == TextFormElement::actions) {
            kind = UiWidgetKind::container;
            style.flow = UiLayoutFlow::horizontal_wrap;
            style.justification = UiLayoutJustification::end;
        } else if (element == TextFormElement::color_row) {
            // The hex field and the eyedropper side by side, 12 DIP apart.
            kind = UiWidgetKind::container;
            style.flow = UiLayoutFlow::horizontal;
            style.vertical_alignment = UiLayoutAlignment::center;
            style.gap = kColorPickerSwatchGap;
        } else if (element == TextFormElement::qr) {
            // A fixed light square, centred, with room around it.
            kind = UiWidgetKind::container;
            style.width = ui_layout_fixed(kQrSide);
            style.height = ui_layout_fixed(kQrSide);
            style.margin.top = style.margin.bottom = 8.f;
        } else if (picker_part(element)) {
            // Square, hue bar and swatches fill the width at fixed heights (the swatches' set at layout); the
            // eyedropper is a square button.
            kind = UiWidgetKind::button;
            const float side = ui_theme_metrics(scene->theme_styles).control_height;
            style.height = ui_layout_fixed(element == TextFormElement::color_field ? kColorPickerFieldHeight
                                           : element == TextFormElement::color_hue ? kColorPickerHueHeight
                                                                                   : side);
            if (element == TextFormElement::eyedropper)
                style.width = ui_layout_fixed(side);
        } else if (element == TextFormElement::edit) {
            kind = UiWidgetKind::text_field;
            style.flags |= ui_layout_node_clip_children;
        } else if (element == TextFormElement::option || radio(element)) {
            // A full-width checkbox or radio row: the marker, then the wrapped label.
            kind = UiWidgetKind::button;
            style.flags |= ui_layout_node_text_content;
            style.padding = {kOptionBox + kOptionGap, 0, 0, 0};
            style.minimum_size = {0, kOptionBox};
            style.margin.top = element == TextFormElement::option ? 4.f : 0.f;
        } else if (element == TextFormElement::select) {
            // A field showing the chosen option, a chevron at its end.
            kind = UiWidgetKind::button;
            style.flags |= ui_layout_node_text_content;
            const float padding = ui_theme_metrics(scene->theme_styles).form_field_padding;
            style.padding = {padding, maximum(0.f, padding - 2), padding + kOptionBox, maximum(0.f, padding - 2)};
            style.minimum_size = {0, ui_theme_metrics(scene->theme_styles).control_height};
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
        const TextFormElement parent_element = parent_of(element);
        const UiWidgetHandle parent = parent_element == TextFormElement::count
                                          ? ui_widget_invalid_handle()
                                          : scene->elements[index(parent_element)];
        const UiWidgetResult added = widgets.widget(parent, {i + 1, name_list_at(names, i)}, kind, flags, style);
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
Color button_fill(const TextFormScene& scene, TextFormElement element, const Theme& theme) noexcept {
    const bool accept = element == TextFormElement::accept;
    const Color fill = accept ? (scene.content.danger ? theme.tokens.accent.danger : theme.tokens.emphasis.fill)
                              : theme.interactive;
    if (!button_enabled(scene, element))
        return ui_theme_color_alpha(fill, .5f);
    const auto handle = scene.elements[index(element)];
    const bool hovered = ui_widget_handle_matches(scene.widgets.hovered, handle);
    const bool pressed = hovered && ui_widget_handle_matches(scene.widgets.captured, handle);
    if (!accept)
        return pressed || hovered ? theme.interactive_hover : fill;
    return pressed ? shade(fill, .82f) : hovered ? shade(fill, .9f) : fill;
}
bool fill(PaintStream* stream, const UiRect& rectangle, const Color& color) noexcept {
    return !ui_rect_has_area(rectangle) || paint_stream_push(stream, rectangle, color);
}
// The check/radio marker centred on the first label line (20 DIP for one line of body text).
UiRect marker_box(const UiLayoutNode* node) noexcept {
    if (!node)
        return {};
    const float line = minimum(ui_rect_height(node->rectangle), 20.0f);
    const float top = node->rectangle.top + (line - kOptionBox) * .5f;
    return {node->rectangle.left, top, node->rectangle.left + kOptionBox, top + kOptionBox};
}
bool group_index(TextFormElement element, u32 first, u32 count, u32* at) noexcept {
    const u32 value = index(element);
    if (value < first || value >= first + count)
        return false;
    *at = value - first;
    return true;
}
} // namespace

const UiLayoutNode* text_form_node(const TextFormScene& scene, TextFormElement element) noexcept {
    return index(element) < kTextFormElements
               ? ui_widget_manager_layout_node(scene.widgets, scene.elements[index(element)])
               : nullptr;
}

bool text_form_choice_index(TextFormElement element, u32* at) noexcept {
    return group_index(element, kChoiceFirst, kTextFormChoices, at);
}

bool text_form_select_index(TextFormElement element, u32* at) noexcept {
    return group_index(element, kSelectFirst, kTextFormSelectOptions, at);
}

UiRect text_form_choice_marker(const TextFormScene& scene, TextFormElement element) noexcept {
    return marker_box(radio(element) && built(scene, element) ? text_form_node(scene, element) : nullptr);
}

UiRect text_form_option_box(const TextFormScene& scene) noexcept {
    return marker_box(scene.content.option.bytes ? text_form_node(scene, TextFormElement::option) : nullptr);
}

bool text_form_bind_measure_revision(TextFormScene* scene, u16 revision) noexcept {
    if (!revision || scene->measure_revision == revision)
        return false;
    scene->measure_revision = revision;
    scene->layout_signature = 0;
    return true;
}

bool text_form_bind_theme_styles(TextFormScene* scene, const UiResolvedThemeStyles* styles) noexcept {
    const u32 revision = styles && styles->ready ? styles->geometry_revision : 0;
    if (scene->theme_styles == styles && scene->theme_geometry_revision == revision)
        return false;
    scene->theme_styles = styles;
    scene->theme_geometry_revision = revision;
    scene->layout_signature = 0;
    return true;
}

bool text_form_update(TextFormScene* scene, const TextFormContent& content, const UiRect& viewport,
                      UiTextMeasureCache* measure) noexcept {
    if (!measure || !measure->backend || !ui_rect_valid(viewport) ||
        ui_rect_width(viewport) < 96 || ui_rect_height(viewport) < 64 || !content.title.bytes ||
        (!content.confirmation_only && !content.label.bytes && !content.choices.count) ||
        (!content.single_action && !content.cancel.bytes) || (content.detail.bytes && !content.confirmation_only) ||
        !content.accept.bytes || content.choices.count > kTextFormChoices ||
        (!content.choices.data && content.choices.count) || content.select_options.count > kTextFormSelectOptions ||
        (!content.select_options.data && content.select_options.count) ||
        (content.choice_selected != ~0u && content.choice_selected >= content.choices.count) ||
        (content.choice_editor != ~0u && content.choice_editor >= content.choices.count) ||
        (content.select_options.count && content.select_selected >= content.select_options.count) ||
        (content.picker && (content.confirmation_only || !content.swatch ||
                            content.color.suggested.count > kColorPickerSuggestedColors ||
                            (!content.color.suggested.data && content.color.suggested.count))))
        return false;
    u64 signature = hash64_bytes(kHash64Seed, &viewport, sizeof(viewport));
    signature = hash64_mix(signature, scene->theme_geometry_revision);
    signature = hash64_mix(signature, scene->measure_revision);
    signature = hash64_mix(signature, content.confirmation_only);
    signature = hash64_mix(signature, content.single_line);
    signature = hash64_mix(signature, content.option.bytes != 0);
    signature = hash64_mix(signature, present_mask(content));
    signature = hash64_mix(signature, editor_collapsed(content));
    signature = hash64_mix(signature, content.swatch);
    signature = hash64_mix(signature, content.picker | content.color.eyedropper << 1);
    signature = hash64_mix(signature, content.color.suggested.count);
    signature = hash64_mix(signature, content.qr.size);
    for (u32 i = 0; i < kTextFormElements; ++i) {
        if (i == index(TextFormElement::edit) ||
            (i == index(TextFormElement::label) && !present(content, TextFormElement::label)))
            continue;
        const auto text = label(content, static_cast<TextFormElement>(i));
        if (!utf8_validate(text).valid)
            return false;
        signature = hash64_counted_bytes(signature, text.data, text.bytes);
    }
    if (!content.confirmation_only && (!utf8_validate(content.text).valid || !utf8_validate(content.placeholder).valid))
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
        content.choice_selected != scene->content.choice_selected ||
        content.select_selected != scene->content.select_selected ||
        content.select_expanded != scene->content.select_expanded ||
        content.caret_visible != scene->content.caret_visible ||
        content.focus_visible != scene->content.focus_visible || content.error != scene->content.error ||
        content.swatch_rgba != scene->content.swatch_rgba ||
        !utf8_equal(content.placeholder, scene->content.placeholder) ||
        (content.picker &&
         (content.color.hsv.hue != scene->content.color.hsv.hue ||
          content.color.hsv.saturation != scene->content.color.hsv.saturation ||
          content.color.hsv.value != scene->content.color.hsv.value ||
          content.color.focused_swatch != scene->content.color.focused_swatch ||
          content.color.picking != scene->content.color.picking ||
          content.color.suggested.data != scene->content.color.suggested.data));
    copy_value(&scene->content, content);
    scene->measure = measure;
    if ((!scene->widgets.count || present_mask(content) != scene->built) && !build(scene)) {
        text_form_release(scene);
        return false;
    }
    auto* accept =
        ui_widget_manager_resolve(&scene->widgets, scene->elements[index(TextFormElement::accept)]);
    accept->flags =
        content.can_accept ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : ui_widget_none;
    if (auto* cancel = ui_widget_manager_resolve(&scene->widgets, scene->elements[index(TextFormElement::cancel)]))
        cancel->flags =
            content.can_cancel ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable : ui_widget_none;
    const bool collapsed = editor_collapsed(content);
    if (!content.confirmation_only) {
        auto* edit =
            ui_widget_manager_resolve(&scene->widgets, scene->elements[index(TextFormElement::edit)]);
        edit->flags = content.editable && !collapsed ? ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable
                                                     : ui_widget_none;
    }
    scene->active = false;
    if (!was_active || signature != scene->layout_signature) {
        const auto& metrics = ui_theme_metrics(scene->theme_styles);
        const float gap = metrics.form_group_gap * .5f;
        if (!content.confirmation_only) {
            auto* edit_layout =
                ui_widget_manager_layout_node(&scene->widgets, scene->elements[index(TextFormElement::edit)]);
            edit_layout->spec.style.padding =
                collapsed ? UiInsets{}
                          : UiInsets{metrics.form_field_padding, maximum(0.f, metrics.form_field_padding - 2),
                                     edit_end_padding(content, metrics),
                                     maximum(0.f, metrics.form_field_padding - 2)};
            edit_layout->spec.style.height = ui_layout_fixed(
                collapsed             ? 0.f
                : content.single_line ? maximum(metrics.control_height, metrics.form_single_height + 4)
                                      : maximum(metrics.form_multiline_height, metrics.control_height));
        }
        auto* panel = ui_widget_manager_layout_node(&scene->widgets, scene->elements[index(TextFormElement::panel)]);
        panel->spec.style.gap = gap;
        auto* actions =
            ui_widget_manager_layout_node(&scene->widgets, scene->elements[index(TextFormElement::actions)]);
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
        if (content.picker && content.color.suggested.count) {
            auto* swatches = ui_widget_manager_layout_node(&scene->widgets,
                                                           scene->elements[index(TextFormElement::color_swatches)]);
            swatches->spec.style.height = ui_layout_fixed(color_picker_swatches_height(
                panel->spec.style.width.value - panel->spec.style.padding.left - panel->spec.style.padding.right,
                static_cast<u32>(content.color.suggested.count)));
        }
        const float button_width = panel->spec.style.width.value - panel->spec.style.padding.left -
                                   panel->spec.style.padding.right;
        static constexpr TextFormElement buttons[] = {TextFormElement::cancel, TextFormElement::accept};
        UiTextMeasureValue labels[2]{};
        float button_height = metrics.control_height + 4;
        for (u32 i = 0; i < 2; ++i) {
            if (!built(*scene, buttons[i]))
                continue;
            if (!measure_text(scene, buttons[i], {kMaximumUiCoordinate, kMaximumUiCoordinate}, &labels[i]))
                return false;
            button_height = maximum(button_height, labels[i].size.height + 16);
        }
        for (u32 i = 0; i < 2; ++i) {
            const auto element = buttons[i];
            auto* button = ui_widget_manager_layout_node(&scene->widgets, scene->elements[index(element)]);
            if (!button)
                continue;
            // Padding plus one DIP: a label drawn at device pixels can end a fraction past its measured width, which
            // ellipsized "Cancel" in a button sized to it exactly.
            button->spec.style.minimum_size.width =
                minimum(maximum(80.0f, labels[i].size.width + 33), button_width);
            button->spec.style.maximum_size.width = button_width;
            button->spec.style.height = ui_layout_fixed(button_height);
        }
        if (!ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok())
            return false;
        scene->layout_signature = signature;
    }
    scene->viewport = viewport;
    if (content.picker) {
        // "Saturation 40%, brightness 70%" and "Hue 200 degrees", the values screen readers read.
        const auto percent = [](float v) noexcept { return static_cast<u32>(v * 100 + .5f); };
        format_value(scene->color_values[0], "Saturation ", percent(content.color.hsv.saturation), "%, brightness ",
                     percent(content.color.hsv.value), "%");
        format_value(scene->color_values[1], "Hue ", static_cast<u32>(content.color.hsv.hue * 360 + .5f) % 360,
                     " degrees");
        const auto& swatches = content.color.suggested;
        const u32 at = content.color.focused_swatch;
        const u32 bytes = at < swatches.count ? color_hex_format(swatches[at] << 8 | 0xFF, false,
                                                                 {scene->color_values[2], 39})
                                              : 0;
        scene->color_values[2][bytes] = 0;
    }
    scene->text_signature = text_signature;
    scene->composition_signature = composition_signature;
    scene->active = true;
    ui_widget_manager_reconcile_input(&scene->widgets);
    if (changed)
        ++scene->revision;
    if (!was_active || text_form_focused(*scene) == TextFormElement::count) {
        if (!content.confirmation_only && content.editable && !collapsed)
            return text_form_focus(scene, TextFormElement::edit);
        if (content.choices.count && content.editable)
            return text_form_focus(scene, TextFormElement::choice_0);
        if (content.can_cancel && !content.single_action)
            return text_form_focus(scene, TextFormElement::cancel);
        if (content.can_accept)
            return text_form_focus(scene, TextFormElement::accept);
    }
    return true;
}

void text_form_release(TextFormScene* scene) noexcept {
    ui_fixed_widget_storage_release(&scene->storage);
    reset_value(scene);
}
UiRect text_form_swatch_box(const TextFormScene& scene) noexcept {
    const auto* node = text_form_node(scene, TextFormElement::edit);
    if (!scene.active || !scene.content.swatch || !node || editor_collapsed(scene.content))
        return {};
    const auto& metrics = ui_theme_metrics(scene.theme_styles);
    const float side = swatch_side(metrics), right = node->rectangle.right - metrics.form_field_padding;
    const float top = scene.content.single_line ? (node->rectangle.top + node->rectangle.bottom - side) * .5f
                                                : node->rectangle.top + metrics.form_field_padding;
    return {right - side, top, right, top + side};
}
namespace {
TextFormElement element_of(const TextFormScene& scene, UiWidgetHandle handle) noexcept {
    for (u32 i = 0; i < kTextFormElements; ++i)
        if (ui_widget_handle_matches(handle, scene.elements[i]))
            return static_cast<TextFormElement>(i);
    return TextFormElement::count;
}
// Scrolls the widget into view; a layout change bumps the scene revision.
bool reveal(TextFormScene* scene, UiWidgetHandle handle) noexcept {
    const auto revision = scene->widgets.layout.revision;
    const bool valid = ui_widget_reveal(&scene->widgets, handle).ok();
    if (scene->widgets.layout.revision != revision)
        ++scene->revision;
    return valid;
}
} // namespace
TextFormElement text_form_hit_test(const TextFormScene& scene, float x, float y) noexcept {
    return scene.active ? element_of(scene, ui_widget_manager_hit_test(scene.widgets, x, y)) : TextFormElement::count;
}
TextFormElement text_form_focused(const TextFormScene& scene) noexcept {
    return scene.active ? element_of(scene, scene.widgets.focused) : TextFormElement::count;
}
// An edit field is only built when the form is not confirmation-only.
bool text_form_focus(TextFormScene* scene, TextFormElement element) noexcept {
    return scene->active && control(element) && built(*scene, element) &&
           !(element == TextFormElement::edit && editor_collapsed(scene->content)) &&
           ui_widget_manager_focus(&scene->widgets, scene->elements[index(element)]) &&
           reveal(scene, scene->elements[index(element)]);
}
bool text_form_move_focus(TextFormScene* scene, bool reverse) noexcept {
    if (!scene->active)
        return false;
    const auto target = ui_widget_manager_move_focus(&scene->widgets, reverse);
    return target.generation && reveal(scene, target);
}
bool text_form_scroll(TextFormScene* scene, float delta) noexcept {
    if (!scene->active || !ui_coordinate_valid(delta))
        return false;
    UiWidgetScrollGeometry geometry{};
    const auto handle = scene->elements[index(TextFormElement::panel)];
    if (!ui_widget_scroll_geometry(scene->widgets, handle, &geometry))
        return false;
    const float offset = minimum(maximum(0.0f, geometry.offset.y + delta), geometry.maximum.y);
    if (offset == geometry.offset.y)
        return true;
    const bool valid =
        ui_widget_manager_set_scroll(&scene->widgets, handle, {0, offset}) &&
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
        !paint_stream_push_clip(paint, scene.viewport))
        return false;
    bool valid =
        fill(paint, scene.viewport, theme.tokens.overlay.scrim) &&
        ui_surface_paint(paint, panel->rectangle, theme, UiElevation::dialog, theme.scale.radius_lg);
    if (valid && built(scene, TextFormElement::qr)) {
        // As the sign-in QR: light surface, dark modules, its 8 DIP padding the quiet zone; clipped to the panel.
        const auto* node = text_form_node(scene, TextFormElement::qr);
        valid = node && paint_stream_push_clip(paint, panel->rectangle) &&
                paint_stream_push_rounded(paint, node->rectangle, theme.text_primary, 4.f) &&
                qr_code_paint(paint, node->rectangle, scene.content.qr, 1.f, 8.f, theme.base_lowest) &&
                paint_stream_pop_clip(paint);
    }
    const auto focus = text_form_focused(scene);
    const auto& metrics = ui_theme_metrics(scene.theme_styles);
    for (u32 slot = 0; slot < kTextFormElements; ++slot) {
        const auto element = static_cast<TextFormElement>(slot);
        if (!control(element) || !built(scene, element) ||
            (element == TextFormElement::edit && editor_collapsed(scene.content)))
            continue;
        const auto* node = text_form_node(scene, element);
        if (!valid || !node) {
            valid = false;
            break;
        }
        // A button's focus ring sits outside it: clip buttons to the panel
        // rather than the content box they lie in.
        UiRect bounds = node->clip_rectangle;
        if (action(element) || picker_part(element)) {
            // The picker's thumbs also reach past their square and bar.
            const float ring = picker_part(element) ? kColorPickerHueHeight * .5f : theme.scale.space_2 * .75f;
            bounds = ui_rect_intersection(
                {bounds.left - ring, bounds.top - ring, bounds.right + ring, bounds.bottom + ring}, panel->rectangle);
        }
        if (!paint_stream_push_clip(paint, bounds)) {
            valid = false;
            break;
        }
        const bool focused = focus == element;
        const bool outlined = focused && scene.content.focus_visible;
        if (element == TextFormElement::edit) {
            // An error keeps its outline while focused, like settings fields.
            const bool error = scene.content.error;
            valid = paint_stream_push_rounded_border(paint, node->rectangle, theme.base_lowest, error ? theme.tokens.text.danger : outlined ? theme.tokens.border.focus : theme.interactive, metrics.compact_radius, error || outlined ? 2.0f : 1.0f);
            const auto swatch = text_form_swatch_box(scene);
            if (valid && ui_rect_has_area(swatch)) {
                const u32 rgba = scene.content.swatch_rgba;
                const Color color = color_from_rgba(rgba);
                valid = paint_stream_push_rounded_border(paint, swatch, color, theme.tokens.border.strong, theme.scale.radius_xs, 1);
            }
        } else if (element == TextFormElement::color_field || element == TextFormElement::color_hue) {
            const auto& color = scene.content.color;
            valid = element == TextFormElement::color_field
                        ? color_picker_paint_field(paint, node->rectangle, color.hsv, theme, outlined)
                        : color_picker_paint_hue(paint, node->rectangle, color.hsv, theme, outlined);
        } else if (element == TextFormElement::color_swatches) {
            const auto& color = scene.content.color;
            valid = color_picker_paint_swatches(paint, node->rectangle, color.suggested, scene.content.swatch_rgba >> 8,
                                                color.focused_swatch, theme, outlined);
        } else if (element == TextFormElement::eyedropper) {
            // A quiet square button; filled while a pick is under way.
            const auto r = node->rectangle;
            const bool picking = scene.content.color.picking;
            const bool hovered = ui_widget_handle_matches(scene.widgets.hovered, scene.elements[slot]);
            const float icon = ui_rect_width(r) * .5f, cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f;
            const Color fill = picking ? theme.tokens.emphasis.fill : hovered ? theme.interactive_hover : theme.interactive;
            valid = paint_stream_push_rounded_border(paint, r, fill, outlined ? theme.tokens.border.focus : fill,
                                                     metrics.control_radius, outlined ? 2.0f : 1.0f) &&
                    ui_icon_paint(paint, UiIcon::pipette,
                                  {cx - icon * .5f, cy - icon * .5f, cx + icon * .5f, cy + icon * .5f},
                                  picking ? theme.tokens.emphasis.text : ui_surface_foreground(theme, fill));
        } else if (radio(element)) {
            // A radio circle with a dot when chosen.
            u32 at = 0;
            const bool choice = text_form_choice_index(element, &at);
            if (!choice)
                text_form_select_index(element, &at);
            const bool chosen = choice ? scene.content.choice_selected == at : scene.content.select_selected == at;
            const auto r = text_form_choice_marker(scene, element);
            const float radius = ui_rect_width(r) * .5f;
            valid = paint_stream_push_rounded_border(paint, r, theme.base_lowest, outlined ? theme.tokens.border.focus
            : chosen ? theme.tokens.emphasis.fill
                     : theme.text_secondary, radius, outlined ? 2.0f : 1.5f);
            if (valid && chosen) {
                const float inset = ui_rect_width(r) * .28f;
                valid = paint_stream_push_rounded(paint, r.left + inset, r.top + inset, r.right - inset,
                                                  r.bottom - inset, theme.tokens.emphasis.fill, radius - inset);
            }
        } else if (element == TextFormElement::select) {
            // A field like the editor, its chevron pointing at the open state.
            const auto r = node->rectangle;
            const float icon = kOptionBox * .8f, cy = (r.top + r.bottom) * .5f;
            valid = paint_stream_push_rounded_border(paint, r, theme.base_lowest, outlined ? theme.tokens.border.focus : theme.interactive, metrics.compact_radius, outlined ? 2.0f : 1.0f) &&
                    ui_icon_paint(paint, scene.content.select_expanded ? UiIcon::chevron_up : UiIcon::chevron_down,
                                  {r.right - metrics.form_field_padding - icon, cy - icon * .5f,
                                   r.right - metrics.form_field_padding, cy + icon * .5f},
                                  theme.text_secondary);
        } else if (element == TextFormElement::option) {
            // The checkbox: an outlined square, filled with a check when set.
            const auto r = text_form_option_box(scene);
            const bool checked = scene.content.option_checked;
            const Color fill = checked ? theme.tokens.emphasis.fill : theme.base_lowest;
            valid = paint_stream_push_rounded_border(paint, r, fill, outlined ? theme.tokens.border.focus
            : checked ? fill
                      : theme.text_secondary, metrics.compact_radius, outlined ? 2.0f : 1.0f);
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
            valid = paint_stream_push_rounded(paint, r, fill, metrics.control_radius) &&
                    (!outlined || paint_stream_push_rounded_border(
                                      paint, r.left - ring, r.top - ring, r.right + ring, r.bottom + ring,
                                      ui_theme_color_alpha(fill, 0), theme.tokens.border.focus,
                                      metrics.control_radius + ring, 2.0f));
        }
        if (element == TextFormElement::edit) {
            const auto clip = ui_rect_overlap(node->content_rectangle, node->clip_rectangle);
            for (u32 i = 0; valid && i < edit.selections.count; ++i)
                valid = fill(paint, ui_rect_overlap(edit.selections[i], clip), ui_edit_selection_color(theme));
            for (u32 i = 0; valid && i < edit.composition.count; ++i) {
                const auto& mark = edit.composition[i];
                const bool targeted =
                    mark.attributes.attribute == TextCompositionAttribute::target_converted ||
                    mark.attributes.attribute == TextCompositionAttribute::target_not_converted;
                valid = text_composition_paint_mark(paint, mark, clip,
                                                    targeted ? theme.accent : theme.text_primary);
            }
            if (valid && focused && scene.content.caret_visible)
                valid = fill(paint, ui_rect_overlap(edit.caret, clip), theme.text_primary);
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
        if (!built(scene, element) || (element == TextFormElement::edit && editor_collapsed(scene.content)))
            continue;
        const bool hint = element == TextFormElement::edit && !scene.content.text.bytes &&
                          scene.content.placeholder.bytes && !scene.content.confirmation_only;
        const auto text = hint ? scene.content.placeholder : label(scene.content, element);
        if (!text.bytes)
            continue;
        const auto* node = text_form_node(scene, element);
        if (!node)
            return false;
        const auto clip = ui_rect_overlap(node->content_rectangle, node->clip_rectangle);
        if (!ui_rect_has_area(clip))
            continue;
        const bool button = action(element);
        Color color = element == TextFormElement::label || element == TextFormElement::choices_label ||
                              element == TextFormElement::select_label || hint
                          ? theme.text_secondary
                      : element == TextFormElement::status && scene.content.error ? theme.tokens.text.danger
                                                                                  : theme.text_primary;
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
                         element == TextFormElement::edit && !hint ? edit.text_rectangle : node->content_rectangle,
                         clip,
                         color,
                         text_style(element),
                         button ? UiTextAlignment::center : UiTextAlignment::start,
                         UiTextAlignment::start,
                         button || (element == TextFormElement::edit && scene.content.single_line)
                             ? TextWrapping::no_wrap
                             : TextWrapping::wrap,
                         button ? TextOverflow::ellipsis : TextOverflow::clip}))
            return false;
    }
    return true;
}
} // namespace maicord
