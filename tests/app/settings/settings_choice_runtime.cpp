#include "shell/state.hpp"
#include "../../support/ui_scene_fixtures.hpp"
#include "settings/settings.hpp"
#include "plugins/plugins.hpp"
#include "settings_runtime_internal.hpp"
#include "../../support/window_fixture.hpp"
#include "maicord/ui/widgets/settings_choice.hpp"
#include "shell/window_state.hpp"
#include "shell/window_events.hpp"
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
    Window window, previous_window;
    u32 capture_cancellations;
    u32 background_pointer_events;
    wchar_t preferences_path[MAX_PATH];
    bool replace_on_capture, account_owned;
    bool preferences_owned;
    u64 previous_preferences_revision;
    bool previous_preferences_pending, previous_preferences_failed;
} fixture{};
// The client's window procedure, as far as Settings goes: pointer events try Settings first and count what falls
// through; cancellations reach it and the lost capture is counted.
void window_sink(void*, const WindowEvent& event, WindowEventResult* result) noexcept {
    if (input_pointer_step(event) != PointerStep::none || event.type == EventType::pointer_leave) {
        if (application_settings_window_event(event)) {
            result->handled = true;
            return;
        }
        ++fixture.background_pointer_events;
        if (input_press(event))
            g_app.message_text_pointer_selection = true;
    }
    if (event.type == EventType::capture_lost || event.type == EventType::cancel_input || input_focus_lost(event)) {
        application_settings_window_event(event);
        if (event.type == EventType::capture_lost) {
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
}
// The event of a packed client point, delivered through the window service as the system would.
bool inject(const WindowEvent& event) noexcept {
    WindowEventResult result{};
    window_inject(g_app.window, event, &result);
    return result.handled;
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
    fixture.window = window_test::open(window_sink);
    if (!fixture.window)
        return false;
    g_app.window = fixture.window; // The headless window: no desktop input or ShowWindow.
    g_window_state.keyboard_focus = window_focus(fixture.window);
    if (!g_window_state.keyboard_focus)
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
    if (window_pointer_captured(fixture.window))
        window_release_capture(fixture.window);
    window_test::close();
    g_window_state.keyboard_focus = false;
    g_app.window = fixture.previous_window;
    fixture.window = nullptr;
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
        inject(window_test::pointer_move(window_test::x_of(card_point), window_test::y_of(card_point)));
        inject(window_test::pointer_down(window_test::x_of(card_point), window_test::y_of(card_point)));
        native_palette = native_palette && window_pointer_captured(f.window);
        inject(window_test::pointer_up(window_test::x_of(card_point), window_test::y_of(card_point), PointerButton::left));
        native_palette =
            native_palette && !window_pointer_captured(f.window) && g_app.preferences.theme == ClientTheme::amoled &&
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
        inject(window_test::pointer_move(window_test::x_of(section_point), window_test::y_of(section_point)));
        // Hovering repaints (the option a swatch row left hovered clears, a paint-only revision step) but never relays out.
        pointer_settings = scene->widgets.hovered.generation && scene->revision - revision <= 1 &&
                           scene->widgets.generation == generation && g_app.frame_pending;
        inject(window_test::pointer_down(window_test::x_of(section_point), window_test::y_of(section_point)));
        pointer_settings =
            pointer_settings && scene->widgets.captured.generation && window_pointer_captured(f.window);
        inject(window_test::pointer_up(window_test::x_of(section_point), window_test::y_of(section_point), PointerButton::left));
        pointer_settings = pointer_settings && ui_page_key_equal(scene->selected_page, kAccessibilityPage) &&
                           !scene->widgets.captured.generation && !window_pointer_captured(f.window);
        // Smooth scrolling sits below Visual Density; bring it into view.
        if (pointer_settings)
            ui_settings_scene_reveal(scene, smooth);
        pointer_settings = pointer_settings && target_point(smooth, &toggle_point);
    }
    const bool smooth_before = g_app.preferences.smooth_scroll;
    if (pointer_settings) {
        inject(window_test::pointer_move(window_test::x_of(toggle_point), window_test::y_of(toggle_point)));
        inject(window_test::pointer_down(window_test::x_of(toggle_point), window_test::y_of(toggle_point)));
        inject(window_test::pointer_move(window_test::x_of(toggle_point), window_test::y_of(toggle_point), window_test::button_bit(PointerButton::left)));
        inject(window_test::pointer_up(window_test::x_of(toggle_point), window_test::y_of(toggle_point), PointerButton::left));
        pointer_settings = g_app.preferences.smooth_scroll != smooth_before;
    }
    check(pointer_settings && !f.background_pointer_events && !g_app.message_text_pointer_selection,
          "settings pointer: real hidden-window hover press section selection and toggle consume before "
          "message text layer");
    if (pointer_settings) {
        const bool before_cancel = g_app.preferences.smooth_scroll;
        inject(window_test::pointer_down(window_test::x_of(toggle_point), window_test::y_of(toggle_point)));
        inject(window_test::cancel_input());
        inject(window_test::pointer_up(window_test::x_of(toggle_point), window_test::y_of(toggle_point), PointerButton::left));
        auto* scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
        check(g_app.preferences.smooth_scroll == before_cancel && !scene->widgets.captured.generation &&
                  !window_pointer_captured(f.window) && !f.background_pointer_events,
              "settings pointer: native capture cancellation cannot activate or pass release to underlying "
              "content");
        inject(window_test::pointer_down(window_test::x_of(toggle_point), window_test::y_of(toggle_point)));
        const auto* selected = ui_page_registry_selected(*scene->registry);
        if (selected && selected->context)
            ++static_cast<UiSettingsPageContent*>(selected->context)->revision;
        const bool synchronized = application_settings_synchronize();
        inject(window_test::pointer_up(window_test::x_of(toggle_point), window_test::y_of(toggle_point), PointerButton::left));
        check(synchronized && g_app.preferences.smooth_scroll == before_cancel &&
                  !scene->widgets.captured.generation && !window_pointer_captured(f.window) &&
                  !f.background_pointer_events,
              "settings pointer: revised content invalidates held widget admission before native release");
        inject(window_test::pointer_move(window_test::x_of(toggle_point), window_test::y_of(toggle_point)));
        inject(window_test::pointer_leave());
        check(!scene->widgets.hovered.generation && !f.background_pointer_events,
              "settings pointer: native leave clears hover while preserving modal ownership");
    }
    // Every pointer move tracks the option under it: the option a swatch or radio row left hovered clears as soon as
    // the pointer is on any other control, so a segmented row never paints it. (Segment hover itself is covered by
    // the scene test; the segmented rows here are disabled without an account.)
    if (pointer_settings) {
        auto* scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
        scene->palette_hovered_index = 2;
        inject(window_test::pointer_move(window_test::x_of(toggle_point), window_test::y_of(toggle_point)));
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
    const bool focused = window_focus(f.window);
    g_window_state.keyboard_focus = focused;
    check(focused, "choice runtime: owned hidden window focus is established before pointer picker opens");
    bool mouse = focused && application_settings_synchronize() && open() &&
                 popup_scene_resolve_item_layout(g_app.popup_scene, 2, &item) && item.visible;
    if (mouse) {
        const LPARAM point = MAKELPARAM(
            static_cast<short>((item.visible_rectangle.left + item.visible_rectangle.right) * .5f),
            static_cast<short>((item.visible_rectangle.top + item.visible_rectangle.bottom) * .5f));
        mouse = application_settings_window_event(window_test::pointer_down(window_test::x_of(point), window_test::y_of(point)));
        mouse = mouse && application_settings_window_event(window_test::pointer_up(window_test::x_of(point), window_test::y_of(point), PointerButton::left));
    }
    const bool mouse_passed = mouse && effect() == 2 && !g_app.popup_scene.active &&
                              f.capture_cancellations > cancellations && !window_pointer_captured(f.window);
    check(mouse_passed, "choice runtime: mouse release synchronously cancels real hidden-window capture "
                        "before the chosen index is applied");
    bool replaced = open() && popup_scene_resolve_item_layout(g_app.popup_scene, 1, &item) && item.visible;
    if (replaced) {
        const LPARAM point = MAKELPARAM(
            static_cast<short>((item.visible_rectangle.left + item.visible_rectangle.right) * .5f),
            static_cast<short>((item.visible_rectangle.top + item.visible_rectangle.bottom) * .5f));
        replaced = application_settings_window_event(window_test::pointer_down(window_test::x_of(point), window_test::y_of(point)));
        f.replace_on_capture = true;
        replaced = replaced && application_settings_window_event(window_test::pointer_up(window_test::x_of(point), window_test::y_of(point), PointerButton::left));
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
    check(application_settings_window_event(window_test::focus_changed(false)) == false && !g_app.popup_scene.active &&
              application_settings_active(),
          "choice runtime: focus cancellation closes borrowed picker while retaining settings");
    check(open(), "choice runtime: the dropdown opens again after the focus loss");
    check(popup_scene_select_index(&g_app.popup_scene, 0, &changed) && press_key(key::enter) && effect() == 0 &&
              !g_app.popup_scene.active,
          "choice runtime: a recaptured window applies its own choice");
    cleanup();
}
} // namespace maicord
