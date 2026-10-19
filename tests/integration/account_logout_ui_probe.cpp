#include "account_logout_ui_probe.hpp"
#include "probe_present.hpp"
#include "../../tools/perf/probe_window.hpp"
#include "probe_glyph_ink.hpp"
#include "probe_capture.hpp"
#include "application_state.hpp"
#include "application_account_logout.hpp"
#include "application_account_module.hpp"
#include "application_account_menu.hpp"
#include "application_account_switch.hpp"
#include "application_text_form_state.hpp"
#include "application_login_ui.hpp"
#include "application_login_input.hpp"
#include "application_login_geometry.hpp"
#include "application_shell_input.hpp"
#include "application_content_input.hpp"
#include "application_message_forward.hpp"
#include "application_thread_browser.hpp"
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_forum_form.hpp"
#include "application_profile.hpp"
#include "application_self_status.hpp"
#include "application_friends.hpp"
#include "application_settings.hpp"
#include "application_downloads.hpp"
#include "application_tooltip.hpp"
#include "application_scrolling.hpp"
#include "application_message_interaction.hpp"
#include "application_plugins.hpp"
#include "maicord/app/account_credential_forget.hpp"
#include "maicord/app/account_logout_stop.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/account_menu.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"
#include "maicord/render/renderer_presentation.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord::app_detail {
constinit Application g_app{};
}
namespace maicord::logout_native {
using namespace app_detail;
struct Fixture {
    Theme theme;
    AccountPaths paths;
    wchar_t root[kAccountProfileMaximumPathCharacters];
    wchar_t marker[kAccountProfileMaximumPathCharacters];
    wchar_t profile_new[kAccountProfileMaximumPathCharacters];
    wchar_t session_new[kAccountProfileMaximumPathCharacters];
    TextHitSpan hit_spans[kTextHitSpanMaximum];
    TextHitRegion hit_scratch[kRendererContentHitRegionCapacity];
    TextDecoration decorations[kRendererTextDecorationCapacity];
    UiWidget popup_widgets[64];
    UiLayoutNode popup_nodes[64];
    PopupItem popup_items[24];
    PopupRowWidget popup_rows[24];
    GuildRailRowWidget rail_rows[2];
    UiWidget rail_widgets[4];
    UiLayoutNode rail_nodes[4];
    ShellNavigationRowWidget navigation_rows[2];
    UiWidget navigation_widgets[10];
    UiLayoutNode navigation_nodes[10];
    u16 channel_scratch[1];
    u8 visibility[1];
    ChannelSortKey channel_keys[1];
    MessageSceneRowWidget message_rows[1];
    UiWidget message_widgets[kMessageSceneWidgetsPerRow + 1];
    UiLayoutNode message_nodes[kMessageSceneWidgetsPerRow + 1];
    HANDLE stop_entered, stop_allowed, blocked_file;
    u64 now, accepted_id, cancelled_id;
    AccountLogoutResponse receipt;
    u32 begins, posts, takes, cancels, prepares, publishes, deletes, frames, failures, captures;
    u32 unexpected, stop_attempts, stop_observations, account_ui_releases, login_inputs, total_posts;
    UiRect old_accept, old_cancel;
    volatile LONG stop_active;
    DWORD stop_thread;
    bool receipt_ready, fail_stop, refresh_ok, unconfirmed, file_order_error, host_order_error;
};
Fixture* fixture;
bool require(bool valid, u32 stage) noexcept {
    if (!valid) {
        ++fixture->failures;
        log_u64(LogLevel::error, "probe", "account_logout_native_failure_stage", stage);
    }
    return valid;
}
bool append(wchar_t* target, const wchar_t* suffix) noexcept {
    u32 at{}, i{};
    while (target[at])
        ++at;
    while (suffix[i]) {
        if (at + 1 >= kAccountProfileMaximumPathCharacters)
            return false;
        target[at++] = suffix[i++];
    }
    target[at] = 0;
    return true;
}
bool copy(wchar_t* target, const wchar_t* value) noexcept {
    target[0] = 0;
    return append(target, value);
}
bool write(const wchar_t* path) noexcept {
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    constexpr char contents[] = "Original offline logout UI fixture; no credential.";
    DWORD count{};
    const bool valid =
        WriteFile(file, contents, sizeof(contents), &count, nullptr) && count == sizeof(contents);
    return CloseHandle(file) && valid;
}
bool initialize_paths() noexcept {
    auto& f = *fixture;
    const DWORD count = GetTempPathW(kAccountProfileMaximumPathCharacters, f.root);
    if (!count || count >= kAccountProfileMaximumPathCharacters)
        return false;
    wchar_t suffix[64] = L"account-logout-ui-fixture-";
    u32 at{};
    while (suffix[at])
        ++at;
    const DWORD pid = GetCurrentProcessId();
    wchar_t digits[10]{};
    u32 used{}, value = pid;
    do {
        digits[used++] = static_cast<wchar_t>(L'0' + value % 10);
        value /= 10;
    } while (value);
    while (used)
        suffix[at++] = digits[--used];
    suffix[at++] = L'-';
    wchar_t ticks[20]{};
    u64 tick = GetTickCount64();
    do {
        ticks[used++] = static_cast<wchar_t>(L'0' + tick % 10);
        tick /= 10;
    } while (tick);
    while (used)
        suffix[at++] = ticks[--used];
    if (!append(f.root, suffix) || !CreateDirectoryW(f.root, nullptr) ||
        !account_paths_build_slot(f.root, 0, &f.paths).ok() || !account_paths_ensure_directories(f.paths).ok())
        return false;
    return copy(f.marker, f.paths.account_directory) && append(f.marker, L"\\active.logout") &&
           copy(f.profile_new, f.paths.profile) && append(f.profile_new, L".new") &&
           copy(f.session_new, f.paths.session) && append(f.session_new, L".new");
}
bool absent(const wchar_t* path) noexcept {
    const DWORD attributes = GetFileAttributesW(path);
    return attributes == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND;
}
bool credentials_absent() noexcept {
    const auto& f = *fixture;
    return absent(f.paths.profile) && absent(f.paths.session) && absent(f.profile_new) &&
           absent(f.session_new) && !account_credential_forget_probe(f.paths).pending;
}
bool project() noexcept;
bool account_storage() noexcept;
void release_account_storage() noexcept;
} // namespace maicord::logout_native

