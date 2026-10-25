#include "thread_membership_ui_adapter.hpp"
#include "application_state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/permission_bits.hpp"
#include "../../src/app/account_ui_runtime.hpp"

namespace maicord {
namespace {
using namespace app_detail;
constexpr u32 kPopupCapacity = 16;
struct ThreadAdapter {
    AccountUiRuntime* prior_ui;
    AccountHost prior_account;
    MessageViewRuntime prior_view;
    PopupScene prior_popup;
    UiThreadHeader prior_header;
    MessageHistoryRequestOwner prior_request_owner;
    MessageHistoryRequestOwner prior_details_owner;
    PopupOwnerKind prior_popup_owner;
    HWND prior_window;
    u64 prior_scope, prior_request, prior_details_request;
    bool prior_login_visible;
    AccountUiRuntime* ui;
    RoleRecord roles[2];
    PermissionOverwriteRecord overwrites[4];
    MessageRecord records[4];
    u32 order[4];
    u8 text[2048];
    MessageRichRecord rich[4];
    u16 rich_order[4];
    u8 rich_text[2048];
    u64 member_ids[2][8];
    PopupItem popup_items[kPopupCapacity];
    PopupRowWidget popup_rows[kPopupCapacity];
    UiWidget popup_widgets[2 + kPopupCapacity * kPopupWidgetsPerItem];
    UiLayoutNode popup_nodes[2 + kPopupCapacity * kPopupWidgetsPerItem];
    ThreadActionRequest flight;
    u64 flight_id, cancelled_id;
    UiThreadHeader* header;
    UiRect viewport;
    ApplicationThreadActionTicket menu_ticket;
    ApplicationThreadActionTicket header_ticket;
    bool (*refresh)(void*) noexcept;
    void* context;
};
ThreadAdapter* thread_adapter;
constexpr UiThreadMenuActions kMenuActions{96201, 96202, 96203, 96204, 96205, 96206};

ApplicationThreadHeaderActivationSource source(ApplicationThreadActionTicket ticket,
                                               UiThreadHeader* header) noexcept {
    return {thread_adapter, header, &g_app.popup_scene, ticket, kMenuActions,
        [](void*, const ApplicationThreadActionTicket& key) noexcept {
            return application_thread_actions_current(key);
        },
        [](void*, u64 id) noexcept { return application_thread_actions_view(id); },
        [](void*, const ApplicationThreadActionTicket& key, ThreadActionOperation operation) noexcept {
            return application_thread_actions_activate(key, operation);
        },
        [](void*, const ApplicationThreadActionTicket& key) noexcept {
            return application_thread_actions_retry(key);
        },
        [](void*, const ApplicationThreadActionTicket& key) noexcept {
            return application_thread_actions_cancel(key);
        },
        [](void*, const ApplicationThreadActionTicket& key, UiRect anchor) noexcept {
            return thread_membership_ui_adapter_open_menu(key.thread_id, anchor, thread_adapter->viewport);
        },
        [](void*) noexcept { return thread_membership_ui_adapter_refresh(); },
        [](void*, const ApplicationThreadActionTicket& key, ApplicationThreadActionEnqueueResult admission) noexcept {
            return application_thread_actions_admission_status(key, admission);
        }};
}

RestClientSubmitResult WINAPI submit_thread(void*, const ThreadActionRequest* request,
                                            u64 request_id) noexcept {
    if (!thread_adapter || !request_id)
        return {RestClientSubmitError::invalid_input};
    if (!request) {
        if (thread_adapter->flight_id != request_id)
            return {RestClientSubmitError::busy};
        thread_adapter->cancelled_id = request_id;
        return {};
    }
    if (thread_adapter->flight_id)
        return {RestClientSubmitError::busy};
    if (!thread_action_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    thread_adapter->flight = *request;
    thread_adapter->flight_id = request_id;
    return {};
}

bool seed_models(ThreadAdapter& fixture) noexcept {
    auto& ui = *fixture.ui;
    auto& state = ui.chat_state;
    if (!chat_state_store_initialize(&state, {ui.guild_record_storage, 2}, {ui.guild_order_storage, 2},
            {ui.channel_record_storage, 8}, {ui.channel_order_storage, 8}, {ui.chat_state_name_storage, 4096}) ||
        !chat_state_store_bind_permissions(&state, {fixture.roles, 2}, {fixture.overwrites, 4}))
        return false;
    state.self_user_id = kThreadMembershipProbeSelf;
    constexpr u64 permissions = kPermissionViewChannel | kPermissionReadMessageHistory |
        kPermissionSendMessages | kPermissionSendMessagesInThreads;
    if (!chat_state_store_upsert_guild(&state, {kThreadMembershipProbeGuild,
            utf8_literal("Native threads"), GuildMutationKind::create, 96999}).ok() ||
        !chat_state_store_upsert_role(&state, {kThreadMembershipProbeGuild,
            kThreadMembershipProbeGuild, permissions, utf8_literal("Members")}) ||
        !chat_state_store_set_self_roles(&state, kThreadMembershipProbeGuild, 96999,
            kThreadMembershipProbeSelf, {}))
        return false;
    const ChannelInput channels[]{
        {96090, kThreadMembershipProbeGuild, 0, utf8_literal("PROJECT"), 0, 0, 4},
        {kThreadMembershipProbeParent, kThreadMembershipProbeGuild, 96090,
            utf8_literal("design-room"), 0, 0, 0},
        {kThreadMembershipProbePublic, kThreadMembershipProbeGuild, kThreadMembershipProbeParent,
            utf8_literal("Native thread review"), 0, 0, 11},
        {kThreadMembershipProbePrivate, kThreadMembershipProbeGuild, kThreadMembershipProbeParent,
            utf8_literal("Private review"), 1, 0, 12}};
    for (auto channel : channels) {
        if (channel.type == 11 || channel.type == 12)
            channel.thread_archived_known = true;
        if (!chat_state_store_upsert_channel(&state, channel).ok())
            return false;
    }
    chat_state_store_recompute_guild_permissions(&state, kThreadMembershipProbeGuild);
    if (!chat_state_store_set_thread_self_membership(&state, kThreadMembershipProbePublic,
            kThreadMembershipProbeSelf, true) ||
        !chat_state_store_set_thread_self_membership(&state, kThreadMembershipProbePrivate,
            kThreadMembershipProbeSelf, false) ||
        !chat_state_store_set_thread_locked(&state, kThreadMembershipProbePublic,
            kThreadMembershipProbeSelf, false) ||
        !chat_state_store_set_thread_locked(&state, kThreadMembershipProbePrivate,
            kThreadMembershipProbeSelf, false))
        return false;
    if (!chat_state_store_select_channel(&state, kThreadMembershipProbePublic).ok() ||
        !user_store_initialize(&ui.user_store, {ui.user_record_storage, 4},
            {ui.user_order_storage, 4}, {ui.user_name_storage, 2048}) ||
        !user_store_upsert(&ui.user_store, kThreadMembershipProbeSelf,
            utf8_literal("Native reviewer"), utf8_literal("reviewer")).ok() ||
        !user_store_upsert(&ui.user_store, 96006, utf8_literal("Thread author"),
            utf8_literal("author")).ok() ||
        !thread_member_store_initialize(&ui.thread_member_store,
            {fixture.member_ids[0], 8}, {fixture.member_ids[1], 8}) ||
        !thread_member_store_select(&ui.thread_member_store,
            kThreadMembershipProbeGuild, kThreadMembershipProbePublic) ||
        !message_store_initialize(&g_app.message_view.store, {fixture.records, 4}, {fixture.order, 4},
            {fixture.text, sizeof(fixture.text)}) ||
        !message_rich_store_initialize(&ui.message_rich_store, {fixture.rich, 4},
            {fixture.rich_order, 4}, {fixture.rich_text, sizeof(fixture.rich_text)}))
        return false;
    ui.active_channel_id = state.selected_channel_id;
    const Utf8View messages[]{utf8_literal("This conversation stays attached to its parent channel."),
        utf8_literal("Membership and archive actions retain the current thread scope.")};
    for (u32 index = 0; index < 2; ++index) {
        GatewayMessageEvent event{};
        event.id = 96200 + index;
        event.channel_id = kThreadMembershipProbePublic;
        event.author_id = index ? kThreadMembershipProbeSelf : 96006;
        event.timestamp_ms = 1'759'667'400'000ull + index * 60'000ull;
        event.content = messages[index];
        event.fields = gateway_message_field_id | gateway_message_field_channel_id |
            gateway_message_field_author_id | gateway_message_field_timestamp | gateway_message_field_content;
        const auto stored = message_store_upsert(&g_app.message_view.store,
            {event.id, event.channel_id, event.author_id, event.timestamp_ms, 0, event.content, 0});
        const auto* record = stored.ok() ? message_store_resolve(g_app.message_view.store, stored.handle) : nullptr;
        if (!record || !message_rich_store_apply(&ui.message_rich_store, event.id, record->revision, event).ok())
            return false;
    }
    return true;
}
} // namespace

bool thread_membership_ui_adapter_begin(HWND window, bool (*refresh)(void*) noexcept,
                                        void* context) noexcept {
    if (thread_adapter || !window || application_thread_actions_metrics().owner_bytes)
        return false;
    auto* fixture = static_cast<ThreadAdapter*>(VirtualAlloc(nullptr, sizeof(ThreadAdapter),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!fixture)
        return false;
    fixture->ui = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!fixture->ui) {
        VirtualFree(fixture, 0, MEM_RELEASE);
        return false;
    }
    fixture->prior_ui = g_app.account_ui;
    fixture->prior_account = g_app.account;
    fixture->prior_view = g_app.message_view;
    fixture->prior_popup = g_app.popup_scene;
    fixture->prior_header = g_app.thread_header;
    fixture->prior_popup_owner = g_app.popup_owner_kind;
    fixture->prior_request_owner = g_app.message_history_request_owner;
    fixture->prior_details_owner = g_app.details_request_owner;
    fixture->prior_window = g_app.window;
    fixture->prior_scope = g_app.plugin_account_scope_serial;
    fixture->prior_request = g_app.primary_request_id;
    fixture->prior_details_request = g_app.details_request_id;
    fixture->prior_login_visible = g_app.login.visible;
    fixture->refresh = refresh;
    fixture->context = context;
    thread_adapter = fixture;
    g_app.account_ui = fixture->ui;
    g_app.account = {};
    g_app.account.instance = fixture;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 96020;
    g_app.account.api.thread_action = submit_thread;
    g_app.plugin_account_scope_serial = 96010;
    g_app.window = window;
    g_app.login.visible = false;
    g_app.message_view = {};
    g_app.popup_scene = {};
    g_app.thread_header = {};
    g_app.popup_owner_kind = PopupOwnerKind::none;
    g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
    g_app.primary_request_id = 0;
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    g_app.details_request_id = 0;
    if (!popup_scene_initialize(&g_app.popup_scene, {fixture->popup_items, kPopupCapacity},
            {fixture->popup_rows, kPopupCapacity},
            {fixture->popup_widgets, 2 + kPopupCapacity * kPopupWidgetsPerItem},
            {fixture->popup_nodes, 2 + kPopupCapacity * kPopupWidgetsPerItem}) || !seed_models(*fixture)) {
        thread_membership_ui_adapter_end();
        return false;
    }
    return true;
}

void thread_membership_ui_adapter_end() noexcept {
    if (!thread_adapter)
        return;
    auto* fixture = thread_adapter;
    // The offline owner is quiescent. Retire its exact shared lane before the
    // shipping release so an admitted receipt cannot retain freed model data.
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    g_app.details_request_id = 0;
    application_thread_actions_release();
    popup_scene_release(&g_app.popup_scene);
    g_app.account_ui = fixture->prior_ui;
    g_app.account = fixture->prior_account;
    g_app.message_view = fixture->prior_view;
    g_app.popup_scene = fixture->prior_popup;
    g_app.thread_header = fixture->prior_header;
    g_app.popup_owner_kind = fixture->prior_popup_owner;
    g_app.message_history_request_owner = fixture->prior_request_owner;
    g_app.primary_request_id = fixture->prior_request;
    g_app.details_request_owner = fixture->prior_details_owner;
    g_app.details_request_id = fixture->prior_details_request;
    g_app.plugin_account_scope_serial = fixture->prior_scope;
    g_app.window = fixture->prior_window;
    g_app.login.visible = fixture->prior_login_visible;
    thread_adapter = nullptr;
    VirtualFree(fixture->ui, 0, MEM_RELEASE);
    VirtualFree(fixture, 0, MEM_RELEASE);
}
bool thread_membership_ui_adapter_active() noexcept { return thread_adapter != nullptr; }
bool thread_membership_ui_adapter_refresh() noexcept {
    return !thread_adapter || !thread_adapter->refresh || thread_adapter->refresh(thread_adapter->context);
}
ChatStateStore* thread_membership_ui_adapter_state() noexcept {
    return thread_adapter ? &thread_adapter->ui->chat_state : nullptr;
}
UserStore* thread_membership_ui_adapter_users() noexcept {
    return thread_adapter ? &thread_adapter->ui->user_store : nullptr;
}
MessageStore* thread_membership_ui_adapter_store() noexcept {
    return thread_adapter ? &g_app.message_view.store : nullptr;
}
MessageRichStore* thread_membership_ui_adapter_rich_store() noexcept {
    return thread_adapter ? &thread_adapter->ui->message_rich_store : nullptr;
}
PopupScene* thread_membership_ui_adapter_popup() noexcept {
    return thread_adapter ? &g_app.popup_scene : nullptr;
}
UiThreadHeader* thread_membership_ui_adapter_header() noexcept {
    return thread_adapter ? &g_app.thread_header : nullptr;
}
ApplicationThreadActionView thread_membership_ui_adapter_view(u64 thread_id) noexcept {
    return application_thread_actions_view(thread_id);
}
void thread_membership_ui_adapter_bind_header(UiThreadHeader* header, UiRect viewport) noexcept {
    if (thread_adapter) {
        thread_adapter->header = header;
        thread_adapter->viewport = viewport;
        const auto host = header ? application_thread_actions_view(header->view.thread_id)
                                 : ApplicationThreadActionView{};
        thread_adapter->header_ticket = header &&
            application_thread_header_projection(host).source_publication == header->view.source_publication
            ? host.ticket : ApplicationThreadActionTicket{};
    }
}
bool thread_membership_ui_adapter_open_menu(u64 thread_id, UiRect anchor, UiRect viewport) noexcept {
    if (!thread_adapter)
        return false;
    const auto view = application_thread_actions_view(thread_id);
    const auto projected = application_thread_header_projection(view);
    PopupItem items[6]{};
    const u32 count = ui_thread_menu_build(projected, kMenuActions, {items, 6});
    if (!count || !application_thread_actions_current(view.ticket) ||
        !popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, thread_id, {items, count},
            anchor, viewport, 240.f, false, 0, 0, view.ticket.publication_revision).ok())
        return false;
    thread_adapter->menu_ticket = view.ticket;
    return thread_membership_ui_adapter_refresh();
}
ApplicationThreadHeaderActivationResult thread_membership_ui_adapter_activate(
    const UiThreadHeaderTarget& target) noexcept {
    if (!thread_adapter || !thread_adapter->header)
        return {ApplicationThreadActionEnqueueResult::invalid, false, false};
    return application_thread_header_activate(source(thread_adapter->header_ticket, thread_adapter->header), target);
}
ApplicationThreadHeaderActivationResult thread_membership_ui_adapter_menu_activate(u32 action) noexcept {
    if (!thread_adapter)
        return {ApplicationThreadActionEnqueueResult::invalid, false, false};
    return application_thread_menu_activate(source(thread_adapter->menu_ticket, nullptr), action);
}
UiThreadMenuActions thread_membership_ui_adapter_menu_actions() noexcept { return kMenuActions; }
bool thread_membership_ui_adapter_submit(u64 request_id) noexcept {
    if (!thread_adapter || !application_thread_actions_submit(request_id, clock_monotonic_milliseconds()))
        return false;
    g_app.details_request_owner = MessageHistoryRequestOwner::thread_actions;
    g_app.details_request_id = request_id;
    return true;
}
bool thread_membership_ui_adapter_response(u16 status, Utf8View body,
                                           RestClientError error) noexcept {
    if (!thread_adapter || !thread_adapter->flight_id)
        return false;
    RestClientResponse response{};
    response.request_id = thread_adapter->flight_id;
    response.status_code = status;
    response.error = error;
    response.body_bytes = body.bytes;
    thread_adapter->flight_id = 0;
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    g_app.details_request_id = 0;
    return application_thread_actions_response(response, body);
}
bool thread_membership_ui_adapter_flight(u64* request_id, ThreadActionRequest* request) noexcept {
    if (!thread_adapter || !thread_adapter->flight_id)
        return false;
    if (request_id)
        *request_id = thread_adapter->flight_id;
    if (request)
        *request = thread_adapter->flight;
    return true;
}
bool thread_membership_ui_adapter_synchronize() noexcept {
    return thread_adapter && application_thread_actions_synchronize();
}
ApplicationThreadActionMetrics thread_membership_ui_adapter_metrics() noexcept {
    return application_thread_actions_metrics();
}
bool thread_membership_ui_adapter_observe_membership(u64 thread_id, bool joined, bool known) noexcept {
    return thread_adapter && chat_state_store_set_thread_self_membership(
        &thread_adapter->ui->chat_state, thread_id, kThreadMembershipProbeSelf, joined, known) &&
        application_thread_actions_synchronize();
}
bool thread_membership_ui_adapter_observe_archive(u64 thread_id, bool archived) noexcept {
    return thread_adapter && chat_state_store_set_thread_archived(
        &thread_adapter->ui->chat_state, thread_id, kThreadMembershipProbeSelf, archived) &&
        application_thread_actions_synchronize();
}
namespace app_detail {
// UiProbe links the shared production projection/activation helpers while the
// unrelated full interaction TU stays in the shipping executable.
bool application_thread_actions_menu_open(const ApplicationThreadActionTicket& ticket,
                                           UiRect anchor) noexcept {
    return thread_adapter && application_thread_actions_current(ticket) &&
        thread_membership_ui_adapter_open_menu(ticket.thread_id, anchor, thread_adapter->viewport);
}
bool application_refresh_thread_actions_menu() noexcept {
    if (!thread_adapter || !g_app.popup_scene.active ||
        g_app.popup_scene.owner_id != thread_adapter->menu_ticket.thread_id)
        return true;
    if (!application_thread_actions_current(thread_adapter->menu_ticket))
        return popup_scene_close(&g_app.popup_scene);
    return true;
}
} // namespace app_detail
} // namespace maicord
