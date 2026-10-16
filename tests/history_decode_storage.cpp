#include "application_emoji_preferences.hpp"
#include "application_state.hpp"
#include "application_message_history.hpp"
#include "application_message_send.hpp"
#include "application_message_edit.hpp"
#include "application_message_mutation_notice.hpp"
#include "application_search.hpp"
#include "application_attachments.hpp"
#include "application_commands.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_composer_text_input.hpp"
#include "application_thread_actions.hpp"
#include "application_reply_draft.hpp"
#include "maicord/core/memory.hpp"
#include "application_action_captcha_stubs.hpp"
#include "test_output.hpp"

namespace maicord::history_decode_storage_test {
using namespace app_detail;
u32 failures, refreshes;
u32 slowmode_starts;
u64 slowmode_channel;
u32 notice_begins, notice_failures, notice_successes;
u64 notice_channel, notice_message;
bool notice_failed_before_reset;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}
void measurement(const char* name, usize value) noexcept {
    test_write_metric(name, value);
}
bool vm(const void* address, DWORD expected) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    return address && VirtualQuery(address, &info, sizeof(info)) == sizeof(info) && info.State == expected;
}
bool zero(const void* address, usize bytes) noexcept {
    const auto* data = static_cast<const u8*>(address);
    for (usize i = 0; i < bytes; ++i) if (data[i]) return false;
    return true;
}
template <typename T> bool same(const T& left, const T& right) noexcept {
    return bytes_equal(&left, &right, sizeof(T));
}
template <typename T> bool same_slice(Slice<T> left, Slice<T> right) noexcept {
    return left.data == right.data && left.count == right.count;
}
bool same_scratch(const MessageHistoryDecodeScratch& left, const MessageHistoryDecodeScratch& right) noexcept {
    return same_slice(left.page_structurals, right.page_structurals) &&
        same_slice(left.message_structurals, right.message_structurals) &&
        same_slice(left.content, right.content) && same_slice(left.messages, right.messages) &&
        same_slice(left.mention_users, right.mention_users) &&
        same_slice(left.rich.member_roles, right.rich.member_roles) &&
        same_slice(left.rich.reactions, right.rich.reactions) &&
        same_slice(left.rich.attachments, right.rich.attachments) &&
        same_slice(left.rich.embeds, right.rich.embeds) &&
        same_slice(left.rich.embed_fields, right.rich.embed_fields) &&
        same_slice(left.rich.stickers, right.rich.stickers) &&
        same_slice(left.rich.components, right.rich.components) &&
        same_slice(left.rich.snapshot.attachments, right.rich.snapshot.attachments) &&
        same_slice(left.rich.snapshot.embeds, right.rich.snapshot.embeds) &&
        same_slice(left.rich.snapshot.embed_fields, right.rich.snapshot.embed_fields) &&
        same_slice(left.rich.snapshot.stickers, right.rich.snapshot.stickers) &&
        same_slice(left.rich.snapshot.components, right.rich.snapshot.components) &&
        same_slice(left.rich.snapshot.mention_users, right.rich.snapshot.mention_users) &&
        left.maximum_depth == right.maximum_depth;
}
struct Fixture {
    MessageRecord records[4]; u32 order[4], heights[4]; u64 fenwick[4];
    u8 text[4096], draft_text[256], request_text[kMessageComposerMaximumBytes], rich_text[16384];
    char composer_text[256]; MessageDraftRecord drafts[2];
    MessageRichRecord rich_records[8]; u16 rich_order[4];
    RoleRecord roles[2];
    PermissionOverwriteRecord overwrites[2];
    u8 account_sentinel;
} fixture{};

// This fails the real VirtualAlloc commit in arena_allocate: mapped pages
// cannot be committed as a private reservation. It is an injected allocator
// rejection, not a claim to reproduce system-wide out-of-memory pressure.
struct CommitFailure {
    StableStorage& storage; u8* original; HANDLE mapping; u8* view;
    explicit CommitFailure(StableStorage& owner) noexcept : storage(owner), original(owner.arena.base),
        mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READONLY, 0,
            static_cast<DWORD>(owner.arena.reserved), nullptr)),
        view(mapping ? static_cast<u8*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, owner.arena.reserved)) : nullptr) {
        if (view) storage.arena.base = view;
    }
    ~CommitFailure() noexcept {
        storage.arena.base = original;
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
    }
    bool ready() const noexcept { return view != nullptr; }
};

