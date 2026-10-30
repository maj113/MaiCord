// Actual shipping host with typed offline transport and native form/navigation
// seams. No network account, worker, response bank, or test-only host behavior.
#include "../src/app/application_forum_post.hpp"
#include "../src/app/application_state.hpp"
#include "test_output.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/core/slice.hpp"
namespace maicord::forum_test {
u64 now = 1000;
}
namespace maicord {
static u64 forum_fixture_now() noexcept {
    return forum_test::now;
}
} // namespace maicord
#define clock_monotonic_milliseconds forum_fixture_now
#include "../src/app/application_forum_post.cpp"
#undef clock_monotonic_milliseconds

namespace maicord::forum_test {
using namespace app_detail;
u32 failures{}, submissions{}, metadata_submissions{}, cancellations{}, navigations{}, inspections{}, frames{};
u64 accepted{}, cancelled{}, navigated{};
u64 parent_ack_cancelled{};
u32 parent_ack_cancellations{};
void parent_ack_cancel(void*, u64 id) noexcept {
    parent_ack_cancelled = id;
    ++parent_ack_cancellations;
}
u8 actor{};
RestClientSubmitError admission{};
ForumPostRequest submitted{};
char submitted_title[400]{}, submitted_body[8000]{}, body[4096]{};
char submitted_upload_filename[kAttachmentUploadMaximumFilenameBytes]{};
char submitted_attachment_text[kForumPostMaximumBodyBytes]{};
UploadedAttachment submitted_attachments[kAttachmentUploadMaximumCount]{};
u32 body_bytes{};
ApplicationForumPostTicket form{};
bool form_visible{}, form_ok = true, navigate_ok = true, canonical_on_navigate{}, release_on_paint{};
GuildRecord guilds[2]{};
u16 guild_order[2]{};
ChannelRecord channels[64]{};
u16 channel_order[64]{};
u8 names[16384]{};
RoleRecord roles[4]{};
PermissionOverwriteRecord overwrites[4]{};
MessageRecord messages[32]{};
u32 message_order[32]{};
u8 message_text[16384]{};
u32 item_units[32]{};
u64 fenwick[32]{};
char composer_text[16]{};
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
ChannelRecord* row(u64 id) noexcept {
    return const_cast<ChannelRecord*>(
        chat_state_store_find_channel(g_app.account_ui->chat_state, id));
}
void origin() noexcept {
    g_app.account_ui->active_channel_id = 71;
    g_app.account_ui->chat_state.selected_channel_id = 71;
    g_app.account_ui->chat_state.selected_guild_id = 7;
}
void permit(u64 bits = kPermissionViewChannel | kPermissionReadMessageHistory |
                       kPermissionSendMessages) noexcept {
    auto& state = g_app.account_ui->chat_state;
    chat_state_store_upsert_role(&state, {7, 7, bits, utf8_literal("Everyone")});
    chat_state_store_set_self_roles(&state, 7, 99, 5, {});
    chat_state_store_recompute_guild_permissions(&state, 7);
}
RestClientSubmitResult WINAPI submit(void* instance, const ForumPostRequest* request,
                                     u64 id) noexcept {
    if (instance != &actor || !id)
        return {RestClientSubmitError::invalid_input};
    if (!request) {
        ++cancellations;
        cancelled = id;
        return {};
    }
    ++submissions;
    if (request->operation == ForumOperation::parent_metadata)
        ++metadata_submissions;
    if (admission != RestClientSubmitError::none)
        return {admission};
    if (accepted)
        return {RestClientSubmitError::busy};
    if (!forum_post_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    accepted = id;
    submitted = *request;
    if (request->upload_filename.bytes) {
        CopyMemory(submitted_upload_filename, request->upload_filename.data, request->upload_filename.bytes);
        submitted.upload_filename = {submitted_upload_filename, request->upload_filename.bytes};
    }
    if (request->operation == ForumOperation::create) {
        CopyMemory(submitted_title, request->title.data, request->title.bytes);
        CopyMemory(submitted_body, request->content.data, request->content.bytes);
        submitted.title = {submitted_title, request->title.bytes};
        submitted.content = {submitted_body, request->content.bytes};
        u32 used{};
        for (u32 i = 0; i < request->attachments.count; ++i) {
            submitted_attachments[i] = request->attachments[i];
            Utf8View* destination[] = {&submitted_attachments[i].filename, &submitted_attachments[i].uploaded_filename, &submitted_attachments[i].description};
            for (auto* value : destination) {
                CopyMemory(submitted_attachment_text + used, value->data, value->bytes);
                *value = {submitted_attachment_text + used, value->bytes}; used += value->bytes;
            }
        }
        if (request->attachments.count) submitted.attachments = {submitted_attachments, request->attachments.count};
    }
    return {};
}
bool receipt(u64 id, u16 status, Utf8View value = {}, u64 retry = 0, RestClientError error = {},
             u32 code = 0) noexcept {
    RestClientResponse response{};
    response.request_id = id;
    response.status_code = status;
    response.body_bytes = value.bytes;
    response.error = error;
    response.rate_limited = status == 429;
    response.retry_at_ms = retry;
    response.rejection.has_code = code != 0;
    response.rejection.code = code;
    if (id == accepted)
        accepted = 0;
    return application_forum_post_response(response, value);
}
Utf8View metadata(bool required = false) noexcept {
    return required ? utf8_literal("{\"id\":\"70\",\"guild_id\":\"7\",\"type\":15,\"name\":\"Forum\","
                                   "\"flags\":16,\"default_auto_archive_duration\":1440,\"available_tags\":"
                                   "[{\"id\":\"100\",\"name\":\"General\",\"moderated\":false},{\"id\":"
                                   "\"101\",\"name\":\"Staff\",\"moderated\":true}]}")
                    : utf8_literal("{\"id\":\"70\",\"guild_id\":\"7\",\"type\":15,\"name\":\"Forum\","
                                   "\"flags\":0,\"default_auto_archive_duration\":1440,\"available_tags\":["
                                   "{\"id\":\"100\",\"name\":\"General\",\"moderated\":false},{\"id\":"
                                   "\"101\",\"name\":\"Staff\",\"moderated\":true}]}");
}
void append(const char* value) noexcept {
    for (u32 i = 0; value[i]; ++i)
        body[body_bytes++] = value[i];
}
void number(u64 value) noexcept {
    char digits[20];
    u32 count{};
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    while (count)
        body[body_bytes++] = digits[--count];
}
Utf8View post(u64 id, u64 author = 5) noexcept {
    body_bytes = 0;
    append("{\"id\":\"");
    number(id);
    append("\",\"parent_id\":\"70\",\"guild_id\":\"7\",\"type\":11,\"owner_id\":\"5\",\"name\":\"Server "
           "title\",\"thread_metadata\":{\"archived\":false,\"locked\":false,\"auto_archive_duration\":"
           "1440,\"archive_timestamp\":\"2026-10-05T12:00:00Z\"},\"applied_tags\":[");
    for (u32 i = 0; i < submitted.tag_count; ++i) {
        if (i)
            append(",");
        append("\"");
        number(submitted.applied_tag_ids[i]);
        append("\"");
    }
    append("],\"message\":{\"id\":\"");
    number(id + 1000);
    append("\",\"channel_id\":\"");
    number(id);
    append("\",\"guild_id\":\"7\",\"author\":{\"id\":\"");
    number(author);
    append("\",\"username\":\"Self\"},\"content\":\"Starter "
           "message\",\"timestamp\":\"2026-10-05T12:00:00Z\",\"type\":0,\"flags\":0}");
    append("}");
    return {body, body_bytes};
}
void reset_owner() noexcept {
    if (accepted)
        receipt(accepted, 500);
    application_forum_post_release();
    g_app.account.instance = &actor;
    g_app.account.status.profile_id = 2;
    g_app.account.status.running = true;
    g_app.account.status.required_action = AccountRequirement::none;
    g_app.account.api.forum_post = submit;
    admission = {};
    form_ok = navigate_ok = true;
    canonical_on_navigate = release_on_paint = false;
    origin();
    permit();
}
ApplicationForumPostTicket begin(u64 id = 70) noexcept {
    ApplicationForumPostTicket ticket{};
    check(application_forum_post_begin(id, &ticket),
          "forum: real type15 parent begins from ordinary chat origin");
    return ticket;
}
bool load(u64 id, bool required = false) noexcept {
    return application_forum_post_submit(id, now) &&
           submitted.operation == ForumOperation::parent_metadata && receipt(id, 200, metadata(required));
}
bool draft(const ApplicationForumPostTicket& ticket) noexcept {
    return application_forum_post_draft_update(ticket, utf8_literal("Captured title"),
                                               utf8_literal("Starter message"));
}
bool start(const ApplicationForumPostTicket& ticket, u64 id) noexcept {
    return draft(ticket) && application_forum_post_draft_submit(ticket) &&
           application_forum_post_submit(id, now);
}
bool initialize() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui)
        return false;
    auto& state = g_app.account_ui->chat_state;
    if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2}, {channels, 64},
                                        {channel_order, 64}, {names, sizeof(names)}) ||
        !chat_state_store_bind_permissions(&state, {roles, 4}, {overwrites, 4}) ||
        !chat_state_store_upsert_guild(&state,
                                          {7, utf8_literal("Guild"), GuildMutationKind::create, 99})
             .ok() ||
        !chat_state_store_upsert_channel(&state, {70, 7, 0, utf8_literal("Forum"), 0, 0, 15}).ok() ||
        !chat_state_store_upsert_channel(&state, {71, 7, 0, utf8_literal("Chat"), 0, 0, 0}).ok() ||
        !message_store_initialize(&g_app.message_view.store, {messages, 32}, {message_order, 32},
                                  {message_text, sizeof(message_text)}) ||
        !ui_variable_list_initialize(&g_app.message_view.list, {item_units, 32}, {fenwick, 32}, 0, 100) ||
        !message_timeline_initialize(&g_app.message_view.timeline, &g_app.message_view.store,
                                     &g_app.message_view.list, 100, 1000, true) ||
        !message_composer_initialize(&g_app.message_composer, {composer_text, sizeof(composer_text)}))
        return false;
    state.self_user_id = 5;
    g_app.message_pump.read_ack_cancel = parent_ack_cancel;
    g_app.plugin_account_scope_serial = 3;
    g_app.account.instance = &actor;
    g_app.account.status.profile_id = 2;
    g_app.account.status.running = true;
    g_app.account.api.forum_post = submit;
    origin();
    permit();
    return true;
}
void run() noexcept {
    check(initialize(), "forum: actual Core initialization");
    if (!g_app.account_ui)
        return;
    check(application_forum_post_available(70) && !forum_owner && !application_forum_post_available(71),
          "forum: availability is lazy and type15-only without CREATE_PUBLIC_THREADS");
    permit(kPermissionViewChannel | kPermissionReadMessageHistory |
           kPermissionCreatePublicThreads);
    check(!application_forum_post_available(70),
          "forum: CREATE_PUBLIC_THREADS cannot replace required SEND_MESSAGES");
    permit();
    auto ticket = begin();
    check(application_forum_post_view().can_edit && !application_forum_post_view().can_submit &&
              draft(ticket) && application_forum_post_submit(10, now) &&
              application_forum_post_draft_update(ticket, utf8_literal("Edited during GET"),
                                                  utf8_literal("Starter message")) &&
              !receipt(999, 200, metadata()) && forum_owner->flight.id == 10 &&
              receipt(10, 200, metadata()) &&
              utf8_equal(application_forum_post_view().title, utf8_literal("Edited during GET")),
          "forum: fresh typed GET permits draft edits and only exact receipt drains");
    check(application_forum_post_view().tags.count == 2 && !application_forum_post_view().tags[1].enabled &&
              !application_forum_post_tag_toggle(ticket, 101) &&
              !application_forum_post_tag_toggle(ticket, 999),
          "forum: actual inventory excludes unauthorized moderated and invented selections");
    check(chat_state_store_note_history_latest(&g_app.account_ui->chat_state, 70, 850) &&
              chat_state_store_apply_read_state(&g_app.account_ui->chat_state,
                  {70, 800, 3, 70'000, 0x80000005u,
                   read_state_cursor | read_state_mentions | read_state_last_viewed | read_state_flags, {}}) &&
              forum_metadata_fresh(*forum_owner),
          "forum: parent activity and read changes preserve fresh metadata before own-post admission");
    check(application_forum_post_draft_submit(ticket) && application_forum_post_submit(11, now) &&
              submitted.tag_count == 0 && submitted.auto_archive_duration == 1440 &&
              !application_forum_post_draft_update(ticket, utf8_literal("Late edit"),
                                                   utf8_literal("Changed")) &&
              receipt(11, 200, post(900)) && row(900) && row(900)->thread_listed_active &&
              !row(900)->thread_self_joined_known && navigated == 900 &&
              message_store_find(g_app.message_view.store, 1900).ok(),
          "forum: optional tags post atomically; immutable draft; strict receipt publishes Core and uses "
          "normal navigation");
    check(row(70) && row(900) && row(70)->last_message_id == 900 && row(70)->last_read_message_id == 900 &&
              !row(70)->mention_count && row(70)->read_last_viewed == 70'000 &&
              row(70)->read_flags == 0x80000005u && row(900)->thread_parent_creation_noted &&
              parent_ack_cancelled == 70 && parent_ack_cancellations == 1,
          "forum: successful own HTTP creation acknowledges parent once, clears counts and cancels queued parent ACK");
    reset_owner();
    ticket = begin();
    check(load(20, true), "forum: required-tag metadata fixture");
    const auto local_metadata_before = metadata_submissions;
    check(!application_forum_post_draft_submit(ticket) && application_forum_post_view().phase == ForumPhase::editing &&
              application_forum_post_view().error && !application_forum_post_view().can_retry &&
              application_forum_post_view().can_submit &&
              application_forum_post_draft_update(ticket, utf8_literal("Valid title"), {}) &&
              !application_forum_post_view().error && !application_forum_post_view().status.bytes &&
              !application_forum_post_draft_submit(ticket) && application_forum_post_view().error && draft(ticket) &&
              !application_forum_post_view().error && metadata_submissions == local_metadata_before,
          "forum: local title/body validation remains Create post editing and real text edits clear only local error without metadata reload");
    check(!application_forum_post_draft_submit(ticket) && !accepted &&
              application_forum_post_view().phase == ForumPhase::editing && application_forum_post_view().error &&
              application_forum_post_view().can_edit && application_forum_post_view().can_submit &&
              !application_forum_post_view().can_retry && !application_forum_post_pending(now) &&
              metadata_submissions == local_metadata_before,
          "forum: required tag local validation keeps editable error and Create post action with fresh metadata");
    check(draft(ticket) && application_forum_post_view().error && application_forum_post_tag_toggle(ticket, 100) &&
              !application_forum_post_view().error && !application_forum_post_view().status.bytes &&
              application_forum_post_draft_submit(ticket) && application_forum_post_submit(21, now) &&
              submitted.tag_count == 1 && submitted.applied_tag_ids[0] == 100,
          "forum: unchanged text retains local error, real tag correction clears it and reaches atomic POST without another metadata GET");
    ChannelInput newer_post{940, 7, 70, utf8_literal("Newer post"), 0, 0, 11};
    newer_post.thread_archived_known = newer_post.thread_locked_known = true;
    check(chat_state_store_upsert_channel(&g_app.account_ui->chat_state, newer_post).ok() &&
              chat_state_store_note_thread(&g_app.account_ui->chat_state, 70, 940, 9) &&
              chat_state_store_apply_read_state(&g_app.account_ui->chat_state,
                  {70, 900, 2, 0, 0, read_state_cursor | read_state_mentions, {}}),
          "forum: newer Gateway post and authoritative parent count precede delayed own HTTP receipt");
    application_forum_post_gateway(7, 70, 940);
    check(receipt(21, 200, post(901)) && row(70) && row(901) && metadata_submissions == local_metadata_before &&
              row(70)->last_message_id == 940 && row(70)->last_read_message_id == 901 &&
              row(70)->mention_count == 2 && row(70)->read_mentions_incomplete &&
              row(901)->thread_parent_creation_noted && parent_ack_cancellations == 2,
          "forum: delayed own HTTP receipt acknowledges only its post and retains newer parent activity");
    const auto parent_cancels_before_duplicate = parent_ack_cancellations;
    check(chat_state_store_apply_read_state(&g_app.account_ui->chat_state,
              {70, 800, 4, 0, 0, read_state_cursor | read_state_mentions | read_state_manual, {}}) &&
              chat_state_store_note_thread(&g_app.account_ui->chat_state, 70, 901, 5) &&
              row(70)->last_read_message_id == 800 && row(70)->mention_count == 4 &&
              parent_ack_cancellations == parent_cancels_before_duplicate,
          "forum: shared HTTP/Gateway creation receipt preserves a later manual parent unread action");
    reset_owner();
    ticket = begin();
    check(load(22) && start(ticket, 23) &&
              chat_state_store_apply_read_state(&g_app.account_ui->chat_state,
                  {70, 940, 4, 0, 0, read_state_cursor | read_state_mentions, {}}),
          "forum: newer parent ACK is already queued before delayed own HTTP publication");
    check(receipt(23, 200, post(914)) && row(70) && row(914) && row(914)->thread_parent_creation_noted &&
              row(70)->last_message_id == 940 && row(70)->last_read_message_id == 940 &&
              row(70)->mention_count == 4 && parent_ack_cancellations == parent_cancels_before_duplicate,
          "forum: first delayed own HTTP receipt retains a newer queued parent ACK when its cursor does not advance");
    reset_owner();
    permit(kPermissionViewChannel | kPermissionReadMessageHistory |
           kPermissionSendMessages | kPermissionManageThreads);
    ticket = begin();
    check(load(30) && application_forum_post_tag_toggle(ticket, 101),
          "forum: current MANAGE_THREADS allows moderated choice");
    permit();
    application_forum_post_synchronize();
    check(application_forum_post_view().phase == ForumPhase::metadata_queued &&
              application_forum_post_view().selected_tags.count == 1 &&
              application_forum_post_view().tags[1].selected && !application_forum_post_view().tags[1].enabled &&
              load(31) && application_forum_post_view().tags[1].selected &&
              !application_forum_post_view().tags[1].enabled && draft(ticket) &&
              !application_forum_post_draft_submit(ticket),
          "forum: permission withdrawal preserves selected moderated draft tag while reporting ineligible during and after metadata refresh");
    const auto revoked_metadata_before = metadata_submissions;
    check(application_forum_post_tag_toggle(ticket, 101) &&
              !application_forum_post_view().tags[1].selected && !application_forum_post_view().tags[1].enabled &&
              !application_forum_post_view().selected_tags.count && !application_forum_post_view().error &&
              metadata_submissions == revoked_metadata_before,
          "forum: selected ineligible moderated tag remains removable without another fresh metadata GET");
    check(start(ticket, 32) && receipt(32, 500) &&
              application_forum_post_view().uncertain && application_forum_post_view().error &&
              !application_forum_post_view().can_submit &&
              application_forum_post_draft_update(ticket, utf8_literal("Edited uncertain draft"), utf8_literal("Starter message")) &&
              application_forum_post_view().phase == ForumPhase::failed && application_forum_post_view().error &&
              !application_forum_post_view().can_submit && !application_forum_post_pending(now + 100000) &&
              !application_forum_post_retry(ticket) && application_forum_post_inspect(ticket) &&
              inspections == 1 && application_forum_post_begin(70, &ticket) &&
              utf8_equal(application_forum_post_view().body, utf8_literal("Starter message")) &&
              application_forum_post_retry(ticket, true) && load(33) &&
              !application_forum_post_pending(now + 100000),
          "forum: ambiguous server-generated ID cannot auto-retry; inspection preserves draft; explicit "
          "acknowledgment only refreshes metadata");
    check(application_forum_post_draft_submit(ticket) && application_forum_post_submit(34, now) &&
              receipt(34, 200, post(902, 9)) && !row(902) && application_forum_post_view().uncertain,
          "forum: forged nested author cannot publish partial thread");
    reset_owner();
    ticket = begin();
    check(load(40) && start(ticket, 41), "forum: retired flight fixture");
    const auto old_nav = navigations;
    check(application_forum_post_cancel(ticket) && cancelled == 41 && forum_owner->flight.retired,
          "forum: cancel claims exact accepted POST and preserves its immutable drainage");
    auto replacement = begin();
    check(draft(replacement) && !application_forum_post_pending(now) && forum_owner->flight.id == 41 &&
              utf8_equal(forum_owner->flight.request.title, utf8_literal("Captured title")) &&
              receipt(41, 429, {}, now + 700) && navigations == old_nav &&
              !application_forum_post_pending(now + 699) && application_forum_post_next_wake() == now + 700,
          "forum: retired flight blocks replacement and late429 retains actor floor without navigation");
    now += 700;
    check(load(42) && application_forum_post_view().uncertain &&
              application_forum_post_retry(replacement, true) && load(43),
          "forum: retired POST uncertainty survives reopen until explicit acknowledgment");
    reset_owner();
    ticket = begin();
    check(load(50) && start(ticket, 51), "forum: account replacement fixture");
    g_app.account.status.profile_id = 9;
    application_forum_post_synchronize();
    check(forum_owner->retired && receipt(51, 200, post(903)) && !row(903),
          "forum: profile replacement rejects stale model publication");
    reset_owner();
    ticket = begin();
    check(load(60) && start(ticket, 61), "forum: parent generation fixture");
    ++row(70)->generation;
    application_forum_post_synchronize();
    check(forum_owner->retired && receipt(61, 200, post(904)) && !row(904),
          "forum: parent generation replacement cannot resurrect stale receipt");
    reset_owner();
    ticket = begin();
    check(load(70) && start(ticket, 71), "forum: failed navigation fixture");
    navigate_ok = false;
    check(receipt(71, 200, post(905)) && row(905) && forum_owner->known_created == 905 &&
              application_forum_post_view().can_retry &&
              utf8_equal(application_forum_post_view().body, utf8_literal("Starter message")),
          "forum: accepted post retained with intact draft after navigation admission failure");
    const auto sends_before = submissions;
    navigate_ok = true;
    check(application_forum_post_retry(ticket) && submissions == sends_before && navigated == 905,
          "forum: opening known created thread retries navigation without repeating POST");
    for (u32 kind = 0; kind < 2; ++kind) {
        reset_owner(); ticket = begin();
        check(load(200 + kind * 2) && start(ticket, 201 + kind * 2), "forum: retained nameless Gateway fixture");
        const u64 thread_id = 910 + kind;
        ChannelInput nameless{thread_id, 7, 70, {}, 0, 0, 11};
        nameless.name_known = kind != 0;
        nameless.thread_archived_known = nameless.thread_locked_known = true;
        check(chat_state_store_upsert_channel(&g_app.account_ui->chat_state, nameless).ok() &&
                  !chat_state_store_channel_name(g_app.account_ui->chat_state, *row(thread_id)).bytes,
              "forum: actual Core admits unknown and known-empty retained thread names");
        application_forum_post_gateway(7, 70, thread_id);
        const auto nav_before = navigations;
        const auto send_before = submissions;
        check(receipt(201 + kind * 2, 200, post(thread_id)) && navigations == nav_before &&
                  forum_owner->known_created == thread_id && !application_forum_post_view().uncertain &&
                  row(thread_id) && row(thread_id)->thread_parent_creation_noted &&
                  application_forum_post_view().can_retry &&
                  utf8_equal(application_forum_post_view().title, utf8_literal("Captured title")) &&
                  utf8_equal(application_forum_post_view().body, utf8_literal("Starter message")) &&
                  !application_forum_post_retry(ticket) && submissions == send_before &&
                  !chat_state_store_channel_name(g_app.account_ui->chat_state, *row(thread_id)).bytes,
              "forum: nameless canonical row keeps confirmed ID and draft, prevents blank-header navigation and repeat POST");
        nameless.name = utf8_literal("Later canonical name"); nameless.name_known = true;
        check(chat_state_store_upsert_channel(&g_app.account_ui->chat_state, nameless).ok(),
              "forum: later actual canonical name observation");
        application_forum_post_gateway(7, 70, thread_id);
        check(application_forum_post_retry(ticket) && submissions == send_before && navigated == thread_id &&
                  utf8_equal(chat_state_store_channel_name(g_app.account_ui->chat_state, *row(thread_id)),
                             utf8_literal("Later canonical name")),
              "forum: later canonical name unlocks navigation-only recovery without overwriting Gateway or repeating POST");
    }
    for (u32 kind = 0; kind < 2; ++kind) {
        reset_owner(); ticket = begin();
        check(load(210 + kind * 2) && start(ticket, 211 + kind * 2) && application_forum_post_observation_needed(),
              "forum: pre-response off-selected starter mutation fixture");
        const u64 thread_id = 912 + kind;
        ChannelInput canonical{thread_id, 7, 70, utf8_literal("Gateway name"), 0, 0, 11};
        canonical.name_known = true;
        canonical.thread_archived_known = canonical.thread_locked_known = true;
        check(chat_state_store_upsert_channel(&g_app.account_ui->chat_state, canonical).ok(),
              "forum: actual off-selected Gateway thread retained before starter mutation");
        application_forum_post_gateway(7, 70, thread_id);
        if (kind) {
            GatewayMessageEvent updated{}; updated.id = thread_id + 1000; updated.channel_id = thread_id;
            updated.author_id = 5; updated.content = utf8_literal("Fresher off-selected content");
            application_forum_post_message_observe(updated);
        } else application_forum_post_message_deleted(thread_id, thread_id + 1000);
        const auto send_before = submissions;
        check(receipt(211 + kind * 2, 200, post(thread_id)) && navigated == thread_id &&
                  forum_owner->known_created == thread_id && forum_owner->starter_stale &&
                  !message_store_find(g_app.message_view.store, thread_id + 1000).ok() &&
                  submissions == send_before && !application_forum_post_observation_needed() &&
                  utf8_equal(application_forum_post_view().body, utf8_literal("Starter message")),
              "forum: pre-navigation delete/update suppresses stale HTTP starter injection while normal history navigation opens confirmed post");
    }
    reset_owner();
    ticket = begin();
    check(load(72) && start(ticket, 73), "forum: parent metadata fence fixture");
    application_forum_post_gateway(7, 70);
    check(receipt(73, 200, post(906)) && !row(906) && application_forum_post_view().uncertain,
          "forum: changed parent metadata epoch cannot authorize stale POST receipt publication");
    reset_owner();
    ticket = begin();
    check(load(74) && start(ticket, 75), "forum: actual Core delete-race fixture");
    ChannelInput deleted{907, 7, 70, utf8_literal("Gateway post"), 0, 0, 11};
    deleted.thread_archived_known = deleted.thread_locked_known = true;
    check(chat_state_store_upsert_channel(&g_app.account_ui->chat_state, deleted).ok(),
          "forum: actual Gateway thread publication");
    application_forum_post_gateway(7, 70, 907);
    chat_state_store_remove_channel(&g_app.account_ui->chat_state, 907);
    application_forum_post_gateway(7, 70, 907);
    check(receipt(75, 200, post(907)) && !row(907) && application_forum_post_view().uncertain,
          "forum: matching Gateway create-delete cannot be resurrected by late HTTP receipt");
    reset_owner();
    ticket = begin();
    check(load(76) && start(ticket, 77), "forum: canonical starter fixture");
    canonical_on_navigate = true;
    check(receipt(77, 200, post(908)) && message_store_find(g_app.message_view.store, 1908).ok() &&
              utf8_equal(message_store_content(g_app.message_view.store,
                                               message_store_find(g_app.message_view.store, 1908).handle),
                         utf8_literal("Newer Gateway content")),
          "forum: fresher authoritative Gateway starter content wins over HTTP snapshot");
    reset_owner();
    ticket = begin();
    check(load(78) && start(ticket, 79), "forum: reentrant release fixture");
    release_on_paint = true;
    check(
        receipt(79, 200, post(909)) && !forum_owner && !form_visible,
        "forum: paint-triggered account release cannot free callback-pinned owner until response returns");
    reset_owner();
    ticket = begin();
    char unicode[8004];
    for (u32 i = 0; i < 2001; ++i) {
        unicode[i * 4] = '\xf0';
        unicode[i * 4 + 1] = '\x9f';
        unicode[i * 4 + 2] = '\x98';
        unicode[i * 4 + 3] = '\x80';
    }
    check(!application_forum_post_draft_update(ticket, utf8_literal("Title"), {unicode, 8004}) &&
              application_forum_post_draft_update(ticket, utf8_literal("Title"), {unicode, 8000}) &&
              application_forum_post_view().body.bytes == 8000,
          "forum: body bounds count supplementary scalars and UTF8 bytes");
    check(load(80) && application_forum_post_draft_submit(ticket), "forum: bounded busy fixture");
    admission = RestClientSubmitError::busy;
    for (u32 i = 0; i < 21; ++i) {
        application_forum_post_submit(100 + i, now);
        now += 50;
    }
    check(application_forum_post_view().phase == ForumPhase::failed && !forum_owner->flight.active &&
              !application_forum_post_next_wake(),
          "forum: bounded busy admission stops with retained draft and explicit retry");
    admission = {};
    application_forum_post_release();
    check(!forum_owner && !form_visible && !application_forum_post_metrics().owner_bytes,
          "forum: lazy owner and native form release without flight or draft leak");
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
}
} // namespace maicord::forum_test
namespace maicord::app_detail {
Application g_app{};
bool application_request_frame() noexcept {
    ++forum_test::frames;
    return true;
}
bool refresh_account_text(bool) noexcept {
    if (forum_test::release_on_paint) {
        application_forum_post_release();
        return false;
    }
    return true;
}
bool application_settings_active() noexcept {
    return false;
}
bool application_friends_active() noexcept {
    return false;
}
bool application_thread_create_cooldown_active(u64) noexcept { return false; }
void application_thread_create_note_created(u64) noexcept {}
#ifndef MAICORD_FORUM_ACTUAL_ATTACHMENTS
ApplicationForumAttachmentView application_forum_attachments_view(const ApplicationForumPostTicket&) noexcept { return {{}, {}, 0, false, false, true, false}; }
void application_forum_attachments_retire(const ApplicationForumPostTicket&) noexcept {}
void application_forum_attachments_lock(const ApplicationForumPostTicket&, bool) noexcept {}
void application_forum_attachments_refresh(const ApplicationForumPostTicket&) noexcept {}
bool application_forum_attachments_prepare(const ApplicationForumPostTicket&, Slice<UploadedAttachment>, Slice<u64>, u32* count) noexcept { *count = 0; return true; }
bool application_forum_attachments_url_candidate(const ApplicationForumPostTicket&, u64, ForumPostRequest*, u64*) noexcept { return false; }
bool application_forum_attachments_url_admitted(const ApplicationForumPostTicket&, u64) noexcept { return false; }
void application_forum_attachments_url_unadmitted(const ApplicationForumPostTicket&, u64) noexcept {}
void application_forum_attachments_url_response(const ApplicationForumPostTicket&, u64, const RestClientResponse&, Utf8View) noexcept {}
bool application_forum_attachments_decode_media(Utf8View, const ForumPostRequest&, u64, Slice<u32>, ForumPostSnapshot*) noexcept { return false; }
ConstSlice<GatewayMessageAttachment> application_forum_attachments_receipt() noexcept { return {}; }
#endif
bool application_forum_form_open(const ApplicationForumPostTicket& ticket) noexcept {
    if (!forum_test::form_ok || !application_forum_post_current(ticket))
        return false;
    forum_test::form = ticket;
    forum_test::form_visible = true;
    return true;
}
void application_forum_form_close(const ApplicationForumPostTicket& ticket) noexcept {
    if (forum_test::form.serial == ticket.serial)
        forum_test::form_visible = false;
}
bool application_thread_browser_open(u64 parent) noexcept {
    if (parent != 70)
        return false;
    ++forum_test::inspections;
    return true;
}
bool application_navigate_known_message(u64 id, u64 message) noexcept {
    const auto* thread = chat_state_store_find_channel(g_app.account_ui->chat_state, id);
    if (!forum_test::navigate_ok || !message || !thread ||
        !chat_state_store_thread_accessible(g_app.account_ui->chat_state, *thread))
        return false;
    ++forum_test::navigations;
    forum_test::navigated = id;
    if (g_app.account_ui->active_channel_id != id) {
        message_timeline_reset(&g_app.message_view.timeline);
        message_composer_reset(&g_app.message_composer, id, false);
    }
    g_app.account_ui->active_channel_id = id;
    g_app.account_ui->chat_state.selected_channel_id = id;
    if (forum_test::canonical_on_navigate) {
        GatewayMessageEvent event{};
        event.id = message;
        event.channel_id = id;
        event.author_id = 5;
        event.timestamp_ms = forum_owner->candidate.message_timestamp_ms;
        event.content = utf8_literal("Newer Gateway content");
        event.flags = 16;
        event.fields = gateway_message_field_id | gateway_message_field_channel_id |
                       gateway_message_field_author_id | gateway_message_field_content |
                       gateway_message_field_timestamp | gateway_message_field_flags |
                       gateway_message_field_type;
        forum_test::check(message_timeline_apply_created(&g_app.message_view.timeline, event).ok(),
                          "forum: actual canonical Gateway timeline application");
    }
    application_forum_post_synchronize(); // Production navigation refreshes modal owners synchronously.
    return true;
}
} // namespace maicord::app_detail
extern "C" __declspec(noreturn) void WINAPI maicord_application_forum_post_entry() noexcept {
    maicord::forum_test::run();
    ExitProcess(maicord::forum_test::failures ? 1 : 0);
}
