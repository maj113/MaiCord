#include "settings_internal.hpp"
#include "../guild/channel_overview.hpp"
#include "../shell/modal_scroll.hpp"
#include "../shell/input.hpp"
#include "../shell/window_events.hpp"
#include "keybinds_settings.hpp"
#include "../store/store_host.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/widgets/button.hpp"
#include "maicord/ui/widgets/settings_choice.hpp"
#include "maicord/platform/window/key.hpp"

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
        if (code == key::escape) {
            g_app.settings_held_key = code;
            close_choice();
            refresh();
            return true;
        }
        const bool reverse = application_shift_down();
        bool changed = false;
        if (code == key::up || code == key::down || code == key::tab) {
            popup_scene_move_selection(&g_app.popup_scene, code == key::up || (code == key::tab && reverse) ? -1 : 1,
                                       &changed);
            if (changed)
                refresh();
        } else if (code == key::page_up || code == key::page_down) {
            choice_action(code == key::page_up ? kUiSettingsChoicePrevious : kUiSettingsChoiceNext);
        } else if ((code == key::enter || code == key::space) && !repeated) {
            g_app.settings_held_key = code;
            choice_action(popup_scene_selected_action(g_app.popup_scene));
        } else if ((code == key::left || code == key::right) && !repeated) {
            const auto choice = settings->choice;
            close_choice();
            g_app.settings_held_key = code;
            if (choice_control(choice))
                activate({UiSettingsTargetKind::control, choice.token.page, choice.control_id}, code == key::left);
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
    if (code == key::escape) {
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
        if (code == key::enter && !repeated && searching(*settings)) {
            g_app.settings_held_key = code;
            const auto controls = settings->scene.content.controls;
            for (u32 i = 0; i < controls.count; ++i)
                if (controls[i].kind != UiSettingsControlKind::notice) {
                    open_result(*settings, controls[i].id);
                    break;
                }
            return true;
        }
        if (code == key::down && searching(*settings)) {
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
        const bool decrease = code == key::left, increase = code == key::right;
        // Page Up/Down move a slider by a tenth of its range.
        const bool page = focused_control->kind == UiSettingsControlKind::slider && (code == key::page_up || code == key::page_down);
        if (code == key::home || code == key::end || decrease || increase || page) {
            UiSettingsActivation activation{};
            const auto direction = code == key::home   ? UiSettingsSliderKey::home
                                   : code == key::end   ? UiSettingsSliderKey::end
                                   : code == key::page_up ? UiSettingsSliderKey::page_increase
                                   : code == key::page_down  ? UiSettingsSliderKey::page_decrease
                                   : decrease         ? UiSettingsSliderKey::decrease
                                                      : UiSettingsSliderKey::increase;
            if (ui_settings_scene_slider_key(settings->scene, focused, direction, &activation) &&
                activation.selected_choice != focused_control->selected_choice) {
                const bool preview = focused_control->kind == UiSettingsControlKind::slider &&
                                     focused.page.owner_id == kNativeSettingsOwner;
                if (preview && !settings->slider_preview) {
                    settings->slider_baseline = g_app.preferences;
                    settings->slider_target = focused;
                    settings->pressed_account_serial = g_app.plugin_account_scope_serial;
                    settings->pressed_profile = g_app.account.status.profile_id;
                    settings->slider_preview = true;
                }
                if (preview)
                    settings->preference_key = code;
                activate(focused, false, false, activation.selected_choice, !preview);
            }
            return true;
        }
        if (code == key::enter || code == key::space) {
            g_app.settings_held_key = code;
            if (code == key::enter && searching(*settings) && !repeated)
                open_result(*settings, focused.control_id);
            return true;
        }
    }
    if (code == key::tab) {
        ui_settings_scene_move_zone(&settings->scene, reverse);
        refresh();
        return true;
    }
    if (code == key::up || code == key::down) {
        const float before = settings->scene.content_offset;
        if (!ui_settings_scene_move_within(&settings->scene, code == key::up) && zone == UiSettingsFocusZone::none)
            ui_settings_scene_move_zone(&settings->scene, code == key::up);
        settings_glide_reveal(&settings->scene, application_settings_scroll_binding(false), before);
        refresh();
        return true;
    }
    if ((code == key::home || code == key::end) && zone != UiSettingsFocusZone::search) {
        const float before = settings->scene.content_offset;
        u32 steps = 0;
        while (steps < 64 && ui_settings_scene_move_within(&settings->scene, code == key::home))
            ++steps;
        settings_glide_reveal(&settings->scene, application_settings_scroll_binding(false), before);
        refresh();
        return true;
    }
    if ((code == key::enter || code == key::space || code == key::left || code == key::right) && !repeated) {
        if (zone == UiSettingsFocusZone::search)
            return true;
        g_app.settings_held_key = code;
        if (code == key::enter && searching(*settings) && focused.kind == UiSettingsTargetKind::control) {
            open_result(*settings, focused.control_id);
            return true;
        }
        if ((code == key::left || code == key::right) && zone != UiSettingsFocusZone::panel)
            return true;
        activate(focused, code == key::left || reverse, code == key::enter || code == key::space);
        return true;
    }
    if (code == key::page_up || code == key::page_down) {
        settings_glide_to(application_settings_scroll_binding(false),
                          settings->scene.content_offset +
                              (code == key::page_up ? -1.f : 1.f) * ui_rect_height(settings->scene.viewport) * .8f);
        refresh();
    }
    return true;
}
bool application_settings_window_event(const WindowEvent& event) noexcept {
    if (application_channel_overview_window_event(event))
        return true;
    auto* settings = g_app.settings;
    if (!settings || !settings->scene.active)
        return false;
    if (application_store_window_event(event))
        return true;
    if (input_press(event, PointerButton::back) || input_press(event, PointerButton::forward) ||
        input_press(event, PointerButton::middle)) {
        const u32 button = input_press(event, PointerButton::middle) ? key::mbutton
                           : input_press(event, PointerButton::back)   ? key::xbutton1
                                                                     : key::xbutton2;
        if (application_voice_settings_capture(UiInputKind::key_down, button) ||
            application_keybinds_settings_capture(UiInputKind::key_down, button))
            return settled(*settings, true);
    }
    if (event.type == EventType::character && (event.flags & ev_system) == 0) {
        const u32 character = event.character.unit;
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
    // Native action controls receive a second press as a double press, not a press: both are presses here.
    // Inline text editors are routed above this settings-only controller.
    if (event.type == EventType::capture_lost || event.type == EventType::cancel_input || input_focus_lost(event) ||
        event.type == EventType::resize || event.type == EventType::scale_changed) {
        const bool was_choice = settings->choice.active;
        application_settings_cancel_pointer();
        settings->scrollbar_dragging = false;
        if (was_choice)
            refresh();
        else
            application_request_frame();
        return false;
    }
    if (event.type == EventType::pointer_leave) {
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
    if (input_vertical_wheel(event)) {
        if (settings->choice.active) {
            choice_action(event.wheel.dy120 > 0 ? kUiSettingsChoicePrevious : kUiSettingsChoiceNext);
            return true;
        }
        const UiPoint point = client_wheel_dip(event);
        application_modal_scroll_wheel(
            application_settings_scroll_binding(ui_rect_contains(settings->scene.layout.sidebar, point.x, point.y)),
            point, event);
        refresh();
        return true;
    }
    if (const PointerStep step = input_pointer_step(event); step != PointerStep::none) {
        const float x = client_dip_x(event), y = client_dip_y(event);
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
            if (step == PointerStep::move) {
                UiSettingsActivation value{};
                if (ui_settings_scene_slider_pointer(settings->scene, target, x - settings->slider_grab, &value) &&
                    value.selected_choice != control->selected_choice)
                    activate(target, false, false, value.selected_choice, false);
            } else if (step == PointerStep::release) {
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
            if (step == PointerStep::move) {
                bool changed = false;
                popup_scene_set_hover(&g_app.popup_scene, x, y, &changed);
                if (changed)
                    refresh();
            } else if (step == PointerStep::press) {
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
            if (step == PointerStep::release) {
                settings->backdrop_pressed = false;
                application_release_capture();
                if (backdrop())
                    application_settings_close();
            }
            return true;
        }
        if (step == PointerStep::press && backdrop()) {
            ui_scroll_host_cancel(&g_app.scroll_host);
            settings->backdrop_pressed = true;
            application_capture_pointer();
            return true;
        }
        if (settings->search_selecting) {
            if (step == PointerStep::move)
                search_point(*settings, x, y, true);
            else if (step == PointerStep::release) {
                settings->search_selecting = false;
                reset_value(&settings->pressed);
                ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::cancel, {});
                application_release_capture();
            }
            return true;
        }
        if (settings->scrollbar_dragging) {
            if (step == PointerStep::move) {
                const auto geometry = ui_settings_scene_scrollbar(settings->scene, settings->scrollbar_sidebar);
                const float offset =
                    settings->scrollbar_sidebar ? settings->scene.sidebar_offset : settings->scene.content_offset;
                const float desired = ui_scrollbar_scroll_from_pointer(geometry, y, settings->scrollbar_grab);
                ui_settings_scene_scroll(&settings->scene, settings->scrollbar_sidebar, desired - offset);
                refresh();
            } else if (step == PointerStep::release) {
                settings->scrollbar_dragging = false;
                application_release_capture();
            }
            return true;
        }
        if (step == PointerStep::press)
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
                    settings_glide_to(application_settings_scroll_binding(sidebar),
                                      (sidebar ? settings->scene.sidebar_offset : settings->scene.content_offset) +
                                          (y < geometry.thumb.top ? -1.f : 1.f) * ui_rect_height(geometry.track));
                    refresh();
                }
                return true;
            }
        const auto hit = ui_settings_scene_hit_test(settings->scene, x, y);
        const auto* control = ui_settings_scene_control(settings->scene, hit);
        if (step == PointerStep::press && control && control->kind == UiSettingsControlKind::slider &&
            !ui_settings_scene_slider_hit(settings->scene, hit, x, y))
            return true;
        // Every pointer move tracks the option under it (swatches, radios, segments, tabs, buttons, list parts) and
        // clears it elsewhere, so a row never paints the option a previous row left hovered.
        if (step == PointerStep::move && ui_settings_scene_palette_hover(&settings->scene, hit, x, y))
            application_request_frame();
        const auto pointer = ui_button_pointer(&settings->scene.widgets,
                                               step == PointerStep::press ? UiButtonPointerEvent::press
                                               : step == PointerStep::release ? UiButtonPointerEvent::release
                                                                         : UiButtonPointerEvent::move,
                                               {x, y});
        if (pointer.changed)
            application_request_frame();
        if (step == PointerStep::press) {
            const u64 account_serial = g_app.plugin_account_scope_serial, profile = g_app.account.status.profile_id;
            settings->pressed = hit;
            ui_settings_scene_focus(&settings->scene, hit);
            application_focus_window();
            application_capture_pointer();
            refresh();
            if (g_app.settings != settings || !settings->scene.active ||
                account_serial != g_app.plugin_account_scope_serial || profile != g_app.account.status.profile_id)
                return true;
            settings->pressed_revision = settings->scene.revision;
            // A press in the search field places the caret and may drag a selection.
            if (hit.kind == UiSettingsTargetKind::search && application_pointer_captured()) {
                settings->search_selecting = true;
                search_point(*settings, x, y, input_shift(event));
                return true;
            }
            settings->pressed_account_serial = account_serial;
            settings->pressed_profile = profile;
            if (control && control->kind == UiSettingsControlKind::slider && hit.page.owner_id == kNativeSettingsOwner &&
                application_pointer_captured()) {
                settings->slider_dragging = true;
                settings->slider_baseline = g_app.preferences;
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
                    activate(hit, false, false, value.selected_choice, false);
            }
            if (!application_pointer_captured()) {
                reset_value(&settings->pressed);
                if (ui_button_pointer(&settings->scene.widgets, UiButtonPointerEvent::cancel, {}).changed)
                    application_request_frame();
            }
        } else if (step == PointerStep::release) {
            const auto pressed = settings->pressed;
            const u32 revision = settings->pressed_revision;
            const u64 serial = settings->pressed_account_serial, profile = settings->pressed_profile;
            reset_value(&settings->pressed);
            application_release_capture();
            if (g_app.settings == settings && settings->scene.active && serial == g_app.plugin_account_scope_serial &&
                profile == g_app.account.status.profile_id && pointer.activate &&
                ui_settings_target_equal(hit, pressed) && revision == settings->scene.revision) {
                UiSettingsActivation value{};
                if (control && control->kind == UiSettingsControlKind::palette) {
                    if (ui_settings_scene_palette_pointer(settings->scene, hit, x, y, &value))
                        activate(hit, false, false, value.selected_choice);
                } else if (control && control->kind == UiSettingsControlKind::radio) {
                    if (ui_settings_scene_radio_pointer(settings->scene, hit, x, y, &value))
                        activate(hit, false, false, value.selected_choice);
                } else if (control && settings_option_row(control->kind)) {
                    // Tabs, segments, swatches and button groups take the
                    // option under the pointer; a click between them does nothing.
                    if (ui_settings_scene_option_pointer(settings->scene, hit, x, y, &value))
                        activate(hit, false, false, value.selected_choice);
                } else if (control && (control->kind == UiSettingsControlKind::action ||
                                       control->kind == UiSettingsControlKind::notice ||
                                       control->kind == UiSettingsControlKind::status ||
                                       control->kind == UiSettingsControlKind::footnote ||
                                       control->kind == UiSettingsControlKind::markdown)) {
                    if (ui_settings_scene_button_hit(settings->scene, hit, x, y))
                        activate(hit, false, true);
                } else
                    activate(hit, false, true);
            }
        }
        return true;
    }
    if (event.type == EventType::drop_files) {
        dropped_files_release(event.drop.files);
        return true;
    }
    return event.type == EventType::context_menu || input_side_button(event) || input_horizontal_wheel(event);
}

} // namespace maicord::app_detail