// Only these controller boundaries are renamed. Real credential helper calls
// still perform their complete durable marker/deletion policy on fixture files.
namespace maicord {
u64 logout_native_now() noexcept {
    return logout_native::fixture->now;
}
u64 logout_native_unix() noexcept {
    return 1'759'667'400'000ull + logout_native::fixture->now;
}
AccountPathsResult logout_native_paths(AccountPaths* output) noexcept {
    if (!output)
        return {ERROR_INVALID_PARAMETER, AccountPathsError::invalid_input};
    *output = logout_native::fixture->paths;
    return {};
}
AccountCredentialForgetResult logout_native_begin(const AccountPaths& paths,
                                                  const AccountCredentialForgetIo* io = nullptr) noexcept {
    return account_credential_forget_begin(paths, io);
}
AccountCredentialForgetResult logout_native_forget(const AccountPaths& paths, bool joined,
                                                   const AccountCredentialForgetIo* io = nullptr) noexcept {
    auto& f = *logout_native::fixture;
    ++f.deletes;
    if (!joined || app_detail::application_account_logout_host_owned() || ReadAcquire(&f.stop_active))
        f.file_order_error = true;
    return account_credential_forget_complete_after_join(paths, joined, io);
}
AccountHostStopResult logout_native_stop(AccountHost* host, u32 timeout, u64 monotonic,
                                         u64 unix_time) noexcept {
    auto& f = *logout_native::fixture;
    InterlockedExchange(&f.stop_active, 1);
    f.stop_thread = GetCurrentThreadId();
    ++f.stop_attempts;
    if (host == &app_detail::g_app.account || timeout != app_detail::kAccountStopTimeoutMs || !monotonic ||
        !unix_time)
        f.host_order_error = true;
    SetEvent(f.stop_entered);
    AccountHostStopResult result{};
    result.account_status.profile_id = 42;
    if (WaitForSingleObject(f.stop_allowed, 15'000) != WAIT_OBJECT_0 || f.fail_stop) {
        host->stop_failed = true;
        result.error = AccountHostError::account_stop;
        result.native_error = WAIT_TIMEOUT;
    } else {
        *host = {};
    }
    InterlockedExchange(&f.stop_active, 0);
    return result;
}
} // namespace maicord
#define clock_monotonic_milliseconds logout_native_now
#define clock_unix_milliseconds logout_native_unix
#define account_paths_resolve logout_native_paths
#define account_credential_forget_begin logout_native_begin
#define account_credential_forget_complete_after_join logout_native_forget
#define account_host_stop logout_native_stop
#include "../../src/app/application_account_logout.cpp"
#undef clock_monotonic_milliseconds
#undef clock_unix_milliseconds
#undef account_paths_resolve
#undef account_credential_forget_begin
#undef account_credential_forget_complete_after_join
#undef account_host_stop

namespace maicord::logout_native {
using namespace app_detail;
AccountLogoutResult WINAPI server(void* instance, AccountLogoutOperation operation, u64 id,
                                  AccountLogoutResponse* response) noexcept {
    auto& f = *fixture;
    if (ReadAcquire(&f.stop_active) || instance != fixture || !id)
        f.host_order_error = true;
    switch (operation) {
    case AccountLogoutOperation::begin:
        ++f.begins;
        if (f.accepted_id) {
            ++f.unexpected;
            return {RestClientSubmitError::busy};
        }
        if (!account_credential_forget_probe(f.paths).pending || f.prepares != 1)
            f.file_order_error = true;
        f.accepted_id = id;
        ++f.posts;
        ++f.total_posts;
        return {};
    case AccountLogoutOperation::cancel:
        ++f.cancels;
        f.cancelled_id = id;
        return {RestClientSubmitError::none, RestClientTakeError::empty, true, false};
    case AccountLogoutOperation::take:
        ++f.takes;
        if (!f.receipt_ready)
            return {RestClientSubmitError::none, RestClientTakeError::empty, false, false};
        if (!response || f.accepted_id != id) {
            ++f.unexpected;
            return {RestClientSubmitError::invalid_input, RestClientTakeError::empty};
        }
        *response = f.receipt;
        response->request_id = id;
        f.receipt_ready = false;
        return {RestClientSubmitError::none, RestClientTakeError::none, false, true};
    default:
        ++f.unexpected;
        return {RestClientSubmitError::invalid_input};
    }
}
bool project() noexcept {
    auto& r = g_app.renderer;
    if (g_app.login.visible) {
        if (!application_synchronize_login_view())
            return false;
    } else {
        auto metrics = kFriendsShellLayoutMetrics;
        if (!ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::friends, metrics,
                                          r.width * 96.f / r.dpi, r.height * 96.f / r.dpi))
            return false;
    }
    if (!application_account_menu_synchronize() || !application_text_form_synchronize())
        return false;
    renderer_bind_focus_owner(&r, g_app.keyboard_focus_visible ? g_app.context_focus : nullptr);
    return g_app.login.visible && !g_app.text_form ? true
                                                   : renderer_rebuild_content_text(&r, fixture->theme);
}
bool account_storage() noexcept {
    if (g_app.account_ui)
        return true;
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui)
        return false;
    auto& ui = *g_app.account_ui;
    if (!chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 1},
                                        {ui.guild_order_storage, 1}, {ui.channel_record_storage, 1},
                                        {ui.channel_order_storage, 1}, {ui.chat_state_name_storage, 256}) ||
        !user_store_initialize(&ui.user_store, {ui.user_record_storage, 2},
                                       {ui.user_order_storage, 2}, {ui.user_name_storage, 512}) ||
        !user_store_upsert(&ui.user_store, 99, utf8_literal("Alex"), utf8_literal("alex"))
             .ok() ||
        !navigation_view_initialize(&ui.navigation_view, {ui.navigation_guild_rows, 1},
                                            {ui.navigation_channel_rows, 1}, {fixture->channel_scratch, 1},
                                            {fixture->visibility, 1}, {fixture->channel_keys, 1}) ||
        !ui_text_measure_cache_initialize(&ui.text_measure_cache,
                                          {ui.message_text_measure_cache_records, 128},
                                          renderer_text_measure_backend, &g_app.renderer))
        return false;
    // The native shell renderer requires the actual Core timeline even when
    // this account-menu fixture has no visible history. Keep its valid empty,
    // inactive projection; no message controller/history transport is injected.
    if (!message_view_runtime_initialize(&g_app.message_view, ui_extent_units(80), 1, false).ok() ||
        !message_scene_initialize(&g_app.message_scene, &g_app.message_view.timeline,
                                  {fixture->message_rows, 1},
                                  {fixture->message_widgets, kMessageSceneWidgetsPerRow + 1},
                                  {fixture->message_nodes, kMessageSceneWidgetsPerRow + 1}))
        return false;
    ui.chat_state.self_user_id = 99;
    auto& text = ui.renderer_text;
    text.quads = ui.renderer_text_quads;
    text.batches = ui.renderer_text_batches;
    text.utf16 = ui.response_shape_scratch.shape.utf16;
    text.glyphs = ui.response_shape_scratch.shape.glyphs;
    text.runs = ui.response_shape_scratch.shape.runs;
    text.clusters = ui.response_shape_scratch.shape.clusters;
    text.hit_spans = fixture->hit_spans;
    text.hit_region_scratch = fixture->hit_scratch;
    text.decorations = fixture->decorations;
    text.content_semantics = ui.renderer_content_semantics;
    text.content_hit_regions = ui.renderer_content_hit_regions;
    text.quad_capacity = kRendererContentGlyphCapacity;
    text.batch_capacity = kRendererContentBatchCapacity;
    text.utf16_capacity = text.glyph_capacity = text.cluster_capacity = kRendererContentTextUnitCapacity;
    text.run_capacity = kRendererContentRunCapacity;
    text.hit_span_capacity = kTextHitSpanMaximum;
    text.hit_region_scratch_capacity = text.content_hit_region_capacity = kRendererContentHitRegionCapacity;
    text.decoration_capacity = kRendererTextDecorationCapacity;
    text.content_semantic_capacity = kRendererContentSemanticCapacity;
    renderer_bind_navigation_view(&g_app.renderer, &ui.chat_state, &ui.user_store, &ui.navigation_view);
    g_app.renderer.message_measure_cache = &ui.text_measure_cache;
    return ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::friends,
                                        kFriendsShellLayoutMetrics,
                                        g_app.renderer.width * 96.f / g_app.renderer.dpi,
                                        g_app.renderer.height * 96.f / g_app.renderer.dpi) &&
           renderer_bind_ui_scene(&g_app.renderer, &g_app.shell_scene) &&
           renderer_bind_guild_rail_scene(&g_app.renderer, &g_app.guild_rail_scene) &&
           renderer_bind_shell_navigation_scene(&g_app.renderer, &g_app.shell_navigation_scene) &&
           renderer_bind_message_scene(&g_app.renderer, &g_app.message_scene) &&
           renderer_bind_text_storage(&g_app.renderer, &text);
}
void release_account_storage() noexcept {
    if (!g_app.account_ui)
        return;
    if (g_app.text_form || application_account_logout_host_owned()) {
        fixture->host_order_error = true;
        return;
    }
    application_account_menu_release();
    renderer_unbind_text_storage(&g_app.renderer);
    renderer_bind_navigation_view(&g_app.renderer, nullptr, nullptr, nullptr);
    g_app.renderer.message_scene = nullptr;
    message_scene_release(&g_app.message_scene);
    message_view_runtime_release(&g_app.message_view);
    g_app.renderer.ui_scene = nullptr;
    g_app.renderer.message_measure_cache = nullptr;
    g_app.context_focus = nullptr;
    renderer_bind_focus_owner(&g_app.renderer, nullptr);
    SecureZeroMemory(g_app.account_ui, sizeof(AccountUiRuntime));
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    ++fixture->account_ui_releases;
}
bool seed() noexcept {
    auto& f = *fixture;
    if (!require(!application_account_logout_host_owned() && !g_app.text_form, 110))
        return false;
    application_account_logout_release();
    if (!require(!g_app.account_logout, 111))
        return false;
    if (f.blocked_file) {
        CloseHandle(f.blocked_file);
        f.blocked_file = nullptr;
    }
    f.begins = f.posts = f.takes = f.cancels = f.prepares = f.publishes = f.deletes = 0;
    f.accepted_id = f.cancelled_id = 0;
    f.receipt = {};
    f.receipt_ready = f.fail_stop = f.unconfirmed = false;
    ResetEvent(f.stop_entered);
    ResetEvent(f.stop_allowed);
    f.now = 1000;
    if (!account_storage())
        return false;
    g_app.account = {};
    g_app.account.module = GetModuleHandleW(nullptr);
    g_app.account.instance = fixture;
    g_app.account.api.logout = server;
    g_app.account.status.profile_id = 42;
    g_app.account.status.running = true;
    ++g_app.plugin_account_scope_serial;
    g_app.login.visible = false;
    if (!application_synchronize_login_view())
        return false;
    g_app.text_form_held_key = 0;
    g_app.keyboard_focus_visible = true;
    return write(f.paths.profile) && write(f.paths.session) && write(f.profile_new) &&
           write(f.session_new) && project();
}
bool resize(u32 width, u32 height, u32 dpi, const Theme& theme) noexcept {
    fixture->theme = theme;
    renderer_set_dpi(&g_app.renderer, dpi);
    return require(renderer_resize(&g_app.renderer, width, height) && project(), 112);
}
struct Keyboard {
    BYTE prior[256];
    bool valid;
    Keyboard() noexcept : valid(GetKeyboardState(prior) != FALSE) {
        BYTE keys[256]{};
        valid = valid && SetKeyboardState(keys) != FALSE;
    }
    ~Keyboard() noexcept {
        if (valid)
            SetKeyboardState(prior);
    }
};
bool key(u32 code, bool repeated = false) noexcept {
    Keyboard keyboard;
    return keyboard.valid && application_text_form_input(UiInputKind::key_down, code, repeated);
}
bool up(u32 code) noexcept {
    return application_text_form_input(UiInputKind::key_up, code, false);
}
LPARAM center(UiRect rectangle) noexcept {
    const float scale = g_app.renderer.dpi / 96.f;
    return MAKELPARAM(static_cast<u16>((rectangle.left + rectangle.right) * .5f * scale),
                      static_cast<u16>((rectangle.top + rectangle.bottom) * .5f * scale));
}
bool click(UiRect rectangle) noexcept {
    const auto point = center(rectangle);
    LRESULT result{};
    return application_text_form_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point, &result) &&
           application_text_form_window_message(WM_LBUTTONUP, 0, point, &result);
}
bool pointer(TextFormElement element) noexcept {
    if (!g_app.text_form)
        return false;
    const auto* node = text_form_node(g_app.text_form->scene, element);
    if (!node)
        return false;
    const auto point = center(node->rectangle);
    LRESULT result{};
    return application_text_form_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point, &result) &&
           application_text_form_window_message(WM_LBUTTONUP, 0, point, &result);
}
bool menu(bool keyboard) noexcept {
    const auto* footer =
        ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::channel_footer));
    if (!footer)
        return false;
    if (keyboard) {
        Keyboard state;
        const auto widget = footer->widget;
        if (!state.valid || !ui_widget_manager_focus(&g_app.shell_scene.widgets, widget))
            return false;
        g_app.context_focus = &g_app.shell_scene.widgets;
        if (!application_account_menu_key(VK_RETURN))
            return false;
        if (!ui_input_consume_key_tail(&g_app.composer_menu_held_key,
                                       {UiInputKind::key_up, VK_RETURN, false}))
            return false;
    } else {
        auto avatar = footer->rectangle;
        avatar.left += 10;
        avatar.right = avatar.left + 32;
        avatar.top = (footer->rectangle.top + footer->rectangle.bottom - 32) * .5f;
        avatar.bottom = avatar.top + 32;
        if (!handle_shell_click(center(avatar)))
            return false;
    }
    return g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::account_menu;
}
bool launch_from_menu() noexcept {
    const auto action = static_cast<u32>(UiAccountMenuAction::log_out);
    for (u32 i = 0; i < g_app.popup_scene.item_count; ++i) {
        if (g_app.popup_scene.items[i].action != action)
            continue;
        UiMenuRowGeometry row{};
        if (!popup_scene_menu_row_geometry(g_app.popup_scene, i, &row))
            return false;
        const auto point = center(row.primary);
        const auto hit = popup_scene_hit_test(
            g_app.popup_scene, static_cast<short>(LOWORD(point)) * 96.f / g_app.renderer.dpi,
            static_cast<short>(HIWORD(point)) * 96.f / g_app.renderer.dpi);
        return hit.enabled && hit.action == action && application_account_menu_action(hit.action) &&
               g_app.text_form && text_form_focused(g_app.text_form->scene) == TextFormElement::cancel;
    }
    return false;
}
bool commit() noexcept {
    if (!g_app.text_form)
        return false;
    fixture->old_accept = text_form_node(g_app.text_form->scene, TextFormElement::accept)->rectangle;
    fixture->old_cancel = text_form_node(g_app.text_form->scene, TextFormElement::cancel)->rectangle;
    if (!pointer(TextFormElement::accept) || !application_text_form_synchronize())
        return false;
    application_account_logout_pump();
    return application_text_form_synchronize() && application_account_logout_blocked() &&
           fixture->posts == 1 && !g_app.text_form->can_cancel && !g_app.text_form->dismissible;
}
bool locked() noexcept {
    auto* form = g_app.text_form;
    if (!form)
        return false;
    const auto posts = fixture->posts;
    LRESULT result{};
    if (!key(VK_ESCAPE) || !up(VK_ESCAPE) || !key(VK_RETURN) || !key(VK_RETURN, true) || !up(VK_RETURN) ||
        !key(VK_SPACE) || !up(VK_SPACE) || !click(fixture->old_cancel) || !click(fixture->old_accept) ||
        !application_text_form_window_message(WM_LBUTTONDOWN, 0, MAKELPARAM(1, 1), &result) ||
        !application_text_form_window_message(WM_LBUTTONUP, 0, MAKELPARAM(1, 1), &result) ||
        !application_text_form_window_message(WM_KILLFOCUS, 0, 0, &result) ||
        !application_text_form_window_message(WM_CANCELMODE, 0, 0, &result))
        return false;
    application_text_form_window_message(WM_SETFOCUS, 0, 0, &result);
    return g_app.text_form == form && fixture->posts == posts && !form->can_cancel && !form->dismissible &&
           !form->can_accept && !form->pointer_button.generation && !form->keyboard.pressed.manager &&
           !form->store.implementation && !form->edit.storage.data;
}
bool wait_stop() noexcept {
    return WaitForSingleObject(fixture->stop_entered, 3000) == WAIT_OBJECT_0 &&
           application_account_logout_host_owned() && fixture->stop_thread != GetCurrentThreadId() &&
           !g_app.account.instance && application_account_logout_notification();
}
bool finish_stop() noexcept {
    const auto signal = application_account_logout_notification();
    if (!signal)
        return false;
    SetEvent(fixture->stop_allowed);
    if (WaitForSingleObject(signal, 3000) != WAIT_OBJECT_0)
        return false;
    application_account_logout_pump();
    return application_text_form_synchronize() && project();
}
bool deliver(u16 http, RestClientError error = RestClientError::none) noexcept {
    fixture->receipt = {};
    fixture->receipt.http_status = http;
    fixture->receipt.transport_error = error;
    fixture->receipt_ready = true;
    fixture->now += 125;
    application_account_logout_pump();
    return wait_stop() && application_text_form_synchronize();
}
} // namespace maicord::logout_native

