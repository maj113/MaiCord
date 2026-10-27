// Exercise the shipping owner directly without adding production test hooks.
#include "../src/app/application_pinned_messages.cpp"
#include "test_output.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord::pins_runtime_test {
using namespace app_detail;
struct AccountFixture {
    MessagePinsRequest request{};
    u64 request_id{}, cancelled_id{};
    u32 submissions{}, cancellations{};
    RestClientSubmitError admission{};
} account;
u32 failures{}, navigations{};
u64 navigated_channel{}, navigated_message{};
ApplicationTextFormRequest confirmation{};
u8 form_marker{};
BYTE saved_keyboard[256]{};
bool keyboard_saved{};
MessageRecord records[4]{};
u32 order[4]{};
u8 text[4096]{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
RestClientSubmitResult WINAPI submit(void*, const MessagePinsRequest* request,
                                     u64 request_id) noexcept {
    if (!request) {
        ++account.cancellations;
        account.cancelled_id = request_id;
        return {};
    }
    ++account.submissions;
    account.request = *request;
    account.request_id = request_id;
    return {account.admission};
}
bool response(u64 ticket, u16 status, Utf8View body = {}) noexcept {
    RestClientResponse value{};
    value.request_id = ticket;
    value.status_code = status;
    return application_pinned_messages_response(value, body);
}
LPARAM point(UiRect bounds) noexcept {
    return MAKELPARAM(static_cast<short>((bounds.left + bounds.right) * .5f),
                      static_cast<short>((bounds.top + bounds.bottom) * .5f));
}
bool header_click() noexcept {
    const auto bounds = ui_scene_header_geometry(g_app.shell_scene).pins;
    if (!ui_rect_has_area(bounds) ||
        !application_pinned_messages_window_message(WM_LBUTTONDOWN, 0, point(bounds))) return false;
    application_pinned_messages_window_message(WM_MOUSEMOVE, 0, point(bounds));
    if (application_pinned_messages_active() || !application_pinned_messages_synchronize() ||
        !g_app.pinned_messages || !g_app.pinned_messages->captured) return false;
    return application_pinned_messages_window_message(WM_LBUTTONUP, 0, point(bounds));
}
UiRect action_bounds(UiPinnedMessagesAction action, u64 message) noexcept {
    const auto* scene = g_app.renderer.pinned_messages_scene;
    if (!scene) return {};
    for (u32 slot = 0; slot < scene->widgets.count; ++slot) {
        if (scene->targets[slot].action != action || scene->targets[slot].message_id != message) continue;
        const auto* node = ui_widget_manager_layout_node(scene->widgets,
            {static_cast<u16>(slot), scene->widgets.generation});
        if (node) return node->rectangle;
    }
    return {};
}
bool layout(float width, float height) noexcept {
    auto metrics = kGuildChannelShellLayoutMetrics;
    if (application_pinned_messages_available())
        metrics.flags = static_cast<u16>(metrics.flags | ui_shell_layout_pins);
    return ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel,
                                        metrics, width, height);
}
void permit(bool read = true, bool pin = true) noexcept {
    auto& channel = g_app.account_ui->channel_record_storage[0];
    channel.permissions_resolved = true;
    channel.effective_permissions = kPermissionViewChannel |
        (read ? kPermissionReadMessageHistory : 0) |
        (pin ? kPermissionPinMessages : 0);
    channel.actions = channel_action_view |
        (read ? channel_action_read_history : 0) |
        (pin ? channel_action_pin_messages : 0);
    ++channel.revision;
    ++g_app.account_ui->chat_state.permission_mutations;
    layout(static_cast<float>(g_app.renderer.width), static_cast<float>(g_app.renderer.height));
}
bool initialize() noexcept {
    BYTE clean_keyboard[256]{};
    keyboard_saved = GetKeyboardState(saved_keyboard) != FALSE;
    if (!keyboard_saved || !SetKeyboardState(clean_keyboard)) return false;
    g_app.window = CreateWindowExW(0, L"STATIC", L"MaiCord pins runtime fixture", WS_POPUP,
                                   -16000, -16000, 1280, 720, nullptr, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.window || !g_app.account_ui) return false;
    auto& ui = *g_app.account_ui;
    if (!chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 2},
                                        {ui.guild_order_storage, 2}, {ui.channel_record_storage, 4},
                                        {ui.channel_order_storage, 4}, {ui.chat_state_name_storage, 4096}) ||
        !user_store_initialize(&ui.user_store, {ui.user_record_storage, 4},
                                       {ui.user_order_storage, 4}, {ui.user_name_storage, 4096}) ||
        !chat_state_store_upsert_guild(&ui.chat_state,
            {7, utf8_literal("Pinned fixture guild"), GuildMutationKind::create, 5}).ok() ||
        !chat_state_store_upsert_channel(&ui.chat_state,
            {70, 7, 0, utf8_literal("fixture-channel"), 0, 0, 0}).ok() ||
        !chat_state_store_upsert_channel(&ui.chat_state,
            {80, 0, 0, utf8_literal("fixture-dm"), 0, 0, 1}).ok() ||
        !ui_scene_initialize_bootstrap(&g_app.shell_scene) ||
        !ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel,
                                      kGuildChannelShellLayoutMetrics, 1280, 720) ||
        !application_history_decode_storage_initialize(ui) ||
        !message_store_initialize(&g_app.message_view.store, {records, 4}, {order, 4}, {text, 4096}) ||
        !message_store_upsert(&g_app.message_view.store,
            {900, 70, 5, 0, 0, utf8_literal("Original timeline message"), 0}).ok()) return false;
    ui.chat_state.self_user_id = 5;
    ui.chat_state.selected_guild_id = 7;
    ui.chat_state.selected_channel_id = 70;
    ui.active_channel_id = 70;
    ui.user_record_storage[0].id = 5;
    ui.user_record_storage[0].occupied = true;
    ui.user_order_storage[0] = 0;
    ui.user_store.count = 1;
    g_app.renderer_ready = true;
    g_app.renderer.width = 1280;
    g_app.renderer.height = 720;
    g_app.renderer.dpi = 96;
    g_app.renderer.ui_scene = &g_app.shell_scene;
    g_app.account.instance = &account;
    g_app.account.status.profile_id = 1;
    g_app.account.status.running = true;
    g_app.plugin_account_scope_serial = 1;
    g_app.account.api.pinned_messages = submit;
    permit();
    return true;
}
void release() noexcept {
    application_pinned_messages_release();
    ui_scene_release(&g_app.shell_scene);
    if (g_app.window) DestroyWindow(g_app.window);
    if (g_app.account_ui) {
        application_history_decode_storage_release(*g_app.account_ui);
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    }
    g_app.window = nullptr;
    g_app.account_ui = nullptr;
    g_app.account.instance = nullptr;
    g_app.text_form = nullptr;
    if (keyboard_saved) SetKeyboardState(saved_keyboard);
}
constexpr auto page_body = utf8_literal(
    R"({"items":[{"pinned_at":"2026-10-05T12:34:56.123456Z","message":{"id":"300","channel_id":"70","author":{"id":"3","username":"Original author"},"content":"Copied original pin","timestamp":"2026-10-05T12:00:00Z","type":0}}],"has_more":false})");
