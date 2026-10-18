#include "reply_draft_ui_adapter.hpp"
#include "application_state.hpp"
#include "application_message_interaction.hpp"
#include "application_message_edit.hpp"
#include "application_message_delete.hpp"
#include "application_reply_draft.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/app/message_draft_persistence.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/button_keyboard.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace {
using namespace app_detail;
struct Adapter {
    Application prior;
    AccountUiRuntime* ui;
    RoleRecord roles[2];
    PermissionOverwriteRecord overwrites[2];
    u16 navigation_scratch[8];
    u8 navigation_visibility[8];
    ChannelSortKey navigation_keys[8];
    u8 pump_scratch[kMessageEventPumpPayloadCapacity];
    u64 bulk_ids[kGatewayBulkDeleteMaximumIds];
    u64 guild_ids[2];
    u32 structurals[512];
    char routed[4096];
    UiWidget popup_widgets[2 + kMessageContextMaximumPopupItems * kPopupWidgetsPerItem];
    UiLayoutNode popup_nodes[2 + kMessageContextMaximumPopupItems * kPopupWidgetsPerItem];
    wchar_t paths[2][kAccountProfileMaximumPathCharacters];
    bool second;
    bool (*refresh)(void*) noexcept;
    void* context;
};
Adapter* adapter;

RestClientSubmitResult WINAPI submit_delete(void*, u64, u64, u64) noexcept { return {}; }

bool seed() noexcept {
    auto& ui = *adapter->ui;
    auto& state = ui.chat_state;
    const u64 permissions = kPermissionViewChannel | kPermissionReadMessageHistory |
        kPermissionSendMessages | kPermissionManageMessages;
    if (!chat_state_store_initialize(&state, {ui.guild_record_storage, 2}, {ui.guild_order_storage, 2},
            {ui.channel_record_storage, 8}, {ui.channel_order_storage, 8}, {ui.chat_state_name_storage, 4096}) ||
        !chat_state_store_bind_permissions(&state, {adapter->roles, 2}, {adapter->overwrites, 2}))
        return false;
    state.self_user_id = kReplyDraftProbeSelf;
    if (!chat_state_store_upsert_guild(&state, {kReplyDraftProbeGuild,
            utf8_literal("Native reply drafts"), GuildMutationKind::create, 97999}).ok() ||
        !chat_state_store_upsert_role(&state, {kReplyDraftProbeGuild, kReplyDraftProbeGuild,
            permissions, utf8_literal("Members")}) ||
        !chat_state_store_set_self_roles(&state, kReplyDraftProbeGuild, 97999, kReplyDraftProbeSelf, {}))
        return false;
    const ChannelInput channels[]{
        {kReplyDraftProbeChannelA, kReplyDraftProbeGuild, 0, utf8_literal("design-room"), 0, 0, 0},
        {kReplyDraftProbeChannelB, kReplyDraftProbeGuild, 0, utf8_literal("draft-review"), 1, 0, 0}};
    for (const auto& channel : channels)
        if (!chat_state_store_upsert_channel(&state, channel).ok()) return false;
    chat_state_store_recompute_guild_permissions(&state, kReplyDraftProbeGuild);
    if (!user_store_initialize(&ui.user_store, {ui.user_record_storage, 4},
            {ui.user_order_storage, 4}, {ui.user_name_storage, 4096}) ||
        !user_store_upsert(&ui.user_store, kReplyDraftProbeSelf,
            utf8_literal("Native reviewer"), utf8_literal("reviewer")).ok() ||
        !user_store_upsert(&ui.user_store, kReplyDraftProbeAuthor,
            utf8_literal("Avery"), utf8_literal("avery")).ok()) return false;
    const MessageInput messages[]{
        {kReplyDraftProbeMessage, kReplyDraftProbeChannelA, kReplyDraftProbeAuthor,
            1'759'667'400'000ull, 0, utf8_literal("The original reply remains a separate message."), 0},
        {kReplyDraftProbeOwnMessage, kReplyDraftProbeChannelA, kReplyDraftProbeSelf,
            1'759'667'460'000ull, 0, utf8_literal("A message to edit while retaining the ordinary reply draft."), 0}};
    for (const auto& message : messages)
        if (!message_store_upsert(&g_app.message_view.store, message).ok()) return false;
    return message_view_runtime_reindex(&g_app.message_view, ui_extent_units(80), ui_extent_units(600), false).ok() &&
        navigation_view_initialize(&ui.navigation_view, {ui.navigation_guild_rows, 2},
            {ui.navigation_channel_rows, 8}, {adapter->navigation_scratch, 8},
            {adapter->navigation_visibility, 8}, {adapter->navigation_keys, 8});
}

