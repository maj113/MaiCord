#include "maicord/ui/login_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {
namespace {

float pixel_truncated(float value) noexcept {
    return static_cast<float>(static_cast<i32>(value));
}

bool push_rounded_truncated(PaintStream* stream, const UiRect& rectangle, const Color& color,
                            float radius) noexcept {
    return paint_stream_push_rounded(stream, pixel_truncated(rectangle.left),
                                     pixel_truncated(rectangle.top), pixel_truncated(rectangle.right),
                                     pixel_truncated(rectangle.bottom), color, radius);
}

bool push_border_truncated(PaintStream* stream, const Theme& theme, const UiRect& rectangle,
                           const Color& fill, const Color& border, float radius,
                           float border_width) noexcept {
    return ui_background_paint_surface(stream,
                                       {pixel_truncated(rectangle.left), pixel_truncated(rectangle.top),
                                        pixel_truncated(rectangle.right),
                                        pixel_truncated(rectangle.bottom)},
                                       theme, fill, border, radius, border_width, .72f);
}

Color packed_rgb(u32 rgb, float alpha = 1.0f) noexcept {
    constexpr float inverse = 1.0f / 255.0f;
    return Color{static_cast<float>((rgb >> 16) & 0xffu) * inverse,
                 static_cast<float>((rgb >> 8) & 0xffu) * inverse,
                 static_cast<float>(rgb & 0xffu) * inverse, alpha};
}

} // namespace

