#include "d3d11_renderer_edit.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/core/utf.hpp"
#include "d3d11_renderer_interaction_state.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
__declspec(noinline) bool measure_text(TextEditLayoutOwner* owner, TextEditMetric* selections,
                                       const UiEditView& edit, const UiRect& r, const TextSystem& text) noexcept {
    u32 anchor = 0, caret = 0;
    if (!utf8_utf16_position_for_byte_offset(edit.text, edit.anchor, &anchor) ||
        !utf8_utf16_position_for_byte_offset(edit.text, edit.caret, &caret))
        return false;
    owner->selections = selections;
    owner->selection_capacity = kTextEditSelectionCapacity;
    return text_edit_layout_measure(
        owner,
        {edit.text, text_edit_signature(edit.text), anchor, caret, r.left, r.top, ui_rect_width(r),
         ui_rect_height(r), kMaximumUiCoordinate,
         text_edit_layout_center_vertically | text_edit_layout_scroll_to_caret},
        text);
}
// Fills a cleared `geometry` from an active layout.
__declspec(noinline) void fill_geometry(const TextEditLayoutOwner& o, UiEditGeometry* geometry) noexcept {
    if (!o.active)
        return;
    geometry->text = {o.origin_x, o.origin_y, o.origin_x + maximum(o.width, 1.0f),
                      o.origin_y + maximum(o.layout_height, 1.0f)};
    geometry->caret = {o.caret.left, o.caret.top, o.caret.left + 1, o.caret.top + o.caret.height};
    static_assert(kTextEditSelectionCapacity == sizeof(geometry->selections) / sizeof(UiRect));
    geometry->selection_count = o.selection_count;
    for (u32 i = 0; i < o.selection_count; ++i) {
        const auto& s = o.selections[i];
        geometry->selections[i] = {s.left, s.top, s.left + s.width, s.top + s.height};
    }
}
} // namespace
bool d3d11_renderer_measure_search_edit(TextEditLayoutOwner* layout, TextEditMetric* selections,
                                        const UiEditView& edit, const UiRect& bounds, const TextSystem& text,
                                        bool invalidate_on_failure, UiEditGeometry* geometry) noexcept {
    if (ui_rect_has_area(bounds) && measure_text(layout, selections, edit, bounds, text)) {
        fill_geometry(*layout, geometry);
        // A long query may scroll its origin more than one viewport width. Keep
        // the draw rectangle intersecting the actual visible text clip.
        geometry->text.right = maximum(bounds.right, geometry->text.right);
        return true;
    }
    if (invalidate_on_failure)
        text_edit_layout_invalidate(layout);
    return false;
}
bool d3d11_renderer_measure_popup_edit(D3D11Renderer* renderer) noexcept {
    if (d3d11_renderer_message_forward_visible(*renderer)) {
        const auto& forward = *renderer->message_forward_scene;
        // Input admission is disabled after submission, but the frozen caption
        // still needs the shared layout for read-only presentation.
        auto caption = forward.content->caption;
        if (!caption.active || forward.content->committed || forward.content->sending)
            caption.anchor = caption.caret = 0;
        return measure_text(&renderer->popup_edit_layout, renderer->popup_edit_selections,
            caption, forward.caption_text, renderer->glyph_text.system);
    }
    text_edit_layout_release(&renderer->popup_edit_layout);
    return true;
}
bool d3d11_renderer_measure_profile_edit(D3D11Renderer* renderer) noexcept {
    const auto* scene = renderer->profile_scene;
    const UiEditView view = scene && scene->active ? scene->edit : UiEditView{};
    if (!view.active) {
        text_edit_layout_release(&renderer->profile_edit_layout);
        return true;
    }
    return measure_text(&renderer->profile_edit_layout, renderer->profile_edit_selections, view,
                        scene->edit_content, renderer->glyph_text.system);
}
PopupEditGeometry d3d11_renderer_popup_edit_geometry(const D3D11Renderer& renderer) noexcept {
    UiEditGeometry geometry{};
    fill_geometry(renderer.popup_edit_layout, &geometry);
    return geometry;
}
UiEditGeometry d3d11_renderer_profile_edit_geometry(const D3D11Renderer& renderer) noexcept {
    UiEditGeometry geometry{};
    fill_geometry(renderer.profile_edit_layout, &geometry);
    return geometry;
}
bool d3d11_renderer_measure_settings_edit(D3D11Renderer* renderer) noexcept {
    reset_value(&renderer->media_picker_edit_geometry);
    const auto* scene=renderer->settings_scene;
    const auto target=scene ? ui_settings_scene_focused(*scene) : UiSettingsTarget{};
    const auto* control=scene ? ui_settings_scene_control(*scene,target) : nullptr;
    // The modal search field is measured like an inline settings editor.
    const bool search = scene && target.kind == UiSettingsTargetKind::search;
    const UiSettingsEditor* editor = search ? &scene->search : control && control->enabled ? control->editor : nullptr;
    UiSettingsControlGeometry geometry{};
    if (!scene || !scene->active || !editor || !editor->view.focused ||
        !(search ? ui_settings_scene_search_geometry(*scene, &geometry)
                 : ui_settings_scene_control_geometry(*scene, target, &geometry)) ||
        d3d11_renderer_form_active(*renderer) ||
        d3d11_renderer_active(renderer->popup_scene)) {
        text_edit_layout_invalidate(&renderer->media_picker_layout); return true;
    }
    const auto& edit=editor->view;
    u32 anchor=0,caret=0;
    if (!utf8_utf16_position_for_byte_offset(edit.text,edit.anchor,&anchor) ||
        !utf8_utf16_position_for_byte_offset(edit.text,edit.caret,&caret)) return false;
    auto& layout=renderer->media_picker_layout;
    layout.selections=renderer->media_picker_edit_selections;
    layout.selection_capacity=kTextEditSelectionCapacity;
    const auto r=geometry.text;
    const u8 options=text_edit_layout_scroll_to_caret | (geometry.multiline ? text_edit_layout_wrap : text_edit_layout_center_vertically);
    if (!text_edit_layout_measure(&layout,{edit.text,text_edit_signature(edit.text),anchor,caret,
        r.left,r.top,ui_rect_width(r),ui_rect_height(r),kMaximumUiCoordinate,options},renderer->glyph_text.system)) return false;
    fill_geometry(layout, &renderer->media_picker_edit_geometry);
    return true;
}
namespace {

void select_edit_geometry(D3D11Renderer* renderer, TextEditLayoutOwner* owner,
                          TextEditLayoutOwner* previous) noexcept {
    if (renderer->edit_selection_owner != owner) {
        text_edit_layout_invalidate(previous);
        renderer->edit_selection_owner = owner;
    }
    owner->selections = renderer->edit_selections;
    owner->selection_capacity = kTextEditSelectionCapacity;
}

} // namespace