bool configure(u64 channel) noexcept {
    auto& ui = *adapter->ui;
    ui.message_draft_session = {};
    if (!message_draft_store_initialize(&ui.message_drafts, {ui.message_draft_records, kMessageDraftCapacity},
            {ui.message_draft_text, kMessageDraftTextCapacity}) ||
        !message_draft_session_configure(ui.message_draft_session, ui.message_drafts,
            adapter->paths[adapter->second ? 1 : 0], adapter->second ? 97021 : 97020,
            clock_monotonic_milliseconds())) return false;
    ui.message_composer_controller = {};
    message_composer_reset(&g_app.message_composer, 0, false);
    return message_composer_controller_initialize(&ui.message_composer_controller, &g_app.message_composer,
        &ui.message_drafts, &g_app.message_view.timeline, {ui.message_request_text, kMessageRequestTextCapacity}) &&
        message_composer_controller_select_channel(&ui.message_composer_controller, channel).ok();
}

bool persist() noexcept {
    auto& ui = *adapter->ui;
    if (!message_composer_controller_commit_draft(&ui.message_composer_controller).ok()) return false;
    message_draft_session_persist(ui.message_draft_session, ui.message_drafts,
        clock_monotonic_milliseconds(), true);
    return ui.message_draft_session.persist_failures == 0 &&
        ui.message_draft_session.persisted_revision == ui.message_drafts.revision;
}
bool persisted_scope() noexcept {
    const HANDLE file = CreateFileW(adapter->paths[adapter->second ? 1 : 0], GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    u8 header[56]{};
    DWORD read = 0;
    const bool read_ok = ReadFile(file, header, sizeof(header), &read, nullptr) != FALSE;
    CloseHandle(file);
    return read_ok && read == sizeof(header) && byte_load_le32(header + 8) == kMessageDraftPersistenceVersion &&
        byte_load_le64(header + 32) == (adapter->second ? 97021ull : 97020ull);
}
} // namespace

bool reply_draft_ui_adapter_begin(HWND window, u32 dpi, bool (*refresh)(void*) noexcept, void* context) noexcept {
    if (adapter || !window || !dpi || !refresh) return false;
    auto* next = static_cast<Adapter*>(VirtualAlloc(nullptr, sizeof(Adapter), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!next) return false;
    next->ui = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!next->ui) { VirtualFree(next, 0, MEM_RELEASE); return false; }
    next->prior = g_app;
    next->refresh = refresh;
    next->context = context;
    adapter = next;
    g_app = {};
    g_app.window = window;
    g_app.account_ui = next->ui;
    g_app.account.instance = next;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 97020;
    g_app.account.api.message_delete = submit_delete;
    g_app.plugin_account_scope_serial = 97010;
    g_app.renderer.dpi = dpi;
    // The real input path performs its usual derived refresh; this adapter's
    // callback supplies native presentation using the single borrowed renderer.
    next->ui->text_rebuild_retry_at_ms = ~0ull;
    wchar_t root[kAccountProfileMaximumPathCharacters]{};
    bool paths = GetTempPathW(kAccountProfileMaximumPathCharacters, root) != 0;
    for (u32 index = 0; index < 2 && paths; ++index) {
        paths = GetTempFileNameW(root, L"mrd", 0, next->paths[index]) != 0;
        if (paths) paths = DeleteFileW(next->paths[index]) != FALSE;
    }
    const auto checked = [](bool ok, u32 stage) noexcept {
        if (!ok) log_u64(LogLevel::error, "probe", "reply_draft_adapter_initialize_failure_stage", stage);
        return ok;
    };
    if (!checked(paths, 1) || !checked(message_composer_initialize_owned(&g_app.message_composer), 2) ||
        !checked(message_composer_scene_initialize_owned(&g_app.message_composer_scene), 3) ||
        !checked(popup_scene_initialize_owned(&g_app.popup_scene, kMessageContextMaximumPopupItems,
            {next->popup_widgets, 2 + kMessageContextMaximumPopupItems * kPopupWidgetsPerItem},
            {next->popup_nodes, 2 + kMessageContextMaximumPopupItems * kPopupWidgetsPerItem}), 4) ||
        !checked(message_view_runtime_initialize(&g_app.message_view, ui_extent_units(80), ui_extent_units(600), false).ok(), 5) ||
        !checked(seed(), 6) || !checked(configure(kReplyDraftProbeChannelA), 7) ||
        !checked(message_event_pump_initialize(&g_app.message_pump, &g_app.message_view.timeline,
            {next->pump_scratch, sizeof(next->pump_scratch)}, {next->bulk_ids, kGatewayBulkDeleteMaximumIds},
            MessageContentAvailability::available), 8) ||
        !checked(message_event_pump_set_state_store(&g_app.message_pump, &next->ui->chat_state, {next->guild_ids, 2}), 9) ||
        !checked(message_edit_runtime_initialize(&g_app.message_edit), 11) ||
        !checked(reply_draft_ui_adapter_select(kReplyDraftProbeChannelA), 10)) {
        reply_draft_ui_adapter_end(); return false;
    }
    message_event_pump_set_reaction_observers(&g_app.message_pump, nullptr, nullptr,
        +[](void*, u64 channel, u64 message) noexcept { application_reply_draft_deleted(channel, message); }, nullptr);
    return true;
}

