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

#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"
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

// ---- Knob page previews --------------------------------------------------------------------------------------
// A minimal installed package: a manifest, a ps_5_0 DXBC header and its source, as the packager lays them out.
u8 package_bytes[640];
void put32(u8* p, u32 v) noexcept {
    for (u32 i = 0; i < 4; ++i)
        p[i] = static_cast<u8>(v >> (8 * i));
}
bool install_effect(const char* slug, const char* uses = "[]", const char* interface = "1") noexcept {
    char manifest[320];
    u32 m = 0;
    const auto add = [&](const char* text) noexcept {
        const u32 n = c_string_length(text);
        copy_bytes(manifest + m, text, n);
        m += n;
    };
    add("{\"interface\":");
    add(interface);
    add(",\"slug\":\"");
    add(slug);
    add("\",\"name\":\"");
    add(slug);
    add("\",\"version\":\"1.0.0\",\"authors\":[{\"name\":\"Maj\"}],\"license\":\"MPL-2.0\",\"blend\":\"normal\",\"uses\":");
    add(uses);
    add(",\"knobs\":[{\"id\":\"speed\",\"type\":\"float\",\"label\":\"Speed\",\"min\":0,\"max\":3,\"default\":1}]}");
    u8 dxbc[48]{};
    copy_bytes(dxbc, "DXBC", 4);
    put32(dxbc + 20, 1);
    put32(dxbc + 24, sizeof(dxbc));
    put32(dxbc + 28, 1);
    put32(dxbc + 32, 36);
    copy_bytes(dxbc + 36, "SHEX", 4);
    put32(dxbc + 40, 4);
    put32(dxbc + 44, 0x50);
    static constexpr char kSource[] = "float4 background(float2 p){return 0;}";
    const struct {
        u32 type;
        const void* data;
        u32 bytes;
    } sections[]{{1, manifest, m}, {2, dxbc, sizeof(dxbc)}, {5, kSource, sizeof(kSource) - 1}};
    clear_bytes(package_bytes, sizeof(package_bytes));
    copy_bytes(package_bytes, "MCFX", 4);
    package_bytes[4] = 1;
    package_bytes[6] = 3;
    u32 at = 16 + 3 * 16;
    for (u32 i = 0; i < 3; ++i) {
        put32(package_bytes + 16 + i * 16, sections[i].type);
        put32(package_bytes + 16 + i * 16 + 4, at);
        put32(package_bytes + 16 + i * 16 + 8, sections[i].bytes);
        copy_bytes(package_bytes + at, sections[i].data, sections[i].bytes);
        at += sections[i].bytes;
    }
    put32(package_bytes + 8, at);
    wchar_t relative[64] = L"effects\\", path[512];
    u32 n = 8;
    for (u32 i = 0; slug[i]; ++i)
        relative[n++] = static_cast<wchar_t>(slug[i]);
    for (const wchar_t c : L"-0123456789abcdef.mcfx")
        relative[n++] = c;
    if (!data_path(path, 504, relative, true).ok())
        return false;
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, package_bytes, at, &written, nullptr) && written == at;
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    return ok;
}
u32 effect_index(const char* slug) noexcept {
    for (u32 i = 0; i < application_effects_count(); ++i)
        if (const EffectInfo* info = application_effects_info(i);
            info && c_string_length(info->slug) == c_string_length(slug) && bytes_equal(info->slug, slug, c_string_length(slug)))
            return i;
    return ~0u;
}
// One frame of the client's order: the effect selection, then Settings (and through it the store).
bool frame() noexcept {
    application_effects_synchronize();
    return application_settings_synchronize();
}
bool live_tile() noexcept {
    for (u32 i = 0; store() && i < store()->count; ++i)
        if (store()->nodes[i].kind == UiStoreKind::preview && store()->nodes[i].variant == static_cast<u8>(UiStoreAspect::live))
            return true;
    return false;
}
bool open_knobs(const char* slug) noexcept {
    // From another page, as the theme editor's Knobs button arrives.
    return application_settings_open_page(kAppearancePage) && frame() &&
           application_store_open_effect_knobs({slug, c_string_length(slug)}) && frame() && frame() && store();
}

// Every installed effect's knob page shows that effect live: the saved background draws as it is, another effect
// draws in its place while the page shows, and the plate turns live once its effect is the one drawing. The
// preview's clock runs under Settings only while the tile is on screen; leaving the page brings the background back.
bool knob_pages_preview_live() noexcept {
    const ApplicationWindowState window = g_window_state;
    g_window_state.visible = g_window_state.foreground = true;
    g_window_state.minimized = false;
    auto& observed = settings_runtime_test::observed;
    const UiRect viewport = observed.viewport;
    observed.viewport = {0, 0, 1000, 480};
    application_effects_reload();
    const u32 before = application_effects_count();
    const bool installed = install_effect("alpha") && install_effect("beta");
    application_effects_reload();
    const u32 alpha = effect_index("alpha"), beta = effect_index("beta");
    const u64 alpha_id = application_effects_identity(alpha), beta_id = application_effects_identity(beta);
    const bool listed = installed && application_effects_count() == before + 2 && alpha_id && beta_id && alpha_id != beta_id;
    const bool background = listed && application_effects_select(alpha) && application_settings_open() && frame() &&
                            g_app.renderer.effect.identity == alpha_id;
    // Another effect's page: it draws in the background's place and its plate is live, its clock running.
    const bool other = background && open_knobs("beta") && g_app.renderer.effect.identity == beta_id && live_tile() &&
                       application_effects_preview_state(beta_id) == EffectTrialState::drawing &&
                       application_effects_preview_running();
    // Scrolled out of view the preview keeps its picture and its clock stops.
    const bool scrolled = other && ui_store_scene_scroll_to(const_cast<UiStoreScene*>(store()), store()->content_height) && frame() &&
                          !application_effects_preview_running() && g_app.renderer.effect.identity == beta_id;
    // The background's own page: it draws as it is.
    const bool own = scrolled && open_knobs("alpha") && g_app.renderer.effect.identity == alpha_id && live_tile() &&
                     application_effects_preview_running();
    // Leaving the page gives the window its background back; nothing runs the clock under Settings.
    const bool left = own && open_knobs("beta") && g_app.renderer.effect.identity == beta_id &&
                      application_settings_open_page(kAppearancePage) && frame() && frame() &&
                      g_app.renderer.effect.identity == alpha_id && !application_effects_preview_running();
    application_settings_close(false);
    application_effects_select_none();
    application_effects_remove("alpha", 5);
    application_effects_remove("beta", 4);
    application_effects_synchronize();
    observed.viewport = viewport;
    g_window_state = window;
    return left && application_effects_count() == before && !g_app.renderer.effect.selected;
}

