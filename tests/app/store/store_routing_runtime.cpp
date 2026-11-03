// The store pages inside Settings, driven the way the client drives them: pointer and wheel messages through a
// window procedure that routes like the client's (application_settings_window_message, which hands the store its
// share), keys through application_settings_input. The real store host, page and scene are linked.
#include "../../support/ui_scene_fixtures.hpp"
#include "shell/state.hpp"
#include "settings/settings.hpp"
#include "store/store_host.hpp"
#include "store/store_model.hpp"
#include "../settings/settings_runtime_internal.hpp"
#include "../settings/settings_runtime_keys.hpp"
#include "effects/effects.hpp"
#include "store/store_source.hpp"
#include "shell/window_state.hpp"
#include "../../../src/app/account/account_ui_runtime.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/store/store_scene.hpp"
#include "maicord/ui/widgets/spinner.hpp"
#include "plugins/catalog_service.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace app_detail {
void store_stub_catalog_index(const CatalogIndex*) noexcept;                // store_runtime_stubs.cpp
void store_stub_catalog_operation(const CatalogOperation& operation) noexcept; // store_runtime_stubs.cpp
} // namespace app_detail

namespace store_routing_runtime {
using namespace app_detail;

struct Fixture {
    PopupItem items[34];
    PopupRowWidget rows[34];
    UiWidget widgets[70];
    UiLayoutNode nodes[70];
    HWND window, previous_window;
    ATOM window_class;
    wchar_t preferences_path[MAX_PATH];
    bool account_owned, preferences_owned;
    u32 unrouted_pointer;
} fixture{};
StoreModel model{};
constexpr wchar_t window_class_name[] = L"MaiCordStoreRoutingRuntimeFixture";

// The client's order for the settings modal: Settings (and through it the store) first, anything left falls through.
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == WM_MOUSELEAVE ||
        message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS) {
        if (application_settings_window_message(message, wparam, lparam))
            return 0;
        if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST)
            ++fixture.unrouted_pointer;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool initialize() noexcept {
    if (g_app.account_ui || g_app.settings || g_app.preferences_writer)
        return false;
    wchar_t temporary[MAX_PATH]{};
    const DWORD chars = GetTempPathW(MAX_PATH, temporary);
    if (!chars || chars >= MAX_PATH || !GetTempFileNameW(temporary, L"MSR", 0, fixture.preferences_path) ||
        !client_preferences_writer_create(&g_app.preferences_writer, fixture.preferences_path))
        return false;
    fixture.preferences_owned = true;
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui)
        return false;
    fixture.account_owned = true;
    g_app.account.status.profile_id = 99;
    g_app.login.visible = false;
    g_app.renderer_ready = true;
    g_app.renderer.dpi = 96;
    g_app.renderer.caption_height = 0;
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
    fixture.window = CreateWindowExW(0, window_class_name, L"Store routing fixture", WS_POPUP, 0, 0, 1000, 700,
                                     nullptr, nullptr, window_class.hInstance, nullptr);
    if (!fixture.window)
        return false;
    g_app.window = fixture.window; // Owned hidden window; no desktop input or ShowWindow.
    SetFocus(fixture.window);
    g_window_state.keyboard_focus = GetFocus() == fixture.window;
    return GetFocus() == fixture.window &&
           popup_scene_initialize(&g_app.popup_scene, {fixture.items, 34}, {fixture.rows, 34}, {fixture.widgets, 70},
                                  {fixture.nodes, 70});
}