bool reset_models(AccountUiRuntime& ui) noexcept {
    slowmode_starts = 0; slowmode_channel = 0;
    auto& view = g_app.message_view;
    g_app.message_pump = {};
    if (!message_store_initialize(&view.store, {fixture.records, 4}, {fixture.order, 4},
            {fixture.text, sizeof(fixture.text)}) ||
        !ui_variable_list_initialize(&view.list, {fixture.heights, 4}, {fixture.fenwick, 4}, 0, 100) ||
        !message_timeline_initialize(&view.timeline, &view.store, &view.list, 100, 100, true) ||
        !message_rich_store_initialize(&ui.message_rich_store, {fixture.rich_records, 8},
            {fixture.rich_order, 4}, {fixture.rich_text, sizeof(fixture.rich_text)}) ||
        !message_timeline_bind_rich_store(&view.timeline, &ui.message_rich_store) ||
        !chat_state_store_initialize(&ui.chat_state, {ui.guild_record_storage, kChatStateGuildCapacity},
            {ui.guild_order_storage, kChatStateGuildCapacity}, {ui.channel_record_storage, kChatStateChannelCapacity},
            {ui.channel_order_storage, kChatStateChannelCapacity}, {ui.chat_state_name_storage, kChatStateNameBytes}) ||
        !user_store_initialize(&ui.user_store, {ui.user_record_storage, kUserCapacity},
            {ui.user_order_storage, kUserCapacity}, {ui.user_name_storage, kUserNameBytes}) ||
        !guild_member_store_initialize(&ui.guild_member_store,
            {ui.guild_member_record_storage, kGuildMemberCapacity}, {ui.guild_member_order_storage, kGuildMemberCapacity}) ||
        !message_composer_initialize(&g_app.message_composer, {fixture.composer_text, sizeof(fixture.composer_text)}) ||
        !message_draft_store_initialize(&ui.message_drafts, {fixture.drafts, 2},
            {fixture.draft_text, sizeof(fixture.draft_text)}) ||
        !message_composer_controller_initialize(&ui.message_composer_controller, &g_app.message_composer,
            &ui.message_drafts, &view.timeline, {fixture.request_text, sizeof(fixture.request_text)})) return false;
    message_composer_reset(&g_app.message_composer, 7, false);
    ui.active_channel_id = 7; ui.chat_state.self_user_id = 11;
    g_app.account.instance = &fixture.account_sentinel;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 12;
    g_app.account.status.required_action = AccountRequirement::none;
    g_app.account.stop_failed = false;
    g_app.plugin_account_scope_serial = 13;
    g_app.login.visible = false;
    // Rich history includes guild-member roles. Its real observer requires a
    // known guild channel before it can commit the decoded member identities.
    if (!chat_state_store_upsert_guild(&ui.chat_state,
            {8, utf8_literal("Guild"), GuildMutationKind::create, 99}).ok() ||
        !chat_state_store_upsert_channel(&ui.chat_state, {7, 8, 0, utf8_literal("chat"), 0, 0, 0}).ok() ||
        !chat_state_store_bind_permissions(&ui.chat_state, {fixture.roles, 2}, {fixture.overwrites, 2}) ||
        !chat_state_store_upsert_role(&ui.chat_state, {8, 8,
            kPermissionViewChannel | kPermissionReadMessageHistory | kPermissionSendMessages,
            utf8_literal("Everyone")}) ||
        !chat_state_store_set_self_roles(&ui.chat_state, 8, 99, 11, {})) return false;
    chat_state_store_recompute_guild_permissions(&ui.chat_state, 8);
    const auto* channel = chat_state_store_find_channel(ui.chat_state, 7);
    constexpr u32 required = channel_action_view | channel_action_read_history |
        channel_action_send_messages;
    if (!channel || !channel->permissions_resolved || (channel->actions & required) != required) return false;
    g_app.message_pump.timeline = &view.timeline;
    g_app.message_pump.users = &ui.user_store;
    g_app.message_pump.state = &ui.chat_state;
    g_app.message_pump.members = &ui.guild_member_store;
    return true;
}
bool fresh_bank(AccountUiRuntime& ui) noexcept {
    application_history_decode_storage_release(ui);
    return application_history_decode_storage_initialize(ui);
}
bool content(u64 id, Utf8View expected) noexcept {
    const auto found = message_store_find(g_app.message_view.store, id);
    if (!found.ok()) return false;
    const auto actual = message_store_content(g_app.message_view.store, found.handle);
    return actual.bytes == expected.bytes && bytes_equal(actual.data, expected.data, actual.bytes);
}
u64 queue_send(AccountUiRuntime& ui) noexcept {
    message_composer_insert_utf8(&g_app.message_composer, utf8_literal("local"));
    const auto queued = message_composer_controller_queue_send(&ui.message_composer_controller, true, 11,
        1725012296789ull, clock_monotonic_milliseconds());
    if (!queued.ok() || !message_composer_controller_mark_submitted(&ui.message_composer_controller, queued.nonce).ok()) return 0;
    return queued.nonce;
}
constexpr auto rich_page = utf8_literal(
    R"([{"id":"500","channel_id":"7","author":{"id":"21","username":"alpha"},"content":"rich","timestamp":"2024-08-30T14:00:00Z","mentions":[{"id":"22","username":"Beta"}],"member":{"roles":["44","45"]},"reactions":[{"count":2,"count_details":{"normal":2,"burst":0},"me":true,"emoji":{"id":"44","name":"party","animated":true}}],"attachments":[{"id":"70","filename":"clip.mp4","width":0,"height":null,"size":123,"url":"https://cdn.discordapp.com/attachments/7/70/clip.mp4"}],"embeds":[{"title":"Build","type":"rich","thumbnail":{"url":"https://example.com/thumb.png","width":null,"height":0},"fields":[{"name":"Target","value":"Release"}]}],"sticker_items":[{"id":"88","name":"Wave","format_type":2}],"components":[{"type":10,"id":1,"content":"Component"}]}])");
constexpr auto send_body = utf8_literal(
    R"({"id":"900","channel_id":"7","author":{"id":"11","username":"self"},"content":"server","timestamp":"2024-08-30T12:00:00Z","attachments":[{"id":"901","filename":"image.png","size":64,"url":"https://cdn.discordapp.com/attachments/7/901/image.png","width":32,"height":32}]})");
constexpr auto edit_body = utf8_literal(
    R"({"id":"900","channel_id":"7","author":{"id":"11","username":"self"},"content":"edited","timestamp":"2024-08-30T12:00:00Z","edited_timestamp":"2024-08-30T13:00:00Z"})");
constexpr auto second_send_body = utf8_literal(
    R"({"id":"902","channel_id":"7","author":{"id":"11","username":"self"},"content":"second","timestamp":"2024-08-30T14:00:00Z"})");

void storage(AccountUiRuntime& ui, usize page) noexcept {
    check(application_history_decode_storage_initialize(ui), "history production bank reserve succeeds");
    auto& owner = ui.history_decode_storage; const auto* base = owner.arena.base;
    check(base && vm(base, MEM_RESERVE) && !owner.capacity && !owner.arena.committed &&
        !owner.arena.offset && !owner.arena.commit_count &&
        owner.reserved_capacity == sizeof(HistoryDecodeBank) / kHistoryDecodeStorageUnitBytes &&
        owner.element_bytes == kHistoryDecodeStorageUnitBytes && owner.alignment == alignof(HistoryDecodeBank),
        "history initial actual VM is reserved with zero commitment");
    measurement("MEASURE history_initial_committed_bytes=", owner.arena.committed);
    check(!application_history_decode_storage_initialize(ui) && owner.arena.base == base,
        "history repeated initialization preserves original reservation");
    MessageHistoryDecodeScratch scratch{}; scratch.maximum_depth = 123;
    const auto unchanged = scratch; owner.reserved_capacity = 0;
    check(!application_message_decode_scratch(ui, &scratch) && same(scratch, unchanged) &&
        vm(base, MEM_RESERVE) && !owner.capacity && !owner.arena.committed,
        "history denied admission preserves output and actual initial VM");
    owner.reserved_capacity = sizeof(HistoryDecodeBank) / kHistoryDecodeStorageUnitBytes;
    check(!application_message_decode_scratch(ui, nullptr) && !owner.arena.committed,
        "history null scratch output cannot trigger commitment");
    const bool admitted = application_message_decode_scratch(ui, &scratch);
    check(admitted, "history first access admits complete typed bank");
    if (!admitted) return;
    const usize committed = (kHistoryDecodeBaseBytes + page - 1) & ~(page - 1);
    check(owner.capacity == kHistoryDecodeBaseBytes / kHistoryDecodeStorageUnitBytes &&
        owner.arena.offset == kHistoryDecodeBaseBytes &&
        owner.arena.committed == committed && owner.arena.commit_count == 1 &&
        vm(base, MEM_COMMIT) && vm(base + committed - 1, MEM_COMMIT) && zero(base, kHistoryDecodeBaseBytes) &&
        vm(base + committed, MEM_RESERVE),
        "history first access commits exactly OS-rounded typed bank and untouched bytes are zero");
    const auto* bank = stable_storage_data<HistoryDecodeBank>(owner);
    check(scratch.messages.data == bank->messages && scratch.messages.count == kMessageHistoryRequestLimit &&
        scratch.mention_users.data == bank->named_users && scratch.mention_users.count == kMessageHistoryNamedUserCapacity &&
        scratch.rich.member_roles.data == bank->member_roles && scratch.rich.member_roles.count == kMessageHistoryMemberRoleCapacity &&
        scratch.rich.reactions.data == bank->reactions && scratch.rich.reactions.count == kMessageHistoryReactionCapacity &&
        scratch.rich.attachments.data == bank->attachments && scratch.rich.attachments.count == kMessageHistoryAttachmentCapacity &&
        scratch.rich.embeds.data == bank->embeds && scratch.rich.embeds.count == kMessageHistoryEmbedCapacity &&
        scratch.rich.embed_fields.data == bank->embed_fields && scratch.rich.embed_fields.count == kMessageHistoryEmbedFieldCapacity &&
        scratch.rich.stickers.data == bank->stickers && scratch.rich.stickers.count == kMessageHistoryStickerCapacity &&
        scratch.rich.components.data == bank->components && scratch.rich.components.count == kMessageHistoryComponentCapacity &&
        scratch.page_structurals.data == ui.message_history_page_structurals &&
        scratch.message_structurals.data == ui.message_history_object_structurals &&
        scratch.content.data == reinterpret_cast<char*>(ui.event_payload_scratch) && scratch.maximum_depth == 64,
        "history all nine typed slices retain original maximum capacities and existing structural/content unions");
    measurement("MEASURE history_first_committed_bytes=", owner.arena.committed);
    const auto first = scratch;
    for (u32 i = 0; i < 100; ++i) check(application_message_decode_scratch(ui, &scratch), "history repeated access succeeds");
    check(same_scratch(first, scratch) && owner.arena.base == base && owner.arena.commit_count == 1 &&
        owner.arena.committed == committed, "history repeat access retains identical slices and performs no extra commit");
    check(!scratch.rich.snapshot.attachments.data && !scratch.rich.snapshot.embeds.data &&
        !scratch.rich.snapshot.mention_users.data,
        "ordinary history exposes no uncommitted optional snapshot pointers");
    const auto forwarded_body = utf8_literal(R"({"message_snapshots":[{"message":{"content":"quote"}}]})");
    const auto before_snapshot = scratch;
    owner.reserved_capacity = owner.capacity;
    check(!application_message_decode_scratch(ui, &scratch, forwarded_body) &&
        same_scratch(before_snapshot, scratch) && owner.arena.committed == committed,
        "snapshot denied optional admission preserves output and committed base bank");
    owner.reserved_capacity = sizeof(HistoryDecodeBank) / kHistoryDecodeStorageUnitBytes;
    check(application_message_decode_scratch(ui, &scratch, forwarded_body),
        "forwarded history admits full independent snapshot pools in existing reservation");
    const usize full_committed = (sizeof(HistoryDecodeBank) + page - 1) & ~(page - 1);
    check(owner.arena.base == base && owner.arena.committed == full_committed &&
        owner.arena.commit_count == 2 && scratch.rich.attachments.data == first.rich.attachments.data &&
        scratch.rich.snapshot.attachments.data == bank->snapshot.attachments &&
        scratch.rich.snapshot.attachments.count == kMessageHistoryAttachmentCapacity &&
        scratch.rich.snapshot.embeds.data == bank->snapshot.embeds &&
        scratch.rich.snapshot.embed_fields.count == kMessageHistoryEmbedFieldCapacity &&
        scratch.rich.snapshot.stickers.count == kMessageHistoryStickerCapacity &&
        scratch.rich.snapshot.components.count == kMessageHistoryComponentCapacity &&
        scratch.rich.snapshot.mention_users.data == bank->snapshot.named_users &&
        scratch.rich.snapshot.mention_users.count == kMessageHistoryNamedUserCapacity,
        "snapshot pools keep full separate limits without new allocation owner or relocating outer arrays");
    check(application_message_body_may_have_snapshot(utf8_literal(R"({"message_\u0073napshots":[]})")) &&
        !application_message_body_may_have_snapshot(utf8_literal(R"({"content":"ordinary"})")),
        "optional admission conservatively includes escaped property names");
    measurement("MEASURE history_snapshot_additional_committed_bytes=", full_committed - committed);
    application_history_decode_storage_release(ui);
    check(!owner.arena.base && !owner.capacity && vm(base, MEM_FREE), "history actual account bank release frees reservation");
    application_history_decode_storage_release(ui);
}

void history(AccountUiRuntime& ui) noexcept {
    if (!fresh_bank(ui) || !reset_models(ui)) { check(false, "history integration initializes"); return; }
    g_app.message_pump.catchup_required = true;
    MessageHistoryCatchup catchup{}; MessageHistoryQuery query{};
    check(message_history_catchup_begin(&catchup, &g_app.message_pump, 7, 25) &&
        message_history_catchup_take_query(&catchup, &query), "history actual catchup awaits first response");
    RestClientResponse response{}; response.status_code = 200; response.body_bytes = rich_page.bytes;
    MessageHistoryRestResult accepted{}; MessageHistoryDecodeScratch scratch{};
    const auto before = catchup; const auto pump = g_app.message_pump; const auto store = g_app.message_view.store;
    const auto* base = ui.history_decode_storage.arena.base;
    {
        CommitFailure failure(ui.history_decode_storage);
        check(failure.ready(), "history actual commit failure mapping initializes");
        if (failure.ready()) check(!application_message_history_catchup_response(ui, catchup, g_app.message_pump,
                response, rich_page, 100, &accepted, &scratch) && same(catchup, before) && same(g_app.message_pump, pump) &&
            same(g_app.message_view.store, store) && !ui.user_store.count && !ui.guild_member_store.count &&
            !ui.history_decode_storage.arena.committed && !ui.history_decode_storage.arena.commit_count && vm(base, MEM_RESERVE),
            "history production catchup boundary rejects real allocator failure before decode or model mutation");
    }
    const bool integrated = application_message_history_catchup_response(ui, catchup, g_app.message_pump, response, rich_page,
        100, &accepted, &scratch) && accepted.accepted() && accepted.decode.page.count == 1 &&
        catchup.state == MessageHistoryCatchupState::complete && content(500, utf8_literal("rich")) &&
        scratch.messages[0].mention_users.count == 1 && scratch.messages[0].member_roles.count == 2 &&
        scratch.messages[0].reactions.count == 1 && scratch.messages[0].attachments.count == 1 &&
        scratch.messages[0].embeds.count == 1 && scratch.messages[0].embed_fields.count == 1 &&
        scratch.messages[0].stickers.count == 1 && scratch.messages[0].components.count == 1 &&
        ui.history_decode_storage.arena.base == base && ui.history_decode_storage.arena.commit_count == 1;
    check(integrated, "history actual catchup retries admission and preserves all decoded rich output at stable bank address");
    if (!integrated) {
        measurement("DIAGNOSTIC history_rest_error=", static_cast<usize>(accepted.error));
        measurement("DIAGNOSTIC history_decode_error=", static_cast<usize>(accepted.decode.error));
        measurement("DIAGNOSTIC history_message_error=", static_cast<usize>(accepted.decode.message_error));
        measurement("DIAGNOSTIC history_apply_error=", static_cast<usize>(accepted.apply.error));
        measurement("DIAGNOSTIC history_catchup_state=", static_cast<usize>(catchup.state));
        measurement("DIAGNOSTIC history_page_count=", accepted.decode.page.count);
        measurement("DIAGNOSTIC history_store_count=", g_app.message_view.store.count);
    }
    const auto* rich = message_rich_store_find(ui.message_rich_store, 500);
    memset(ui.event_payload_scratch, 0xa5, sizeof(ui.event_payload_scratch));
    check(rich && rich->attachment_count == 1 && rich->embed_count == 1 && rich->sticker_count == 1 &&
        rich->component_count == 1 && content(500, utf8_literal("rich")) &&
        message_rich_store_string(ui.message_rich_store, rich->attachments[0].url).bytes,
        "history rich and message stores retain owned fidelity after decode content is reused");
    // A success with no body remains a decode failure, without allocating scratch.
    check(fresh_bank(ui) && reset_models(ui), "history empty response fixture resets");
    g_app.message_pump.catchup_required = true; catchup = {};
    check(message_history_catchup_begin(&catchup, &g_app.message_pump, 7, 25) &&
        message_history_catchup_take_query(&catchup, &query), "history empty response awaits page");
    response.body_bytes = 0;
    check(application_message_history_catchup_response(ui, catchup, g_app.message_pump, response, {}, 100,
        &accepted, &scratch) && accepted.error == MessageHistoryRestError::decode &&
        accepted.decode.error == MessageHistoryDecodeError::invalid_input &&
        !ui.history_decode_storage.arena.committed, "history absent success body preserves classification and zero commitment");
}

void forwarded_history(AccountUiRuntime& ui) noexcept {
    constexpr auto body = utf8_literal(R"([
      {"id":"600","channel_id":"7","author":{"id":"21","username":"sender"},"content":"outer","timestamp":"2024-08-30T14:00:00Z",
       "message_reference":{"type":1,"message_id":"90","channel_id":"9"},"attachments":[{"id":"60","filename":"outer.txt","size":10,"url":"https://cdn.discordapp.com/attachments/7/60/outer.txt"}],
       "message_snapshots":[{"message":{"content":"quoted <@11>","timestamp":"2024-08-29T14:00:00Z","type":0,"mentions":[{"id":"11","username":"quoted-self"}],"attachments":[{"id":"61","filename":"inner.txt","size":11,"url":"https://cdn.discordapp.com/attachments/9/61/inner.txt"}]}}]},
      {"id":"599","channel_id":"7","author":{"id":"21","username":"sender"},"content":"second","timestamp":"2024-08-30T13:00:00Z",
       "message_reference":{"type":1,"message_id":"89","channel_id":"9"},"message_snapshots":[{"message":{"content":"second quote","timestamp":"2024-08-29T13:00:00Z","type":0,"attachments":[{"id":"62","filename":"second.txt","size":12,"url":"https://cdn.discordapp.com/attachments/9/62/second.txt"}]}}]}
    ])");
    check(fresh_bank(ui) && reset_models(ui), "forwarded production history fixture initializes");
    MessageHistoryDecodeScratch scratch{};
    check(application_message_decode_scratch(ui, &scratch), "forwarded fixture admits only ordinary base first");
    MessageHistoryCatchup catchup{}; MessageHistoryQuery query{};
    g_app.message_pump.catchup_required = true;
    check(message_history_catchup_begin(&catchup, &g_app.message_pump, 7, 25) &&
        message_history_catchup_take_query(&catchup, &query), "forwarded actual catchup awaits page");
    const auto before = catchup; const auto pump = g_app.message_pump;
    const auto store = g_app.message_view.store; const auto old_scratch = scratch;
    const u32 reserved = ui.history_decode_storage.reserved_capacity;
    ui.history_decode_storage.reserved_capacity = ui.history_decode_storage.capacity;
    RestClientResponse response{}; response.status_code = 200; response.body_bytes = body.bytes;
    MessageHistoryRestResult accepted{};
    check(!application_message_history_catchup_response(ui, catchup, g_app.message_pump,
        response, body, 100, &accepted, &scratch) && same(catchup, before) && same(pump, g_app.message_pump) &&
        same(store, g_app.message_view.store) && same_scratch(old_scratch, scratch),
        "forwarded optional capacity denial precedes HTTP tracker, users and resident timeline mutation");
    ui.history_decode_storage.reserved_capacity = reserved;
    const bool ok = application_message_history_catchup_response(ui, catchup, g_app.message_pump,
        response, body, 100, &accepted, &scratch) && accepted.accepted() && accepted.decode.page.count == 2;
    check(ok, "forwarded production catchup retries optional admission and decodes two immutable snapshots");
    if (!ok) return;
    check(scratch.messages[0].attachments.count == 1 && scratch.messages[0].snapshot.attachments.count == 1 &&
        scratch.messages[0].attachments.data != scratch.messages[0].snapshot.attachments.data &&
        scratch.messages[0].snapshot.attachments.data != scratch.messages[1].snapshot.attachments.data &&
        scratch.messages[0].snapshot.attachments[0].id == 61 && scratch.messages[1].snapshot.attachments[0].id == 62,
        "history outer and each snapshot publish disjoint independent descriptor slices");
    const auto* rich = message_rich_store_find(ui.message_rich_store, 600);
    const auto* child = message_rich_store_snapshot(ui.message_rich_store, 600);
    const auto found = message_store_find(g_app.message_view.store, 600);
    const auto* message = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    check(rich && child && child->forwarded_snapshot_child && rich->attachment_count == 1 &&
        child->attachment_count == 1 && child->attachments[0].id == 61 &&
        utf8_equal(message_rich_store_string(ui.message_rich_store, child->snapshot.content), utf8_literal("quoted <@11>")) &&
        message && !(message->presentation_flags & message_presentation_mentions_current_user) &&
        user_store_find(ui.user_store, 11),
        "forwarded production history owns child media/text and hydrates quoted names without notifying outer mentions");
    memset(ui.event_payload_scratch, 0xa5, sizeof(ui.event_payload_scratch));
    check(content(600, utf8_literal("outer")) && child &&
        utf8_equal(message_rich_store_string(ui.message_rich_store, child->snapshot.content), utf8_literal("quoted <@11>")),
        "forwarded retained snapshot survives shared decode text arena reuse");
}

void send(AccountUiRuntime& ui) noexcept {
    if (!fresh_bank(ui) || !reset_models(ui)) { check(false, "send integration initializes"); return; }
    const u64 nonce = queue_send(ui); check(nonce != 0, "send actual controller queued and submitted");
    RestClientResponse response{}; response.status_code = 200; response.body_bytes = send_body.bytes;
    const auto controller = ui.message_composer_controller; const auto store = g_app.message_view.store;
    const auto rejection = g_app.message_rejection; const auto rejection_channel = g_app.message_rejection_channel_id;
    const u32 refresh_before = refreshes;
    {
        CommitFailure failure(ui.history_decode_storage); check(failure.ready(), "send failure mapping initializes");
        if (failure.ready()) check(!accept_message_send_response(response, send_body) &&
            same(ui.message_composer_controller, controller) && same(g_app.message_view.store, store) &&
            same(g_app.message_rejection, rejection) && g_app.message_rejection_channel_id == rejection_channel &&
            content(nonce, utf8_literal("local")) && refreshes == refresh_before && !ui.history_decode_storage.arena.committed &&
            !slowmode_starts,
            "send actual response entrypoint rejects commit failure before HTTP tracker, decode, model and UI mutation");
    }
    const auto* base = ui.history_decode_storage.arena.base;
    check(accept_message_send_response(response, send_body) && content(900, utf8_literal("server")) &&
        !message_store_find(g_app.message_view.store, nonce).ok() && !ui.message_composer_controller.request_count &&
        !ui.message_composer_controller.inflight_nonce && ui.history_decode_storage.arena.commit_count == 1 &&
        slowmode_starts == 1 && slowmode_channel == 7,
        "send actual response retries and reconciles authoritative output on first bank commitment");
    const auto* detail = message_rich_store_find(ui.message_rich_store, 900);
    check(detail && detail->attachment_count == 1 && detail->attachments[0].size == 64,
        "send actual response retains unchanged attachment fidelity");
    check(queue_send(ui) != 0, "send second actual request submits");
    response.body_bytes = second_send_body.bytes;
    check(accept_message_send_response(response, second_send_body) && content(902, utf8_literal("second")) &&
        ui.history_decode_storage.arena.base == base && ui.history_decode_storage.arena.commit_count == 1 &&
        slowmode_starts == 2 && slowmode_channel == 7,
        "send subsequent actual response reuses committed bank without allocation");
}

void send_lifecycle(AccountUiRuntime& ui) noexcept {
    if (!reset_models(ui)) { check(false, "send cooldown lifecycle initializes"); return; }
    const auto saved_scene = g_app.message_composer_scene;
    auto& composer = g_app.message_composer;
    auto& controller = ui.message_composer_controller;
    g_app.message_composer_scene = {};
    g_app.message_composer_scene.active = true;
    g_app.message_composer_scene.model = &composer;
    g_app.message_composer_scene.channel_allows_send = true;
    check(message_composer_insert_utf8(&composer, utf8_literal("local")) == MessageComposerResult::changed &&
        queue_message_send() && controller.request_count == 1 && !controller.inflight_nonce &&
        !composer.bytes && !slowmode_starts,
        "send local queue admission without a submitted request does not predict slowmode");
    u64 queued_nonce = 0;
    for (const auto& request : controller.requests)
        if (request.occupied) { queued_nonce = request.nonce; break; }
    check(queued_nonce && message_composer_insert_utf8(&composer, utf8_literal("next draft")) ==
            MessageComposerResult::changed &&
        message_composer_controller_submit_failed(&controller, queued_nonce, clock_monotonic_milliseconds()).ok() &&
        !slowmode_starts && utf8_equal(message_composer_text(composer), utf8_literal("next draft")),
        "send local admission failure leaves the next draft without a predicted cooldown");
    g_app.message_composer_scene = saved_scene;

    struct RejectionCase { u16 status; RestClientError error; u32 code; const char* label; };
    constexpr RejectionCase rejections[] = {
        {403, RestClientError::none, 0, "send terminal 403 does not start predicted slowmode"},
        {403, RestClientError::none, 200000, "send AutoMod rejection does not start predicted slowmode"},
        {200, RestClientError::receive, 0, "send failed transport with 2xx status does not start predicted slowmode"},
        {429, RestClientError::none, 0, "send server retry wait does not start predicted slowmode"},
    };
    constexpr u64 retry_base = 900'000'000'000ull;
    for (const auto& rejection : rejections) {
        if (!reset_models(ui)) { check(false, "send rejection cooldown fixture initializes"); return; }
        const u64 nonce = queue_send(ui);
        RestClientResponse response{};
        response.status_code = rejection.status; response.error = rejection.error;
        response.rejection = {rejection.code, rejection.code != 0, false};
        response.rate_limited = rejection.status == 429;
        response.retry_at_ms = response.rate_limited ? retry_base + 500 : 0;
        check(nonce && accept_message_send_response(response, {}) && !slowmode_starts, rejection.label);
        if (response.rate_limited) {
            MessageComposerSubmit submission{};
            check(message_composer_controller_prepare_submit(&controller, retry_base + 499, &submission).error ==
                    MessageComposerControllerError::not_found &&
                message_composer_controller_prepare_submit(&controller, retry_base + 500, &submission).ok() &&
                submission.nonce == nonce && !slowmode_starts,
                "send authoritative 429 still refuses 499 and admits 500 without a local prediction");
        }
    }
    const Utf8View invalid_bodies[] = {{}, utf8_literal("{")};
    for (const auto body : invalid_bodies) {
        if (!reset_models(ui)) { check(false, "send invalid body cooldown fixture initializes"); return; }
        const u64 nonce = queue_send(ui);
        RestClientResponse response{}; response.status_code = 200; response.body_bytes = body.bytes;
        check(nonce && accept_message_send_response(response, body) && slowmode_starts == 1 &&
            slowmode_channel == 7 && !controller.inflight_nonce && controller.request_count == 1 &&
            content(nonce, utf8_literal("local")),
            "send successful receipt starts slowmode even when its absent or malformed body cannot reconcile");
    }
    if (!reset_models(ui)) { check(false, "send offscreen cooldown fixture initializes"); return; }
    const u64 offscreen_nonce = queue_send(ui);
    check(message_composer_controller_select_channel(&controller, 9).ok(), "send cooldown navigation selects another composer");
    ui.active_channel_id = 9;
    RestClientResponse success{}; success.status_code = 200; success.body_bytes = send_body.bytes;
    check(offscreen_nonce && accept_message_send_response(success, send_body) && slowmode_starts == 1 &&
        slowmode_channel == 7 && composer.channel_id == 9 && !controller.request_count,
        "send offscreen success starts cooldown for the captured request channel, not the active composer");
    check(!accept_message_send_response(success, send_body) && slowmode_starts == 1 && slowmode_channel == 7,
        "send duplicate receipt without an inflight request cannot restart any cooldown");

    for (u32 drained = 0; drained < 2; ++drained) {
        if (!reset_models(ui)) { check(false, "send Gateway-first cooldown fixture initializes"); return; }
        const u64 nonce = queue_send(ui);
        check(nonce && message_send_tracker_observe_gateway(&controller.sends, &g_app.message_view.timeline,
            nonce, 7, 11, 900, clock_monotonic_milliseconds()).ok(),
            "send Gateway-first cooldown observes delivery before its response");
        if (drained) {
            bool changed = false;
            MessageSendCompletion completion{};
            const u64 deadline = message_composer_controller_next_wake_at(controller);
            check(deadline && message_composer_controller_tick(&controller, deadline, &changed) &&
                message_composer_controller_take_completion(&controller, &completion) && completion.gateway_observed &&
                !controller.request_count && controller.inflight_nonce == nonce &&
                message_composer_controller_select_channel(&controller, 9).ok(),
                "send Gateway-first completion drains its request while retaining the pending HTTP nonce");
            ui.active_channel_id = 9;
        }
        check(accept_message_send_response(success, send_body) && !controller.inflight_nonce &&
            slowmode_starts == (drained ? 0u : 1u) && slowmode_channel == (drained ? 0u : 7u),
            drained ? "send drained late success has no request channel and cannot cool down the current channel"
                    : "send matched Gateway-first success predicts slowmode even without another model change");
    }
}

void edit(AccountUiRuntime& ui) noexcept {
    if (!fresh_bank(ui) || !reset_models(ui)) { check(false, "edit integration initializes"); return; }
    check(message_store_upsert(&g_app.message_view.store, {900, 7, 11, 1725012296789ull, 0,
        utf8_literal("old"), 0}).ok(), "edit authoritative record exists");
    auto& edit = g_app.message_edit;
    check(message_edit_runtime_initialize(&edit) && message_edit_runtime_reserve(&edit, 6, 0), "edit real request storage initializes");
    copy_bytes(edit.content, "edited", 6); edit.content_bytes = 6; edit.channel_id = 7;
    edit.message_id = 900; edit.phase = MessageEditRequestPhase::submitting;
    copy_bytes(ui.response_shape_scratch.response, edit_body.data, edit_body.bytes);
    RestClientResponse response{}; response.status_code = 200; response.body_bytes = edit_body.bytes;
    const auto before = edit; const auto store = g_app.message_view.store; const u32 refresh_before = refreshes;
    {
        CommitFailure failure(ui.history_decode_storage); check(failure.ready(), "edit failure mapping initializes");
        if (failure.ready()) check(!accept_message_edit_response(response) && same(edit, before) &&
            same(g_app.message_view.store, store) && content(900, utf8_literal("old")) &&
            bytes_equal(edit.content, "edited", 6) && refreshes == refresh_before && !ui.history_decode_storage.arena.committed,
            "edit actual response entrypoint rejects commit failure before decode, reconciliation or request reset");
    }
    const auto* base = ui.history_decode_storage.arena.base;
    check(accept_message_edit_response(response) && content(900, utf8_literal("edited")) &&
        !edit.message_id && !edit.content_bytes && edit.phase == MessageEditRequestPhase::idle &&
        ui.history_decode_storage.arena.commit_count == 1,
        "edit actual response retries with original request and applies full fidelity after admission");
    edit.message_id = 900; edit.channel_id = 7; edit.phase = MessageEditRequestPhase::submitting;
    check(accept_message_edit_response(response) && ui.history_decode_storage.arena.base == base &&
        ui.history_decode_storage.arena.commit_count == 1 && content(900, utf8_literal("edited")),
        "edit repeated response uses stable committed scratch without allocating");
    message_edit_runtime_release(&edit);
}

void caption_edit(AccountUiRuntime& ui) noexcept {
    if (!fresh_bank(ui) || !reset_models(ui) || !message_edit_runtime_initialize(&g_app.message_edit)) {
        check(false, "caption edit fixture initializes"); return;
    }
    auto& edit = g_app.message_edit;
    auto& composer = g_app.message_composer;
    g_app.message_composer_scene.channel_allows_send = true;
    const auto inserted = message_store_upsert(&g_app.message_view.store, {900, 7, 11, 1725012296789ull, 0, {}, 0});
    const auto* record = inserted.ok() ? message_store_resolve(g_app.message_view.store, inserted.handle) : nullptr;
    GatewayMessageAttachment attachment{}; attachment.id = 901; attachment.filename = utf8_literal("image.png");
    attachment.url = utf8_literal("https://cdn.discordapp.com/attachments/7/901/image.png"); attachment.size = 64;
    GatewayMessageEvent rich{}; rich.id = 900; rich.fields = gateway_message_field_attachments;
    rich.attachments = {&attachment, 1};
    check(record && message_rich_store_apply(&ui.message_rich_store, 900, record->revision, rich).ok(),
        "caption edit starts with authoritative owned attachment");
    message_composer_restore(&composer, 7, utf8_literal("draft"), 2, 4);
    check(application_reply_draft_set(800, false), "caption fixture actual draft owner retains reply target and ping policy");
    const bool sendable = application_thread_actions_channel_sendable(7);
    const bool available = application_message_edit_available(900);
    const bool began = available && begin_message_edit(900);
    check(sendable, "caption fixture actual scoped actor and role-derived channel can send");
    check(available, "caption fixture actual authoritative rich source is editable");
    check(began, "caption fixture production begin_message_edit admits source and saved draft");
    if (!sendable || !available || !began) {
        measurement("DIAG caption_actor_running=", g_app.account.status.running);
        measurement("DIAG caption_actor_instance=", g_app.account.instance != nullptr);
        measurement("DIAG caption_actor_profile=", g_app.account.status.profile_id);
        measurement("DIAG caption_actor_scope=", g_app.plugin_account_scope_serial);
        measurement("DIAG caption_actor_self=", ui.chat_state.self_user_id);
        measurement("DIAG caption_active_channel=", ui.active_channel_id);
        measurement("DIAG caption_composer_channel=", composer.channel_id);
        measurement("DIAG caption_edit_message=", edit.message_id);
        const auto* channel = chat_state_store_find_channel(ui.chat_state, 7);
        measurement("DIAG caption_channel_actions=", channel ? channel->actions : 0);
        measurement("DIAG caption_channel_resolved=", channel && channel->permissions_resolved);
        measurement("DIAG caption_source_author=", record ? record->author_id : 0);
        measurement("DIAG caption_source_delivery=", record ? static_cast<u32>(record->delivery) : ~0u);
        measurement("DIAG caption_source_content=", record ? static_cast<u32>(record->content_availability) : ~0u);
    }
    check(available && began && edit.editor_active &&
        !composer.bytes && application_reply_draft().message_id == 800 &&
        !application_reply_draft().mention_reply_author &&
        application_message_edit_retains_content(),
        "caption editor admits attachment-only message while saving reply and draft");
    check(message_composer_insert_utf8(&composer, utf8_literal("caption")) == MessageComposerResult::changed &&
        queue_message_edit() && edit.phase == MessageEditRequestPhase::queued && edit.content_bytes == 7 &&
        bytes_equal(edit.content, "caption", 7), "caption editor queues added text");
    const u32 admitted_notices = notice_begins;
    RestClientResponse failure{}; failure.status_code = 403;
    edit.phase = MessageEditRequestPhase::submitting;
    const u32 failures_before = notice_failures;
    check(accept_message_edit_response(failure) && edit.editor_active && edit.phase == MessageEditRequestPhase::idle &&
        composer.bytes == 7 && notice_failures == failures_before + 1 && notice_channel == 7 && notice_message == 900 &&
        notice_failed_before_reset, "caption edit permission failure retains draft and reports scoped notice before reset");
    check(queue_message_edit() && notice_begins == admitted_notices + 1,
        "caption edit retry admits a new scoped mutation notice");
    failure.error = RestClientError::timeout; edit.phase = MessageEditRequestPhase::submitting;
    check(accept_message_edit_response(failure) && edit.phase == MessageEditRequestPhase::idle && composer.bytes == 7 &&
        notice_failures == failures_before + 2, "caption edit transport failure retains editable draft and emits notice");
    check(queue_message_edit(), "caption edit retry after transport failure queues");
    failure = {}; failure.status_code = 429; failure.rate_limited = true; failure.retry_at_ms = 123456;
    edit.phase = MessageEditRequestPhase::submitting;
    check(accept_message_edit_response(failure) && edit.phase == MessageEditRequestPhase::retry_wait &&
        edit.retry_at_ms == 123456 && notice_failures == failures_before + 2,
        "caption edit authoritative rate limit retains request without terminal failure notice");
    failure.rejection.captcha = true;
    edit.phase = MessageEditRequestPhase::submitting;
    check(accept_message_edit_response(failure) && edit.editor_active && edit.phase == MessageEditRequestPhase::idle &&
        notice_failures == failures_before + 3,
        "caption edit required-action rate limit terminates retry and preserves draft");
    check(queue_message_edit(), "caption fixture queues after required-action response");
    constexpr auto caption_response = utf8_literal(
        R"({"id":"900","channel_id":"7","author":{"id":"11","username":"self"},"content":"caption","timestamp":"2024-08-30T12:00:00Z","edited_timestamp":"2024-08-30T13:00:00Z","attachments":[{"id":"901","filename":"image.png","size":64,"url":"https://cdn.discordapp.com/attachments/7/901/image.png"}]})");
    copy_bytes(ui.response_shape_scratch.response, caption_response.data, caption_response.bytes);
    RestClientResponse response{}; response.status_code = 200; response.body_bytes = caption_response.bytes;
    edit.phase = MessageEditRequestPhase::submitting;
    const u32 successes_before = notice_successes;
    check(accept_message_edit_response(response) && content(900, utf8_literal("caption")) && !edit.message_id &&
        composer.bytes == 5 && composer.anchor == 2 && composer.caret == 4 &&
        application_reply_draft().message_id == 800 && !application_reply_draft().mention_reply_author &&
        notice_successes == successes_before + 1 && notice_channel == 7 && notice_message == 900,
        "caption success restores saved draft selection and reply mention policy");
    record = message_store_resolve(g_app.message_view.store, message_store_find(g_app.message_view.store, 900).handle);
    const auto* owned = message_rich_store_find(ui.message_rich_store, 900);
    check(record && owned && owned->message_revision == record->revision && owned->attachment_count == 1 &&
        owned->attachments[0].id == 901, "caption PATCH response retains attachment at exact content revision");
    check(begin_message_edit(900) && message_composer_restore(&composer, 7, {}, 0, 0) && queue_message_edit() &&
        edit.phase == MessageEditRequestPhase::queued && !edit.content_bytes,
        "caption editor can clear text while retaining authoritative attachment");
    constexpr auto cleared_response = utf8_literal(
        R"({"id":"900","channel_id":"7","author":{"id":"11","username":"self"},"content":"","timestamp":"2024-08-30T12:00:00Z","edited_timestamp":"2024-08-30T14:00:00Z"})");
    copy_bytes(ui.response_shape_scratch.response, cleared_response.data, cleared_response.bytes);
    response.body_bytes = cleared_response.bytes; edit.phase = MessageEditRequestPhase::submitting;
    check(accept_message_edit_response(response) && content(900, {}) && application_message_edit_available(900),
        "caption-only clear preserves omitted attachment metadata through shared rich merge");
    ui.active_channel_id = 8;
    check(!application_message_edit_available(900) && !begin_message_edit(900),
        "caption edit cannot target a channel other than the active view");
    ui.active_channel_id = 7;
    check(begin_message_edit(900), "caption editor opens before attachment removal race");
    record = message_store_resolve(g_app.message_view.store, message_store_find(g_app.message_view.store, 900).handle);
    GatewayMessageEvent removed{}; removed.id = 900; removed.fields = gateway_message_field_attachments;
    check(record && message_rich_store_apply(&ui.message_rich_store, 900, record->revision, removed).ok() &&
        queue_message_edit() && edit.phase == MessageEditRequestPhase::idle &&
        !application_message_edit_retains_content(), "authoritative attachment removal blocks empty caption submission");
    check(cancel_message_edit_editor(), "removed attachment editor cancellation restores draft");
    record = message_store_resolve(g_app.message_view.store, message_store_find(g_app.message_view.store, 900).handle);
    check(record && message_rich_store_apply(&ui.message_rich_store, 900, record->revision, rich).ok(),
        "caption fixture restores authoritative attachment");

    check(message_store_upsert(&g_app.message_view.store, {902, 7, 11, 1725012296789ull, 0, utf8_literal("plain"), 0}).ok() &&
        begin_message_edit(902) && message_composer_restore(&composer, 7, {}, 0, 0) && queue_message_edit() &&
        edit.phase == MessageEditRequestPhase::idle && content(902, utf8_literal("plain")),
        "empty plain edit remains unsent and does not implicitly delete the message");
    check(cancel_message_edit_editor(), "plain empty editor cancellation restores draft");
    check(begin_message_edit(900), "attachment editor reopens for admission race guards");
    // Change the resident revision without replacing its rich entry. The old
    // attachment cannot authorize an empty edit of this newer message.
    check(message_store_upsert(&g_app.message_view.store, {900, 7, 11, 1725012296789ull, 0, {}, 4}).ok() &&
        queue_message_edit() && edit.phase == MessageEditRequestPhase::idle &&
        !application_message_edit_available(900) && !application_message_edit_retains_content(),
        "stale rich revision cannot authorize empty caption submission");
    check(cancel_message_edit_editor(), "stale caption editor cancellation succeeds");
    const auto current = message_store_find(g_app.message_view.store, 900);
    record = current.ok() ? message_store_resolve(g_app.message_view.store, current.handle) : nullptr;
    check(record && message_rich_store_apply(&ui.message_rich_store, 900, record->revision, rich).ok() &&
        begin_message_edit(900), "caption fixture refreshes authoritative rich revision");
    g_app.message_composer_scene.channel_allows_send = false;
    check(queue_message_edit() && edit.phase == MessageEditRequestPhase::idle,
        "retained attachment cannot bypass channel send permission");
    g_app.message_composer_scene.channel_allows_send = true;
    composer.text[0] = static_cast<char>(0xc0); composer.bytes = 1;
    check(queue_message_edit() && edit.phase == MessageEditRequestPhase::idle,
        "retained attachment cannot bypass UTF-8 content validation");
    message_composer_restore(&composer, 7, {}, 0, 0);
    check(message_store_upsert(&g_app.message_view.store, {900, 7, 12, 1725012296789ull, 0, {}, 0}).ok() &&
        queue_message_edit() && edit.phase == MessageEditRequestPhase::idle,
        "caption submit rechecks author ownership after editor opens");
    check(cancel_message_edit_editor(), "foreign-author edit cancellation restores draft");
    check(message_store_upsert(&g_app.message_view.store, {903, 7, 11, 1725012296789ull, 0, utf8_literal("voice"), 1u << 13}).ok() &&
        !application_message_edit_available(903) && !begin_message_edit(903), "voice messages cannot enter caption editor");
    check(message_store_upsert(&g_app.message_view.store, {903, 7, 11, 1725012296789ull, 0, utf8_literal("component"), 1u << 15}).ok() &&
        !application_message_edit_available(903), "Components V2 cannot enter text caption editor");
    check(message_store_upsert(&g_app.message_view.store, {903, 7, 11, 1725012296789ull, 0, utf8_literal("restricted"), 0,
        MessageContentAvailability::restricted}).ok() && !application_message_edit_available(903),
        "unavailable message content cannot be replaced by caption editor");
    check(message_store_upsert(&g_app.message_view.store, {903, 7, 11, 1725012296789ull, 0, utf8_literal("pending"), 0,
        MessageContentAvailability::available, MessageDeliveryState::pending}).ok() && !application_message_edit_available(903),
        "pending local message cannot enter authoritative caption editor");
    message_edit_runtime_release(&edit);
    check(application_reply_draft_cancel(), "caption fixture clears retained reply through its real owner");
}

void run() noexcept {
    SYSTEM_INFO system{}; GetSystemInfo(&system); const usize page = system.dwPageSize;
    check(page && !(page & (page - 1)), "history OS page size valid"); if (!page || (page & (page - 1))) return;
    auto* owner = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(owner != nullptr, "history actual production owner allocates"); if (!owner) return;
    g_app.account_ui = owner;
    MEMORY_BASIC_INFORMATION info{};
    check(VirtualQuery(owner, &info, sizeof(info)) == sizeof(info) && info.State == MEM_COMMIT &&
        info.RegionSize == ((sizeof(AccountUiRuntime) + page - 1) & ~(page - 1)),
        "history production owner commitment follows actual compiled size");
    measurement("MEASURE account_ui_owner_bytes=", sizeof(AccountUiRuntime));
    measurement("MEASURE account_ui_owner_committed_bytes=", info.RegionSize);
    measurement("MEASURE history_decode_bank_bytes=", sizeof(HistoryDecodeBank));
    measurement("MEASURE history_storage_owner_bytes=", sizeof(StableStorage));
    measurement("MEASURE history_snapshot_bank_bytes=", sizeof(HistorySnapshotDecodeBank));
    measurement("MEASURE history_inline_owner_bytes_removed=", kHistoryDecodeBaseBytes - sizeof(StableStorage));
    storage(*owner, page); history(*owner); forwarded_history(*owner);
    send(*owner); send_lifecycle(*owner); edit(*owner); caption_edit(*owner);
    const auto* base = owner->history_decode_storage.arena.base;
    application_history_decode_storage_release(*owner);
    check(!owner->history_decode_storage.arena.base && vm(base, MEM_FREE), "history final production owner releases active reservation");
    g_app.account_ui = nullptr; g_app.account.instance = nullptr;
    g_app.account.status = {}; g_app.plugin_account_scope_serial = 0;
    VirtualFree(owner, 0, MEM_RELEASE);
}
}

// Presentation and attachment effects are unrelated to decode admission. The
// actual response implementations are linked unchanged into this fixture.
namespace maicord::app_detail {
ApplicationEmojiUse application_emoji_preferences_scan(Utf8View) noexcept {
    return {};
}
void application_emoji_preferences_track_message(const ApplicationEmojiUse&) noexcept {
}
UiSearchScene* application_search_scene() noexcept { return nullptr; }
bool application_search_source_ticket(u32, ApplicationSearchSourceTicket*) noexcept { return false; }
bool application_search_source_resolve(const ApplicationSearchSourceTicket&, bool,
                                     ApplicationSearchSourceView*) noexcept { return false; }
void application_message_mutation_notice_begin(ApplicationMessageMutationKind, u64 channel, u64 message) noexcept {
    ++history_decode_storage_test::notice_begins;
    history_decode_storage_test::notice_channel = channel; history_decode_storage_test::notice_message = message;
}
void application_message_mutation_notice_failed(ApplicationMessageMutationKind, u64 channel, u64 message,
                                               const RestClientResponse&) noexcept {
    ++history_decode_storage_test::notice_failures;
    history_decode_storage_test::notice_channel = channel; history_decode_storage_test::notice_message = message;
    history_decode_storage_test::notice_failed_before_reset = g_app.message_edit.message_id == message &&
        g_app.message_edit.channel_id == channel && g_app.message_edit.phase == MessageEditRequestPhase::submitting;
}
void application_message_mutation_notice_succeeded(ApplicationMessageMutationKind, u64 channel, u64 message) noexcept {
    ++history_decode_storage_test::notice_successes;
    history_decode_storage_test::notice_channel = channel; history_decode_storage_test::notice_message = message;
}
bool refresh_account_text(bool) noexcept { ++history_decode_storage_test::refreshes; return true; }
bool refresh_account_frame(bool) noexcept { ++history_decode_storage_test::refreshes; return true; }
bool refresh_message_composer_ui() noexcept { ++history_decode_storage_test::refreshes; return true; }
void reset_message_composer_caret_timing(bool) noexcept {}
void application_composer_text_input_suspend() noexcept {}
bool application_composer_text_input_composing() noexcept { return false; }
bool stop_local_typing(u64) noexcept { return true; }
bool application_composer_prepare_send(bool* handled) noexcept { *handled = false; return true; }
void application_composer_note_sent(u64 channel) noexcept {
    ++history_decode_storage_test::slowmode_starts;
    history_decode_storage_test::slowmode_channel = channel;
}
bool application_mention_confirmation_request(bool* opened) noexcept {
    *opened = false;
    return true;
}
bool application_commands_send(bool* handled) noexcept { *handled = false; return true; }
bool application_plugins_compose(bool* handled) noexcept { *handled = false; return true; }
bool application_attachment_prepare_send(u64, Slice<UploadedAttachment>, Slice<u64>, u32* count, Slice<u64>) noexcept { *count = 0; return true; }
void application_attachment_did_queue(ConstSlice<u64>, u64) noexcept {}
void application_attachment_complete(u64) noexcept {}
void application_message_forward_completion(const MessageSendCompletion&) noexcept {}
}

extern "C" __declspec(noreturn) void WINAPI maicord_history_decode_storage_entry() noexcept {
    maicord::history_decode_storage_test::run();
    ExitProcess(maicord::history_decode_storage_test::failures ? 1 : 0);
}