// A selected package's ambient inputs (interface 2's clock and appearance): they reach the renderer as the host eased
// them; Windows turning dark under a light theme starts a transition the background clock wakes for; reduced
// motion settles it at once; and an interface-1 package, without them, never asks for a transition frame.
bool ambient_inputs_follow_the_system() noexcept {
    const ApplicationWindowState window = g_window_state;
    const ClientTheme theme = g_app.preferences.theme;
    const bool cached = g_app.surface_motion.system_preference_cached, animation = g_app.surface_motion.client_area_animation;
    const bool reduced = g_app.preferences.reduced_motion, renderer_reduced = g_app.renderer.reduced_motion;
    g_window_state.visible = g_window_state.foreground = true;
    g_window_state.minimized = false;
    g_app.surface_motion.system_preference_cached = g_app.surface_motion.client_area_animation = true;
    g_app.preferences.reduced_motion = g_app.renderer.reduced_motion = false;
    g_app.preferences.theme = ClientTheme::light;
    application_settings_close(false);
    application_effects_reload();
    const u32 before = application_effects_count();
    const bool installed = install_effect("gamma", "[\"clock\",\"appearance\"]", "2") && install_effect("delta");
    // The same inputs under interface 1 are refused, so an older client never meets them half-supported.
    const bool refused = install_effect("epsilon", "[\"clock\"]", "1");
    application_effects_reload();
    const u32 gamma = effect_index("gamma"), delta = effect_index("delta");
    const bool listed = installed && refused && application_effects_count() == before + 2 && effect_index("epsilon") == ~0u &&
                        application_theme().appearance == ThemeAppearance::light;
    const auto& ambient = g_app.renderer.effect.ambient;
    // Light theme, light Windows: the inputs settle on the local clock and on light, and nothing eases.
    application_effects_set_system_dark(false);
    SYSTEMTIME first{}, second{};
    GetLocalTime(&first);
    const bool selected = listed && application_effects_select(gamma) && frame() &&
                          g_app.renderer.effect.identity == application_effects_identity(gamma);
    GetLocalTime(&second);
    const auto at = [&](const SYSTEMTIME& t) noexcept {
        const float day = (t.wHour * 60.f + t.wMinute) / 1440.f, delta = ambient[effect_ambient_day] - day;
        return delta < 1e-5f && delta > -1e-5f && ambient[effect_ambient_month] == t.wMonth;
    };
    const bool clock = selected && (at(first) || at(second)) && ambient[effect_ambient_year] >= 0 &&
                       ambient[effect_ambient_year] < 1 && ambient[effect_ambient_dark] == 0 &&
                       !application_effects_transition_wake();
    // Windows turns dark (WM_SETTINGCHANGE "ImmersiveColorSet"): dark wins, and the appearance starts gliding.
    g_app.frame_pending = false;
    application_effects_set_system_dark(true);
    const bool requested = g_app.frame_pending;
    const bool transition = clock && requested && frame() && ambient[effect_ambient_dark] < 1 &&
                            application_effects_transition_wake() != 0;
    // Reduced motion: the change is instant and no frame is asked for.
    g_app.renderer.reduced_motion = true;
    const bool instant = transition && frame() && ambient[effect_ambient_dark] == 1 && !application_effects_transition_wake();
    g_app.renderer.reduced_motion = false;
    // An interface-1 package: no ambient input, no transition, whatever Windows does.
    application_effects_set_system_dark(false);
    const bool old = instant && application_effects_select(delta) && frame() && !application_effects_transition_wake();
    application_effects_set_system_dark(application_effects_read_system_dark());
    application_effects_select_none();
    application_effects_remove("gamma", 5);
    application_effects_remove("delta", 5);
    wchar_t relative[] = L"effects\\epsilon-0123456789abcdef.mcfx", path[512];
    if (data_path(path, 504, relative, false).ok())
        DeleteFileW(path);
    application_effects_synchronize();
    g_app.preferences.theme = theme;
    g_app.preferences.reduced_motion = reduced;
    g_app.renderer.reduced_motion = renderer_reduced;
    g_app.surface_motion.system_preference_cached = cached;
    g_app.surface_motion.client_area_animation = animation;
    g_window_state = window;
    return old && application_effects_count() == before && !g_app.renderer.effect.selected;
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
    check(knob_pages_preview_live(),
          "store routing: every installed effect's knob page draws that effect live in its preview, the clock running only "
          "while the tile is on screen, and leaving the page restores the background");
    check(ambient_inputs_follow_the_system(),
          "effect inputs: an interface-2 package gets the local clock and the appearance; Windows turning dark glides "
          "it with transition frames, reduced motion makes it instant, and an interface-1 package gets none");
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