void cleanup() noexcept {
    if (fixture.account_owned) {
        application_settings_release();
        popup_scene_release(&g_app.popup_scene);
        g_app.popup_scene = {};
        g_app.popup_owner_kind = PopupOwnerKind::none;
        if (GetCapture() == fixture.window)
            ReleaseCapture();
    }
    if (fixture.preferences_owned) {
        application_preferences_shutdown();
        if (g_app.preferences_writer)
            return; // Retain every borrowed owner on a failed join.
        fixture.preferences_owned = false;
        DeleteFileW(fixture.preferences_path);
    }
    if (fixture.window)
        DestroyWindow(fixture.window);
    fixture.window = nullptr;
    g_window_state.keyboard_focus = false;
    g_app.window = fixture.previous_window;
    if (fixture.window_class)
        UnregisterClassW(window_class_name, GetModuleHandleW(nullptr));
    fixture.window_class = 0;
    if (fixture.account_owned) {
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
        g_app.account_ui = nullptr;
        fixture.account_owned = false;
    }
}

const UiSettingsScene* scene() noexcept {
    return g_app.renderer.settings_scene;
}
const UiStoreScene* store() noexcept {
    return application_store_active() ? g_app.renderer.settings_store : nullptr;
}
LPARAM pack(float x, float y) noexcept {
    return MAKELPARAM(static_cast<short>(x), static_cast<short>(y));
}
// The centre of the visible widget the settings scene resolves to `target` (a sidebar entry, the close button).
bool settings_point(UiSettingsTarget target, float* x, float* y) noexcept {
    const auto* s = scene();
    if (!s)
        return false;
    for (u32 i = 0; i < s->widgets.count; ++i) {
        const auto* node = ui_widget_manager_layout_node(s->widgets, UiWidgetHandle{static_cast<u16>(i), s->widgets.generation});
        if (!node)
            continue;
        const auto visible = ui_rect_intersection(node->rectangle, node->clip_rectangle);
        if (!ui_rect_has_area(visible))
            continue;
        const float cx = (visible.left + visible.right) * .5f, cy = (visible.top + visible.bottom) * .5f;
        const auto hit = ui_settings_scene_hit_test(*s, cx, cy);
        if (hit.kind == target.kind && (target.kind != UiSettingsTargetKind::page || ui_page_key_equal(hit.page, target.page))) {
            *x = cx;
            *y = cy;
            return true;
        }
    }
    return false;
}
// A press and release through the window procedure, as a click arrives.
void click(float x, float y) noexcept {
    SendMessageW(fixture.window, WM_MOUSEMOVE, 0, pack(x, y));
    SendMessageW(fixture.window, WM_LBUTTONDOWN, MK_LBUTTON, pack(x, y));
    SendMessageW(fixture.window, WM_LBUTTONUP, 0, pack(x, y));
    application_settings_synchronize();
}
bool click_target(UiSettingsTarget target) noexcept {
    float x = 0, y = 0;
    if (!settings_point(target, &x, &y))
        return false;
    click(x, y);
    return true;
}
// A click on the visible part of the store control `action` (estimated text can push a header button past the
// panel's edge).
bool click_store(u32 action) noexcept {
    UiRect rect{};
    if (!store() || !ui_store_scene_rect(*store(), action, &rect))
        return false;
    rect = ui_rect_intersection(rect, store()->panel);
    if (!ui_rect_has_area(rect))
        return false;
    click((rect.left + rect.right) * .5f, (rect.top + rect.bottom) * .5f);
    return true;
}
// A press and release on row `index` of the open menu.
bool choose(u32 index) noexcept {
    PopupItemLayout layout{};
    if (!g_app.popup_scene.active || !popup_scene_resolve_item_layout(g_app.popup_scene, index, &layout) ||
        !ui_rect_has_area(layout.visible_rectangle))
        return false;
    const auto& r = layout.visible_rectangle;
    click((r.left + r.right) * .5f, (r.top + r.bottom) * .5f);
    return true;
}
// The sort dropdown shows `label`.
bool sort_label(Utf8View label) noexcept {
    const auto* s = store();
    for (u32 i = 0; s && i < s->count; ++i)
        if (s->nodes[i].action == store_action(StoreAct::sort))
            return utf8_equal(s->nodes[i].text2, label);
    return false;
}
// A notch of the wheel; the message carries screen coordinates.
void wheel(float x, float y, i32 delta) noexcept {
    POINT screen{static_cast<LONG>(x), static_cast<LONG>(y)};
    ClientToScreen(fixture.window, &screen);
    SendMessageW(fixture.window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(delta))),
                 MAKELPARAM(static_cast<short>(screen.x), static_cast<short>(screen.y)));
    application_settings_synchronize();
}
bool key(u32 code) noexcept {
    const bool down = application_settings_input(UiInputKind::key_down, code, false);
    application_settings_input(UiInputKind::key_up, code, false);
    application_settings_synchronize();
    return down;
}
bool selected(UiPageKey page) noexcept {
    return scene() && application_settings_active() && ui_page_key_equal(scene()->selected_page, page);
}
bool open_store(UiPageKey page) noexcept {
    return application_settings_open_page(page) && application_settings_synchronize() && selected(page) &&
           application_store_active() && store() && store()->page_root != kUiStoreNone;
}