namespace maicord::logout_native {
bool form_pixels(const D3D11_MAPPED_SUBRESOURCE& mapped) noexcept {
    const auto& form = *g_app.text_form;
    constexpr TextFormElement elements[]{TextFormElement::title, TextFormElement::context,
                                         TextFormElement::status};
    for (const auto element : elements) {
        const auto label = element == TextFormElement::title     ? form.scene.content.title
                           : element == TextFormElement::context ? form.scene.content.context
                                                                 : form.scene.content.status;
        if (!label.bytes)
            continue;
        const auto* node = text_form_node(form.scene, element);
        if (!node ||
            !require(probe_ink(mapped, ui_rect_intersection(node->content_rectangle, node->clip_rectangle),
                               fixture->theme.text_primary),
                     201 + static_cast<u32>(element)))
            return false;
    }
    if (!form.request.confirmation_only || form.edit.storage.data || form.store.implementation)
        return require(false, 220);
    return true;
}
bool visual(const D3D11_MAPPED_SUBRESOURCE& mapped) noexcept {
    auto& r = g_app.renderer;
    if (g_app.text_form && !form_pixels(mapped))
        return false;
    if (g_app.login.visible) {
        const bool modal = g_app.text_form != nullptr;
        const auto heading = login_scene_content_rectangle(g_app.login_scene, LoginElement::heading);
        if (!require(probe_ink(mapped, heading, fixture->theme.text_primary, modal), 221))
            return false;
        if (modal) {
            const auto metrics = renderer_login_form_text_metrics(r);
            if (!require(!g_app.account_ui && !r.chat_state && !r.user_store && !r.navigation_view &&
                             !r.ui_scene && r.login_form_text && r.content_text && metrics.owner_bytes &&
                             metrics.committed_bytes && metrics.commands && r.form_text_first_batch > 0 &&
                             r.form_text_first_batch < r.content_text->batch_count,
                         222))
                return false;
        } else {
            if (!require(!r.login_form_text && !r.content_text && r.glyph_text.title_quad_count, 223))
                return false;
            if (g_app.login.status == RendererLoginStatus::signed_out ||
                g_app.login.status == RendererLoginStatus::signed_out_unconfirmed) {
                if (!require(
                        probe_ink(mapped,
                            login_scene_content_rectangle(g_app.login_scene, LoginElement::status_message),
                            fixture->theme.text_secondary),
                        224))
                    return false;
                if (g_app.login.status == RendererLoginStatus::signed_out_unconfirmed &&
                    !require(
                        probe_ink(mapped,
                            login_scene_content_rectangle(g_app.login_scene, LoginElement::status_account),
                            fixture->theme.text_secondary),
                        225))
                    return false;
            }
        }
    } else if (!g_app.text_form) {
        const auto* footer = ui_scene_resolve(g_app.shell_scene,
                                              ui_scene_find(g_app.shell_scene, UiSemantic::channel_footer));
        if (!footer || !require(probe_ink(mapped, footer->rectangle, fixture->theme.text_primary), 226))
            return false;
    }
    return true;
}
bool capture(const wchar_t* filename) noexcept {
    if (!project())
        return require(false, 230);
    auto& r = g_app.renderer;
    if (!d3d11_renderer_paint_layout(&r, fixture->theme,
                                     renderer_capture_presentation_stamp(r, fixture->theme)) ||
        !d3d11_renderer_draw_stream(&r, fixture->theme, true, true, nullptr, 10000))
        return require(false, 231);
    ProbeTargetReadback readback;
    if (const u32 failure = probe_target_readback(r, &readback))
        return require(false, 231 + failure);
    const auto& mapped = readback.mapped;
    if (!visual(mapped))
        return false;
    if (!require(probe_write_capture(readback, filename), 237))
        return false;
    ++fixture->captures;
    return true;
}
struct WarmSnapshot {
    u32 values[12];
    usize owner_committed;
    u64 form_signature;
};
WarmSnapshot snapshot() noexcept {
    auto& r = g_app.renderer;
    const auto metrics = renderer_login_form_text_metrics(r);
    return {{g_app.shell_scene.layout_passes, g_app.login_scene.layout_passes,
             g_app.text_form->scene.revision, r.content_text->rebuilds, r.glyph_text.cache_builds,
             r.paint_stream_builds, r.paint_instance_uploads, r.text_instance_uploads,
             r.media_instance_uploads, r.painted_frames, fixture->frames,
             g_app.text_form->measure->backend_calls},
            metrics.committed_bytes,
            g_app.text_form->scene.text_signature};
}
bool warm(u32 sample) noexcept {
    for (u32 i = 0; i < 4; ++i)
        if (!project() || !probe_present(&g_app.renderer, fixture->theme))
            return false;
    auto& r = g_app.renderer;
    const auto before = snapshot();
    const auto skips = r.unchanged_present_skips;
    const auto posts = fixture->total_posts;
    const auto metrics = renderer_login_form_text_metrics(r);
    for (u32 i = 0; i < 64; ++i) {
        application_account_logout_pump();
        if (!project() || !probe_present(&r, fixture->theme))
            return false;
    }
    const auto after = snapshot();
    constexpr const char* names[]{"shell_layout_delta", "login_layout_delta",  "form_revision_delta",
                                  "text_build_delta",   "glyph_cache_delta",   "paint_build_delta",
                                  "paint_upload_delta", "text_upload_delta",   "media_upload_delta",
                                  "painted_delta",      "frame_request_delta", "measure_delta"};
    log_u64(LogLevel::info, "logout_warm", "sample", sample);
    bool valid = r.unchanged_present_skips - skips == 64 &&
                 before.owner_committed == after.owner_committed &&
                 before.form_signature == after.form_signature && fixture->total_posts == posts;
    for (u32 i = 0; i < 12; ++i) {
        log_u64(LogLevel::info, "logout_warm", names[i], after.values[i] - before.values[i]);
        valid = valid && before.values[i] == after.values[i];
    }
    log_u64(LogLevel::info, "logout_warm", "unchanged_skips", r.unchanged_present_skips - skips);
    log_u64(LogLevel::info, "logout_warm", "owner_committed_delta",
            after.owner_committed - before.owner_committed);
    log_u64(LogLevel::info, "logout_warm", "form_signature_changed",
            after.form_signature != before.form_signature);
    log_u64(LogLevel::info, "logout_warm", "post_delta", fixture->total_posts - posts);
    log_u64(LogLevel::info, "logout_warm", "owner_bytes", metrics.owner_bytes);
    log_u64(LogLevel::info, "logout_warm", "reserved_bytes", metrics.reserved_bytes);
    log_u64(LogLevel::info, "logout_warm", "committed_bytes", metrics.committed_bytes);
    log_u64(LogLevel::info, "logout_warm", "commands", metrics.commands);
    log_u64(LogLevel::info, "logout_warm", "display_bytes", metrics.display_bytes);
    log_u64(LogLevel::info, "logout_warm", "glyph_instance_capacity", r.pipeline.glyph_instance_capacity);
    return require(valid, 250 + sample);
}
} // namespace maicord::logout_native

