#include "account_menu_ui_probe.hpp"
#include "probe_present.hpp"
#include "maicord/ui/account_menu.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/scene.hpp"
#include "settings_theme_probe.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

namespace maicord {
bool account_menu_ui_probe(D3D11Renderer& renderer,
                           bool (*capture)(const wchar_t*, const Theme&) noexcept) noexcept {
    if (!renderer.ui_scene || !renderer.dpi)
        return false;
    const auto footer = ui_widget_manager_find(renderer.ui_scene->widgets,
                                               static_cast<u32>(UiSemantic::channel_footer) + 1);
    const auto* source = ui_widget_manager_layout_node(renderer.ui_scene->widgets, footer);
    if (!source)
        return false;
    UiAccountMenuModel model{};
    PopupScene popup{};
    PopupItem items[kUiAccountMenuItemCapacity]{};
    PopupRowWidget rows[kUiAccountMenuItemCapacity]{};
    UiWidget widgets[kUiAccountMenuItemCapacity * 2 + 2]{};
    UiLayoutNode nodes[kUiAccountMenuItemCapacity * 2 + 2]{};
    const auto* original_popup = renderer.popup_scene;
    const auto* original_focus = renderer.focus_owner;
    bool restored = false;
    struct Restore {
        D3D11Renderer& renderer;
        PopupScene& popup;
        const PopupScene* original;
        const UiWidgetManager* focus;
        bool& restored;
        ~Restore() noexcept {
            renderer_bind_popup_scene(&renderer, original);
            renderer_bind_focus_owner(&renderer, focus);
            popup_scene_close(&popup);
            renderer.retained_presentation.valid = false;
            restored = renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                       probe_present(&renderer, kDefaultTheme);
            popup_scene_release(&popup);
        }
    };
    bool valid = false;
    {
        Restore restore{renderer, popup, original_popup, original_focus, restored};
        const UiRect viewport{0, 0, static_cast<float>(renderer.width) * 96.f / renderer.dpi,
                              static_cast<float>(renderer.height) * 96.f / renderer.dpi};
        valid =
            ui_account_menu_build(&model, {0x4143434f554e54ull, utf8_literal("Native Account"),
                                           utf8_literal("native.user"), 0, false, UiPresence::unknown}) &&
            popup_scene_initialize(&popup, {items, kUiAccountMenuItemCapacity},
                                   {rows, kUiAccountMenuItemCapacity},
                                   {widgets, kUiAccountMenuItemCapacity * 2 + 2},
                                   {nodes, kUiAccountMenuItemCapacity * 2 + 2}) &&
            ui_account_menu_open_popup(
                &popup, model, ui_rect_intersection(source->rectangle, source->clip_rectangle), viewport)
                .ok() &&
            renderer_bind_popup_scene(&renderer, &popup);
        PopupItemLayout edit{};
        bool changed = false;
        valid = valid && popup_scene_resolve_item_layout(popup, 2, &edit) &&
                popup_scene_set_hover(
                    &popup, (edit.visible_rectangle.left + edit.visible_rectangle.right) * .5f,
                    (edit.visible_rectangle.top + edit.visible_rectangle.bottom) * .5f, &changed) &&
                popup_scene_selected_action(popup) == static_cast<u32>(UiAccountMenuAction::edit_profile) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme) &&
                d3d11_renderer_interaction_text_state_current(renderer) &&
                (!capture || capture(L"account-menu-identity-edit-hover.bmp", kDefaultTheme));
        if (!valid)
            log_u64(LogLevel::error, "probe", "account_menu_native_failure_stage", 1);
        const bool pointer_passed = valid;
        renderer_bind_focus_owner(&renderer, &popup.widgets);
        valid = valid && popup_scene_move_selection(&popup, 1, &changed) &&
                popup_scene_selected_action(popup) == static_cast<u32>(UiAccountMenuAction::copy_user_id) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme) &&
                d3d11_renderer_interaction_text_state_current(renderer) &&
                (!capture || capture(L"account-menu-copy-keyboard.bmp", kDefaultTheme));
        if (pointer_passed && !valid)
            log_u64(LogLevel::error, "probe", "account_menu_native_failure_stage", 2);
        const bool original_passed = valid;
        popup_scene_close(&popup);
        renderer_bind_focus_owner(&renderer, nullptr);
        valid = valid &&
                ui_account_menu_build(&model,
                                      {0x4143434f554e54ull, utf8_literal("Native Account"),
                                       utf8_literal("native.user"), 0, false, UiPresence::offline, true}) &&
                model.count == 9 && (model.items[7].flags & popup_item_checked) &&
                ui_account_menu_open_popup(&popup, model,
                                           ui_rect_intersection(source->rectangle, source->clip_rectangle),
                                           viewport)
                    .ok();
        for (u32 i = 0; valid && i < model.count; ++i) {
            PopupItemLayout row{};
            valid = popup_scene_resolve_item_layout(popup, i, &row) && row.visible && row.label &&
                    ui_rect_equal(row.visible_rectangle, row.label->rectangle);
        }
        PopupItemLayout idle{};
        valid = valid && popup_scene_resolve_item_layout(popup, 5, &idle) &&
                popup_scene_set_hover(
                    &popup, (idle.visible_rectangle.left + idle.visible_rectangle.right) * .5f,
                    (idle.visible_rectangle.top + idle.visible_rectangle.bottom) * .5f, &changed) &&
                popup_scene_selected_action(popup) == static_cast<u32>(UiAccountMenuAction::status_idle) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme) &&
                d3d11_renderer_interaction_text_state_current(renderer) &&
                (!capture || capture(L"account-menu-status-invisible-idle-hover.bmp", kDefaultTheme));
        if (original_passed && !valid)
            log_u64(LogLevel::error, "probe", "account_menu_native_failure_stage", 3);
        const bool status_passed = valid;
        popup_scene_close(&popup);
        valid = valid &&
                ui_account_menu_build(&model, {0x4143434f554e54ull, utf8_literal("Native Account"),
                                               utf8_literal("native.user"), 0, false, UiPresence::idle,
                                               true, true}) &&
                model.count == 9 &&
                utf8_equal(model.items[5].detail, utf8_literal("Updating status…")) &&
                ui_account_menu_open_popup(&popup, model,
                                           ui_rect_intersection(source->rectangle, source->clip_rectangle),
                                           viewport)
                    .ok();
        UiMenuRowGeometry pending{};
        valid = valid && popup_scene_menu_row_geometry(popup, 5, &pending) &&
                ui_rect_has_area(pending.detail) && pending.primary.bottom <= pending.detail.top &&
                popup_scene_select_index(&popup, 5, &changed) &&
                popup_scene_selected_action(popup) == static_cast<u32>(UiAccountMenuAction::status_idle) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme) &&
                d3d11_renderer_interaction_text_state_current(renderer) &&
                (!capture || capture(L"account-menu-status-pending.bmp", kDefaultTheme));
        if (status_passed && !valid)
            log_u64(LogLevel::error, "probe", "account_menu_native_failure_stage", 4);
        const bool pending_passed = valid;
        auto preferences = client_preferences_defaults();
        preferences.theme = ClientTheme::custom_gradient;
        preferences.gradient = background_theme_defaults();
        preferences.gradient.colors[0] = 0xef2737;
        for (u32 i = 1; i < 5; ++i)
            preferences.gradient.colors[i] = 0;
        preferences.gradient.color_count = 1;
        preferences.gradient.intensity = 100;
        preferences.gradient.effect = BackgroundEffect::none;
        const Theme gradient = client_theme_projection(preferences);
        PaintStream paint{};
        valid = valid && paint_stream_initialize(&paint) &&
                paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                popup_presentation_build(popup, gradient, &paint) && paint.count &&
                popup.panel_material == PopupPanelMaterial::gradient &&
                paint.clip_depth == 1 && paint.rejected == 0;
        // The drop shadow paints before the panel plate; find the gradient plate among the quads.
        bool gradient_plate = false;
        for (u32 quad = 0; valid && quad < paint.count; ++quad)
            gradient_plate = gradient_plate || (paint.quads[quad].border_width == -7 &&
                                                paint.quads[quad].reserved_x == -1 &&
                                                paint.quads[quad].fill.alpha == 1);
        valid = valid && gradient_plate;
        paint_stream_release(&paint);
        valid = valid && renderer_rebuild_content_text(&renderer, gradient) &&
                probe_present(&renderer, gradient) &&
                d3d11_renderer_interaction_text_state_current(renderer) &&
                d3d11_renderer_draw_stream(&renderer, gradient, false, false, nullptr);
        if (valid) {
            settings_theme_probe_detail::Pixels pixels(renderer);
            u32 rgb = 0;
            valid = pixels.point(popup.rectangle.left + 6, popup.rectangle.top + 16, &rgb);
            const float reveal = ui_elevation_gradient_share(gradient, UiElevation::popover);
            const u32 channels[]{0x37, 0x27, 0xef};
            for (u32 channel = 0; valid && channel < 3; ++channel) {
                const i32 expected = static_cast<i32>(channels[channel] * reveal + .5f);
                const i32 delta = static_cast<i32>((rgb >> (channel * 8)) & 255) - expected;
                valid = delta >= -2 && delta <= 2;
            }
            valid = valid && popup_scene_menu_row_geometry(popup, 0, &pending) &&
                    pixels.ink(pending.primary, gradient.text_primary);
        }
        valid = valid && (!capture || capture(L"account-menu-gradient.bmp", gradient));
        if (pending_passed && !valid)
            log_u64(LogLevel::error, "probe", "account_menu_native_failure_stage", 5);
        const bool gradient_passed = valid;
        popup_scene_close(&popup);
        const UiRect short_viewport{viewport.left, viewport.top, viewport.right,
                                    minimum(viewport.bottom, 236.f)};
        const UiRect short_anchor{source->rectangle.left, short_viewport.bottom - 16,
                                  source->rectangle.right, short_viewport.bottom - 8};
        valid = valid && ui_account_menu_open_popup(&popup, model, short_anchor, short_viewport).ok() &&
                popup.scroll_max > 0 &&
                popup_scene_resolve_item_layout(popup, 0, &edit) && edit.visible &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme) &&
                d3d11_renderer_interaction_text_state_current(renderer) &&
                (!capture || capture(L"account-menu-short-identity.bmp", kDefaultTheme));
        renderer_bind_focus_owner(&renderer, &popup.widgets);
        valid = valid && popup_scene_select_index(&popup, 7, &changed) && changed &&
                popup_scene_resolve_item_layout(popup, 7, &edit) && edit.visible &&
                edit.row->rectangle.bottom <= edit.clip_rectangle.bottom &&
                popup_scene_selected_action(popup) == static_cast<u32>(UiAccountMenuAction::status_invisible) &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                probe_present(&renderer, kDefaultTheme) &&
                d3d11_renderer_interaction_text_state_current(renderer) &&
                (!capture || capture(L"account-menu-short-last-status.bmp", kDefaultTheme));
        if (gradient_passed && !valid)
            log_u64(LogLevel::error, "probe", "account_menu_native_failure_stage", 6);
    }
    log_write(valid && restored ? LogLevel::info : LogLevel::error, "probe",
              valid && restored ? "account_menu_native_passed" : "account_menu_native_failed");
    return valid && restored;
}
} // namespace maicord