// The Effects page's "Performance defaults" card reads the global rows from the model: they are the user's.
bool global_universal_follows_settings() noexcept {
    constexpr EffectUserSettings changed{4, 2, EffectBudget::high, EffectUnfocused::pause};
    const EffectUserSettings saved = application_effects_user_settings();
    const bool set = application_effects_set_user_settings(changed);
    store_model_fill(&model, {});
    const StoreUniversal& u = model.global_universal;
    const bool matches = set && u.resolution == 4 && u.frame_rate == 2 && u.budget == static_cast<u8>(EffectBudget::high) &&
                         u.unfocused == static_cast<u8>(EffectUnfocused::pause);
    application_effects_set_user_settings(saved);
    return matches;
}

// A listed theme's Contrast fact is the WCAG level its swatches' text keeps over their background.
CatalogIndex index{};
bool theme_contrast_is_computed() noexcept {
    constexpr u32 passes_aaa[4]{0x000000, 0x111111, 0xffffff, 0}, passes_aa[4]{0xffffff, 0xf0f0f0, 0x767676, 0x5865f2},
        fails[4]{0xffffff, 0xf0f0f0, 0xaaaaaa, 0x5865f2};
    const auto same = [](Utf8View a, Utf8View b) noexcept { return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes); };
    index.item_count = 1;
    index.items[0].kind = CatalogKind::theme;
    index.items[0].has_swatches = true;
    copy_bytes(index.items[0].swatches, passes_aa, sizeof(passes_aa));
    store_stub_catalog_index(&index);
    store_model_fill(&model, {});
    store_stub_catalog_index(nullptr);
    return model.count >= 1 && model.items[0].kind == CatalogKind::theme && same(model.items[0].contrast, utf8_literal("WCAG AA for text")) &&
           same(store_contrast_text(passes_aaa), utf8_literal("WCAG AAA for text")) &&
           same(store_contrast_text(fails), utf8_literal("Below WCAG AA for text"));
}