namespace maicord::app_detail {
using namespace logout_native;
bool application_login_auth_cooldown_active() noexcept {
    return false;
}
bool application_request_frame() noexcept {
    ++fixture->frames;
    return true;
}
const Theme& application_theme() noexcept {
    return fixture->theme;
}
UiRect application_viewport_dips() noexcept {
    return {0, 0, g_app.renderer.width * 96.f / g_app.renderer.dpi,
            g_app.renderer.height * 96.f / g_app.renderer.dpi};
}
bool refresh_account_text(bool) noexcept {
    return fixture->refresh_ok = fixture->refresh_ok && project();
}
bool refresh_account_frame(bool) noexcept {
    return refresh_account_text(true);
}
// Saved-account switching has its own runtime coverage; this probe keeps the
// single-account menu.
bool application_account_switch_available() noexcept {
    return false;
}
bool application_account_switch_panel(UiAccountSwitcherModel*, u64, UiAccountSwitcherPanel) noexcept {
    return false;
}
bool application_account_switch_action(UiAccountSwitcherPanel, u32) noexcept {
    return false;
}
void application_prepare_account_stop() noexcept {
    ++fixture->prepares;
    application_account_menu_clear();
    ++g_app.plugin_account_scope_serial;
    // Mirror the surrounding quiescence ownership: retain committed form focus
    // and AccountUI label/measure storage until the actual form is detached.
    if (!g_app.text_form || g_app.context_focus != &g_app.text_form->scene.widgets)
        fixture->host_order_error = true;
}
void application_account_stop_observed(const AccountHostStopResult&, bool had_account) noexcept {
    ++fixture->stop_observations;
    if (!had_account || application_account_logout_host_owned() || ReadAcquire(&fixture->stop_active))
        fixture->host_order_error = true;
}
bool application_account_publish_logged_out(bool unconfirmed) noexcept {
    if (g_app.text_form || application_account_logout_host_owned() || g_app.account.instance ||
        g_app.account.login_instance || !credentials_absent()) {
        fixture->host_order_error = true;
        return false;
    }
    release_account_storage();
    ++fixture->publishes;
    fixture->unconfirmed = unconfirmed;
    g_app.login.visible = true;
    g_app.login.status =
        unconfirmed ? RendererLoginStatus::signed_out_unconfirmed : RendererLoginStatus::signed_out;
    secure_edit_clear(&g_app.login.password);
    secure_edit_clear(&g_app.login.mfa);
    g_app.login.remote_restart_at_ms = 0;
    g_app.login.focus = LoginControl::login;
    return application_refresh_login_view();
}
// Dormant adjacent surfaces fail closed if a scenario accidentally routes to
// them. They do not replace any exercised logout, form, login or menu action.
bool unexpected() noexcept {
    ++fixture->unexpected;
    return false;
}
bool application_settings_active() noexcept {
    return false;
}
bool application_settings_open() noexcept {
    return unexpected();
}
bool application_friends_active() noexcept {
    return false;
}
bool application_friends_requests_active() noexcept {
    return false;
}
bool application_friends_show() noexcept {
    return unexpected();
}
bool application_private_channel_picker(u64) noexcept {
    return false;
}
bool application_guild_menu_toggle(bool) noexcept {
    return false;
}
bool application_private_channel_call(u64) noexcept {
    return false;
}
bool application_private_channel_rename(u64) noexcept {
    return false;
}
bool application_private_channel_title_hit(float, float) noexcept {
    return false;
}
u16 application_message_action_bar_capabilities(u64) noexcept {
    return 0;
}
bool application_message_forward_available(u64) noexcept {
    return false;
}
MessageProfileHit application_message_profile_hit_test(float, float) noexcept {
    return {};
}
bool application_message_guild_tag_hit(float, float) noexcept {
    return false;
}
bool application_thread_browser_read_blocked() noexcept {
    return false;
}
UiThreadBrowserScene* application_thread_browser_scene() noexcept {
    return nullptr;
}
bool application_thread_browser_toggle() noexcept {
    return unexpected();
}
bool application_preferences_member_visibility(bool) noexcept {
    return unexpected();
}
bool application_preferences_dm_profile_visibility(bool) noexcept {
    return unexpected();
}
bool application_downloads_open() noexcept {
    return unexpected();
}
bool application_profile_open(u64, UiRect, ProfileAnchor) noexcept {
    return unexpected();
}
bool application_profile_open_edit(u64, UiRect, ProfileAnchor) noexcept {
    return unexpected();
}
bool application_custom_status_available() noexcept {
    return false;
}
bool application_custom_status_snapshot(SelfCustomStatus*) noexcept {
    return false;
}
bool application_custom_status_request(const SelfCustomStatus*) noexcept {
    return false;
}
bool application_custom_status_open() noexcept {
    return false;
}
GatewayClientSelfStatusSnapshot application_self_status_snapshot() noexcept {
    return {};
}
bool application_self_status_request(GatewayUserStatus) noexcept {
    return unexpected();
}
void application_tooltip_clear() noexcept {
}
void application_composer_text_input_suspend() noexcept {
}
void end_message_text_selection() noexcept {
}
void end_message_composer_pointer_selection() noexcept {
}
void end_scrollbar_drag() noexcept {
}
void end_pane_resize() noexcept {
    g_app.pane_resize = PaneResizeKind::none;
}
bool stop_local_typing(u64) noexcept {
    return true;
}
void reset_message_composer_caret_timing(bool) noexcept {
}
bool application_forum_form_active() noexcept {
    return false;
}
bool application_forum_post_current(const ApplicationForumPostTicket&) noexcept {
    return false;
}
bool application_forum_form_suspend_child(const ApplicationForumPostTicket&) noexcept {
    return unexpected();
}
void application_forum_form_resume_child(const ApplicationForumPostTicket&) noexcept {
    ++fixture->unexpected;
}
void set_popup_owner(PopupOwnerKind value) noexcept {
    g_app.popup_owner_kind = value;
}
bool dismiss_popup() noexcept {
    popup_scene_close(&g_app.popup_scene);
    set_popup_owner(PopupOwnerKind::none);
    return true;
}
bool application_voice_shell_click(float, float) noexcept {
    return false;
}
bool application_plugins_slot_action(u64, NativePluginSdkSlotTarget, u64) noexcept {
    return false;
}
bool application_plugins_render_click(const MessageTextHit&, UiRect) noexcept {
    return false;
}
bool application_plugins_slot_activate(u64, NativePluginSdkSlotTarget, u64, UiRect) noexcept {
    return false;
}
bool application_plugins_surface_hit(float, float, ApplicationPluginSurfaceHit*) noexcept {
    return false;
}
bool application_plugins_surface_click(float, float) noexcept {
    return false;
}
bool application_plugins_surface_hover(float, float) noexcept {
    return false;
}
bool application_voice_shell_hover(float, float) noexcept {
    return false;
}
void application_channel_prefetch_hover(u64) noexcept {}
bool application_voice_hand_cursor(float, float) noexcept {
    return false;
}
bool application_message_content_cursor(float, float, UiCursor*) noexcept {
    return false;
}
bool application_video_viewer_cursor(UiPoint, UiCursor*) noexcept {
    return false;
}
} // namespace maicord::app_detail

