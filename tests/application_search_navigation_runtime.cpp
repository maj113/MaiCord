// Exercise the shipping navigation owner and real metadata decoder/model apply.
// Only account transport, accepted Search authority, and final UI selection are mocked.
#include "../src/app/application_search_navigation.cpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/permission_bits.hpp"

namespace maicord::search_navigation_test {
using namespace app_detail;
u32 failures{}, fetches{}, moves{}, composer_refreshes{}, frames{}, details_cancels{}, history_cancels{};
u64 fetched_channel{}, cancelled_id{};
u32 membership_fetches{};
ApplicationThreadActionTicket membership_ticket{};
ApplicationThreadMembershipStatus membership_status{};
bool source_valid{}, retire_source_on_move{}, resident_jump{};
RestClientSubmitError admission{};
ApplicationSearchNavigationSource accepted_source{};
Utf8View shown{};
GuildRecord guilds[2];
ChannelRecord channels[12];
RoleRecord roles[2];
PermissionOverwriteRecord overwrites[24];
u16 guild_order[2], channel_order[12];
u8 names[4096];
MessageRecord messages[8];
u32 message_order[8];
u8 message_text[2048];
void check(bool ok, const char* label) noexcept {
    test_check(&failures, ok, label);
}
bool contains(Utf8View text, Utf8View part) noexcept {
    for (u32 i=0; i+part.bytes<=text.bytes; ++i)
        if (bytes_equal(text.data+i, part.data, part.bytes)) return true;
    return false;
}
RestClientSubmitResult WINAPI fetch(void*, ChannelOverviewOperation op, u64 channel,
                                          const ChannelOverviewPatch* patch, u64) noexcept {
    check(op==ChannelOverviewOperation::fetch && !patch, "navigation uses GET metadata without patch");
    ++fetches; fetched_channel=channel;
    return {admission};
}
RestClientSubmitResult WINAPI cancel_details(void*, const MessagePinsRequest*, u64 id) noexcept {
    ++details_cancels; cancelled_id=id; return {};
}
RestClientSubmitResult WINAPI cancel_history(void*, AccountMessageSearchOperation op,
                                                     const MessageSearchQuery* query, u64 id) noexcept {
    check(op==AccountMessageSearchOperation::cancel && !query, "history cancellation uses exact primary sentinel");
    ++history_cancels; cancelled_id=id; return {};
}
bool reset(u64 target=70) noexcept {
    application_message_navigation_release();
    navigation={}; g_app.message_mutation_notice={}; shown={};
    source_valid=true; retire_source_on_move=false; resident_jump=false; admission={};
    moves=fetches=details_cancels=history_cancels=0;
    membership_fetches = 0; membership_ticket = {}; membership_status = ApplicationThreadMembershipStatus::unknown;
    accepted_source={{900, 5, 0}, target, 7, false};
    g_app.account_ui->active_channel_id=60;
    g_app.plugin_account_scope_serial=3; g_app.account.status.profile_id=5;
    ++g_app.load_trace.channel_epoch;
    g_app.pending_message_jump_channel_id=g_app.pending_message_jump_message_id=0;
    auto& state=g_app.account_ui->chat_state;
    if (!chat_state_store_initialize(&state,{guilds,2},{guild_order,2},{channels,12},{channel_order,12},{names,sizeof(names)}) ||
        !chat_state_store_bind_permissions(&state,{roles,2},{overwrites,24})) return false;
    constexpr u64 rights=kPermissionViewChannel | (1ull<<16);
    constexpr u64 guild_ids[]{7,8};
    for (u64 id : guild_ids) {
        if (!chat_state_store_upsert_guild(&state,{id,utf8_literal("Server"),GuildMutationKind::create,3}).ok() ||
            !chat_state_store_upsert_role(&state,{id,id,rights,utf8_literal("everyone")}) ||
            !chat_state_store_set_self_roles(&state,id,3,2,{})) return false;
    }
    if (!chat_state_store_upsert_channel(&state,{60,7,0,utf8_literal("Source"),0,0,0}).ok()) return false;
    state.selected_channel_id=60;
    chat_state_store_recompute_guild_permissions(&state,7);
    return message_store_initialize(&g_app.message_view.store,{messages,8},{message_order,8},{message_text,sizeof(message_text)}) &&
        message_store_upsert(&g_app.message_view.store,{600,60,2,1,0,utf8_literal("Source link"),0}).ok();
}
bool timeline_source(ApplicationMessageNavigationSource* source) noexcept {
    const auto found=message_store_find(g_app.message_view.store,600);
    const auto* row=message_store_resolve(g_app.message_view.store,found.handle);
    return found.ok() && row && application_message_navigation_source(60,600,found.handle,row->revision,source);
}
bool response(u64 id, Utf8View body, u16 status=200) noexcept {
    RestClientResponse value{}; value.request_id=id; value.body_bytes=body.bytes; value.status_code=status;
    return application_search_navigation_response(value,body);
}
constexpr auto ordinary=utf8_literal(R"({"id":"70","guild_id":"7","type":0,"name":"Target","permission_overwrites":[]})");
void rate_retry_tests() noexcept {
    // These are the same server receipts used by navigation and guild-settings
    // reads. Challenge classification must win even if stale rate fields exist.
    RestClientResponse eligible{};
    eligible.status_code = 429;
    eligible.rate_limited = true;
    eligible.retry_at_ms = 1;
    check(rest_rate_retry_deadline(eligible) == 1,
          "rate retry preserves an exact server deadline without a local minimum");
    for (u32 kind = 0; kind < 8; ++kind) {
        auto rejected = eligible;
        switch (kind) {
        case 0:
            rejected.rate_limited = false;
            break;
        case 1:
            rejected.retry_at_ms = 0;
            break;
        case 2:
            rejected.rejection.captcha = true;
            break;
        case 3:
            rejected.rejection = {40002, true, false};
            break;
        case 4:
            rejected.rejection = {40004, true, false};
            break;
        case 5:
            rejected.rejection = {20026, true, false};
            break;
        case 6:
            rejected.error = RestClientError::receive;
            break;
        case 7:
            rejected.status_code = 503;
            break;
        }
        check(!rest_rate_retry_deadline(rejected),
              "rate retry rejects challenges, missing authority and transport failures");
    }
    // Every rejected receipt reaches the same terminal owner branch; one
    // challenge with stale rate fields stands in for the whole class.
    auto rejected = eligible;
    rejected.rejection.captcha = true;
    check(reset() && application_search_navigation_begin(accepted_source.ticket) &&
              application_search_navigation_submit(501, 1),
          "terminal metadata rate fixture admits one actual request");
    rejected.request_id = 501;
    check(application_search_navigation_response(rejected, {}) &&
              navigation.phase == MessageNavigationPhase::idle &&
              !application_search_navigation_pending(~u64{0}) &&
              !application_search_navigation_next_wake() &&
              !application_search_navigation_submit(502, ~u64{0}) && fetches == 1 && !moves,
          "terminal metadata receipt schedules no wake or automatic readmission");

    check(reset() && application_search_navigation_begin(accepted_source.ticket) &&
              application_search_navigation_submit(503, 1) && response(503, ordinary),
          "terminal history rate fixture admits its destination");
    application_search_navigation_history_submitted(70, 900, 504);
    rejected.request_id = 504;
    MessageHistoryRestResult failed{};
    failed.error = MessageHistoryRestError::rate_limit;
    application_search_navigation_history_response(rejected, failed);
    // The production receipt caller retires its pending jump after delivery.
    g_app.pending_message_jump_channel_id = g_app.pending_message_jump_message_id = 0;
    application_search_navigation_synchronize();
    check(navigation.phase == MessageNavigationPhase::idle &&
              !application_search_navigation_next_wake() && !g_app.pending_message_jump_channel_id &&
              !g_app.pending_message_jump_message_id &&
              application_message_mutation_notice_is_error() && fetches == 1 && moves == 1,
          "terminal history receipt cannot recreate a pending around-message request");
    check(reset() && application_search_navigation_begin(accepted_source.ticket) &&
              application_search_navigation_submit(505, 1),
          "exact metadata deadline fixture admits");
    eligible.request_id = 505;
    check(application_search_navigation_response(eligible, {}) && navigation.retry_at == 1,
          "actual metadata owner preserves a server deadline below the old synthetic floor");
    check(reset() && application_search_navigation_begin(accepted_source.ticket) &&
              application_search_navigation_submit(506, 1) && response(506, ordinary),
          "exact history deadline fixture admits");
    application_search_navigation_history_submitted(70, 900, 507);
    eligible.request_id = 507;
    application_search_navigation_history_response(eligible, failed);
    check(navigation.phase == MessageNavigationPhase::history_retry && navigation.retry_at == 1,
          "actual history owner preserves a server deadline below the old synthetic floor");
}
void tests() noexcept {
    rate_retry_tests();
    check(reset() && application_search_navigation_begin(accepted_source.ticket) &&
        application_search_navigation_pending(1) && !moves, "uncached Jump queues metadata and does not report navigation");
    check(application_search_navigation_submit(10,1) && fetched_channel==70 && response(10,ordinary) && moves==1 &&
        g_app.pending_message_jump_channel_id==70 && navigation.admitted, "real GET decode hydrates readable channel and admits actual around-message navigation");
    application_search_navigation_history_submitted(70,900,11);
    RestClientResponse history{}; history.request_id=11; history.status_code=403;
    MessageHistoryRestResult failed{}; failed.error=MessageHistoryRestError::http_status;
    application_search_navigation_history_response(history,failed);
    check(application_message_mutation_notice_is_error() && contains(application_message_mutation_notice_text(),utf8_literal("cannot read")),
        "admitted history403 produces visible real composer notice");

    const u16 statuses[]{403,404};
    for (u16 status:statuses) {
        check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(20,1),"error metadata request admits");
        check(response(20,{},status) && !moves && shown.bytes && !chat_state_store_find_channel(g_app.account_ui->chat_state,70),
            "metadata403/404 never fabricates an accessible channel and gives visible feedback");
    }
    check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(30,1),"wrong metadata fixture admits");
    check(response(30,utf8_literal(R"({"id":"70","guild_id":"8","type":0,"name":"Wrong","permission_overwrites":[]})")) && !moves && shown.bytes,
        "different guild metadata rejected before model mutation");
    check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(31,1) &&
        response(31,utf8_literal("{broken")) && !moves && shown.bytes,"malformed metadata retains Search feedback without navigation");
    check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(32,1) &&
        response(32,utf8_literal(R"({"id":"70","guild_id":"7","type":0,"name":"Denied","permission_overwrites":[{"id":"7","type":0,"allow":"0","deny":"1024"}]})")) &&
        !moves && shown.bytes && !(chat_state_store_find_channel(g_app.account_ui->chat_state,70)->actions & channel_action_view),
        "authoritative permission overwrite denial cannot turn an accepted Search row into accessible navigation");

    check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(40,1),"retirement fixture admits");
    source_valid=false; application_search_navigation_synchronize();
    check(details_cancels==1 && cancelled_id==40 && response(40,ordinary) && !moves && !navigation.request_id,
        "source retirement cancels exact DETAILS request and drains obsolete response without hydration");

    check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(50,1),"newer Gateway fixture admits");
    check(chat_state_store_upsert_channel(&g_app.account_ui->chat_state,{70,7,0,utf8_literal("New Gateway name"),0,0,0}).ok(),"newer Gateway record arrives");
    chat_state_store_recompute_guild_permissions(&g_app.account_ui->chat_state,7);
    check(response(50,ordinary) && moves==1 &&
        contains(chat_state_store_channel_name(g_app.account_ui->chat_state,
            *chat_state_store_find_channel(g_app.account_ui->chat_state,70)),utf8_literal("New Gateway")),
        "older GET cannot overwrite the newer Gateway publication");
    check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(51,1),"exact response fixture admits");
    check(!response(52,ordinary) && navigation.request_id==51 && !moves,
        "different request completion cannot consume the live DETAILS ticket");
    check(response(51,ordinary),"matching completion still admits after ignored foreign response");

    check(reset(71) && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(60,1),"thread metadata fixture admits");
    constexpr auto child=utf8_literal(R"({"id":"71","guild_id":"7","parent_id":"72","type":11,"name":"Thread","thread_metadata":{"archived":true}})");
    check(response(60,child) && !moves && navigation.phase==MessageNavigationPhase::parent && application_search_navigation_submit(61,1) && fetched_channel==72,
        "uncached thread fetches bounded missing parent before admitting child");
    check(response(61,utf8_literal(R"({"id":"72","guild_id":"7","type":0,"name":"Parent","permission_overwrites":[]})")) &&
        application_search_navigation_submit(62,1) && fetched_channel==71 && response(62,child) && moves==1,
        "parent permissions hydrate then thread refetch admits around history");
    const auto* thread=chat_state_store_find_channel(g_app.account_ui->chat_state,71);
    check(thread && thread->thread_archived_known && thread->thread_archived && thread->parent_id==72,
        "real decoder/model preserves thread metadata");
    check(!membership_fetches, "archived public history never requires Join or current-member fetch");

    check(reset(71) && application_search_navigation_begin(accepted_source.ticket) &&
        application_search_navigation_submit(63,1) && response(63,child) &&
        application_search_navigation_submit(64,1), "forum post hydrates missing parent before selection");
    check(response(64,utf8_literal(R"({"id":"72","guild_id":"7","type":15,"name":"Forum","permission_overwrites":[]})")) &&
        application_search_navigation_submit(66,1) && response(66,child) && moves==1 && !membership_fetches,
        "real forum parent permissions allow archived public post reads without Join or Unarchive");

    check(reset(71) && chat_state_store_upsert_channel(&g_app.account_ui->chat_state,
        {72,7,0,utf8_literal("Parent"),0,0,0}).ok() &&
        chat_state_store_upsert_channel(&g_app.account_ui->chat_state,
        {71,7,72,utf8_literal("Private"),0,0,12}).ok(), "private thread and real parent permission model seed");
    chat_state_store_recompute_guild_permissions(&g_app.account_ui->chat_state,7);
    check(application_search_navigation_begin(accepted_source.ticket) && !moves && membership_fetches == 1 &&
        navigation.phase == MessageNavigationPhase::membership &&
        !application_search_navigation_pending(1), "unknown private access waits shared DETAILS member ticket without selection");
    check(chat_state_store_set_thread_self_membership(&g_app.account_ui->chat_state,71,2,true),
        "explicit current-self proof updates central private permission mask");
    membership_status = ApplicationThreadMembershipStatus::joined;
    application_search_navigation_synchronize();
    check(moves == 1 && navigation.admitted, "positive exact membership receipt permits private around-history selection");
    application_search_navigation_history_submitted(71,900,65);
    GatewayEnvelope revoke_envelope{};
    revoke_envelope.event_name = {utf8_literal("THREAD_MEMBERS_UPDATE"),0,false};
    revoke_envelope.has_event_name = true;
    revoke_envelope.data = {utf8_literal(R"({"guild_id":"7","id":"71","removed_member_ids":["2"]})"),0,JsonValueKind::object};
    u32 revoke_structurals[128]{}; char revoke_scratch[2048]{};
    GatewayClientEvent revoke_event{}; ConstSlice<u8> revoke_payload{};
    check(gateway_client_route_dispatch(revoke_envelope,{revoke_structurals,128},32,
        {revoke_scratch,sizeof(revoke_scratch)},{},2,&revoke_event,&revoke_payload).ok() &&
        chat_state_apply_thread_authority(revoke_event,revoke_payload,&g_app.account_ui->chat_state).ok(),
        "actual Gateway self-removal decoder and typed authority revoke admitted private target");
    application_search_navigation_synchronize();
    check(history_cancels == 1 && cancelled_id == 65 && !g_app.pending_message_jump_message_id,
        "membership revocation retires exact admitted history while retaining drain receipt");
    history.request_id = 65;
    application_search_navigation_history_response(history,failed);
    check(!navigation.request_id && moves == 1, "late private history completion cannot revive revoked selection action");
    check(application_search_navigation_begin(accepted_source.ticket) && moves == 1 && membership_fetches == 1 && shown.bytes,
        "known negative membership cannot become an unknown-proof bypass or silent refetch");

    check(reset(71) && chat_state_store_upsert_channel(&g_app.account_ui->chat_state,
        {72,7,0,utf8_literal("Parent"),0,0,0}).ok() &&
        chat_state_store_upsert_channel(&g_app.account_ui->chat_state,
        {71,7,72,utf8_literal("Private"),0,0,12}).ok(), "stale member receipt fixture seeds");
    chat_state_store_recompute_guild_permissions(&g_app.account_ui->chat_state,7);
    check(application_search_navigation_begin(accepted_source.ticket) && membership_fetches == 1,
        "stale member receipt request waits once");
    membership_status = ApplicationThreadMembershipStatus::stale;
    application_search_navigation_synchronize();
    check(!moves && navigation.phase == MessageNavigationPhase::idle && shown.bytes,
        "superseded shared member serial never selects private target and leaves explicit retry feedback");

    check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(70,1),"rate wait fixture admits");
    RestClientResponse limited{}; limited.request_id=70; limited.status_code=429; limited.rate_limited=true;
    limited.retry_at_ms=deadline_after(clock_monotonic_milliseconds(),10000);
    check(application_search_navigation_response(limited,{}) && !application_search_navigation_pending(1) &&
        application_search_navigation_next_wake()==limited.retry_at_ms && contains(shown,utf8_literal("Too many requests")),
        "metadata429 retains bounded owner and explicit rate waiting deadline");

    check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(75,1) && response(75,ordinary),"history429 fixture admits");
    application_search_navigation_history_submitted(70,900,76);
    limited.request_id=76;
    failed.error=MessageHistoryRestError::rate_limit;
    application_search_navigation_history_response(limited,failed);
    g_app.pending_message_jump_channel_id=g_app.pending_message_jump_message_id=0;
    check(navigation.phase==MessageNavigationPhase::history_retry && application_search_navigation_next_wake()==limited.retry_at_ms &&
        contains(application_message_mutation_notice_text(),utf8_literal("Retrying")) && !application_message_mutation_notice_is_error(),
        "admitted history429 keeps explicit waiting notice and exact retry deadline without terminal failure");

    check(reset() && application_search_navigation_link(accepted_source.ticket,{8,80,901,0}) && application_search_navigation_submit(80,1),"cross-server link fixture admits");
    retire_source_on_move=true;
    check(response(80,utf8_literal(R"({"id":"80","guild_id":"8","type":0,"name":"Other server","permission_overwrites":[]})")) && moves==1 && !source_valid,
        "uncached internal link hydrates separately validated target before intentional Search retirement");
    application_search_navigation_history_submitted(80,901,81);
    application_search_navigation_synchronize();
    check(navigation.request_id==81 && !history_cancels,"admitted target authority survives original Search scope retirement");
    history.request_id=81; history.status_code=404;
    application_search_navigation_history_response(history,failed);
    check(contains(application_message_mutation_notice_text(),utf8_literal("unavailable")),"cross-server history404 remains visible after Search disappears");

    check(reset() && application_search_navigation_begin(accepted_source.ticket) && application_search_navigation_submit(90,1) && response(90,ordinary),"history retirement fixture admits");
    application_search_navigation_history_submitted(70,900,91);
    ++g_app.plugin_account_scope_serial; application_search_navigation_synchronize();
    history.request_id=91; history.status_code=200;
    application_search_navigation_history_response(history,failed);
    check(history_cancels==1 && cancelled_id==91 && !navigation.request_id && !g_app.pending_message_jump_message_id,
        "admitted account retirement cancels exact primary history and drains without unrelated notices");
}
void timeline_tests() noexcept {
    ApplicationMessageNavigationSource source{};
    check(reset() && timeline_source(&source),"timeline capture resolves actual authoritative resident handle and source authority");
    check(message_store_upsert(&g_app.message_view.store,{600,60,2,1,2,utf8_literal("Edited source"),0}).ok() &&
        application_message_navigation_link(source,{7,70,900}) && !application_message_navigation_active() && !fetches,
        "edited source revision consumes stale link without admitting transport");
    check(reset() && timeline_source(&source) && message_store_delete(&g_app.message_view.store,600) &&
        message_store_upsert(&g_app.message_view.store,{600,60,2,1,0,utf8_literal("Replacement"),0}).ok() &&
        application_message_navigation_link(source,{7,70,900}) && !application_message_navigation_active(),
        "same message ID with replaced handle generation cannot admit stale source ticket");
    check(reset() && message_store_set_delivery(&g_app.message_view.store,600,MessageDeliveryState::pending) &&
        !timeline_source(&source),"pending local message is not an authoritative permalink source");
    check(reset() && timeline_source(&source),"source ABA capture admits");
    ++g_app.load_trace.channel_epoch;
    check(application_message_navigation_link(source,{7,70,900}) && !application_message_navigation_active(),
        "source leave-and-return epoch rejects otherwise identical channel and resident source");

    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(101,1) &&
        g_app.message_mutation_notice.shown.channel_id==60 && g_app.message_mutation_notice.shown.message_id==600 &&
        contains(application_message_mutation_notice_text(),utf8_literal("Loading channel")),
        "uncached timeline link reports metadata progress on clicked source channel");
    check(message_store_upsert(&g_app.message_view.store,{600,60,2,1,2,utf8_literal("Changed during GET"),0}).ok(),
        "actual source revision changes while GET is pending");
    application_search_navigation_synchronize();
    check(details_cancels==1 && cancelled_id==101 && navigation.request_id==101 && response(101,ordinary) && !moves &&
        !chat_state_store_find_channel(g_app.account_ui->chat_state,70),
        "source edit retires exact DETAILS flight and rejects late metadata publication");

    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(102,1),"timeline successful admission seeds");
    retire_source_on_move=true;
    check(response(102,ordinary) && moves==1 && navigation.admitted &&
        g_app.account_ui->chat_state.selected_channel_id==70,
        "timeline retirement during actual destination selection preserves admitted navigation");
    application_search_navigation_history_submitted(70,900,103);
    check(application_message_navigation_history_current(103,70,900) &&
        application_message_navigation_history_current(999,70,900),
        "exact admitted around-history is current while unrelated legacy receipt retains its own owner");
    application_search_navigation_release();
    check(application_message_navigation_history_current(103,70,900),"Search panel retirement preserves independently admitted timeline intent");
    ++g_app.load_trace.channel_epoch;
    check(!application_message_navigation_history_current(103,70,900),
        "destination leave-and-return ABA denies exact history before decoder can reset timeline");
    application_search_navigation_synchronize();
    check(history_cancels==1 && cancelled_id==103 && !application_message_navigation_history_current(103,70,900),
        "retired history ID stays fenced until exact completion drains");
    RestClientResponse history{}; history.request_id=103; history.status_code=200;
    MessageHistoryRestResult applied{}; applied.error=MessageHistoryRestError::none;
    application_search_navigation_history_response(history,applied);
    check(!navigation.request_id && !g_app.pending_message_jump_message_id,
        "late stale history completion drains without recreating pending jump");

    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(104,1),"timeline channel deletion seeds");
    application_message_navigation_gateway_deleted(7,70);
    check(details_cancels==1 && response(104,ordinary) && !moves &&
        !chat_state_store_find_channel(g_app.account_ui->chat_state,70),
        "uncached target deletion prevents older GET resurrection");
    check(reset(71) && timeline_source(&source) && application_message_navigation_link(source,{7,71,900}) &&
        application_search_navigation_submit(105,1),"unknown parent deletion seeds");
    application_message_navigation_gateway_deleted(7,72);
    check(response(105,utf8_literal(R"({"id":"71","guild_id":"7","parent_id":"72","type":11,"name":"Thread","thread_metadata":{"archived":false}})")) &&
        !moves && !application_message_navigation_active() && !chat_state_store_find_channel(g_app.account_ui->chat_state,71),
        "parent deleted before target GET reveals parent ID is retained in bounded tombstone ledger");
    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(106,1),"foreign deletion seeds");
    application_message_navigation_gateway_deleted(8,70);
    check(response(106,ordinary) && moves==1,"different guild deletion cannot retire captured target guild");
    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(107,1),"deletion ledger capacity seeds");
    for (u64 id=1000;id<1009;++id) application_message_navigation_gateway_deleted(7,id);
    check(details_cancels==1 && response(107,ordinary) && !moves && !application_message_navigation_active(),
        "bounded deletion ledger overflow fails closed without inventing absent authority");

    const bool deletes_source[]{true,false};
    for (bool source_deleted:deletes_source) {
        check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
            application_search_navigation_submit(108,1),"message deletion flight seeds");
        application_message_navigation_message_deleted(source_deleted?60:70,source_deleted?600:900);
        check(details_cancels==1 && response(108,ordinary) && !moves,
            "source or uncached target message deletion retires exact hydration before publication");
    }
    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(109,1),"READY retirement seeds");
    application_message_navigation_gateway_reset();
    check(details_cancels==1 && response(109,ordinary) && !moves,
        "fresh READY retires same actor flight before replacement authority is projected");

    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(112,1),"scope replacement flight seeds");
    ++g_app.plugin_account_scope_serial;
    application_search_navigation_synchronize();
    check(details_cancels==1 && response(112,ordinary) && !moves &&
        !chat_state_store_find_channel(g_app.account_ui->chat_state,70),
        "same module and profile with retired account scope cannot publish old metadata");
    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(113,1),"source authority withdrawal seeds");
    const PermissionOverwriteInput deny{7,0,kPermissionViewChannel,0};
    check(chat_state_store_append_channel_overwrite(&g_app.account_ui->chat_state,60,deny),
        "actual source permission overwrite withdrawal applies");
    chat_state_store_recompute_guild_permissions(&g_app.account_ui->chat_state,7);
    application_search_navigation_synchronize();
    check(details_cancels==1 && response(113,ordinary) && !moves,
        "source read authority is required through receipt admission without trusting original click");

    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(114,1),"Escape owned flight seeds");
    check(application_message_navigation_cancel() && !application_message_navigation_cancel() && details_cancels==1 &&
        navigation.request_id==114 && application_message_navigation_link(source,{7,70,901}) && fetches==1,
        "cancel retains exact drain ownership and consumes a second intent without a replacement request");
    check(response(114,ordinary) && !moves && !navigation.request_id,
        "matching cancelled response releases the shared metadata lane without hydration");
    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}) &&
        application_search_navigation_submit(115,1) && response(115,ordinary) && application_message_navigation_cancel(),
        "synchronous history admission cancellation seeds admitted target");
    application_search_navigation_history_submitted(70,900,116);
    check(history_cancels==1 && cancelled_id==116 && !application_message_navigation_history_current(116,70,900) &&
        !response(116,ordinary),"accepted PRIMARY ID remains fenced even when cancellation preceded submission callback");
    history.request_id=116;
    application_search_navigation_history_response(history,applied);
    check(!navigation.request_id,"synchronously cancelled PRIMARY completion drains through history boundary only");
    check(reset() && application_search_navigation_begin(accepted_source.ticket) &&
        application_search_navigation_submit(117,1) && application_message_navigation_cancel() && timeline_source(&source),
        "retired Search request and fresh resident timeline source coexist");
    source_valid=false;
    check(application_message_navigation_link(source,{7,70,900}) && fetches==1 && navigation.request_id==117 &&
        g_app.message_mutation_notice.shown.channel_id==60 && g_app.message_mutation_notice.shown.message_id==600 &&
        contains(application_message_mutation_notice_text(),utf8_literal("Another message")),
        "new timeline attempt shows source busy notice while cancelled Search ID drains");
    const auto busy_frames=frames;
    application_message_navigation_link(source,{7,70,900});
    check(frames==busy_frames && response(117,ordinary) && !moves && timeline_source(&source) &&
        application_message_navigation_link(source,{7,70,900}) && application_search_navigation_submit(118,1) && response(118,ordinary) && moves==1,
        "unchanged busy notice is stable and exact retired receipt releases fresh retry admission");
    check(application_message_navigation_owner_bytes()<=512,"shared owner footprint stays bounded independently of UI and decoder scratch");

    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{0,70,900}) && !fetches &&
        contains(application_message_mutation_notice_text(),utf8_literal("unavailable")),
        "missing private channel is visibly unavailable without ungrounded @me hydration");
    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{99,70,900}) && !fetches &&
        contains(application_message_mutation_notice_text(),utf8_literal("unavailable")),
        "unknown guild link fails visibly without treating unchanged source as stale");
    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{8,60,900}) && !moves &&
        contains(application_message_mutation_notice_text(),utf8_literal("unavailable")),
        "known destination with mismatched guild identity is unavailable before selection");
    check(reset() && chat_state_store_upsert_channel(&g_app.account_ui->chat_state,{80,0,0,utf8_literal("DM"),0,0,1}).ok() &&
        timeline_source(&source) && application_message_navigation_link(source,{0,80,900}) && moves==1 && !fetches,
        "known readable DM uses same admitted history owner without metadata guess");

    check(reset() && timeline_source(&source) && application_message_navigation_link(source,{7,70,900}),"busy metadata stage seeds");
    admission=RestClientSubmitError::busy;
    const auto before=frames;
    check(!application_search_navigation_submit(110,1) && !application_search_navigation_submit(111,51) && frames==before,
        "unchanged busy metadata retries do not repeatedly dirty source notice frames");
    check(application_message_navigation_cancel() && !application_message_navigation_active() && !details_cancels,
        "Escape cancels queued metadata before transport admission");
}
}
namespace maicord::app_detail {
ApplicationThreadActionEnqueueResult application_thread_actions_fetch(u64 id, ApplicationThreadActionTicket* ticket) noexcept {
    using namespace search_navigation_test;
    ++membership_fetches;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state,id);
    if (!ticket || !channel) return ApplicationThreadActionEnqueueResult::stale;
    membership_ticket = {membership_fetches,g_app.plugin_account_scope_serial,g_app.account.status.profile_id,
        g_app.account_ui->chat_state.self_user_id,id,reinterpret_cast<usize>(g_app.account.instance),
        channel->revision,1,channel->generation};
    *ticket = membership_ticket; membership_status = ApplicationThreadMembershipStatus::pending;
    return ApplicationThreadActionEnqueueResult::accepted;
}
ApplicationThreadMembershipStatus application_thread_actions_membership(const ApplicationThreadActionTicket& ticket) noexcept {
    return ticket.serial == search_navigation_test::membership_ticket.serial ? search_navigation_test::membership_status
        : ApplicationThreadMembershipStatus::stale;
}
Utf8View application_thread_actions_admission_text(ApplicationThreadActionEnqueueResult) noexcept { return utf8_literal("Unable to check access."); }
bool application_search_navigation_source(const ApplicationSearchSourceTicket& ticket, ApplicationSearchNavigationSource* out) noexcept {
    using namespace search_navigation_test;
    if (!source_valid || !out || ticket.message_id!=accepted_source.ticket.message_id || ticket.publication_revision!=accepted_source.ticket.publication_revision) return false;
    *out=accepted_source; return true;
}
bool application_search_source_notice(const ApplicationSearchSourceTicket& ticket, Utf8View text) noexcept {
    ApplicationSearchNavigationSource source{};
    if (!application_search_navigation_source(ticket,&source)) return false;
    search_navigation_test::shown=text; return true;
}
bool application_navigate_known_message(u64 channel, u64 message) noexcept {
    using namespace search_navigation_test;
    ++moves; g_app.account_ui->active_channel_id=channel;
    g_app.account_ui->chat_state.selected_channel_id=channel; ++g_app.load_trace.channel_epoch;
    if (retire_source_on_move) {
        source_valid=false;
        message_store_initialize(&g_app.message_view.store,{messages,8},{message_order,8},{message_text,sizeof(message_text)});
    }
    application_search_navigation_synchronize(); // Selection can synchronize Search before helper returns.
    if (!resident_jump) { g_app.pending_message_jump_channel_id=channel; g_app.pending_message_jump_message_id=message; }
    return true;
}
bool refresh_message_composer_ui() noexcept { ++search_navigation_test::composer_refreshes; return true; }
bool application_request_frame() noexcept { ++search_navigation_test::frames; return true; }
Application g_app{};
}
extern "C" void maicord_application_search_navigation_runtime_entry() noexcept {
    using namespace maicord; using namespace app_detail;
    g_app.account_ui=static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr,sizeof(AccountUiRuntime),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if (!g_app.account_ui) ExitProcess(1);
    g_app.account.instance=&search_navigation_test::accepted_source;
    g_app.account.status.running=true; g_app.account.status.profile_id=5; g_app.plugin_account_scope_serial=3;
    g_app.account.api.channel_overview=search_navigation_test::fetch;
    g_app.account.api.pinned_messages=search_navigation_test::cancel_details;
    g_app.account.api.message_search=search_navigation_test::cancel_history;
    search_navigation_test::tests();
    search_navigation_test::timeline_tests();
    application_message_navigation_release(); VirtualFree(g_app.account_ui,0,MEM_RELEASE);
    ExitProcess(search_navigation_test::failures?1:0);
}