bool d3d11_renderer_measure_login_edit(D3D11Renderer* renderer, const LoginView& view) noexcept {
    if (!renderer) {
        return false;
    }
    renderer->login_caret_visible = false;
    renderer->login_composition_metric_count = 0;
    if (!view.edit_active) {
        text_edit_layout_invalidate(&renderer->login_edit);
        return true;
    }
    const bool login_field = view.focus == LoginControl::login;
    const bool secret_field = view.focus == LoginControl::password || view.focus == LoginControl::phone_code;
    if (!login_field && !secret_field) {
        text_edit_layout_invalidate(&renderer->login_edit);
        return false;
    }
    const Utf8View text = login_field ? view.login : view.password_mask;
    const Utf8Validation validation = utf8_validate(text);
    if (!validation.valid || view.edit_anchor > validation.codepoints ||
        view.edit_caret > validation.codepoints) {
        text_edit_layout_invalidate(&renderer->login_edit);
        return false;
    }
    u32 anchor_utf16 = 0;
    u32 caret_utf16 = 0;
    if (!utf8_utf16_position_for_codepoint(text, view.edit_anchor, &anchor_utf16) ||
        !utf8_utf16_position_for_codepoint(text, view.edit_caret, &caret_utf16)) {
        text_edit_layout_invalidate(&renderer->login_edit);
        return false;
    }
    const UiRect field = login_scene_content_rectangle(
        *renderer->login_scene, login_field ? LoginElement::login_field : LoginElement::password_field);
    // The content rectangle already excludes field padding. Match the full
    // rectangle and vertical alignment used by login_text_presentation.
    const float width = maximum(1.0f, ui_rect_width(field));
    const float height = maximum(1.0f, ui_rect_height(field));
    const TextEditLayoutInput input{
        text,         text_edit_signature(text),
        anchor_utf16, caret_utf16,
        field.left,   field.top,
        width,        height,
        height,       text_edit_layout_center_vertically,
    };
    select_edit_geometry(renderer, &renderer->login_edit, &renderer->composer_layout);
    if (!text_edit_layout_measure(&renderer->login_edit, input, renderer->glyph_text.system)) {
        return false;
    }
    const bool phone = login_phone_mode(view.status);
    if (phone) {
        auto& edit = renderer->login_edit;
        const auto caret = login_phone_caret_rectangle(field, caret_utf16);
        edit.caret.left = caret.left; edit.caret.top = caret.top;
        edit.caret.width = ui_rect_width(caret); edit.caret.height = ui_rect_height(caret);
        edit.selection_count = 0;
        for (u32 i = minimum(anchor_utf16, caret_utf16); i < maximum(anchor_utf16, caret_utf16); ++i) {
            if (edit.selection_count == edit.selection_capacity) return false;
            const auto cell = login_phone_cell_rectangle(field, i);
            edit.selections[edit.selection_count++] = {static_cast<u16>(i), 1, cell.left, cell.top, ui_rect_width(cell), ui_rect_height(cell)};
        }
    }
    for (u32 index = 0; index < view.edit_composition.count; ++index) {
        const TextCompositionSpan& span = view.edit_composition[index];
        u32 first_utf16 = 0;
        u32 last_utf16 = 0;
        if (span.start >= span.end || span.end > validation.codepoints ||
            !utf8_utf16_position_for_codepoint(text, span.start, &first_utf16) ||
            !utf8_utf16_position_for_codepoint(text, span.end, &last_utf16)) {
            text_edit_layout_invalidate(&renderer->login_edit);
            return false;
        }
        if (phone) {
            for (u32 i = span.start; i < span.end; ++i) {
                if (renderer->login_composition_metric_count == kRendererLoginCompositionCapacity) return false;
                const auto cell = login_phone_cell_rectangle(field, i);
                renderer->login_composition_metrics[renderer->login_composition_metric_count++] =
                    {{static_cast<u16>(i), 1, cell.left, cell.top, ui_rect_width(cell), ui_rect_height(cell)}, span};
            }
            continue;
        }
        // One composition span can cross several bidi runs. Use the existing
        // renderer capacity rather than rejecting a third visual rectangle.
        TextEditMetric metrics[kRendererLoginCompositionCapacity]{};
        const u32 remaining=kRendererLoginCompositionCapacity-renderer->login_composition_metric_count;
        UINT32 actual = 0;
        const bool result = text_edit_layout_raw_range_geometry(renderer->login_edit,
            first_utf16,last_utf16,{metrics,remaining},&actual);
        if (!result || actual > remaining) {
            text_edit_layout_invalidate(&renderer->login_edit);
            return false;
        }
        for (u32 metric = 0; metric < actual; ++metric) {
            renderer->login_composition_metrics[renderer->login_composition_metric_count++] =
                RendererLoginCompositionMetric{metrics[metric], span};
        }
    }
    renderer->login_caret_visible = true;
    return true;
}

bool d3d11_renderer_measure_composer_edit(D3D11Renderer* renderer) noexcept {
    if (!renderer) {
        return false;
    }
    select_edit_geometry(renderer, &renderer->composer_layout, &renderer->login_edit);
    return text_composer_layout_measure(&renderer->composer_layout, renderer->message_composer_scene,
                                          renderer->glyph_text.system);
}

} // namespace maicord