// A busy item on a shown store page drives the shared spinner clock the way the frame clock samples it: the next
// spinner step is armed (none under reduced motion); with nothing busy, or with the page hidden, nothing wakes.
bool busy_spinner_wakes(UiPageKey plugins, UiPageKey other) noexcept {
    constexpr char kSlug[] = "busy-demo";
    constexpr u32 bytes = sizeof(kSlug) - 1;
    copy_bytes(index.text, kSlug, bytes);
    index.text_bytes = bytes;
    index.item_count = 1;
    reset_value(&index.items[0]);
    index.items[0].kind = CatalogKind::plugin;
    index.items[0].slug = index.items[0].name = {0, bytes};
    CatalogOperation op{};
    op.action = CatalogAction::install;
    op.phase = CatalogPhase::downloading;
    op.kind = CatalogKind::plugin;
    op.slug_bytes = static_cast<u8>(bytes);
    copy_bytes(op.slug, kSlug, bytes);
    store_stub_catalog_index(&index);
    store_stub_catalog_operation(op);
    constexpr u64 now = 10'000'005;
    const auto wake = [](bool reduced_motion) noexcept {
        UiSpinnerClock clock{};
        ui_spinner_clock_tick(&clock, now, application_store_spinner_scene() != nullptr, reduced_motion);
        return clock.wake_ms;
    };
    // The item's detail: its stretched "Installing..." button spans the column. (A card's footer button is laid out
    // after text this fixture cannot measure, which pushes it past the panel's edge.)
    const bool opened = open_store(plugins) && click_store(store_action(StoreAct::card, 0)) && store() && store()->page_root != kUiStoreNone;
    bool detail = false;
    for (u32 i = 0; opened && i < store()->count; ++i)
        detail = detail || (store()->nodes[i].kind == UiStoreKind::button &&
                            (store()->nodes[i].flags & (ui_store_busy | ui_store_stretch)) == (ui_store_busy | ui_store_stretch));
    const bool busy = detail && application_store_spinner_scene() && application_store_spinner_scene() == store();
    const u64 near = busy ? wake(false) : 0;
    const bool spins = busy && near > now && near <= now + kUiSpinnerStepMs && wake(true) == 0;
    const bool hidden = application_settings_open_page(other) && application_settings_synchronize() &&
                        !application_store_spinner_scene() && wake(false) == 0;
    op.phase = CatalogPhase::done;
    store_stub_catalog_operation(op);
    const bool idle = open_store(plugins) && !application_store_spinner_scene() && wake(false) == 0;
    store_stub_catalog_index(nullptr);
    reset_value(&op);
    store_stub_catalog_operation(op);
    application_settings_close(false);
    return spins && hidden && idle;
}

} // namespace store_routing_runtime