bool login_presentation_build(const LoginScene& scene, const LoginPresentationInput& input,
                              const Theme& theme, float viewport_width, float viewport_height, u32 dpi,
                              PaintStream* stream) noexcept {
    if (!stream || dpi == 0 || !ui_extent_valid(viewport_width) || !ui_extent_valid(viewport_height) ||
        viewport_width <= 0.0f || viewport_height <= 0.0f || scene.widgets.count == 0 ||
        (!input.edit_selections.data && input.edit_selections.count != 0) ||
        (!input.edit_composition.data && input.edit_composition.count != 0)) {
        return false;
    }

    if (!paint_stream_reset(stream, viewport_width, viewport_height)) {
        return false;
    }
    if (!paint_stream_push(stream, 0, 0, viewport_width, viewport_height, theme.base_lowest))
        return false;
    if (ui_background_active(theme)) {
        stream->quads[0].border_width = -3;
        if (!paint_stream_push(stream, 0, 0, viewport_width, viewport_height,
                               ui_background_canvas_color(theme, theme.base_lowest)))
            return false;
    }
    if (scene.restoring_session)
        return true;
    const UiRect card = login_scene_rectangle(scene, LoginElement::card);
    const UiRect login_field = login_scene_rectangle(scene, LoginElement::login_field);
    const UiRect password_field = login_scene_rectangle(scene, LoginElement::password_field);
    const UiRect submit = login_scene_rectangle(scene, LoginElement::submit);
    const UiRect qr = login_scene_rectangle(scene, LoginElement::qr);
    if (!push_border_truncated(stream, theme, card, theme.surface, {}, scene.full_viewport ? 0.0f : 8.0f,
                               0) ||
        (!scene.phone_mode && !scene.browser_mode &&
         !push_border_truncated(
             stream, theme, login_field, theme.base_lowest,
             scene.focus_visible && scene.focus == LoginControl::login ? theme.tokens.border.focus : theme.base_lowest,
             4.0f, 1.0f)) ||
        (!scene.credential_fields_hidden &&
         !push_border_truncated(stream, theme, password_field, theme.base_lowest,
                                scene.focus_visible && scene.focus == LoginControl::password
                                    ? theme.tokens.border.focus
                                    : theme.base_lowest,
                                4.0f, 1.0f))) {
        return false;
    }

    const UiRect reveal = login_password_reveal_rectangle(scene);
    if (ui_rect_has_area(reveal)) {
        const float extent = ui_icon_button_extent(ui_rect_height(reveal));
        const float x = pixel_truncated((reveal.left + reveal.right - extent) * 0.5f);
        const float y = pixel_truncated((reveal.top + reveal.bottom - extent) * 0.5f);
        if (!ui_icon_paint(stream, scene.password_revealed ? UiIcon::eye_off : UiIcon::eye,
                           {x, y, x + extent, y + extent}, theme.text_secondary))
            return false;
    }

    if (scene.phone_mode) {
        for (u32 i = 0; i < kLoginPhoneCodeDigits; ++i) {
            const auto cell = login_phone_cell_rectangle(password_field, i);
            const bool focused = scene.focus_visible && scene.focus == LoginControl::phone_code &&
                                 input.edit_caret.left >= cell.left && input.edit_caret.left <= cell.right;
            if (!push_border_truncated(stream, theme, cell, theme.base_lowest,
                                       focused ? theme.tokens.border.focus : theme.interactive, 4.0f, 1.0f))
                return false;
        }
        if (scene.focus_visible && scene.focus == LoginControl::phone_resend &&
            !push_border_truncated(stream, theme, login_scene_rectangle(scene, LoginElement::mfa_sms),
                                   theme.base_low, theme.tokens.border.focus, 4.0f, 1.0f))
            return false;
    }

    if (input.edit_active) {
        const UiRect field = scene.focus == LoginControl::login ? login_field : password_field;
        for (u32 index = 0; index < input.edit_composition.count; ++index) {
            const LoginCompositionGeometry& decoration = input.edit_composition[index];
            if ((decoration.span.color_flags & text_composition_color_background) == 0) {
                continue;
            }
            const UiRect clipped{maximum(field.left + 2.0f, decoration.rectangle.left),
                                 maximum(field.top + 2.0f, decoration.rectangle.top),
                                 minimum(field.right - 2.0f, decoration.rectangle.right),
                                 minimum(field.bottom - 2.0f, decoration.rectangle.bottom)};
            if (ui_rect_has_area(clipped) &&
                !paint_stream_push(stream, pixel_truncated(clipped.left), pixel_truncated(clipped.top),
                                   pixel_truncated(clipped.right + 0.999f),
                                   pixel_truncated(clipped.bottom + 0.999f),
                                   packed_rgb(decoration.span.background_rgb))) {
                return false;
            }
        }
        Color selection_color = theme.accent;
        selection_color.alpha = 0.35f;
        for (u32 index = 0; index < input.edit_selections.count; ++index) {
            const UiRect& selection = input.edit_selections[index];
            const UiRect clipped{maximum(field.left + 2.0f, selection.left),
                                 maximum(field.top + 2.0f, selection.top),
                                 minimum(field.right - 2.0f, selection.right),
                                 minimum(field.bottom - 2.0f, selection.bottom)};
            if (ui_rect_has_area(clipped)) {
                if (!paint_stream_push(stream, pixel_truncated(clipped.left), pixel_truncated(clipped.top),
                                       pixel_truncated(clipped.right + 0.999f),
                                       pixel_truncated(clipped.bottom + 0.999f), selection_color)) {
                    return false;
                }
            }
        }
        for (u32 index = 0; index < input.edit_composition.count; ++index) {
            const LoginCompositionGeometry& decoration = input.edit_composition[index];
            const UiRect clipped{maximum(field.left + 2.0f, decoration.rectangle.left),
                                 maximum(field.top + 2.0f, decoration.rectangle.top),
                                 minimum(field.right - 2.0f, decoration.rectangle.right),
                                 minimum(field.bottom - 2.0f, decoration.rectangle.bottom)};
            if (ui_rect_has_area(clipped) &&
                !text_composition_paint_line(stream, clipped, decoration.span, theme.accent)) {
                return false;
            }
        }
        if (input.caret_visible) {
            const float left =
                minimum(maximum(input.edit_caret.left, field.left + 2.0f), field.right - 3.0f);
            const float top = maximum(field.top + 4.0f, input.edit_caret.top);
            const float bottom = minimum(field.bottom - 4.0f, input.edit_caret.bottom);
            if (bottom > top && !paint_stream_push(stream, pixel_truncated(left), pixel_truncated(top),
                                                   pixel_truncated(left) + 1.0f,
                                                   pixel_truncated(bottom + 0.999f), theme.text_primary)) {
                return false;
            }
        }
    }

    for (u32 index = 0; index < kLoginMfaPresentationMethodCount; ++index) {
        const LoginMfaPresentationMethod& method = kLoginMfaPresentationMethods[index];
        if ((scene.mfa_capabilities & method.capability) == 0) {
            continue;
        }
        const bool selected = scene.selected_mfa_capability == method.capability;
        const bool focused = scene.focus_visible && scene.focus == method.control;
        if (!push_border_truncated(stream, theme, login_scene_rectangle(scene, method.element),
                                   selected ? theme.interactive : theme.base_lowest,
                                   focused || selected ? theme.tokens.emphasis.outline : theme.interactive, 4.0f, 1.0f)) {
            return false;
        }
    }
    if ((scene.back_only || scene.phone_mode || scene.reset_mode || scene.recovery_mode) &&
        scene.focus_visible && scene.focus == LoginControl::back &&
        !push_border_truncated(stream, theme, login_scene_rectangle(scene, LoginElement::forgot_password),
                               theme.base_low, theme.tokens.border.focus, 4.0f, 1.0f))
        return false;
    if (scene.browser_available && scene.focus_visible && scene.focus == LoginControl::browser_sign_in &&
        !push_border_truncated(stream, theme, login_scene_rectangle(scene, LoginElement::browser_sign_in),
                               theme.base_low, theme.tokens.border.focus, 4.0f, 1.0f))
        return false;
    if (!push_rounded_truncated(stream, submit, scene.submit_disabled ? theme.interactive : theme.tokens.emphasis.fill,
                                4.0f)) {
        return false;
    }

    if (!scene.qr_visible) {
        return true;
    }
    if (!push_rounded_truncated(stream, qr, theme.text_primary, 4.0f)) {
        return false;
    }
    if (!input.qr_code) {
        // Keep the QR surface white while the first challenge or a replacement
        // challenge is pending. A dark placeholder reads as a disabled or
        // redacted QR and visibly flashes between automatic refreshes.
        return true;
    }

    QrCodeRect rectangles[384]{};
    const QrCodeRectResult compacted = qr_code_compact_rects(
        *input.qr_code,
        Slice<QrCodeRect>{rectangles, static_cast<u32>(sizeof(rectangles) / sizeof(rectangles[0]))});
    constexpr i32 kQuietModules = 4;
    constexpr i32 kTotalModules = static_cast<i32>(kQrCodeSize) + kQuietModules * 2;
    const float dpi_scale = static_cast<float>(dpi) / 96.0f;
    const i32 left_px = static_cast<i32>(qr.left * dpi_scale + 0.5f);
    const i32 top_px = static_cast<i32>(qr.top * dpi_scale + 0.5f);
    const i32 extent_px = static_cast<i32>((qr.right - qr.left) * dpi_scale + 0.5f);
    const i32 module_px = extent_px / kTotalModules;
    if (!compacted.ok() || module_px <= 0) {
        return false;
    }
    const i32 symbol_px = module_px * kTotalModules;
    const i32 origin_x_px = left_px + (extent_px - symbol_px) / 2;
    const i32 origin_y_px = top_px + (extent_px - symbol_px) / 2;
    const float dip_per_pixel = 1.0f / dpi_scale;
    for (u32 index = 0; index < compacted.count; ++index) {
        const QrCodeRect rectangle = rectangles[index];
        const float left =
            static_cast<float>(origin_x_px + (kQuietModules + rectangle.left) * module_px) * dip_per_pixel;
        const float top =
            static_cast<float>(origin_y_px + (kQuietModules + rectangle.top) * module_px) * dip_per_pixel;
        const float right =
            static_cast<float>(origin_x_px + (kQuietModules + rectangle.right) * module_px) * dip_per_pixel;
        const float bottom =
            static_cast<float>(origin_y_px + (kQuietModules + rectangle.bottom) * module_px) *
            dip_per_pixel;
        if (!paint_stream_push(stream, left, top, right, bottom, theme.base_lowest)) {
            return false;
        }
    }
    return true;
}

} // namespace maicord
