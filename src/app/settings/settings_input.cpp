#include "settings_internal.hpp"
#include "../guild/channel_overview.hpp"
#include "../shell/modal_scroll.hpp"
#include "../shell/input.hpp"
#include "keybinds_settings.hpp"
#include "../store/store_host.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/widgets/button.hpp"
#include "maicord/ui/widgets/settings_choice.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord::app_detail {

bool application_settings_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if (application_channel_overview_input(kind, code, repeated))
        return true;
    if (g_app.settings && kind == UiInputKind::key_up && g_app.settings->preference_key == code) {
        g_app.settings->preference_key = 0;
        g_app.settings->slider_preview = false;
        application_settings_commit_preferences();
        return true;
    }
    if (ui_input_consume_key_tail(&g_app.settings_held_key, {kind, code, repeated}))
        return true;
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active) {
        if (kind == UiInputKind::key_down && !repeated &&
            shortcut_matches(ShortcutAction::open_settings, shortcut_chord_from_keyboard(code))) {
            if (application_settings_open())
                g_app.settings_held_key = code;
            return g_app.settings_held_key != 0;
        }
        return false;
    }
    if (application_voice_settings_capture(kind, code) || application_keybinds_settings_capture(kind, code))
        return settled(*settings, true);
    if (application_alt_f4(kind, code))
        return false;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return true;
    if (settings->choice.active) {
        if (!choice_popup_current(settings->choice)) {
            close_choice();
            refresh();
            return true;
        }
        if (code == VK_ESCAPE) {
            g_app.settings_held_key = code;
            close_choice();
            refresh();
            return true;
        }
        const bool reverse = application_shift_down();
        bool changed = false;
        if (code == VK_UP || code == VK_DOWN || code == VK_TAB) {
            popup_scene_move_selection(&g_app.popup_scene, code == VK_UP || (code == VK_TAB && reverse) ? -1 : 1,
                                       &changed);
            if (changed)
                refresh();
        } else if (code == VK_PRIOR || code == VK_NEXT) {
            choice_action(code == VK_PRIOR ? kUiSettingsChoicePrevious : kUiSettingsChoiceNext);
        } else if ((code == VK_RETURN || code == VK_SPACE) && !repeated) {
            g_app.settings_held_key = code;
            choice_action(popup_scene_selected_action(g_app.popup_scene));
        } else if ((code == VK_LEFT || code == VK_RIGHT) && !repeated) {
            const auto choice = settings->choice;
            close_choice();
            g_app.settings_held_key = code;
            if (choice_control(choice))
                activate({UiSettingsTargetKind::control, choice.token.page, choice.control_id}, code == VK_LEFT,
                         choice.token.page.owner_id == kNativeSettingsOwner ? nullptr : &choice.token);
            if (plugin_ui_scope_current(g_app.plugin_account_scope_serial, choice.account_serial) &&
                choice.profile_id == g_app.account.status.profile_id)
                refresh();
        }
        return true;
    }
    if (application_store_input(kind, code, repeated)) {
        g_app.settings_held_key = code;
        return refresh();
    }
    const bool control_key = application_control_down();
    const bool reverse = application_shift_down();
    const auto focused = ui_settings_scene_focused(settings->scene);
    const auto zone = ui_settings_scene_focus_zone(settings->scene);
    if (code == 'F' && control_key && !repeated) {
        g_app.settings_held_key = code;
        ui_settings_scene_focus(&settings->scene, {UiSettingsTargetKind::search, {}, 0});
        return refresh();
    }
    if (code == VK_ESCAPE) {
        g_app.settings_held_key = code;
        // Escape unwinds: search text, then the dialog.
        if (settings->search_bytes) {
            set_search(*settings, {});
            ui_settings_scene_focus(&settings->scene, {UiSettingsTargetKind::search, {}, 0});
            return refresh();
        }
        application_settings_close();
        return true;
    }
    if (zone == UiSettingsFocusZone::search) {
        if (search_key(*settings, code, control_key && !application_alt_down(), reverse))
            return true;
        if (code == VK_RETURN && !repeated && searching(*settings)) {
            g_app.settings_held_key = code;
            const auto controls = settings->scene.content.controls;
            for (u32 i = 0; i < controls.count; ++i)
                if (controls[i].kind != UiSettingsControlKind::notice) {
                    open_result(*settings, controls[i].id);
                    break;
                }
            return true;
        }
        if (code == VK_DOWN && searching(*settings)) {
            ui_settings_scene_move_zone(&settings->scene, false);
            ui_settings_scene_move_zone(&settings->scene, false);
            refresh();
            return true;
        }
    }
    const auto* focused_control = ui_settings_scene_control(settings->scene, focused);
    if (focused_control && (focused_control->kind == UiSettingsControlKind::slider ||
                            focused_control->kind == UiSettingsControlKind::palette ||
                            focused_control->kind == UiSettingsControlKind::radio)) {
        const bool decrease = code == VK_LEFT, increase = code == VK_RIGHT;
        // Page Up/Down move a slider by a tenth of its range.
        const bool page = focused_control->kind == UiSettingsControlKind::slider && (code == VK_PRIOR || code == VK_NEXT);
        if (code == VK_HOME || code == VK_END || decrease || increase || page) {
            UiSettingsActivation activation{};
            const auto direction = code == VK_HOME   ? UiSettingsSliderKey::home
                                   : code == VK_END   ? UiSettingsSliderKey::end
                                   : code == VK_PRIOR ? UiSettingsSliderKey::page_increase
                                   : code == VK_NEXT  ? UiSettingsSliderKey::page_decrease
                                   : decrease         ? UiSettingsSliderKey::decrease
                                                      : UiSettingsSliderKey::increase;
            if (ui_settings_scene_slider_key(settings->scene, focused, direction, &activation) &&
                activation.selected_choice != focused_control->selected_choice) {
                const bool preview = focused_control->kind == UiSettingsControlKind::slider &&
                                     focused.page.owner_id == kNativeSettingsOwner;
                if (preview && !settings->slider_preview) {
                    settings->slider_baseline = g_app.preferences;
                    if (!capture_slider_theme(*settings, focused))
                        return true;
                    settings->slider_target = focused;
                    settings->pressed_account_serial = g_app.plugin_account_scope_serial;
                    settings->pressed_profile = g_app.account.status.profile_id;
                    settings->slider_preview = true;
                }
                if (preview)
                    settings->preference_key = code;
                activate(focused, false, nullptr, false, activation.selected_choice, !preview);
            }
            return true;
        }
        if (code == VK_RETURN || code == VK_SPACE) {
            g_app.settings_held_key = code;
            if (code == VK_RETURN && searching(*settings) && !repeated)
                open_result(*settings, focused.control_id);
            return true;
        }
    }
    if (code == VK_TAB) {
        ui_settings_scene_move_zone(&settings->scene, reverse);
        refresh();
        return true;
    }
    if (code == VK_UP || code == VK_DOWN) {
        const float before = settings->scene.content_offset;
        if (!ui_settings_scene_move_within(&settings->scene, code == VK_UP) && zone == UiSettingsFocusZone::none)
            ui_settings_scene_move_zone(&settings->scene, code == VK_UP);
        settings_glide_reveal(before);
        refresh();
        return true;
    }
    if ((code == VK_HOME || code == VK_END) && zone != UiSettingsFocusZone::search) {
        const float before = settings->scene.content_offset;
        u32 steps = 0;
        while (steps < 64 && ui_settings_scene_move_within(&settings->scene, code == VK_HOME))
            ++steps;
        settings_glide_reveal(before);
        refresh();
        return true;
    }
    if ((code == VK_RETURN || code == VK_SPACE || code == VK_LEFT || code == VK_RIGHT) && !repeated) {
        if (zone == UiSettingsFocusZone::search)
            return true;
        g_app.settings_held_key = code;
        if (code == VK_RETURN && searching(*settings) && focused.kind == UiSettingsTargetKind::control) {
            open_result(*settings, focused.control_id);
            return true;
        }
        if ((code == VK_LEFT || code == VK_RIGHT) && zone != UiSettingsFocusZone::panel)
            return true;
        activate(focused, code == VK_LEFT || reverse, nullptr, code == VK_RETURN || code == VK_SPACE);
        return true;
    }
    if (code == VK_PRIOR || code == VK_NEXT) {
        settings_glide_to(false, settings->scene.content_offset +
                                     (code == VK_PRIOR ? -1.f : 1.f) * ui_rect_height(settings->scene.viewport) * .8f);
        refresh();
    }
    return true;
}
bool application_settings_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (application_channel_overview_window_message(message, wparam, lparam))
        return true;
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active)
        return false;
    if (application_store_window_message(message, wparam, lparam))
        return true;
    if (message == WM_XBUTTONDOWN || message == WM_MBUTTONDOWN) {
        const u32 button = message == WM_MBUTTONDOWN               ? VK_MBUTTON
                           : GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? VK_XBUTTON1
                                                                    : VK_XBUTTON2;
        if (application_voice_settings_capture(UiInputKind::key_down, button) ||
            application_keybinds_settings_capture(UiInputKind::key_down, button))
            return settled(*settings, true);
    }
    if (message == WM_CHAR) {
        const u32 character = static_cast<u32>(wparam);
        if (application_store_character(character))
            return true;
        if (character < 0x20 || character == 0x7f || character > 0xffff || settings->choice.active)
            return true;
        const auto zone = ui_settings_scene_focus_zone(settings->scene);
        // Typing from the sidebar jumps to search; panel rows keep Space.
        if (zone == UiSettingsFocusZone::search || zone == UiSettingsFocusZone::sidebar ||
            zone == UiSettingsFocusZone::none)
            search_character(*settings, character);
        return true;
    }
    // Native action controls receive a second press as DBLCLK, not DOWN.
    // Inline text editors are routed above this settings-only controller.
    const bool double_click = message == WM_LBUTTONDBLCLK;
    if (double_click)
        message = WM_LBUTTONDOWN;
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS || message == WM_SIZE ||
        message == WM_DPICHANGED) {
        const bool was_choice = settings->choice.active;
        application_settings_cancel_pointer();
        settings->scrollbar_dragging = false;
        if (was_choice)
            refresh();
        else
            application_request_frame();
        return false;
    }
    if (message == WM_MOUSELEAVE) {
        if (settings->choice.active) {
            bool changed = false;
            popup_scene_set_hover(&g_app.popup_scene, -1, -1, &changed);
            if (changed)
                refresh();
        }
        const auto pointer = ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::move, {-1, -1});
        if (pointer.changed)
            application_request_frame();
        return true;
    }
    if (message == WM_MOUSEWHEEL) {
        if (settings->choice.active) {
            choice_action(static_cast<short>(HIWORD(wparam)) > 0 ? kUiSettingsChoicePrevious : kUiSettingsChoiceNext);
            return true;
        }
        UiPoint point{};
        if (!application_wheel_point(lparam, &point))
            return true;
        application_modal_scroll_wheel(
            application_settings_scroll_binding(ui_rect_contains(settings->scene.layout.sidebar, point.x, point.y)),
            point, wparam);
        refresh();
        return true;
    }
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
        const float x = client_dip_x(lparam), y = client_dip_y(lparam);
        if (settings->slider_dragging) {
            const auto target = settings->pressed;
            const auto* control = ui_settings_scene_control(settings->scene, target);
            const bool current = control && control->kind == UiSettingsControlKind::slider &&
                                 target.page.owner_id == kNativeSettingsOwner &&
                                 settings->pressed_account_serial == g_app.plugin_account_scope_serial &&
                                 settings->pressed_profile == g_app.account.status.profile_id &&
                                 settings->slider_generation == settings->scene.widgets.generation &&
                                 ui_rect_equal(settings->slider_viewport, settings->scene.viewport) &&
                                 application_pointer_captured();
            if (!current) {
                application_settings_cancel_pointer();
                return true;
            }
            if (message == WM_MOUSEMOVE) {
                UiSettingsActivation value{};
                if (ui_settings_scene_slider_pointer(settings->scene, target, x - settings->slider_grab, &value) &&
                    value.selected_choice != control->selected_choice)
                    activate(target, false, nullptr, false, value.selected_choice, false);
            } else if (message == WM_LBUTTONUP) {
                settings->slider_preview = false;
                application_settings_cancel_pointer();
                application_settings_commit_preferences();
            }
            return true;
        }
        if (settings->choice.active) {
            if (!choice_popup_current(settings->choice)) {
                close_choice();
                refresh();
                return true;
            }
            const auto hit = popup_scene_hit_test(g_app.popup_scene, x, y);
            if (message == WM_MOUSEMOVE) {
                bool changed = false;
                popup_scene_set_hover(&g_app.popup_scene, x, y, &changed);
                if (changed)
                    refresh();
            } else if (message == WM_LBUTTONDOWN) {
                if (!hit.inside) {
                    close_choice();
                    refresh();
                    return true;
                }
                const auto choice = settings->choice;
                application_focus_window();
                if (!choice_popup_current(choice))
                    return true;
                application_capture_pointer();
                if (!choice_popup_current(choice))
                    return true;
                g_app.settings->choice.pressed_action = hit.enabled && application_pointer_captured() ? hit.action : 0;
            } else {
                auto choice = settings->choice;
                settings->choice.pressed_action = 0;
                const bool accepted = hit.enabled && hit.action == choice.pressed_action;
                close_choice();
                choice.popup_generation = g_app.popup_scene.widgets.generation;
                application_release_capture();
                // Capture cancellation may run nested UI. Both the closed bank
                // and copied control token must remain current before dispatch.
                if (accepted)
                    choice_action(hit.action, &choice);
            }
            return true;
        }
        // Like the official client's modals, a click on the scrim (press and release
        // outside the dialog) closes Settings.
        const auto backdrop = [&]() noexcept {
            const auto& scene = settings->scene;
            return scene.presentation == UiSettingsPresentation::modal && !scene.layout.compact &&
                   ui_rect_contains(scene.viewport, x, y) && !ui_rect_contains(scene.layout.modal, x, y);
        };
        if (settings->backdrop_pressed) {
            if (message == WM_LBUTTONUP) {
                settings->backdrop_pressed = false;
                application_release_capture();
                if (backdrop())
                    application_settings_close();
            }
            return true;
        }
        if (message == WM_LBUTTONDOWN && backdrop()) {
            ui_scroll_host_cancel(&g_app.scroll_host);
            settings->backdrop_pressed = true;
            application_capture_pointer();
            return true;
        }
        if (settings->search_selecting) {
            if (message == WM_MOUSEMOVE)
                search_point(*settings, x, y, true);
            else if (message == WM_LBUTTONUP) {
                settings->search_selecting = false;
                reset_value(&settings->pressed);
                ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::cancel, {});
                application_release_capture();
            }
            return true;
        }
        // The thin scrollbars widen under the pointer and while dragged.
        {
            bool changed = false;
            for (u32 i = 0; i < 2; ++i) {
                const bool sidebar = i == 0;
                const u8 look = settings->scrollbar_dragging && settings->scrollbar_sidebar == sidebar ? 2
                                : ui_scrollbar_track_hit(ui_settings_scene_scrollbar(settings->scene, sidebar), x, y) ? 1
                                                                                                                      : 0;
                changed = ui_settings_scene_set_scrollbar_state(&settings->scene, sidebar, look) || changed;
            }
            if (changed)
                application_request_frame();
        }
        if (settings->scrollbar_dragging) {
            if (message == WM_MOUSEMOVE) {
                const auto geometry = ui_settings_scene_scrollbar(settings->scene, settings->scrollbar_sidebar);
                const float offset =
                    settings->scrollbar_sidebar ? settings->scene.sidebar_offset : settings->scene.content_offset;
                const float desired = ui_scrollbar_scroll_from_pointer(geometry, y, settings->scrollbar_grab);
                ui_settings_scene_scroll(&settings->scene, settings->scrollbar_sidebar, desired - offset);
                refresh();
            } else if (message == WM_LBUTTONUP) {
                settings->scrollbar_dragging = false;
                application_release_capture();
            }
            return true;
        }
        if (message == WM_LBUTTONDOWN)
            for (u32 i = 0; i < 2; ++i) {
                const bool sidebar = i == 0;
                const auto geometry = ui_settings_scene_scrollbar(settings->scene, sidebar);
                if (!ui_scrollbar_track_hit(geometry, x, y))
                    continue;
                ui_scroll_host_cancel(&g_app.scroll_host);
                if (ui_scrollbar_thumb_hit(geometry, x, y)) {
                    settings->scrollbar_dragging = true;
                    settings->scrollbar_sidebar = sidebar;
                    settings->scrollbar_grab = y - geometry.thumb.top;
                    application_capture_pointer();
                    if (!application_pointer_captured())
                        settings->scrollbar_dragging = false;
                } else {
                    settings_glide_to(sidebar, (sidebar ? settings->scene.sidebar_offset
                                                        : settings->scene.content_offset) +
                                                   (y < geometry.thumb.top ? -1.f : 1.f) *
                                                       ui_rect_height(geometry.track));
                    refresh();
                }
                return true;
            }
        const auto hit = ui_settings_scene_hit_test(settings->scene, x, y);
        SettingBinding hit_binding{};
        if (hit.kind == UiSettingsTargetKind::control && hit.page.owner_id == kNativeSettingsOwner)
            control_node(hit.control_id, &hit_binding);
        // Preview replaces its button with Apply at the same position. The
        // second click of Preview's double-click must not commit that new
        // action; a fresh press/release still applies.
        if (double_click && hit_binding == SettingBinding::custom_apply) {
            application_settings_cancel_pointer();
            return true;
        }
        const auto* control = ui_settings_scene_control(settings->scene, hit);
        if (message == WM_LBUTTONDOWN && control && control->kind == UiSettingsControlKind::slider &&
            !ui_settings_scene_slider_hit(settings->scene, hit, x, y))
            return true;
        if (message == WM_MOUSEMOVE && control &&
            (control->kind == UiSettingsControlKind::palette || control->kind == UiSettingsControlKind::radio) &&
            ui_settings_scene_palette_hover(&settings->scene, hit, x, y))
            application_request_frame();
        const auto pointer = ui_button_pointer(&settings->scene.widgets,
                                               message == WM_LBUTTONDOWN ? UiButtonPointerEvent::press
                                               : message == WM_LBUTTONUP ? UiButtonPointerEvent::release
                                                                         : UiButtonPointerEvent::move,
                                               {x, y});
        if (pointer.changed)
            application_request_frame();
        if (message == WM_LBUTTONDOWN) {
            const u64 account_serial = g_app.plugin_account_scope_serial, profile = g_app.account.status.profile_id;
            const u64 catalog_revision = application_plugins_catalog_revision();
            settings->pressed = hit;
            ui_settings_scene_focus(&settings->scene, hit);
            application_focus_window();
            application_capture_pointer();
            refresh();
            if (g_app.settings != settings || !settings->scene.active ||
                account_serial != g_app.plugin_account_scope_serial || profile != g_app.account.status.profile_id ||
                catalog_revision != application_plugins_catalog_revision())
                return true;
            settings->pressed_revision = settings->scene.revision;
            settings->pressed_catalog_revision = catalog_revision;
            // A press in the search field places the caret and may drag a selection.
            if (hit.kind == UiSettingsTargetKind::search && application_pointer_captured()) {
                settings->search_selecting = true;
                search_point(*settings, x, y, (wparam & MK_SHIFT) != 0);
                return true;
            }
            settings->pressed_account_serial = account_serial;
            settings->pressed_profile = profile;
            if (control && control->kind == UiSettingsControlKind::slider && hit.page.owner_id == kNativeSettingsOwner &&
                application_pointer_captured()) {
                settings->slider_dragging = true;
                settings->slider_baseline = g_app.preferences;
                if (!capture_slider_theme(*settings, hit)) {
                    application_settings_cancel_pointer();
                    return true;
                }
                settings->slider_target = hit;
                settings->slider_preview = true;
                settings->slider_generation = settings->scene.widgets.generation;
                settings->slider_viewport = settings->scene.viewport;
                UiSettingsSliderGeometry slider{};
                settings->slider_grab = 0;
                if (ui_settings_scene_slider_geometry(settings->scene, hit, &slider))
                    settings->slider_grab = ui_slider_grab_offset(slider.thumb, x, y);
                UiSettingsActivation value{};
                if (ui_settings_scene_slider_pointer(settings->scene, hit, x - settings->slider_grab, &value))
                    activate(hit, false, nullptr, false, value.selected_choice, false);
            }
            settings->pressed_plugin =
                hit.kind == UiSettingsTargetKind::control && hit.page.owner_id != kNativeSettingsOwner;
            if (settings->pressed_plugin &&
                !native_plugin_host_settings_token(&settings->plugins, hit.page, &settings->pressed_token).ok())
                reset_value(&settings->pressed);
            if (!application_pointer_captured()) {
                reset_value(&settings->pressed);
                if (ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::cancel, {}).changed)
                    application_request_frame();
            }
        } else if (message == WM_LBUTTONUP) {
            const auto pressed = settings->pressed;
            const auto token = settings->pressed_token;
            const bool plugin = settings->pressed_plugin;
            const u32 revision = settings->pressed_revision;
            const u64 catalog_revision = settings->pressed_catalog_revision;
            const u64 serial = settings->pressed_account_serial, profile = settings->pressed_profile;
            reset_value(&settings->pressed);
            settings->pressed_plugin = false;
            application_release_capture();
            if (g_app.settings == settings && settings->scene.active && serial == g_app.plugin_account_scope_serial &&
                profile == g_app.account.status.profile_id &&
                catalog_revision == application_plugins_catalog_revision() && pointer.activate &&
                ui_settings_target_equal(hit, pressed) && revision == settings->scene.revision) {
                UiSettingsActivation value{};
                if (control && control->kind == UiSettingsControlKind::palette) {
                    if (ui_settings_scene_palette_pointer(settings->scene, hit, x, y, &value))
                        activate(hit, false, plugin ? &token : nullptr, false, value.selected_choice);
                } else if (control && control->kind == UiSettingsControlKind::radio) {
                    if (ui_settings_scene_radio_pointer(settings->scene, hit, x, y, &value))
                        activate(hit, false, plugin ? &token : nullptr, false, value.selected_choice);
                } else if (control && option_row(control->kind)) {
                    // Tabs, segments, swatches and button groups take the
                    // option under the pointer; a click between them does nothing.
                    if (ui_settings_scene_option_pointer(settings->scene, hit, x, y, &value))
                        activate(hit, false, plugin ? &token : nullptr, false, value.selected_choice);
                } else if (plugin && control && control->kind == UiSettingsControlKind::slider) {
                    // Plugin sliders take the clicked step; dragging stays native-only.
                    if (ui_settings_scene_slider_pointer(settings->scene, hit, x, &value))
                        activate(hit, false, &token, false, value.selected_choice);
                } else if (control && (control->kind == UiSettingsControlKind::action ||
                                       control->kind == UiSettingsControlKind::notice ||
                                       control->kind == UiSettingsControlKind::status ||
                                       control->kind == UiSettingsControlKind::footnote ||
                                       control->kind == UiSettingsControlKind::markdown)) {
                    if (ui_settings_scene_button_hit(settings->scene, hit, x, y))
                        activate(hit, false, plugin ? &token : nullptr, true);
                } else
                    activate(hit, false, plugin ? &token : nullptr, true);
            }
        }
        return true;
    }
    if (message == WM_DROPFILES) {
        DragFinish(reinterpret_cast<HDROP>(wparam));
        return true;
    }
    return message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
           message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_MOUSEHWHEEL ||
           message == WM_LBUTTONDBLCLK;
}

} // namespace maicord::app_detail
