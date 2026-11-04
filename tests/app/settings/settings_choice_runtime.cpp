#include "shell/state.hpp"
#include "../../support/ui_scene_fixtures.hpp"
#include "settings/settings.hpp"
#include "plugins/plugins.hpp"
#include "settings_runtime_internal.hpp"
#include "maicord/ui/widgets/settings_choice.hpp"
#include "shell/window_state.hpp"
#include "../../../src/app/account/account_ui_runtime.hpp"
#include "settings_runtime_keys.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord {
namespace settings_choice_runtime {
using namespace app_detail;
// The Appearance Background effect dropdown: the settings choice popup over a real catalog control. (The
// widget test pages a 1024-choice list; this runtime covers the controller's ownership and invalidation.)
struct Fixture {
    PopupItem items[34];
    PopupRowWidget rows[34];
    UiWidget widgets[70];
    UiLayoutNode nodes[70];
    HWND window, previous_window;
    u32 capture_cancellations;
    u32 background_pointer_events;
    wchar_t preferences_path[MAX_PATH];
    ATOM window_class;
    bool replace_on_capture, account_owned;
    bool preferences_owned;
    u64 previous_preferences_revision;
    bool previous_preferences_pending, previous_preferences_failed;
} fixture{};
constexpr wchar_t window_class_name[] = L"MaiCordSettingsChoiceRuntimeFixture";
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        message == WM_LBUTTONDBLCLK || message == WM_MOUSELEAVE) {
        if (application_settings_window_message(message, wparam, lparam))
            return 0;
        ++fixture.background_pointer_events;
        if (message == WM_LBUTTONDOWN)
            g_app.message_text_pointer_selection = true;
    }
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS) {
        application_settings_window_message(message, wparam, lparam);
        if (message == WM_CAPTURECHANGED) {
            ++fixture.capture_cancellations;
            if (fixture.replace_on_capture) {
                fixture.replace_on_capture = false;
                const PopupItem replacement{utf8_literal("Reentrant replacement popup"), 731,
                                            popup_item_enabled};
                g_app.popup_owner_kind = PopupOwnerKind::channel;
                popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, 88, {&replacement, 1},
                                 {40, 60, 41, 61}, {0, 0, 1000, 700}, 240);
            }
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool press_key(u32 code) noexcept {
    return application_settings_input(UiInputKind::key_down, code, false) &&
           application_settings_input(UiInputKind::key_up, code, false);
}
bool target_point(UiSettingsTarget target, LPARAM* point) noexcept {
    const auto* scene = g_app.renderer.settings_scene;
    if (!scene || !point)
        return false;
    // Rows answer the pointer only on their control (switch, select, button).
    UiSettingsControlGeometry geometry{};
    if (target.kind == UiSettingsTargetKind::control &&
        ui_settings_scene_control_geometry(*scene, target, &geometry)) {
        const float x = (geometry.clip.left + geometry.clip.right) * .5f,
                    y = (geometry.clip.top + geometry.clip.bottom) * .5f;
        if (!ui_settings_target_equal(ui_settings_scene_hit_test(*scene, x, y), target))
            return false;
        *point = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
        return true;
    }
    for (u32 i = 0; i < scene->widgets.count; ++i) {
        const auto handle = UiWidgetHandle{static_cast<u16>(i), scene->widgets.generation};
        const auto* node = ui_widget_manager_layout_node(scene->widgets, handle);
        if (!node)
            continue;
        const auto visible = ui_rect_intersection(node->rectangle, node->clip_rectangle);
        if (!ui_rect_has_area(visible))
            continue;
        const float x = (visible.left + visible.right) * .5f, y = (visible.top + visible.bottom) * .5f;
        if (!ui_settings_target_equal(ui_settings_scene_hit_test(*scene, x, y), target))
            continue;
        *point = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
        return true;
    }
    return false;
}
bool open(u32 key_code = key::enter) noexcept {
    auto* scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
    return application_settings_open_page(kAppearancePage) &&
           (scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene)) &&
           ui_settings_scene_focus(scene, {UiSettingsTargetKind::control, kAppearancePage, cid("appearance.background_effect")}) &&
           press_key(key_code) && g_app.popup_scene.active &&
           g_app.popup_owner_kind == PopupOwnerKind::settings_choice;
}
// The built-in effect the preferences hold (choices 0..3 of the dropdown).
u32 effect() noexcept {
    return static_cast<u32>(g_app.preferences.gradient.effect);
}
bool initialize() noexcept {
    if (g_app.account_ui || g_app.settings)
        return false;
    // Pointer toggles exercise real persistence, always through an explicit
    // fixture writer rather than the Windows user's ordinary preference file.
    if (g_app.preferences_writer)
        return false;
    wchar_t temporary[MAX_PATH]{};
    const DWORD chars = GetTempPathW(MAX_PATH, temporary);
    if (!chars || chars >= MAX_PATH || !GetTempFileNameW(temporary, L"MSC", 0, fixture.preferences_path) ||
        !client_preferences_writer_create(&g_app.preferences_writer, fixture.preferences_path))
        return false;
    fixture.preferences_owned = true;
    fixture.previous_preferences_revision = g_app.preferences_request_revision;
    fixture.previous_preferences_pending = g_app.preferences_save_pending;
    fixture.previous_preferences_failed = g_app.preferences_save_failed;
    g_app.preferences_request_revision = 0;
    g_app.preferences_save_pending = false;
    g_app.preferences_save_failed = false;
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui)
        return false;
    fixture.account_owned = true;
    g_app.account.status.profile_id = 98;
    g_app.login.visible = false;
    g_app.renderer_ready = true;
    g_app.renderer.dpi = 96;
    g_app.context_focus = nullptr;
    g_app.text_form = nullptr;
    g_app.settings_held_key = 0;
    application_plugins_account_scope_invalidated();
    fixture.previous_window = g_app.window;
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = window_class_name;
    fixture.window_class = RegisterClassW(&window_class);
    if (!fixture.window_class)
        return false;
    fixture.window = CreateWindowExW(0, window_class_name, L"Settings choice fixture", WS_POPUP, 0, 0, 1000,
                                     700, nullptr, nullptr, window_class.hInstance, nullptr);
    if (!fixture.window)
        return false;
    g_app.window = fixture.window; // Owned hidden window; no desktop input or ShowWindow.
    SetFocus(fixture.window);
    g_window_state.keyboard_focus = GetFocus() == fixture.window;
    if (GetFocus() != fixture.window)
        return false;
    return popup_scene_initialize(&g_app.popup_scene, {fixture.items, 34}, {fixture.rows, 34},
                                  {fixture.widgets, 70}, {fixture.nodes, 70});
}
void cleanup() noexcept {
    if (fixture.preferences_owned) {
        application_preferences_shutdown();
        if (g_app.preferences_writer)
            return; // Retain every borrowed owner on a failed join.
        fixture.preferences_owned = false;
        g_app.preferences_request_revision = fixture.previous_preferences_revision;
        g_app.preferences_save_pending = fixture.previous_preferences_pending;
        g_app.preferences_save_failed = fixture.previous_preferences_failed;
        DeleteFileW(fixture.preferences_path);
        fixture.preferences_path[0] = 0;
    }
    if (!fixture.account_owned)
        return;
    application_settings_release();
    popup_scene_release(&g_app.popup_scene);
    g_app.popup_scene = {};
    if (GetCapture() == fixture.window)
        ReleaseCapture();
    if (fixture.window)
        DestroyWindow(fixture.window);
    g_window_state.keyboard_focus = false;
    g_app.window = fixture.previous_window;
    fixture.window = nullptr;
    if (fixture.window_class)
        UnregisterClassW(window_class_name, GetModuleHandleW(nullptr));
    fixture.window_class = 0;
    g_app.context_focus = nullptr;
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    fixture.account_owned = false;
}
} // namespace settings_choice_runtime
void test_application_settings_choice_runtime(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace settings_choice_runtime;
    using namespace app_detail;
    const bool initialized = initialize();
    check(initialized, "choice runtime: real controller, Background effect dropdown and shared popup initialize");
    if (!initialized) {
        cleanup();
        return;
    }
    auto& f = fixture;
    auto& projection = settings_runtime_test::custom_theme_projection;
    projection = {};
    projection.revision = 1;
    const auto theme = g_app.preferences.theme;
    const auto preference_revision = g_app.preferences_request_revision;
    const UiSettingsTarget theme_target{UiSettingsTargetKind::control, kAppearancePage, cid("appearance.theme")};
    bool native_palette = application_settings_open();
    auto* native_scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
    const auto* native_theme =
        native_scene ? ui_settings_scene_control(*native_scene, theme_target) : nullptr;
    native_palette =
        native_palette && native_theme && native_theme->kind == UiSettingsControlKind::palette &&
        native_theme->choices.count == 3 && ui_settings_scene_focus(native_scene, theme_target) &&
        press_key(key::enter) && !g_app.popup_scene.active && g_app.preferences.theme == theme &&
        g_app.preferences_request_revision == preference_revision;
    LPARAM card_point = 0;
    bool found_card = false;
    if (native_scene) {
        for (i32 y = 0; y < 700 && !found_card; y += 4)
            for (i32 x = 0; x < 1000 && !found_card; x += 4) {
                UiSettingsActivation candidate{};
                if (ui_settings_target_equal(ui_settings_scene_hit_test(*native_scene,
                                                                        static_cast<float>(x),
                                                                        static_cast<float>(y)),
                                             theme_target) &&
                    ui_settings_scene_palette_pointer(*native_scene, theme_target, static_cast<float>(x),
                                                      static_cast<float>(y), &candidate) &&
                    candidate.selected_choice == 2) {
                    card_point = MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
                    found_card = true;
                }
            }
    }
    if (found_card) {
        SendMessageW(f.window, WM_MOUSEMOVE, 0, card_point);
        SendMessageW(f.window, WM_LBUTTONDOWN, MK_LBUTTON, card_point);
        native_palette = native_palette && GetCapture() == f.window;
        SendMessageW(f.window, WM_LBUTTONUP, 0, card_point);
        native_palette =
            native_palette && GetCapture() != f.window && g_app.preferences.theme == ClientTheme::amoled &&
            g_app.preferences_request_revision == preference_revision + 1 && !g_app.popup_scene.active;
    }
    check(native_palette && found_card, "choice runtime: native Theme swatches activate the exact swatch "
                                        "through real pointer capture");
    LPARAM section_point = 0, toggle_point = 0;
    const UiSettingsTarget accessibility{UiSettingsTargetKind::page, kAccessibilityPage, 0};
    const UiSettingsTarget smooth{UiSettingsTargetKind::control, kAccessibilityPage, cid("accessibility.smooth_scroll")};
    bool pointer_settings = target_point(accessibility, &section_point);
    if (pointer_settings) {
        auto* scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
        const auto revision = scene->revision;
        const auto generation = scene->widgets.generation;
        g_app.frame_pending = false;
        SendMessageW(f.window, WM_MOUSEMOVE, 0, section_point);
        // Hovering repaints (the option a swatch row left hovered clears, a paint-only revision step) but never relays out.
        pointer_settings = scene->widgets.hovered.generation && scene->revision - revision <= 1 &&
                           scene->widgets.generation == generation && g_app.frame_pending;
        SendMessageW(f.window, WM_LBUTTONDOWN, MK_LBUTTON, section_point);
        pointer_settings =
            pointer_settings && scene->widgets.captured.generation && GetCapture() == f.window;
        SendMessageW(f.window, WM_LBUTTONUP, 0, section_point);
        pointer_settings = pointer_settings && ui_page_key_equal(scene->selected_page, kAccessibilityPage) &&
                           !scene->widgets.captured.generation && GetCapture() != f.window;
        // Smooth scrolling sits below Visual Density; bring it into view.
        if (pointer_settings)
            ui_settings_scene_reveal(scene, smooth);
        pointer_settings = pointer_settings && target_point(smooth, &toggle_point);
    }
    const bool smooth_before = g_app.preferences.smooth_scroll;
    if (pointer_settings) {
        SendMessageW(f.window, WM_MOUSEMOVE, 0, toggle_point);
        SendMessageW(f.window, WM_LBUTTONDOWN, MK_LBUTTON, toggle_point);
        SendMessageW(f.window, WM_MOUSEMOVE, MK_LBUTTON, toggle_point);
        SendMessageW(f.window, WM_LBUTTONUP, 0, toggle_point);
        pointer_settings = g_app.preferences.smooth_scroll != smooth_before;
    }
    check(pointer_settings && !f.background_pointer_events && !g_app.message_text_pointer_selection,
          "settings pointer: real hidden-window hover press section selection and toggle consume before "
          "message text layer");
    if (pointer_settings) {
        const bool before_cancel = g_app.preferences.smooth_scroll;
        SendMessageW(f.window, WM_LBUTTONDOWN, MK_LBUTTON, toggle_point);
        SendMessageW(f.window, WM_CANCELMODE, 0, 0);
        SendMessageW(f.window, WM_LBUTTONUP, 0, toggle_point);
        auto* scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
        check(g_app.preferences.smooth_scroll == before_cancel && !scene->widgets.captured.generation &&
                  GetCapture() != f.window && !f.background_pointer_events,
              "settings pointer: native capture cancellation cannot activate or pass release to underlying "
              "content");
        SendMessageW(f.window, WM_LBUTTONDOWN, MK_LBUTTON, toggle_point);
        const auto* selected = ui_page_registry_selected(*scene->registry);
        if (selected && selected->context)
            ++static_cast<UiSettingsPageContent*>(selected->context)->revision;
        const bool synchronized = application_settings_synchronize();
        SendMessageW(f.window, WM_LBUTTONUP, 0, toggle_point);
        check(synchronized && g_app.preferences.smooth_scroll == before_cancel &&
                  !scene->widgets.captured.generation && GetCapture() != f.window &&
                  !f.background_pointer_events,
              "settings pointer: revised content invalidates held widget admission before native release");
        SendMessageW(f.window, WM_MOUSEMOVE, 0, toggle_point);
        SendMessageW(f.window, WM_MOUSELEAVE, 0, 0);
        check(!scene->widgets.hovered.generation && !f.background_pointer_events,
              "settings pointer: native leave clears hover while preserving modal ownership");
    }
    // Every pointer move tracks the option under it: the option a swatch or radio row left hovered clears as soon as
    // the pointer is on any other control, so a segmented row never paints it. (Segment hover itself is covered by
    // the scene test; the segmented rows here are disabled without an account.)
    if (pointer_settings) {
        auto* scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
        scene->palette_hovered_index = 2;
        SendMessageW(f.window, WM_MOUSEMOVE, 0, toggle_point);
        check(scene->palette_hovered_index == ~u32{0},
              "settings pointer: moving onto another control clears the option a previous row left hovered");
    }
    bool changed = false;
    const bool opened = open();
    const u32 initial = effect();
    check(opened && g_app.popup_scene.item_count >= kBackgroundEffectCount &&
              (g_app.popup_scene.items[initial].flags & popup_item_checked) && effect() == initial,
          "choice runtime: Enter opens the list with the current value checked, without cycling or choosing");
    check(popup_scene_select_index(&g_app.popup_scene, 1, &changed) && press_key(key::enter) && effect() == 1 &&
              !g_app.popup_scene.active,
          "choice runtime: Enter on an item dismisses the popup and applies that choice");
    check(open(key::space) && g_app.popup_scene.selected_index == 1 &&
              (g_app.popup_scene.items[1].flags & popup_item_checked) && press_key(key::escape) &&
              application_settings_active() && !g_app.popup_scene.active && effect() == 1,
          "choice runtime: Space opens on the current value and Escape closes only the picker");
    check(press_key(key::right) && effect() == 2 && press_key(key::left) && effect() == 1 && !g_app.popup_scene.active,
          "choice runtime: Left and Right step the value without opening a popup");
    ++projection.revision;
    application_settings_synchronize();
    PopupItemLayout item{};
    const u32 cancellations = f.capture_cancellations;
    // Real mouse dispatch follows its window's activation/focus transition.
    // Establish that state before opening; a loss during a live picker must
    // continue to cancel it, including the nested hidden-window focus messages.
    SetFocus(f.window);
    g_window_state.keyboard_focus = GetFocus() == f.window;
    const bool focused = GetFocus() == f.window;
    check(focused, "choice runtime: owned hidden window focus is established before pointer picker opens");
    bool mouse = focused && application_settings_synchronize() && open() &&
                 popup_scene_resolve_item_layout(g_app.popup_scene, 2, &item) && item.visible;
    if (mouse) {
        const LPARAM point = MAKELPARAM(
            static_cast<short>((item.visible_rectangle.left + item.visible_rectangle.right) * .5f),
            static_cast<short>((item.visible_rectangle.top + item.visible_rectangle.bottom) * .5f));
        mouse = application_settings_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point);
        mouse = mouse && application_settings_window_message(WM_LBUTTONUP, 0, point);
    }
    const bool mouse_passed = mouse && effect() == 2 && !g_app.popup_scene.active &&
                              f.capture_cancellations > cancellations && GetCapture() != f.window;
    check(mouse_passed, "choice runtime: mouse release synchronously cancels real hidden-window capture "
                        "before the chosen index is applied");
    bool replaced = open() && popup_scene_resolve_item_layout(g_app.popup_scene, 1, &item) && item.visible;
    if (replaced) {
        const LPARAM point = MAKELPARAM(
            static_cast<short>((item.visible_rectangle.left + item.visible_rectangle.right) * .5f),
            static_cast<short>((item.visible_rectangle.top + item.visible_rectangle.bottom) * .5f));
        replaced = application_settings_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point);
        f.replace_on_capture = true;
        replaced = replaced && application_settings_window_message(WM_LBUTTONUP, 0, point);
    }
    const bool replacement_passed = replaced && effect() == 2 && g_app.popup_scene.active &&
                                    g_app.popup_scene.items[0].action == 731;
    check(replacement_passed, "choice runtime: capture cancellation's reentrant replacement bank rejects "
                              "the old choice and preserves its new owner");
    f.replace_on_capture = false;
    popup_scene_close(&g_app.popup_scene);
    g_app.popup_owner_kind = PopupOwnerKind::none;
    check(open(), "choice runtime: revised selected control can recapture a window");
    ++projection.revision;
    const u32 chosen = effect();
    check(popup_scene_select_index(&g_app.popup_scene, 3, &changed) && application_settings_synchronize() &&
              press_key(key::enter) && !g_app.popup_scene.active && effect() == chosen,
          "choice runtime: a revised page expires the copied choice token before the choice is applied");
    check(open(), "choice runtime: current descriptors reopen after revision refresh");
    auto* scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
    check(ui_settings_scene_select(scene, kAccessibilityPage) && application_settings_synchronize() &&
              !g_app.popup_scene.active && effect() == chosen,
          "choice runtime: switching pages drops the old borrowing window before text projection");
    check(open(), "choice runtime: the dropdown recaptures after native page selection");
    const auto serial = g_app.plugin_account_scope_serial;
    application_plugins_account_scope_invalidated();
    check(g_app.plugin_account_scope_serial == serial + 1 && !g_app.popup_scene.active &&
              !g_app.popup_scene.items[0].label.data && effect() == chosen,
          "choice runtime: real account invalidation releases borrowed descriptors immediately");
    check(open(), "choice runtime: current account scope opens a new choice window");
    ++g_app.popup_scene.opens;
    check(popup_scene_select_index(&g_app.popup_scene, 3, &changed) && press_key(key::enter) && !g_app.popup_scene.active &&
              effect() == chosen,
          "choice runtime: reopened popup identity invalidates the old selection even with unchanged "
          "content");
    check(open(), "choice runtime: popup identity can be recaptured");
    check(application_settings_window_message(WM_KILLFOCUS, 0, 0) == false && !g_app.popup_scene.active &&
              application_settings_active(),
          "choice runtime: focus cancellation closes borrowed picker while retaining settings");
    check(open(), "choice runtime: the dropdown opens again after the focus loss");
    check(popup_scene_select_index(&g_app.popup_scene, 0, &changed) && press_key(key::enter) && effect() == 0 &&
              !g_app.popup_scene.active,
          "choice runtime: a recaptured window applies its own choice");
    cleanup();
}
} // namespace maicord
