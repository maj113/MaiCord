#include "thread_browser_ui_adapter.hpp"
#include "forum_post_ui_adapter.hpp"
#include "application_state.hpp"
#include "maicord/protocol/thread_browser.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/time.hpp"

namespace maicord {
namespace {
using namespace app_detail;
struct BrowserAdapter {
    Application prior;
    RoleRecord roles[2];
    PermissionOverwriteRecord overwrites[4];
    ThreadBrowserRequest flight;
    char flight_name[kThreadBrowserNameMaximumBytes];
    UiPoint pressed_point;
    u64 flight_id;
    u32 submissions, cancellations;
};
BrowserAdapter* browser_adapter;
RestClientSubmitResult WINAPI browser_submit(void*, const ThreadBrowserRequest* request, u64 id) noexcept {
    if (!browser_adapter || !id) return {RestClientSubmitError::invalid_input};
    if (!request) {
        if (id != browser_adapter->flight_id) return {RestClientSubmitError::busy};
        ++browser_adapter->cancellations;
        return {};
    }
    if (browser_adapter->flight_id) return {RestClientSubmitError::busy};
    if (!thread_browser_request_valid(*request)) return {RestClientSubmitError::invalid_input};
    browser_adapter->flight = *request;
    if (request->name.bytes) copy_bytes(browser_adapter->flight_name, request->name.data, request->name.bytes);
    browser_adapter->flight.name = {browser_adapter->flight_name, request->name.bytes};
    browser_adapter->flight_id = id;
    ++browser_adapter->submissions;
    return {};
}
bool seed() noexcept {
    auto& ui = *g_app.account_ui;
    auto& state = ui.chat_state;
    constexpr u64 permissions = kPermissionViewChannel | kPermissionReadMessageHistory |
        kPermissionSendMessages | kPermissionSendMessagesInThreads;
    if (!chat_state_store_initialize(&state, {ui.guild_record_storage, 2}, {ui.guild_order_storage, 2},
        {ui.channel_record_storage, 256}, {ui.channel_order_storage, 256}, {ui.chat_state_name_storage, 64 * 1024}) ||
        !chat_state_store_bind_permissions(&state, {browser_adapter->roles, 2}, {browser_adapter->overwrites, 4})) return false;
    state.self_user_id = kThreadMembershipProbeSelf;
    if (!chat_state_store_upsert_guild(&state, {kThreadMembershipProbeGuild,
            utf8_literal("Native thread discovery"), GuildMutationKind::create, 96999}).ok() ||
        !chat_state_store_upsert_role(&state, {kThreadMembershipProbeGuild, kThreadMembershipProbeGuild,
            permissions, utf8_literal("Members")}) ||
        !chat_state_store_set_self_roles(&state, kThreadMembershipProbeGuild, 96999,
            kThreadMembershipProbeSelf, {})) return false;
    const ChannelInput parent{kThreadBrowserProbeParent, kThreadMembershipProbeGuild, 0,
        utf8_literal("design-room"), 0, 0, 0};
    if (!chat_state_store_upsert_channel(&state, parent).ok()) return false;
    const ChannelInput forum{kThreadBrowserProbeForum, kThreadMembershipProbeGuild, 0,
        utf8_literal("product-forum"), 1, 0, 15};
    ChannelInput forum_topic{kThreadBrowserProbeForum + 10, kThreadMembershipProbeGuild, kThreadBrowserProbeForum,
        utf8_literal("Forum topic · native review"), 0, 0, 11};
    forum_topic.thread_archived_known = true;
    if (!chat_state_store_upsert_channel(&state, forum).ok() ||
        !chat_state_store_upsert_channel(&state, forum_topic).ok() ||
        !chat_state_store_set_thread_active(&state, forum_topic.id, state.self_user_id, true) ||
        !chat_state_store_set_thread_listing_synced(&state, forum.id, state.self_user_id)) return false;
    const Utf8View names[]{utf8_literal("Native planning · 日本語"), utf8_literal("Release notes and next steps"),
        utf8_literal("Private design review"), utf8_literal("Long discussion name that remains readable within its card"),
        utf8_literal("Accessibility and input review")};
    for (u32 i = 0; i < 5; ++i) {
        ChannelInput input{kThreadBrowserProbeFirstActive + i, kThreadMembershipProbeGuild,
            kThreadBrowserProbeParent, names[i], i, 0, static_cast<u8>(i == 2 ? 12 : 11)};
        input.thread_archived_known = true;
        if (!chat_state_store_upsert_channel(&state, input).ok() ||
            !chat_state_store_set_thread_active(&state, input.id, state.self_user_id, true) ||
            !chat_state_store_set_thread_self_membership(&state, input.id, state.self_user_id, i == 2 || i == 0)) return false;
    }
    chat_state_store_recompute_guild_permissions(&state, kThreadMembershipProbeGuild);
    if (!chat_state_store_set_thread_listing_synced(&state, kThreadBrowserProbeParent, state.self_user_id) ||
        !chat_state_store_select_channel(&state, kThreadBrowserProbeParent).ok()) return false;
    ui.active_channel_id = kThreadBrowserProbeParent;
    return true;
}
LPARAM point(UiPoint p) noexcept {
    return MAKELPARAM(static_cast<u16>(p.x * g_app.renderer.dpi / 96.f + .5f),
        static_cast<u16>(p.y * g_app.renderer.dpi / 96.f + .5f));
}
} // namespace

bool thread_browser_ui_adapter_begin(HWND window, D3D11Renderer& renderer,
    bool (*refresh)(void*) noexcept, void* context) noexcept {
    if (browser_adapter || application_thread_browser_metrics().owner_bytes) return false;
    auto* value = static_cast<BrowserAdapter*>(VirtualAlloc(nullptr, sizeof(BrowserAdapter), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!value) return false;
    value->prior = g_app;
    browser_adapter = value;
    if (!thread_membership_ui_adapter_begin(window, refresh, context)) {
        browser_adapter = nullptr; VirtualFree(value, 0, MEM_RELEASE); return false;
    }
    g_app.renderer = renderer; // Borrow GPU owners; never shut down this shallow view.
    g_app.renderer_ready = false; // The refresh callback rebuilds the actual renderer once.
    g_app.shell_scene = *renderer.ui_scene;
    g_app.account.api.thread_browser = browser_submit;
    g_app.context_focus = nullptr;
    g_app.composer_text_input = nullptr;
    if (!seed()) { thread_browser_ui_adapter_end(); return false; }
    return true;
}
void thread_browser_ui_adapter_end() noexcept {
    if (!browser_adapter) return;
    auto* value = browser_adapter;
    application_thread_browser_close();
    if (value->flight_id) thread_browser_ui_adapter_response(0, {}, RestClientError::cancelled);
    application_thread_browser_release();
    thread_membership_ui_adapter_end();
    g_app = value->prior;
    browser_adapter = nullptr;
    VirtualFree(value, 0, MEM_RELEASE);
}
void thread_browser_ui_adapter_viewport(D3D11Renderer& renderer, const UiScene& shell) noexcept {
    if (!browser_adapter) return;
    g_app.renderer.width = renderer.width;
    g_app.renderer.height = renderer.height;
    g_app.renderer.dpi = renderer.dpi;
    g_app.shell_scene = shell;
}
ChatStateStore* thread_browser_ui_adapter_state() noexcept { return browser_adapter ? &g_app.account_ui->chat_state : nullptr; }
UserStore* thread_browser_ui_adapter_users() noexcept { return browser_adapter ? &g_app.account_ui->user_store : nullptr; }
bool thread_browser_ui_adapter_active_inventory(bool rows, bool synced, bool online) noexcept {
    if (!browser_adapter) return false;
    auto& state = g_app.account_ui->chat_state;
    for (u32 i = 0; i < 5; ++i)
        if (!chat_state_store_set_thread_active(&state, kThreadBrowserProbeFirstActive + i, state.self_user_id, rows)) return false;
    if (!chat_state_store_set_thread_listing_synced(&state, kThreadBrowserProbeParent, state.self_user_id, synced)) return false;
    g_app.account.status.running = online;
    application_thread_browser_gateway(kThreadMembershipProbeGuild, kThreadBrowserProbeParent);
    return application_thread_browser_synchronize();
}
bool thread_browser_ui_adapter_action(UiThreadBrowserAction action) noexcept {
    auto* scene = application_thread_browser_scene();
    if (!scene) return false;
    for (u32 i = 0; i < scene->widgets.count; ++i) {
        const auto value = ui_thread_browser_scene_target(*scene, {static_cast<u16>(i), scene->widgets.generation});
        if (value.action == action) return application_thread_browser_activate(value);
    }
    return false;
}
bool thread_browser_ui_adapter_pointer(UiWidgetHandle widget, bool down) noexcept {
    auto* scene = application_thread_browser_scene();
    if (!browser_adapter || !scene) return false;
    if (down) {
        const auto* node = ui_thread_browser_scene_current(*scene)
            ? ui_widget_manager_layout_node(scene->widgets, widget) : nullptr;
        if (!node || !ui_rect_has_area(node->rectangle)) return false;
        const auto r = node->rectangle;
        browser_adapter->pressed_point = {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
    }
    return application_thread_browser_window_message(down ? WM_LBUTTONDOWN : WM_LBUTTONUP,
        down ? MK_LBUTTON : 0, point(browser_adapter->pressed_point));
}
bool thread_browser_ui_adapter_header_pointer(bool down) noexcept {
    if (!browser_adapter) return false;
    if (down) {
        const auto r = ui_scene_header_geometry(g_app.shell_scene).threads;
        if (!ui_rect_has_area(r)) return false;
        browser_adapter->pressed_point = {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
    }
    return application_thread_browser_window_message(down ? WM_LBUTTONDOWN : WM_LBUTTONUP,
        down ? MK_LBUTTON : 0, point(browser_adapter->pressed_point));
}
bool thread_browser_ui_adapter_key(u32 code, bool shift, bool repeated) noexcept {
    if (!browser_adapter) return false;
    BYTE prior[256]{}, keys[256]{};
    if (!GetKeyboardState(prior)) return false;
    keys[VK_SHIFT] = shift ? 0x80 : 0;
    if (!SetKeyboardState(keys)) return false;
    const bool accepted = application_thread_browser_input(UiInputKind::key_down, code, repeated);
    application_thread_browser_input(UiInputKind::key_up, code, false);
    return SetKeyboardState(prior) != FALSE && accepted;
}
bool thread_browser_ui_adapter_submit(u64 id) noexcept {
    return browser_adapter && application_thread_browser_submit(id, clock_monotonic_milliseconds());
}
bool thread_browser_ui_adapter_response(u16 status, Utf8View body, RestClientError error, u64 response_id) noexcept {
    if (!browser_adapter || !browser_adapter->flight_id) return false;
    const u64 id = response_id ? response_id : browser_adapter->flight_id;
    RestClientResponse response{};
    response.request_id = id; response.status_code = status; response.error = error; response.body_bytes = body.bytes;
    if (id == browser_adapter->flight_id) browser_adapter->flight_id = 0;
    return application_thread_browser_response(response, body);
}
bool thread_browser_ui_adapter_flight(ThreadBrowserRequest* request, u64* id) noexcept {
    if (!browser_adapter || !browser_adapter->flight_id || !request || !id) return false;
    *request = browser_adapter->flight; *id = browser_adapter->flight_id; return true;
}
u32 thread_browser_ui_adapter_submissions() noexcept { return browser_adapter ? browser_adapter->submissions : 0; }
u32 thread_browser_ui_adapter_cancellations() noexcept { return browser_adapter ? browser_adapter->cancellations : 0; }
bool thread_browser_ui_adapter_retire_scope(bool unavailable) noexcept {
    if (!browser_adapter) return false;
    if (unavailable) {
        if (!chat_state_store_upsert_guild(&g_app.account_ui->chat_state,
            {kThreadMembershipProbeGuild, {}, GuildMutationKind::unavailable, 0}).ok()) return false;
    } else {
        ++g_app.plugin_account_scope_serial;
        ++g_app.account.status.profile_id;
    }
    return application_thread_browser_synchronize();
}
bool thread_browser_ui_adapter_page(Slice<u8> storage, u32 count, u64 first, bool archived, bool more, Utf8View* output) noexcept {
    if (!output || count > 100) return false;
    ProtocolWriter w{storage};
    if (!protocol_writer_append_literal(&w, "{\"threads\":[")) return false;
    for (u32 i = 0; i < count; ++i) {
        if ((i && !protocol_writer_append_byte(&w, ',')) ||
            !protocol_writer_append_literal(&w, "{\"id\":\"") || !protocol_writer_append_u64(&w, first + i) ||
            !protocol_writer_append_literal(&w, "\",\"parent_id\":\"96100\",\"guild_id\":\"96000\",\"type\":") ||
            !protocol_writer_append_u64(&w, i % 4 == 0 ? 12 : 11) ||
            !protocol_writer_append_literal(&w, ",\"name\":\"Design discussion ") || !protocol_writer_append_u64(&w, i + 1) ||
            !protocol_writer_append_literal(&w, " · native review\",\"thread_metadata\":{\"archived\":") ||
            !protocol_writer_append_c_string(&w, archived || i % 2 == 0 ? "true" : "false") ||
            !protocol_writer_append_literal(&w, ",\"locked\":false}")) return false;
        if (i % 4 == 0 && !protocol_writer_append_literal(&w,
            ",\"member\":{\"join_timestamp\":\"2024-08-30T12:34:56Z\",\"flags\":0}")) return false;
        if (!protocol_writer_append_byte(&w, '}')) return false;
    }
    if (!protocol_writer_append_literal(&w, "],\"has_more\":") ||
        !protocol_writer_append_c_string(&w, more ? "true}" : "false}")) return false;
    *output = {reinterpret_cast<const char*>(storage.data), w.used}; return true;
}
} // namespace maicord

// Only these surrounding surfaces are omitted by this fixture. Navigation is
// recorded by selecting Core state; no successful HTTP join/send is fabricated.
namespace maicord::app_detail {
bool application_settings_active() noexcept { return false; }
bool application_search_close_panel() noexcept { return true; }
void application_pinned_messages_close() noexcept {}
bool application_navigate_known_message(u64 id, u64 message) noexcept {
    if (forum_post_ui_adapter_active()) return forum_post_ui_adapter_navigate(id, message);
    if (!g_app.account_ui || message || !chat_state_store_select_channel(&g_app.account_ui->chat_state, id).ok()) return false;
    g_app.account_ui->active_channel_id = id;
    return true;
}
} // namespace maicord::app_detail