void test_application_store_routing_runtime(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace store_routing_runtime;
    const bool initialized = initialize();
    check(initialized, "store routing: real Settings, store host and window procedure initialize");
    if (!initialized) {
        cleanup();
        return;
    }
    check(global_universal_follows_settings(), "store routing: the Performance defaults rows read the user's effect settings");
    check(theme_contrast_is_computed(), "store routing: a listed theme's Contrast fact is the WCAG level of its text over its background");
    const bool immediate = g_app.scroll_host.immediate;
    g_app.scroll_host.immediate = true;
    constexpr UiPageKey stores[]{{1, static_cast<u64>(SettingsPanel::plugins) + 1},
                                 {1, static_cast<u64>(SettingsPanel::themes) + 1},
                                 {1, static_cast<u64>(SettingsPanel::effects) + 1}};
    const UiSettingsTarget close_target{UiSettingsTargetKind::close, {}, 0};
    const UiSettingsTarget appearance{UiSettingsTargetKind::page, kAppearancePage, 0};
    for (const auto page : stores) {
        // A click in the store column is the store's: it takes the press and its release.
        bool store_click = open_store(page);
        UiRect host{}, clip{};
        if (store_click && ui_settings_scene_host_rect(*scene(), &host, &clip)) {
            const float x = (clip.left + clip.right) * .5f, y = (clip.top + clip.bottom) * .5f;
            const u32 unrouted = fixture.unrouted_pointer;
            SendMessageW(fixture.window, WM_LBUTTONDOWN, MK_LBUTTON, pack(x, y));
            store_click = GetCapture() == fixture.window && scene()->focus.kind == UiSettingsTargetKind::host;
            SendMessageW(fixture.window, WM_LBUTTONUP, 0, pack(x, y));
            store_click = store_click && GetCapture() != fixture.window && fixture.unrouted_pointer == unrouted;
        } else
            store_click = false;
        check(store_click, "store routing: a click in the store column is taken by the store");
        // Then the sidebar and the close button still answer.
        check(click_target(appearance) && selected(kAppearancePage) && !application_store_active(),
              "store routing: a sidebar click on a store page switches the page");
        check(click_target(appearance) && selected(kAppearancePage),
              "store routing: the sidebar keeps working after leaving a store page");
        check(open_store(page) && click_target(close_target) && !application_settings_active() &&
                  !application_store_active(),
              "store routing: the close button closes Settings from a store page");
        check(open_store(page) && key(VK_ESCAPE) && !application_settings_active(),
              "store routing: Escape closes Settings from a store page");
        // The sort menu: a pointer choice picks its row; a click outside dismisses it and the next click acts.
        check(open_store(page) && click_store(store_action(StoreAct::sort)) && g_app.popup_scene.active &&
                  choose(1) && !g_app.popup_scene.active && sort_label(utf8_literal("Newest")),
              "store routing: a pointer choice in the sort menu sorts by it");
        check(click_store(store_action(StoreAct::sort)) && g_app.popup_scene.active,
              "store routing: the sort menu opens again");
        check(click_target(close_target) && !g_app.popup_scene.active && application_settings_active() &&
                  click_target(close_target) && !application_settings_active(),
              "store routing: a click outside the sort menu dismisses it and the close button then works");
        // The install-from-link dialog (themes and effects) is modal until Escape closes it.
        if (page.page_id == stores[0].page_id)
            continue;
        check(open_store(page) && click_store(store_action(StoreAct::header_link)) && store()->dialog_open &&
                  !store()->dialog_bar,
              page.page_id == stores[1].page_id ? "store routing: the install-from-link dialog opens (Themes)"
                                                : "store routing: the install-from-link dialog opens (Effects)");
        check(click_target(appearance) && selected(page) && store() && store()->dialog_open,
              "store routing: the open dialog keeps the page under it");
        // The link field has the focus: the first Escape leaves it, the second closes the dialog.
        check(key(VK_ESCAPE) && store() && store()->dialog_open && key(VK_ESCAPE) && store() && !store()->dialog_open &&
                  application_settings_active(),
              "store routing: Escape leaves the link field, then closes the dialog and keeps Settings");
        check(click_target(appearance) && selected(kAppearancePage) && click_target(close_target) &&
                  !application_settings_active(),
              "store routing: after the dialog the sidebar and the close button work");
    }
    check(busy_spinner_wakes(stores[0], kAppearancePage),
          "store routing: a busy item on the shown store page arms the next spinner step; none or a hidden page does not");
    // The wheel scrolls the category list over its rows and over its scrollbar, which sits in the sidebar's padding.
    auto& observed = settings_runtime_test::observed;
    const UiRect viewport = observed.viewport;
    observed.viewport = {0, 0, 1000, 260};
    bool sidebar_wheel = open_store(stores[0]);
    // Selecting a store page reveals its entry; start from the top of the list.
    auto* settings_scene = const_cast<UiSettingsScene*>(scene());
    if (sidebar_wheel)
        ui_settings_scene_scroll(settings_scene, true, -settings_scene->sidebar_offset);
    const auto bar = sidebar_wheel ? ui_settings_scene_scrollbar(*scene(), true) : UiScrollbarGeometry{};
    sidebar_wheel = sidebar_wheel && bar.visible && scene()->sidebar_offset == 0;
    if (sidebar_wheel) {
        const float x = (bar.track.left + bar.track.right) * .5f, y = (bar.track.top + bar.track.bottom) * .5f;
        wheel(x, y, -WHEEL_DELTA);
        const float over_bar = scene()->sidebar_offset;
        wheel(bar.track.left - 40, y, -WHEEL_DELTA);
        sidebar_wheel = over_bar > 0 && scene()->sidebar_offset > over_bar && fixture.unrouted_pointer == 0;
    }
    check(sidebar_wheel, "store routing: the wheel scrolls the sidebar over its rows and over its scrollbar");
    observed.viewport = viewport;
    application_settings_close(false);
    g_app.scroll_host.immediate = immediate;
    cleanup();
}

} // namespace maicord
