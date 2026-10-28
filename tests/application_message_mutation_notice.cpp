#include "application_message_mutation_notice.hpp"
#include "application_message_delete.hpp"
#include "application_message_reaction.hpp"
#include "application_state.hpp"
#include "application_reply_draft.hpp"
#include "application_text_form.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/app/message_reaction_controller.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "application_message_navigation.hpp"

namespace maicord::mutation_notice_test {
using namespace app_detail;
u32 failures{}, text_refreshes{}, frame_refreshes{};
u8 account_marker{};
u64 reaction_request_serial = 100, accepted_reaction_request{};
u32 reaction_submissions{};
MessageRecord records[8]{};
u32 order[8]{}, heights[8]{};
u64 fenwick[8]{};
u8 text[4096]{}, rich_text[4096]{};
MessageRichRecord rich_records[8]{};
u16 rich_order[8]{};
char draft[128]{};
MessageDraftRecord draft_records[2]{};
u8 draft_storage[256]{}, request_storage[kMessageComposerMaximumBytes]{};
ApplicationTextFormRequest form{};
char form_context[2048]{};
u32 form_opens{};

RestClientSubmitResult WINAPI submit_reaction_api(void* instance, u64 channel_id, u64 message_id,
                                                  u64 emoji_id, Utf8View label,
                                                  MessageReactionType type, bool add,
                                                  u64 request_id) noexcept {
    if (instance != &account_marker || channel_id != 70 || message_id != 900 || emoji_id ||
        !utf8_equal(label, utf8_literal("😀")) || type != MessageReactionType::normal || !add ||
        !request_id)
        return {RestClientSubmitError::invalid_input};
    accepted_reaction_request = request_id;
    ++reaction_submissions;
    return {};
}

void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}
RestClientResponse response(u16 status,
                            RestClientError error = RestClientError::none) noexcept {
    RestClientResponse value{};
    value.status_code = status;
    value.error = error;
    return value;
}
ChannelRecord* channel() noexcept {
    return const_cast<ChannelRecord*>(
        chat_state_store_find_channel(g_app.account_ui->chat_state, 70));
}
bool model_reset() noexcept {
    reset_message_delete();
    reset_message_reaction();
    g_app.message_mutation_notice = {};
    const bool initialized =
        message_store_initialize(&g_app.message_view.store, {records, 8}, {order, 8},
                                 {text, sizeof(text)}) &&
        ui_variable_list_initialize(&g_app.message_view.list, {heights, 8}, {fenwick, 8}, 0, 100) &&
        message_timeline_initialize(&g_app.message_view.timeline, &g_app.message_view.store,
                                    &g_app.message_view.list, 100, 100, true) &&
        message_rich_store_initialize(&g_app.account_ui->message_rich_store, {rich_records, 8},
                                      {rich_order, 8}, {rich_text, sizeof(rich_text)}) &&
        message_timeline_bind_rich_store(&g_app.message_view.timeline,
                                         &g_app.account_ui->message_rich_store);
    if (!initialized ||
        !message_draft_store_initialize(&g_app.account_ui->message_drafts, {draft_records, 2},
                                        {draft_storage, sizeof(draft_storage)}) ||
        !message_composer_controller_initialize(&g_app.account_ui->message_composer_controller,
                                                &g_app.message_composer, &g_app.account_ui->message_drafts,
                                                &g_app.message_view.timeline,
                                                {request_storage, sizeof(request_storage)}))
        return false;
    GatewayMessageEvent message{};
    message.id = 900;
    message.channel_id = 70;
    message.author_id = 5;
    message.timestamp_ms = 1000;
    message.content = utf8_literal("Authoritative message");
    message.fields = gateway_message_field_id | gateway_message_field_channel_id |
                     gateway_message_field_author_id | gateway_message_field_timestamp |
                     gateway_message_field_content;
    return message_timeline_apply_created(&g_app.message_view.timeline, message).ok();
}
bool initialize() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui)
        return false;
    auto& ui = *g_app.account_ui;
    if (!chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, 2},
                                        {ui.guild_order_storage, 2}, {ui.channel_record_storage, 4},
                                        {ui.channel_order_storage, 4}, {ui.chat_state_name_storage, 4096}) ||
        !chat_state_store_upsert_channel(&ui.chat_state,
                                            {70, 0, 0, utf8_literal("Notice DM"), 0, 0, 1})
             .ok() ||
        !chat_state_store_upsert_channel(&ui.chat_state,
                                            {80, 0, 0, utf8_literal("Other DM"), 0, 0, 1})
             .ok() ||
        !message_composer_initialize(&g_app.message_composer, {draft, sizeof(draft)}))
        return false;
    ui.chat_state.self_user_id = 5;
    ui.active_channel_id = ui.chat_state.selected_channel_id = 70;
    g_app.plugin_account_scope_serial = 10;
    g_app.account.instance = &account_marker;
    g_app.account.status.profile_id = 20;
    g_app.account.status.running = true;
    g_app.account.api.message_reaction = submit_reaction_api;
    const u64 channel_ids[]{70, 80};
    for (const u64 id : channel_ids) {
        auto* value =
            const_cast<ChannelRecord*>(chat_state_store_find_channel(ui.chat_state, id));
        if (!value)
            return false;
        value->actions = channel_action_view | channel_action_read_history |
                         channel_action_add_reactions | channel_action_send_messages;
    }
    message_composer_reset(&g_app.message_composer, 70, false);
    return model_reset();
}
void notice_tests() noexcept {
    using Kind = ApplicationMessageMutationKind;
    const auto forbidden = response(403), missing = response(404), server = response(500);
    static_assert(static_cast<u32>(Kind::edit) == 0 && static_cast<u32>(Kind::erase) == 1 &&
                  static_cast<u32>(Kind::reaction) == 2 && static_cast<u32>(Kind::attachment_open) == 3 &&
                  static_cast<u32>(Kind::mark_unread) == 4);
    using Stage = ApplicationMessageNavigationStage;
    application_message_mutation_notice_begin(Kind::navigate_message, 70, 900);
    const Stage pending_stages[]{
        Stage::waiting_channel, Stage::loading_channel, Stage::loading_parent, Stage::waiting_membership,
        Stage::opening_history, Stage::rate_limited,    Stage::busy,           Stage::cancelled};
    const Utf8View pending_labels[]{
        utf8_literal("Loading channel… Esc to cancel"), utf8_literal("Loading channel… Esc to cancel"),
        utf8_literal("Loading channel… Esc to cancel"),      utf8_literal("Checking access… Esc to cancel"),
        utf8_literal("Opening message… Esc to cancel"),     utf8_literal("Retrying… Esc to cancel"),
        utf8_literal("Another message is opening."),       utf8_literal("Opening cancelled.")};
    for (u32 stage = 0; stage < 8; ++stage) {
        application_message_mutation_notice_navigation_pending(70, 900, pending_stages[stage]);
        check(utf8_equal(application_message_mutation_notice_text(), pending_labels[stage]) &&
                  !application_message_mutation_notice_is_error(),
              "mutation notice: compact native message-link phases preserve captured source owner");
    }
    const Stage failure_stages[]{Stage::failure_parent, Stage::failure_membership, Stage::permission_denied,
                                 Stage::source_changed, Stage::message_missing};
    for (const auto stage : failure_stages) {
        application_message_mutation_notice_begin(Kind::navigate_message, 70, 900);
        application_message_mutation_notice_navigation_failed(70, 900, stage, server);
        check(application_message_mutation_notice_text().bytes &&
                  application_message_mutation_notice_is_error() &&
                  g_app.message_mutation_notice.navigation_stage == stage,
              "mutation notice: categorical navigation failures remain bounded and use error presentation");
    }
    application_message_mutation_notice_begin(Kind::navigate_message, 70, 900);
    application_message_mutation_notice_navigation_pending(70, 900, Stage::loading_parent);
    g_app.account_ui->active_channel_id = 80;
    check(!application_message_mutation_notice_text().bytes,
          "mutation notice: preadmission permalink notice never appears in unrelated channel");
    g_app.account_ui->active_channel_id = 70;
    application_message_mutation_notice_navigation_failed(80, 900, Stage::permission_denied, forbidden);
    check(utf8_equal(application_message_mutation_notice_text(), utf8_literal("Loading channel… Esc to cancel")),
          "mutation notice: different-channel receipt cannot replace source navigation stage");
    application_message_mutation_notice_succeeded(Kind::navigate_message, 70, 900);
    check(!application_message_mutation_notice_text().bytes &&
              g_app.message_mutation_notice.navigation_stage == Stage::idle,
          "mutation notice: exact settled permalink clears bounded stage");
    application_message_mutation_notice_begin(Kind::mark_unread, 70, 900);
    g_app.account_ui->active_channel_id = 80;
    application_message_mutation_notice_failed(Kind::mark_unread, 70, 900, forbidden);
    check(!application_message_mutation_notice_text().bytes &&
              g_app.message_mutation_notice.phase == ApplicationMessageMutationNoticePhase::failed,
          "mutation notice: offselected manual read failure remains captured without appearing in another "
          "channel");
    g_app.account_ui->active_channel_id = 70;
    check(utf8_equal(application_message_mutation_notice_text(),
               utf8_literal("Couldn't mark as unread. Try again.")) &&
              application_message_mutation_notice_is_error(),
          "mutation notice: returning to captured channel reveals terminal manual failure");
    application_message_mutation_notice_clear(Kind::mark_unread, 80, 900);
    check(application_message_mutation_notice_is_error(),
          "mutation notice: unrelated channel cannot dismiss manual failure");
    application_message_mutation_notice_clear(Kind::mark_unread, 70, 900);
    check(!application_message_mutation_notice_text().bytes &&
              !application_message_mutation_notice_is_error(),
          "mutation notice: exact explicit manual retry dismisses retained failure");
    application_message_mutation_notice_begin(Kind::mark_unread, 70, 900);
    application_message_mutation_notice_failed(Kind::mark_unread, 70, 900, server);
    check(utf8_equal(application_message_mutation_notice_text(),
               utf8_literal("Couldn't mark as unread. Try again.")),
          "mutation notice: unknown manual transport result describes uncertainty and reachable retry");
    ++g_app.plugin_account_scope_serial;
    check(!application_message_mutation_notice_text().bytes,
          "mutation notice: retired manual account receipt cannot appear later");
    --g_app.plugin_account_scope_serial;
    check(message_composer_insert_utf8(&g_app.message_composer, utf8_literal("Unsaved edited draft")) ==
              MessageComposerResult::changed,
          "mutation notice: unsaved composer draft exists");
    application_message_mutation_notice_begin(Kind::edit, 70, 900);
    application_message_mutation_notice_failed(Kind::edit, 70, 900, forbidden);
    check(utf8_equal(application_message_mutation_notice_text(),
               utf8_literal("You can't edit this message.")) &&
              application_message_mutation_notice_is_error() &&
              utf8_equal(message_composer_text(g_app.message_composer), utf8_literal("Unsaved edited draft")),
          "mutation notice: edit403 tells the user permission failed and preserves the real draft");
    application_message_mutation_notice_begin(Kind::edit, 70, 900);
    application_message_mutation_notice_failed(Kind::edit, 70, 900, missing);
    check(utf8_equal(application_message_mutation_notice_text(),
               utf8_literal("This message was deleted.")),
          "mutation notice: missing edit target distinguishes unavailable message and retained draft");
    application_message_mutation_notice_begin(Kind::erase, 70, 900);
    application_message_mutation_notice_failed(Kind::erase, 70, 900,
                                               response(0, RestClientError::receive));
    check(utf8_equal(application_message_mutation_notice_text(),
               utf8_literal("Couldn't confirm the message was deleted.")),
          "mutation notice: delete transport failure communicates uncertainty rather than confirmed "
          "deletion");
    application_message_mutation_notice_begin(Kind::reaction, 70, 900);
    application_message_mutation_notice_failed(Kind::reaction, 70, 900, forbidden);
    check(utf8_equal(application_message_mutation_notice_text(),
               utf8_literal("You cannot change reactions on this message.")),
          "mutation notice: reaction denial is visible and action-specific");

    application_message_mutation_notice_begin(Kind::attachment_open, 70, 900);
    application_message_mutation_notice_pending(Kind::attachment_open, 70, 900);
    check(utf8_equal(application_message_mutation_notice_text(), utf8_literal("Opening attachment…")) &&
              !application_message_mutation_notice_is_error(),
          "mutation notice: attachment refresh has visible pending feedback without an error state");
    application_message_mutation_notice_succeeded(Kind::attachment_open, 70, 900);
    check(!application_message_mutation_notice_text().bytes &&
              !application_message_mutation_notice_is_error(),
          "mutation notice: successful attachment preparation clears its exact pending status");
    application_message_mutation_notice_begin(Kind::attachment_open, 70, 900);
    application_message_mutation_notice_pending(Kind::attachment_open, 70, 900);
    application_message_mutation_notice_failed(Kind::attachment_open, 70, 900, server);
    check(
        utf8_equal(application_message_mutation_notice_text(),
             utf8_literal("Couldn't open the attachment. Try again.")) &&
            application_message_mutation_notice_is_error(),
        "mutation notice: failed attachment preparation replaces pending status with a retry instruction");

    application_message_mutation_notice_begin(Kind::erase, 70, 900);
    application_message_mutation_notice_begin(Kind::edit, 70, 901);
    application_message_mutation_notice_failed(Kind::edit, 70, 901, forbidden);
    const auto displayed = g_app.message_mutation_notice.shown;
    application_message_mutation_notice_succeeded(Kind::erase, 70, 900);
    check(g_app.message_mutation_notice.shown.message_id == displayed.message_id &&
              g_app.message_mutation_notice.kind == Kind::edit &&
              application_message_mutation_notice_is_error(),
          "mutation notice: older delete success cannot dismiss newer edit failure");
    application_message_mutation_notice_begin(Kind::reaction, 70, 900);
    application_message_mutation_notice_pending(Kind::reaction, 70, 900);
    application_message_mutation_notice_failed(Kind::reaction, 70, 901, forbidden);
    application_message_mutation_notice_succeeded(Kind::reaction, 80, 900);
    check(g_app.message_mutation_notice.phase == ApplicationMessageMutationNoticePhase::pending &&
              g_app.message_mutation_notice.shown.message_id == 900 &&
              g_app.message_mutation_notice.requests[static_cast<u32>(Kind::reaction)].message_id == 900,
          "mutation notice: wrong message or channel receipt cannot consume another pending action");

    for (u32 fence = 0; fence < 9; ++fence) {
        g_app.message_mutation_notice = {};
        application_message_mutation_notice_begin(Kind::edit, 70, 900);
        const auto serial = g_app.plugin_account_scope_serial;
        const auto profile = g_app.account.status.profile_id;
        const auto self = g_app.account_ui->chat_state.self_user_id;
        auto* observed_channel = channel();
        const auto generation = observed_channel->generation;
        switch (fence) {
        case 0:
            ++g_app.plugin_account_scope_serial;
            break;
        case 1:
            ++g_app.account.status.profile_id;
            break;
        case 2:
            ++g_app.account_ui->chat_state.self_user_id;
            break;
        case 3:
            g_app.account_ui->active_channel_id = 80;
            break;
        case 4:
            ++channel()->generation;
            break;
        case 5:
            g_app.account.instance = nullptr;
            break;
        case 6:
            g_app.account.stop_failed = true;
            break;
        case 7:
            g_app.login.visible = true;
            break;
        case 8:
            observed_channel->occupied = false;
            break;
        }
        application_message_mutation_notice_failed(Kind::edit, 70, 900, forbidden);
        check(!application_message_mutation_notice_text().bytes &&
                  !application_message_mutation_notice_is_error() &&
                  g_app.message_mutation_notice.phase == ApplicationMessageMutationNoticePhase::empty,
              "mutation notice: account epoch profile user channel generation and retired host fence late "
              "failures");
        g_app.plugin_account_scope_serial = serial;
        g_app.account.status.profile_id = profile;
        g_app.account_ui->chat_state.self_user_id = self;
        g_app.account_ui->active_channel_id = 70;
        observed_channel->generation = generation;
        observed_channel->occupied = true;
        g_app.account.instance = &account_marker;
        g_app.account.stop_failed = g_app.login.visible = false;
    }
    g_app.message_mutation_notice = {};
    application_message_mutation_notice_begin(Kind::edit, 70, 900);
    application_message_mutation_notice_failed(Kind::edit, 70, 900, forbidden);
    ++g_app.plugin_account_scope_serial;
    check(!application_message_mutation_notice_text().bytes &&
              g_app.message_mutation_notice.phase == ApplicationMessageMutationNoticePhase::empty,
          "mutation notice: an already displayed result retires when its account epoch changes");
    application_message_mutation_notice_begin(Kind::edit, 999, 900);
    application_message_mutation_notice_failed(Kind::edit, 999, 900, forbidden);
    check(!application_message_mutation_notice_text().bytes,
          "mutation notice: missing channel cannot create a visible owner or result");
}
void delete_policy_tests() noexcept {
    check(model_reset(), "delete policy: authoritative fixture initializes");
    const auto found = message_store_find(g_app.message_view.store, 900);
    auto* record = const_cast<MessageRecord*>(message_store_resolve(g_app.message_view.store, found.handle));
    if (!record)
        return;
    auto& navigation = g_app.account_ui->navigation_view;
    auto* saved_rows = navigation.channel_rows;
    const u32 saved_count = navigation.channel_count;
    ChannelViewRow row{};
    row.id = 70;
    navigation.channel_rows = &row;
    navigation.channel_count = 1;
    const u16 undeletable[]{1, 2, 3, 4, 5, 21, 35, 56, 57, 64, 68};
    for (const u16 type : undeletable) {
        record->type = type;
        for (u32 moderator = 0; moderator < 2; ++moderator) {
            row.actions = moderator ? channel_action_manage_messages : 0;
            check(!message_can_delete(*record) && !queue_message_delete(900) &&
                      !application_message_delete_request(900, false) && !g_app.message_delete.message_id,
                  "delete policy: undeletable service types reject author and moderator actions");
        }
    }
    const u16 deletable[]{0, 6, 7, 8, 12, 18, 19, 20, 23, 25, 26, 27, 31, 32, 36, 41, 44, 46, 47, 65, 67};
    for (const u16 type : deletable) {
        record->type = type;
        row.actions = 0;
        check(message_can_delete(*record) && queue_message_delete(900) &&
                  application_message_delete_pending_available(),
              "delete policy: author retains deletion of ordinary and deletable service messages");
        reset_message_delete();
        record->author_id = 6;
        check(!message_can_delete(*record), "delete policy: other authors require manage messages");
        row.actions = channel_action_manage_messages;
        check(message_can_delete(*record), "delete policy: moderator retains deletable service messages");
        record->author_id = 5;
    }
    record->type = 24;
    row.actions = 0;
    check(!message_can_delete(*record) && !queue_message_delete(900),
          "delete policy: own automod message still requires manage messages");
    row.actions = channel_action_manage_messages;
    check(message_can_delete(*record) && queue_message_delete(900) &&
              application_message_delete_pending_available(),
          "delete policy: manage messages permits automod deletion");
    auto throttled = response(429);
    throttled.rate_limited = true;
    throttled.retry_at_ms = 12345;
    check(accept_message_delete_response(throttled), "delete policy: automod delete enters retry wait");
    row.actions = 0;
    check(!application_message_delete_pending_available(),
          "delete policy: automod retry rechecks manage messages after permission loss");
    reset_message_delete();
    record->type = 0;
    check(application_message_delete_request(900, false), "delete policy: confirmation captures allowed message");
    record->type = 3;
    check(!form.valid(form.owner) && form.save(form.owner, {}) && !g_app.message_delete.message_id,
          "delete policy: confirmation refuses message changed into an undeletable call");
    record->type = 0;
    check(queue_message_delete(900) && accept_message_delete_response(throttled),
          "delete policy: ordinary delete enters retry wait");
    record->type = 21;
    check(!application_message_delete_pending_available(),
          "delete policy: retry rechecks source type even without a revision change");
    reset_message_delete();
    navigation.channel_rows = saved_rows;
    navigation.channel_count = saved_count;
}
void delete_tests() noexcept {
    check(model_reset(), "mutation delete: real timeline initializes");
    const auto forbidden = response(403);
    const u32 refresh_before = text_refreshes;
    check(queue_message_delete(900) && accept_message_delete_response(forbidden) &&
              !g_app.message_delete.message_id && message_store_find(g_app.message_view.store, 900).ok() &&
              application_message_mutation_notice_is_error() && text_refreshes == refresh_before + 1,
          "mutation delete: production403 retains message publishes error and refreshes visible notice");
    check(queue_message_delete(900) &&
              accept_message_delete_response(response(0, RestClientError::timeout)) &&
              message_store_find(g_app.message_view.store, 900).ok() &&
              utf8_equal(application_message_mutation_notice_text(),
                   utf8_literal("Couldn't confirm the message was deleted.")),
          "mutation delete: production uncertain transport receipt retains message and warns before retry");
    auto restricted = response(429);
    restricted.rate_limited = true;
    restricted.retry_at_ms = 12345;
    restricted.rejection = {20026, true, false};
    check(queue_message_delete(900) && accept_message_delete_response(restricted) &&
              !g_app.message_delete.message_id && message_store_find(g_app.message_view.store, 900).ok() &&
              utf8_equal(application_message_mutation_notice_text(),
                   utf8_literal("Discord restricted this action. Check your account in Discord.")),
          "mutation delete: required-action429 is terminal and retains target instead of scheduling "
          "prohibited retry");
    auto throttled = response(429);
    throttled.rate_limited = true;
    throttled.retry_at_ms = 12345;
    check(queue_message_delete(900) && accept_message_delete_response(throttled) &&
              g_app.message_delete.message_id == 900 && g_app.message_delete.retry_at_ms == 12345 &&
              !application_message_mutation_notice_is_error(),
          "mutation delete: ordinary429 retains exact pending action and authoritative retry deadline");
    check(application_reply_draft_set(900, false),
          "mutation delete: actual draft owner captures reply policy");
    g_app.message_edit.channel_id = 70;
    g_app.message_edit.saved_reply_message_id = 900;
    g_app.message_edit.saved_reply_mention_suppressed = true;
    const u32 frame_before = frame_refreshes;
    check(accept_message_delete_response(response(404)) &&
              !message_store_find(g_app.message_view.store, 900).ok() &&
              !application_reply_draft().message_id && application_reply_draft().mention_reply_author &&
              !application_message_mutation_notice_text().bytes &&
              !g_app.message_edit.saved_reply_message_id &&
              !g_app.message_edit.saved_reply_mention_suppressed && frame_refreshes == frame_before + 1,
          "mutation delete: production404 resolves already removed target clears reply and reconciles "
          "timeline");
    check(model_reset() && queue_message_delete(900),
          "mutation delete: overlapping result fixture queues actual deletion");
    application_message_mutation_notice_begin(ApplicationMessageMutationKind::edit, 70, 901);
    application_message_mutation_notice_failed(ApplicationMessageMutationKind::edit, 70, 901, forbidden);
    check(accept_message_delete_response(response(204)) &&
              !message_store_find(g_app.message_view.store, 900).ok() &&
              application_message_mutation_notice_is_error() &&
              g_app.message_mutation_notice.kind == ApplicationMessageMutationKind::edit &&
              g_app.message_mutation_notice.shown.message_id == 901,
          "mutation delete: actual success cannot clear a newer unrelated edit failure");
}
bool form_context_contains(Utf8View needle) noexcept {
    for (u32 i = 0; i + needle.bytes <= form.context.bytes; ++i)
        if (bytes_equal(form.context.data + i, needle.data, needle.bytes))
            return true;
    return false;
}
void delete_confirmation_tests() noexcept {
    check(model_reset(), "delete confirmation: fixture initializes");
    form_opens = 0;
    check(application_message_delete_request(900, false) && form_opens == 1 && !g_app.message_delete.message_id &&
              form.confirmation_only && utf8_equal(form.title, utf8_literal("Delete message?")) &&
              utf8_equal(form.accept, utf8_literal("Delete")) &&
              form_context_contains(utf8_literal("Authoritative message")) && form.valid(form.owner),
          "delete confirmation: opens a confirmation with a preview and queues nothing");
    check(form.save(form.owner, {}) && g_app.message_delete.message_id == 900,
          "delete confirmation: accept revalidates and queues the delete");
    check(!application_message_delete_request(900, false) && form_opens == 1,
          "delete confirmation: a pending delete refuses another request");
    check(model_reset() && application_message_delete_request(900, false) && form_opens == 2 &&
              message_timeline_remove_local(&g_app.message_view.timeline, 900).ok() && !form.valid(form.owner) &&
              form.save(form.owner, {}) && !g_app.message_delete.message_id,
          "delete confirmation: accept after the message vanished closes without deleting");
    check(model_reset() && application_message_delete_request(900, true) && form_opens == 2 &&
              g_app.message_delete.message_id == 900,
          "delete confirmation: Shift skips the dialog");
    check(model_reset() && !application_message_delete_request(901, false) && form_opens == 2,
          "delete confirmation: unknown message is refused");
    reset_message_delete();
}
bool submit_queued_reaction(u64 now_ms = 0) noexcept {
    const auto id = ++reaction_request_serial;
    return application_message_reaction_submit(id, now_ms ? now_ms : clock_monotonic_milliseconds()) &&
           accepted_reaction_request == id;
}
bool queue_reaction() noexcept {
    const auto result = application_queue_message_reaction_emoji(900, 0, utf8_literal("😀"), false);
    return (result == ApplicationMessageReactionEnqueueResult::accepted ||
            result == ApplicationMessageReactionEnqueueResult::coalesced) &&
           submit_queued_reaction();
}
bool reaction_response(RestClientResponse receipt) noexcept {
    if (!accepted_reaction_request)
        return false;
    receipt.request_id = accepted_reaction_request;
    const bool handled = application_message_reaction_response(receipt);
    if (handled)
        accepted_reaction_request = 0;
    return handled;
}
bool reacted() noexcept {
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, 900);
    return rich && rich->reaction_count == 1 && (rich->reactions[0].flags & 1u) != 0;
}
void reaction_tests() noexcept {
    check(model_reset(), "mutation reaction: real rich timeline initializes");
    const bool first_queued = queue_reaction();
    auto unrelated = response(403);
    unrelated.request_id = accepted_reaction_request + 1;
    check(first_queued && !application_message_reaction_response(unrelated) && reacted() &&
              g_app.message_reaction.phase == MessageReactionRequestPhase::submitting,
          "mutation reaction: wrong nonzero transport receipt cannot consume the admitted production "
          "request");
    check(first_queued && reacted() && reaction_response(response(403)) && !reacted() &&
              !g_app.message_reaction.message_id && application_message_mutation_notice_is_error(),
          "mutation reaction: production403 rolls back optimistic membership and publishes permission "
          "failure");
    check(queue_reaction() && reaction_response(response(400)) && !reacted() &&
              !g_app.message_reaction.message_id && application_message_mutation_notice_is_error(),
          "mutation reaction: production400 is terminal and does not automatically repeat invalid action");
    check(queue_reaction() && reaction_response(response(404)) && !reacted() &&
              !g_app.message_reaction.message_id &&
              utf8_equal(application_message_mutation_notice_text(),
                   utf8_literal("This message is no longer available.")),
          "mutation reaction: production404 rolls back without retry and reports missing target");
    check(
        queue_reaction() && reaction_response(response(500)) && reacted() &&
            g_app.message_reaction.phase == MessageReactionRequestPhase::retry_wait &&
            g_app.message_reaction.retries == 1 && !application_message_mutation_notice_is_error(),
        "mutation reaction: production transient5xx retains optimistic state for bounded scheduled retry");
    for (u32 retry = 1; retry <= kMessageReactionMaximumRetries; ++retry) {
        const auto due = g_app.message_reaction.retry_at_ms;
        const auto before = reaction_submissions;
        check(due && !application_message_reaction_submit(++reaction_request_serial, due - 1) &&
                  reaction_submissions == before,
              "mutation reaction: retry transport cannot be admitted before its actual owner deadline");
        const bool handled = submit_queued_reaction(due) && reaction_response(response(503));
        check(handled && (retry < kMessageReactionMaximumRetries
                              ? reacted() && g_app.message_reaction.retries == retry + 1 &&
                                    !application_message_mutation_notice_is_error()
                              : !reacted() && !g_app.message_reaction.message_id &&
                                    application_message_mutation_notice_is_error()),
              "mutation reaction: three genuine bounded retry receipts retain then restore membership with "
              "explicit feedback");
    }
    auto restricted = response(429);
    restricted.rate_limited = true;
    restricted.retry_at_ms = 12345;
    restricted.rejection = {20026, true, false};
    check(queue_reaction() && reaction_response(restricted) && !reacted() &&
              !g_app.message_reaction.message_id && application_message_mutation_notice_is_error(),
          "mutation reaction: required-action429 rolls back and stops automatic retries");
    check(queue_reaction() && reaction_response(response(0, RestClientError::timeout)) &&
              !reacted() && !g_app.message_reaction.message_id &&
              application_message_mutation_notice_is_error(),
          "mutation reaction: transport failure rolls back and shows explicit user retry feedback");
    auto throttled = response(429);
    throttled.rate_limited = true;
    throttled.retry_at_ms = clock_monotonic_milliseconds() + 10000;
    check(queue_reaction() && reaction_response(throttled) && reacted() &&
              g_app.message_reaction.phase == MessageReactionRequestPhase::retry_wait &&
              g_app.message_reaction.retry_at_ms == throttled.retry_at_ms &&
              g_app.message_reaction.retries == 1 && !application_message_mutation_notice_is_error(),
          "mutation reaction: ordinary429 preserves pending membership and exact retry deadline");
    const auto before_retry = reaction_submissions;
    check(
        !application_message_reaction_submit(++reaction_request_serial, throttled.retry_at_ms - 1) &&
            reaction_submissions == before_retry && reacted(),
        "mutation reaction: future429 floor prevents early submission without undoing pending membership");
    check(submit_queued_reaction(throttled.retry_at_ms) && reaction_response(response(204)) && reacted() &&
              !g_app.message_reaction.message_id && !application_message_mutation_notice_text().bytes,
          "mutation reaction: production success retains authoritative accepted reaction and clears its "
          "notice");
}
void run() noexcept {
    check(initialize(),
          "mutation notice: actual account identity and occupied channel fixture initializes");
    if (g_app.account_ui && g_app.message_composer.text && channel()) {
        notice_tests();
        delete_policy_tests();
        delete_tests();
        delete_confirmation_tests();
        reaction_tests();
    }
    message_composer_release(&g_app.message_composer);
    reset_message_reaction();
    if (g_app.account_ui)
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    g_app.account.instance = nullptr;
}
} // namespace maicord::mutation_notice_test

namespace maicord::app_detail {
void application_emoji_preferences_track_reaction(ReactionEmojiRef) noexcept {
}
bool refresh_account_text(bool) noexcept {
    ++mutation_notice_test::text_refreshes;
    return true;
}
bool refresh_account_frame(bool) noexcept {
    ++mutation_notice_test::frame_refreshes;
    return true;
}
void application_media_picker_reaction_changed() noexcept {}
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    using namespace mutation_notice_test;
    ++form_opens;
    form = request;
    if (request.context.bytes > sizeof(form_context))
        return false;
    copy_bytes(form_context, request.context.data, request.context.bytes);
    form.context = {form_context, request.context.bytes};
    return true;
}
} // namespace maicord::app_detail
extern "C" void maicord_message_mutation_notice_tests_entry() noexcept {
    maicord::mutation_notice_test::run();
    ExitProcess(maicord::mutation_notice_test::failures ? 1 : 0);
}