void run() noexcept {
    check(initialize(), "pins runtime: hidden native host and real model initialize");
    if (!g_app.window || !g_app.account_ui) { release(); return; }
    permit(false, false);
    check(!application_pinned_messages_available() && !application_pinned_messages_toggle() &&
          !g_app.pinned_messages && !account.submissions,
          "pins runtime: missing history authority cannot allocate panel or submit");
    permit();
    const auto initial_header = ui_scene_header_geometry(g_app.shell_scene).pins;
    const bool scope_press = ui_rect_has_area(initial_header) &&
        application_pinned_messages_window_message(WM_LBUTTONDOWN, 0, point(initial_header));
    ++g_app.plugin_account_scope_serial;
    check(scope_press && application_pinned_messages_synchronize() &&
          g_app.pinned_messages && !g_app.pinned_messages->captured &&
          !application_pinned_messages_window_message(WM_LBUTTONUP, 0, point(initial_header)) &&
          !application_pinned_messages_active() && !account.submissions,
          "pins runtime: account scope change between header press and release cancels activation");
    check(header_click() && application_pinned_messages_active() &&
          g_app.renderer.pinned_messages_scene && application_pinned_messages_pending(1),
          "pins runtime: real header down-hover-frame-up opens once and queues bounded fetch");
    if (!g_app.pinned_messages) { release(); return; }
    auto& owner = *g_app.pinned_messages;
    g_app.renderer.width = 640;
    layout(640, 720);
    check(application_pinned_messages_synchronize() && application_pinned_messages_read_blocked(),
          "pins runtime: narrow overlay blocks background conversation reading");
    const auto composer_bounds = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::composer);
    g_app.context_focus = nullptr;
    g_app.message_composer.focused = true;
    const bool composer_passes = owner.scene.viewport.bottom <= composer_bounds.top &&
        !application_pinned_messages_window_message(WM_LBUTTONDOWN, 0, point(composer_bounds)) &&
        !application_pinned_messages_input(UiInputKind::character, 'a', false) &&
        !application_pinned_messages_input(UiInputKind::key_down, VK_RETURN, false) &&
        g_app.message_composer.focused;
    g_app.context_focus = &owner.scene.widgets;
    g_app.message_composer.focused = false;
    check(composer_passes && application_pinned_messages_input(UiInputKind::character, 'a', false),
          "pins runtime: narrow pane preserves composer bounds and input while pin focus alone captures characters");
    g_app.renderer.width = 1280;
    layout(1280, 720);
    check(application_pinned_messages_synchronize() && !application_pinned_messages_read_blocked(),
          "pins runtime: wide side panel restores independent conversation reading");
    account.admission = RestClientSubmitError::busy;
    check(!application_pinned_messages_submit(10, 1) && owner.controller.queued &&
          !owner.controller.request_id,
          "pins runtime: shared DETAILS busy keeps unsent fetch intact");
    account.admission = RestClientSubmitError::none;
    check(application_pinned_messages_submit(10, 1) && account.request.query.channel_id == 70 &&
          account.request.operation == MessagePinsOperation::fetch && !application_pinned_messages_can_pin(900),
          "pins runtime: exact account request fences mutation while a fetch is outstanding");
    check(!response(99, 200, page_body) && owner.controller.request_id == 10,
          "pins runtime: unrelated shared-lane receipt is never consumed");
    check(response(10, 200, page_body) && owner.controller.authoritative && owner.controller.page.count == 1 &&
          owner.controller.rows[0].message_id == 300 && application_pinned_messages_can_pin(300),
          "pins runtime: production codec publishes original pin identity into owned page");
    const u32 stable_content_revision = owner.content.revision;
    const u32 stable_scene_revision = owner.scene.revision;
    const u32 stable_source_revision = owner.source.revision;
    bool stable_frames = true;
    for (u32 frame = 0; frame < 4; ++frame) stable_frames = application_pinned_messages_synchronize() && stable_frames;
    check(stable_frames && owner.content.revision == stable_content_revision &&
          owner.scene.revision == stable_scene_revision && owner.source.revision == stable_source_revision,
          "pins runtime: unchanged frames preserve content scene and source revisions");
    FillMemory(g_app.account_ui->event_payload_scratch, kGatewayClientDispatchPayloadCapacity, 'x');
    check(utf8_equal(pinned_message_content(owner.controller.rows[0]), utf8_literal("Copied original pin")) &&
          utf8_equal(pinned_message_author(owner.controller.rows[0]), utf8_literal("Original author")) &&
          g_app.message_view.store.count == 1,
          "pins runtime: owned page survives shared decode scratch reuse without entering conversation store");
    owner.controller.rows[0].type = 6;
    check(!application_pinned_messages_can_pin(300) && !application_pinned_messages_action(300, false),
          "pins runtime: a pinned-message system notification cannot itself be pinned or unpinned");
    owner.controller.rows[0].type = 0;
    check(message_store_set_delivery(&g_app.message_view.store, 900, MessageDeliveryState::pending) &&
          !application_pinned_messages_can_pin(900),
          "pins runtime: unsent timeline message cannot be pinned");
    message_store_set_delivery(&g_app.message_view.store, 900, MessageDeliveryState::authoritative);
    message_store_upsert(&g_app.message_view.store, {901, 80, 5, 0, 0, utf8_literal("Foreign message"), 0});
    check(!application_pinned_messages_can_pin(901), "pins runtime: foreign-channel timeline target is rejected");
    const auto jump_bounds = action_bounds(UiPinnedMessagesAction::jump, 300);
    check(ui_rect_has_area(jump_bounds) &&
          application_pinned_messages_window_message(WM_LBUTTONDOWN, 0, point(jump_bounds)) &&
          application_pinned_messages_window_message(WM_MOUSEMOVE, 0, point(jump_bounds)) &&
          application_pinned_messages_window_message(WM_LBUTTONUP, 0, point(jump_bounds)) && navigations == 1,
          "pins runtime: native press survives hover-only scene revision before original-message Jump");
    check(ui_pinned_messages_scene_focus(&owner.scene, {UiPinnedMessagesAction::jump, 70, 300}) &&
          application_pinned_messages_input(UiInputKind::key_down, VK_RETURN, false) &&
          application_pinned_messages_input(UiInputKind::key_up, VK_RETURN, false) &&
          navigations == 2 && navigated_channel == 70 && navigated_message == 300,
          "pins runtime: native scene keyboard action navigates original channel and message IDs");
    check(application_pinned_messages_action(900, true), "pins runtime: native confirmation opens for cancellation");
    const u32 before_cancelled_prompt = account.submissions;
    application_text_form_close(false, false);
    message_store_set_presentation_flags(&g_app.message_view.store, 900, message_presentation_mentions_current_user);
    check(application_pinned_messages_synchronize() && !owner.confirming &&
          !application_pinned_messages_pending(1) && account.submissions == before_cancelled_prompt,
          "pins runtime: dismissing confirmation never queues a hidden Pin mutation");
    check(application_pinned_messages_action(900, true) && confirmation.confirmation_only &&
          confirmation.valid && confirmation.save && confirmation.valid(confirmation.owner) &&
          !application_pinned_messages_pending(1),
          "pins runtime: Pin opens scoped native confirmation without submitting wire mutation");
    const auto obsolete = confirmation;
    permit(true, false);
    check(!obsolete.valid(obsolete.owner) && !obsolete.save(obsolete.owner, {}) &&
          ui_rect_has_area(ui_scene_header_geometry(g_app.shell_scene).pins),
          "pins runtime: lost pin permission rejects Save while retained read authority keeps browse launcher");
    application_text_form_close(false, false);
    application_pinned_messages_synchronize();
    permit();
    application_pinned_messages_synchronize();
    check(application_pinned_messages_action(900, true) && confirmation.save(confirmation.owner, {}),
          "pins runtime: fresh authorized confirmation queues Pin once");
    application_text_form_close(false, false);
    check(application_pinned_messages_submit(11, 1) && account.request.operation == MessagePinsOperation::pin &&
          account.request.message_id == 900 && !application_pinned_messages_action(900, false, false),
          "pins runtime: confirmed Pin uses original target and rejects concurrent mutations");
    check(response(11, 204) && application_pinned_messages_pending(1) &&
          application_pinned_messages_submit(12, 1) && response(12, 200, page_body),
          "pins runtime: successful mutation refetches authoritative page before another action");
    check(records[0].presentation_flags == (message_presentation_mentions_current_user | message_presentation_pinned),
          "pins runtime: successful Pin receipt adds resident pin flag without clearing mention state");
    for (u32 kind = 0; kind < 3; ++kind) {
        const u64 ticket = 40 + kind;
        check(application_pinned_messages_action(900, false, false) &&
              application_pinned_messages_submit(ticket, 1),
              "pins runtime: explicit mutation seeds terminal 429 receipt fixture");
        RestClientResponse limited{};
        limited.request_id = ticket;
        limited.status_code = 429;
        limited.rate_limited = kind != 2;
        limited.rate.retry_after_ms = 600'000;
        limited.retry_at_ms = kind ? clock_monotonic_milliseconds() + 60'000 : 0;
        limited.rejection.captcha = kind == 0;
        limited.rejection.has_code = kind == 1;
        limited.rejection.code = kind == 1 ? 40004 : 0;
        const u32 submissions = account.submissions;
        check(application_pinned_messages_response(limited, {}) &&
              owner.controller.failure == PinnedMessageFailure::rejected &&
              !owner.controller.request_id && !owner.controller.queued && !owner.controller.retry_at_ms &&
              !application_pinned_messages_next_wake() &&
              !application_pinned_messages_pending(~u64{0}) &&
              !application_pinned_messages_submit(100 + ticket, ~u64{0}) &&
              account.submissions == submissions &&
              records[0].presentation_flags == (message_presentation_mentions_current_user | message_presentation_pinned),
              "pins runtime: challenge and nonauthoritative 429 receipts ignore raw Retry-After without queued mutation wake or readmission");
    }
    check(application_pinned_messages_action(900, false, false) && application_pinned_messages_submit(50, 1),
          "pins runtime: explicit mutation seeds genuine authoritative rate limit");
    RestClientResponse limited{};
    limited.request_id = 50;
    limited.status_code = 429;
    limited.rate_limited = true;
    limited.retry_at_ms = clock_monotonic_milliseconds() + 60'000;
    limited.rate.retry_after_ms = 600'000;
    const u32 limited_submissions = account.submissions;
    check(application_pinned_messages_response(limited, {}) && owner.controller.queued &&
          owner.controller.failure == PinnedMessageFailure::rate_limited &&
          owner.controller.retry_at_ms == limited.retry_at_ms &&
          application_pinned_messages_next_wake() == limited.retry_at_ms &&
          !application_pinned_messages_pending(limited.retry_at_ms - 1) &&
          !application_pinned_messages_submit(51, limited.retry_at_ms - 1) &&
          account.submissions == limited_submissions,
          "pins runtime: genuine authority flag schedules the exact server deadline without reconstructing it from raw headers");
    check(application_pinned_messages_submit(51, limited.retry_at_ms) &&
          account.submissions == limited_submissions + 1 && response(51, 400) &&
          !application_pinned_messages_next_wake() && !owner.controller.queued,
          "pins runtime: genuine rate limit readmits one mutation at its deadline and a terminal response ends the retry");
    message_store_upsert(&g_app.message_view.store,
        {300, 70, 3, 0, 0, utf8_literal("Resident original pin"), 0,
         MessageContentAvailability::available, MessageDeliveryState::authoritative,
         message_presentation_mentions_current_user | message_presentation_pinned});
    check(application_pinned_messages_action(300, false) && confirmation.valid(confirmation.owner) &&
          confirmation.save(confirmation.owner, {}),
          "pins runtime: fetched row Unpin uses the same scoped native confirmation validator");
    application_text_form_close(false, false);
    check(application_pinned_messages_submit(13, 1) && account.request.operation == MessagePinsOperation::unpin &&
          account.request.message_id == 300 && response(13, 204),
          "pins runtime: confirmed Unpin sends exact original pinned-row identity");
    const auto unpinned = message_store_find(g_app.message_view.store, 300);
    const auto* unpinned_record = unpinned.ok() ? message_store_resolve(g_app.message_view.store, unpinned.handle) : nullptr;
    check(unpinned_record && unpinned_record->presentation_flags == message_presentation_mentions_current_user,
          "pins runtime: successful Unpin receipt removes resident pin flag while preserving mentions");
    check(application_pinned_messages_submit(14, 1) && response(14, 200, page_body) &&
          application_pinned_messages_action(900, false, false) && application_pinned_messages_submit(16, 1),
          "pins runtime: next resident Unpin submits against current authoritative generation");
    application_pinned_messages_gateway(70);
    UiPinnedMessagesRow retained{};
    check(application_pinned_messages_synchronize() && owner.source.resolve(owner.source.context, 0, &retained) &&
          !retained.jump_enabled && !ui_pinned_messages_scene_focus(&owner.scene,
              {UiPinnedMessagesAction::jump, 70, 300}),
          "pins runtime: invalidated retained row remains readable but cannot navigate before authoritative refresh");
    message_store_set_presentation_flags(&g_app.message_view.store, 900,
        message_presentation_mentions_current_user | message_presentation_pinned);
    check(response(16, 204) && records[0].presentation_flags ==
          (message_presentation_mentions_current_user | message_presentation_pinned) && application_pinned_messages_pending(1),
          "pins runtime: newer Gateway invalidation prevents older Unpin receipt overriding resident state");
    check(application_pinned_messages_submit(17, 1), "pins runtime: authoritative refresh submits");
    ++g_app.plugin_account_scope_serial;
    check(application_pinned_messages_synchronize() && !application_pinned_messages_active() &&
          account.cancelled_id == 17 && !g_app.renderer.pinned_messages_scene,
          "pins runtime: account scope replacement cancels exact submitted ticket and retires borrowed projection");
    check(response(17, 200, page_body) && !owner.controller.request_id && !owner.controller.authoritative,
          "pins runtime: stale successful receipt drains ownership without publishing foreign-account page");
    check(!obsolete.valid(obsolete.owner), "pins runtime: old confirmation cannot revive across account scope");
    check(application_pinned_messages_toggle() && application_pinned_messages_submit(15, 1) && response(15, 403) &&
          owner.controller.read_denied && !application_pinned_messages_can_pin(900),
          "pins runtime: server history denial overrides unchanged cached channel authority");
    const u32 before_retry = account.submissions;
    const bool retry_activated = ui_pinned_messages_scene_focus(&owner.scene, {UiPinnedMessagesAction::retry, 70, 0}) &&
        application_pinned_messages_input(UiInputKind::key_down, VK_RETURN, false) &&
        application_pinned_messages_input(UiInputKind::key_up, VK_RETURN, false);
    check(retry_activated && application_pinned_messages_pending(1) && account.submissions == before_retry &&
          application_pinned_messages_submit(18, 1) && account.submissions == before_retry + 1 &&
          account.request.operation == MessagePinsOperation::fetch && response(18, 403) &&
          application_pinned_messages_synchronize() && application_pinned_messages_synchronize() &&
          owner.controller.read_denied && !application_pinned_messages_pending(1) &&
          account.submissions == before_retry + 1,
          "pins runtime: explicit read Retry submits one fetch while repeated denied frames never resubmit");
    application_pinned_messages_close();
    check(!application_pinned_messages_active() && !application_pinned_messages_next_wake(),
          "pins runtime: close retires scene and retry deadlines");
    release();
    check(!g_app.pinned_messages && !g_app.renderer.pinned_messages_scene,
          "pins runtime: release clears lazy owner and borrowed renderer projection");
}
} // namespace maicord::pins_runtime_test

namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept { return true; }
TextSystem renderer_text_system(D3D11Renderer*, UiTextFace) noexcept { return {}; }
namespace app_detail {
bool refresh_account_text(bool) noexcept { return true; }
bool application_settings_active() noexcept { return false; }
bool application_friends_active() noexcept { return false; }
void application_composer_text_input_suspend() noexcept {}
bool application_search_close_panel() noexcept { return true; }
bool application_navigate_notification(u64 channel, u64 message) noexcept {
    ++pins_runtime_test::navigations;
    pins_runtime_test::navigated_channel = channel;
    pins_runtime_test::navigated_message = message;
    return true;
}
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    pins_runtime_test::confirmation = request;
    g_app.text_form = reinterpret_cast<ApplicationTextForm*>(&pins_runtime_test::form_marker);
    return true;
}
void application_text_form_close(bool, bool) noexcept { g_app.text_form = nullptr; }
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept {
    if (g_app.text_form && pins_runtime_test::confirmation.valid == validator) {
        g_app.text_form = nullptr;
    }
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_application_pinned_messages_runtime_entry() noexcept {
    maicord::pins_runtime_test::run();
    maicord::test_exit(maicord::pins_runtime_test::failures, "application pinned messages runtime");
}