namespace maicord::logout_native {
bool retry(u32 key_code) noexcept {
    if (!g_app.text_form || !g_app.text_form->can_accept || g_app.text_form->can_cancel ||
        g_app.text_form->dismissible ||
        text_form_focused(g_app.text_form->scene) != TextFormElement::accept)
        return false;
    return key(key_code) && key(key_code, true) && up(key_code) && application_text_form_synchronize() &&
           project();
}
bool status(Utf8View value) noexcept {
    return g_app.text_form && utf8_equal(g_app.text_form->scene.content.status, value);
}
bool scenarios() noexcept {
    auto& f = *fixture;
    if (!require(seed() && resize(960, 760, 96, kDefaultTheme) && menu(false) &&
                     capture(L"account-logout-menu-dark96.bmp"),
                 1))
        return false;
    if (!require(launch_from_menu() && capture(L"account-logout-confirm-dark96.bmp"), 2))
        return false;
    if (!require(key(VK_RETURN) && up(VK_RETURN) && !g_app.text_form && !f.posts &&
                     !account_credential_forget_probe(f.paths).pending &&
                     capture(L"account-logout-cancelled-dark96.bmp"),
                 3))
        return false;
    log_write(LogLevel::info, "probe", "account_logout_native_default_cancel_zero_post");
    if (!require(menu(true) && launch_from_menu() && resize(960, 900, 144, kLightTheme) &&
                     text_form_focused(g_app.text_form->scene) == TextFormElement::cancel &&
                     capture(L"account-logout-confirm-light144.bmp"),
                 4))
        return false;
    if (!require(commit() && locked() && status(utf8_literal("Logging out…")) &&
                     capture(L"account-logout-pending-light144.bmp") && warm(1),
                 5))
        return false;
    log_write(LogLevel::info, "probe", "account_logout_native_committed_input_locked_one_post");
    f.fail_stop = true;
    if (!require(deliver(204) && resize(960, 760, 96, kDefaultTheme) && locked() &&
                     status(utf8_literal("Logging out…")) &&
                     !application_account_logout_next_wake() &&
                     capture(L"account-logout-teardown-wait-dark96.bmp"),
                 6))
        return false;
    if (!require(finish_stop() && g_app.account.instance == fixture && f.stop_attempts == 1 &&
                     f.posts == 1 && !credentials_absent() &&
                     capture(L"account-logout-stop-failed-dark96.bmp"),
                 7))
        return false;
    ResetEvent(f.stop_entered);
    ResetEvent(f.stop_allowed);
    f.fail_stop = false;
    f.blocked_file = CreateFileW(f.paths.profile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!require(f.blocked_file != INVALID_HANDLE_VALUE && retry(VK_RETURN) && wait_stop() &&
                     f.stop_attempts == 2 && f.posts == 1 && finish_stop() && g_app.text_form &&
                     g_app.text_form->can_accept && account_credential_forget_probe(f.paths).pending &&
                     resize(960, 900, 144, kLightTheme) &&
                     capture(L"account-logout-delete-failed-light144.bmp"),
                 8))
        return false;
    CloseHandle(f.blocked_file);
    f.blocked_file = nullptr;
    if (!require(retry(VK_SPACE) && !g_app.text_form && !g_app.account_ui && !f.unconfirmed &&
                     !application_account_logout_blocked() && credentials_absent() && f.posts == 1 &&
                     f.stop_attempts == 2 && capture(L"account-logout-signed-out-confirmed-light144.bmp"),
                 9))
        return false;
    log_write(LogLevel::info, "probe", "account_logout_native_join_then_cleanup_retry_no_repost");
    if (!require(seed() && resize(960, 760, 96, kDefaultTheme) && menu(false) && launch_from_menu() &&
                     commit() && deliver(0, RestClientError::cancelled) && finish_stop() &&
                     f.unconfirmed && !g_app.account_ui && credentials_absent() && f.posts == 1 &&
                     capture(L"account-logout-signed-out-unconfirmed-dark96.bmp"),
                 10))
        return false;
    log_write(LogLevel::info, "probe", "account_logout_native_unconfirmed_is_local_only");
    if (!require(seed(), 11))
        return false;
    // Interrupted startup owns no DLL/account instance and no AccountUI. The
    // real durable marker helper creates the recovery obligation before open.
    g_app.account = {};
    release_account_storage();
    g_app.login.visible = true;
    g_app.login.status = RendererLoginStatus::idle;
    if (!require(application_refresh_login_view() && account_credential_forget_begin(f.paths).ok() &&
                     !application_account_logout_prepare_startup(&f.paths) &&
                     application_account_logout_blocked() && g_app.text_form &&
                     g_app.text_form->request.login_owner && !g_app.account_ui && !f.posts &&
                     capture(L"account-logout-startup-recovery-dark96.bmp"),
                 12))
        return false;
    if (!require(resize(960, 900, 144, kLightTheme) &&
                     capture(L"account-logout-startup-recovery-light144.bmp") && warm(2),
                 13))
        return false;
    if (!require(retry(VK_RETURN) && !g_app.text_form && !g_app.account_ui && !f.posts &&
                     !application_account_logout_blocked() && credentials_absent() &&
                     capture(L"account-logout-startup-complete-light144.bmp"),
                 14))
        return false;
    const auto field = login_scene_rectangle(g_app.login_scene, LoginElement::login_field);
    const auto point = center(field);
    if (!require(application_login_position_edit(LoginControl::login, static_cast<short>(LOWORD(point)),
                                                 static_cast<short>(HIWORD(point)), false) &&
                     application_login_handle_character('A') && g_app.login.login.storage.bytes == 1 &&
                     !g_app.account.instance && !g_app.account_ui,
                 15))
        return false;
    ++f.login_inputs;
    log_write(LogLevel::info, "probe", "account_logout_native_startup_model_free_modal_then_login");
    log_write(LogLevel::info, "probe",
              "account_logout_native_injected_server_and_lifecycle_real_native_ui");
    log_write(LogLevel::info, "probe",
              "account_logout_native_real_fixture_files_async_join_no_live_revocation");
    log_u64(LogLevel::info, "probe", "account_logout_native_fixture_bytes", sizeof(Fixture));
    log_u64(LogLevel::info, "probe", "account_logout_native_account_ui_bytes", sizeof(AccountUiRuntime));
    log_u64(LogLevel::info, "probe", "account_logout_native_controller_bytes",
            sizeof(ApplicationAccountLogout));
    log_u64(LogLevel::info, "probe", "account_logout_native_captures", f.captures);
    log_u64(LogLevel::info, "probe", "account_logout_native_total_posts", f.total_posts);
    log_u64(LogLevel::info, "probe", "account_logout_native_stop_attempts", f.stop_attempts);
    log_u64(LogLevel::info, "probe", "account_logout_native_stop_observations", f.stop_observations);
    log_u64(LogLevel::info, "probe", "account_logout_native_account_ui_releases", f.account_ui_releases);
    log_u64(LogLevel::info, "probe", "account_logout_native_unexpected_routes", f.unexpected);
    log_u64(LogLevel::info, "probe", "account_logout_native_order_errors",
            f.file_order_error || f.host_order_error);
    log_u64(LogLevel::info, "probe", "account_logout_native_login_inputs", f.login_inputs);
    return require(f.captures == 13 && f.total_posts == 2 && f.stop_attempts == 3 &&
                       f.stop_observations == 3 && f.account_ui_releases == 3 && !f.unexpected &&
                       !f.file_order_error && !f.host_order_error && f.refresh_ok && !f.failures,
                   16);
}
void remove_files() noexcept {
    if (fixture->blocked_file && fixture->blocked_file != INVALID_HANDLE_VALUE)
        CloseHandle(fixture->blocked_file);
    fixture->blocked_file = nullptr;
    // Every path belongs to this fresh, process-specific temporary-directory
    // fixture created with CREATE_NEW. No recursive or user-profile deletion.
    DeleteFileW(fixture->paths.profile);
    DeleteFileW(fixture->paths.session);
    DeleteFileW(fixture->profile_new);
    DeleteFileW(fixture->session_new);
    DeleteFileW(fixture->marker);
    DeleteFileW(fixture->paths.drafts);
    RemoveDirectoryW(fixture->paths.account_directory);
    RemoveDirectoryW(fixture->paths.application_directory);
    RemoveDirectoryW(fixture->root);
}
} // namespace maicord::logout_native