void reply_draft_ui_adapter_end() noexcept {
    if (!adapter) return;
    auto* prior = adapter;
    reset_message_delete();
    reset_message_edit();
    message_edit_runtime_release(&g_app.message_edit);
    popup_scene_release(&g_app.popup_scene);
    message_composer_scene_release(&g_app.message_composer_scene);
    message_composer_release(&g_app.message_composer);
    message_view_runtime_release(&g_app.message_view);
    g_app = prior->prior;
    adapter = nullptr;
    for (u32 index = 0; index < 2; ++index)
        if (prior->paths[index][0]) DeleteFileW(prior->paths[index]);
    VirtualFree(prior->ui, 0, MEM_RELEASE);
    VirtualFree(prior, 0, MEM_RELEASE);
}

ChatStateStore* reply_draft_ui_adapter_state() noexcept { return adapter ? &adapter->ui->chat_state : nullptr; }
bool reply_draft_ui_adapter_active() noexcept { return adapter != nullptr; }
UserStore* reply_draft_ui_adapter_users() noexcept { return adapter ? &adapter->ui->user_store : nullptr; }
MessageComposer* reply_draft_ui_adapter_composer() noexcept { return adapter ? &g_app.message_composer : nullptr; }
MessageComposerScene* reply_draft_ui_adapter_scene() noexcept { return adapter ? &g_app.message_composer_scene : nullptr; }
MessageComposerController* reply_draft_ui_adapter_controller() noexcept { return adapter ? &adapter->ui->message_composer_controller : nullptr; }
MessageTimeline* reply_draft_ui_adapter_timeline() noexcept { return adapter ? &g_app.message_view.timeline : nullptr; }
PopupScene* reply_draft_ui_adapter_popup() noexcept { return adapter ? &g_app.popup_scene : nullptr; }
MessageComposerReplySnapshot reply_draft_ui_adapter_reply() noexcept { return adapter ? app_detail::application_reply_draft() : MessageComposerReplySnapshot{}; }
Utf8View reply_draft_ui_adapter_label(Slice<char> storage) noexcept { return adapter ? app_detail::application_reply_draft_label(storage) : Utf8View{}; }
bool reply_draft_ui_adapter_editing() noexcept { return adapter && g_app.message_edit.editor_active; }
bool reply_draft_ui_adapter_refresh() noexcept { return adapter && adapter->refresh(adapter->context); }
bool reply_draft_ui_adapter_begin_reply(u64 id) noexcept {
    return adapter && app_detail::begin_message_reply(id) && reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_select(u64 channel) noexcept {
    if (!adapter || !chat_state_store_select_channel(&adapter->ui->chat_state, channel).ok() ||
        !message_composer_controller_select_channel(&adapter->ui->message_composer_controller, channel).ok()) return false;
    adapter->ui->active_channel_id = channel;
    if (!navigation_view_rebuild(&adapter->ui->navigation_view, adapter->ui->chat_state).ok()) return false;
    g_app.message_composer.focused = false;
    g_app.context_focus = nullptr;
    return reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_text(Utf8View text) noexcept {
    return adapter && message_composer_restore(&g_app.message_composer, g_app.message_composer.channel_id,
        text, text.bytes, text.bytes) &&
        message_composer_controller_commit_draft(&adapter->ui->message_composer_controller).ok() &&
        reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_restart() noexcept {
    if (!adapter || !persist() || !persisted_scope()) return false;
    const u64 channel = g_app.message_composer.channel_id;
    return configure(channel) && reply_draft_ui_adapter_select(channel);
}
bool reply_draft_ui_adapter_profile(bool second) noexcept {
    if (!adapter || !persist()) return false;
    adapter->second = second;
    g_app.account.status.profile_id = second ? 97021 : 97020;
    ++g_app.plugin_account_scope_serial;
    return configure(kReplyDraftProbeChannelA) && reply_draft_ui_adapter_select(kReplyDraftProbeChannelA);
}
bool reply_draft_ui_adapter_evict_source() noexcept {
    return adapter && message_store_delete(&g_app.message_view.store, kReplyDraftProbeMessage) &&
        message_view_runtime_reindex(&g_app.message_view, ui_extent_units(80), ui_extent_units(600), false).ok() &&
        reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_author(Utf8View name) noexcept {
    return adapter && user_store_upsert(&adapter->ui->user_store, kReplyDraftProbeAuthor,
        name, utf8_literal("avery")).ok() && reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_delete_rest(u64 id) noexcept {
    if (!adapter || !queue_message_delete(id)) return false;
    RestClientResponse response{};
    response.status_code = 204;
    return accept_message_delete_response(response) && reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_gateway(Utf8View json) noexcept {
    if (!adapter) return false;
    GatewayEnvelope envelope{};
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    return gateway_decode_envelope(json, {adapter->structurals, 512}, 32, &envelope).ok() &&
        envelope.has_event_name && gateway_client_route_dispatch(envelope, {adapter->structurals, 512}, 32,
            {adapter->routed, sizeof(adapter->routed)}, {adapter->bulk_ids, kGatewayBulkDeleteMaximumIds},
            adapter->ui->chat_state.self_user_id, &event, &payload).ok() &&
        message_event_pump_consume(&g_app.message_pump, event, payload).ok() && reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_edit(bool begin) noexcept {
    return adapter && (begin ? begin_message_edit(kReplyDraftProbeOwnMessage) : cancel_message_edit_editor()) &&
        reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_mouse(MessageComposerControl control, u32 dpi) noexcept {
    if (!adapter || !dpi) return false;
    const auto* node = message_composer_scene_layout_node(g_app.message_composer_scene, control);
    if (!node || !ui_rect_has_area(node->rectangle)) return false;
    g_app.renderer.dpi = dpi;
    const u32 x = static_cast<u32>((node->rectangle.left + node->rectangle.right) * .5f * dpi / 96.f + .5f);
    const u32 y = static_cast<u32>((node->rectangle.top + node->rectangle.bottom) * .5f * dpi / 96.f + .5f);
    const UiPoint point{static_cast<float>(x) * 96.f / dpi, static_cast<float>(y) * 96.f / dpi};
    auto& owner = g_app.message_composer_scene.widgets;
    if (message_composer_scene_hit_test(g_app.message_composer_scene, point.x, point.y) != control) return false;
    const auto press = ui_button_pointer(&owner, UiButtonPointerEvent::press, point);
    const auto release = ui_button_pointer(&owner, UiButtonPointerEvent::release, point);
    if (!press.handled || !release.handled || !release.activate ||
        press.target.slot != release.target.slot || press.target.generation != release.target.generation ||
        !ui_widget_manager_input_enabled(owner, release.target) ||
        !message_composer_scene_focus_control(&g_app.message_composer_scene, control)) return false;
    g_app.context_focus = &owner;
    return application_reply_draft_activate(control) && reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_key(MessageComposerControl control, u32 key) noexcept {
    if (!adapter || !message_composer_scene_focus_control(&g_app.message_composer_scene, control)) return false;
    auto& owner = g_app.message_composer_scene.widgets;
    g_app.context_focus = &owner;
    g_app.message_composer.focused = control == MessageComposerControl::edit;
    if (control == MessageComposerControl::edit && key == VK_ESCAPE)
        return application_reply_draft_activate(MessageComposerControl::cancel_reply) && reply_draft_ui_adapter_refresh();
    const auto logical = key == VK_SPACE ? UiButtonKey::space : key == VK_RETURN ? UiButtonKey::enter : UiButtonKey::other;
    const auto ticket = owner.focused;
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    u32 activated = 0;
    const auto dispatch = [&](UiButtonKeyResult result) noexcept {
        if (!result.activate) return true;
        if (result.target.slot != ticket.slot || result.target.generation != ticket.generation ||
            !ui_widget_manager_input_enabled(owner, result.target) ||
            message_composer_scene_focused_control(g_app.message_composer_scene) != control ||
            !application_reply_draft_activate(control)) return false;
        ++activated;
        return true;
    };
    const auto down = ui_button_keyboard_key(&g_app.button_keyboard, &owner, logical, true, false);
    if (!dispatch(down)) return false;
    const auto up = ui_button_keyboard_key(&g_app.button_keyboard, &owner, logical, false, false);
    return down.handled && dispatch(up) && activated == 1 && reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_failed_menu(UiRect anchor, UiRect viewport) noexcept {
    if (!adapter) return false;
    char bytes[96]{};
    MessageComposer external{};
    struct Release { MessageComposer& model; ~Release() noexcept { message_composer_release(&model); } } release{external};
    if (!message_composer_initialize(&external, {bytes, sizeof(bytes)})) return false;
    const auto reply = application_reply_draft();
    message_composer_reset(&external, reply.channel_id, false);
    if (!reply.message_id || message_composer_insert_utf8(&external,
            utf8_literal("A queued reply retains its original text after its target is deleted.")) != MessageComposerResult::changed)
        return false;
    auto& controller = adapter->ui->message_composer_controller;
    const auto queued = message_composer_controller_queue_send(&controller, true, kReplyDraftProbeSelf,
        1'759'667'490'000ull, 1100, reply.message_id, {}, {}, &external, {}, reply.mention_reply_author, &reply);
    if (!queued.ok() || !message_composer_controller_submit_failed(&controller, queued.nonce, 1101).ok()) return false;
    const auto found = message_store_find(g_app.message_view.store, queued.nonce);
    const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!record || !message_context_controller_open(&g_app.message_context, &g_app.popup_scene, *record,
            false, false, false, nullptr, anchor, viewport).ok()) return false;
    g_app.popup_owner_kind = PopupOwnerKind::message;
    return reply_draft_ui_adapter_refresh();
}
bool reply_draft_ui_adapter_deleted_menu() noexcept {
    if (!adapter || !g_app.popup_scene.active ||
        !application_reply_request_deleted(g_app.message_composer.channel_id, g_app.popup_scene.owner_id)) return false;
    bool row = false;
    for (u32 index = 0; index < g_app.popup_scene.item_count; ++index) {
        const auto& item = g_app.popup_scene.items[index];
        if (item.action != static_cast<u32>(MessageContextAction::retry_send)) continue;
        row = !(item.flags & popup_item_enabled) && utf8_equal(item.label, utf8_literal("Original message was deleted")) &&
            !ui_widget_manager_input_enabled(g_app.popup_scene.widgets, g_app.popup_scene.rows[index].widget);
    }
    const auto retry = message_composer_controller_retry(&adapter->ui->message_composer_controller,
        g_app.popup_scene.owner_id, 1102);
    return row && retry.error == MessageComposerControllerError::reply_unavailable;
}
void reply_draft_ui_adapter_close_menu() noexcept {
    if (!adapter) return;
    popup_scene_close(&g_app.popup_scene);
    message_context_controller_close(&g_app.message_context);
    g_app.popup_owner_kind = PopupOwnerKind::none;
}
} // namespace maicord