namespace maicord {
bool account_logout_ui_probe() noexcept {
    using namespace app_detail;
    using namespace logout_native;
    if (!log_open())
        return false;
    fixture = static_cast<Fixture*>(
        VirtualAlloc(nullptr, sizeof(Fixture), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!fixture)
        return false;
    fixture->theme = kDefaultTheme;
    fixture->refresh_ok = true;
    fixture->stop_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    fixture->stop_allowed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = instance;
    type.lpszClassName = L"MaiCordAccountLogoutUiProbe";
    if (!RegisterClassW(&type))
        return false;
    // Never activated, no taskbar button, opened behind every other window.
    constexpr DWORD kWindowExStyle = WS_EX_NOREDIRECTIONBITMAP | probe_window::kExtendedStyle;
    RECT bounds{0, 0, 960, 760};
    if (!AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, FALSE, kWindowExStyle))
        return false;
    HWND window = CreateWindowExW(kWindowExStyle, type.lpszClassName, L"MaiCord native logout",
                                  WS_OVERLAPPEDWINDOW, 32, 32, bounds.right - bounds.left,
                                  bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    if (!window)
        return false;
    g_app.window = window;
    struct Cleanup {
        HWND window;
        bool apartment;
        ~Cleanup() noexcept {
            fixture->fail_stop = false;
            SetEvent(fixture->stop_allowed);
            if (!application_account_logout_blocked() && !application_account_logout_host_owned()) {
                // This dormant host has only the fixture's borrowed module
                // handle/callback; it owns no DLL reference or account worker.
                g_app.account = {};
            }
            if (!application_account_logout_shutdown() || application_account_logout_host_owned()) {
                require(false, 290);
                // Retain every borrowed file/model/host resource if the real
                // kernel join did not publish. Process exit owns final cleanup.
                return;
            }
            if (g_app.text_form)
                application_text_form_close(false, false);
            application_account_logout_release();
            release_account_storage();
            application_release_login_ui();
            popup_scene_release(&g_app.popup_scene);
            ui_scene_release(&g_app.shell_scene);
            renderer_shutdown(&g_app.renderer);
            remove_files();
            CloseHandle(fixture->stop_entered);
            CloseHandle(fixture->stop_allowed);
            DestroyWindow(window);
            if (apartment)
                CoUninitialize();
            VirtualFree(fixture, 0, MEM_RELEASE);
            fixture = nullptr;
            log_close();
        }
    } cleanup{window, SUCCEEDED(apartment)};
    probe_window::show(window);
    SetFocus(window);
    if (!require(SUCCEEDED(apartment) && fixture->stop_entered && fixture->stop_allowed &&
                     initialize_paths() &&
                     renderer_initialize(&g_app.renderer, window),
                 100))
        return false;
    g_app.renderer_ready = true;
    if (!require(ui_scene_initialize_bootstrap(&g_app.shell_scene) &&
                     guild_rail_scene_initialize(&g_app.guild_rail_scene, {fixture->rail_rows, 2},
                                                 {fixture->rail_widgets, 4}, {fixture->rail_nodes, 4}) &&
                     shell_navigation_scene_initialize(
                         &g_app.shell_navigation_scene, {fixture->navigation_rows, 2},
                         {fixture->navigation_widgets, 10}, {fixture->navigation_nodes, 10}) &&
                     popup_scene_initialize(&g_app.popup_scene, {fixture->popup_items, 24},
                                            {fixture->popup_rows, 24}, {fixture->popup_widgets, 64},
                                            {fixture->popup_nodes, 64}) &&
                     renderer_bind_popup_scene(&g_app.renderer, &g_app.popup_scene) &&
                     application_initialize_login_ui(),
                 101))
        return false;
    g_app.login.caret_blink_ms = INFINITE;
    return scenarios();
}
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_logout_ui_probe_entry() noexcept {
    wchar_t enabled[4]{};
    if (GetEnvironmentVariableW(L"MAICORD_UI_ACCOUNT_LOGOUT_PROBE", enabled, 4) != 1 || enabled[0] != L'1')
        ExitProcess(2);
    ExitProcess(maicord::account_logout_ui_probe() ? 0 : 1);
}
