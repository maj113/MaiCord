// Exercise the shipping host, protocol decoders and data model. Only REST,
// monotonic time and frame publication are injected; no account or socket runs.
#include "application_thread_actions.hpp"
#include "application_state.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "test_output.hpp"

#include "maicord/sys/winsock.hpp"

namespace maicord::thread_actions_runtime_test {
u64 now = 1'000;
u32 allocations{}, frees{}, frames{}, failures{};
bool fail_allocation{};
}
static LPVOID WINAPI thread_fixture_allocate(LPVOID address, SIZE_T bytes, DWORD kind, DWORD protect) noexcept {
    auto& count = maicord::thread_actions_runtime_test::allocations;
    if (maicord::thread_actions_runtime_test::fail_allocation) return nullptr;
    auto* result = ::VirtualAlloc(address, bytes, kind, protect);
    if (result) ++count;
    return result;
}
static BOOL WINAPI thread_fixture_free(LPVOID address, SIZE_T bytes, DWORD kind) noexcept {
    const auto result = ::VirtualFree(address, bytes, kind);
    if (result) ++maicord::thread_actions_runtime_test::frees;
    return result;
}
namespace maicord {
static u64 thread_fixture_now() noexcept { return thread_actions_runtime_test::now; }
}
#define VirtualAlloc thread_fixture_allocate
#define VirtualFree thread_fixture_free
#define clock_monotonic_milliseconds thread_fixture_now
#include "../src/app/application_thread_actions.cpp"
#undef clock_monotonic_milliseconds
#undef VirtualFree
#undef VirtualAlloc

namespace maicord::app_detail {
Application g_app{};
bool application_request_frame() noexcept {
    ++thread_actions_runtime_test::frames;
    g_app.frame_pending = true;
    return true;
}
}

namespace maicord::thread_actions_runtime_test {
using namespace app_detail;
using Op = ThreadActionOperation;
using Admission = ApplicationThreadActionEnqueueResult;
using Phase = ApplicationThreadActionPhase;
using Membership = ApplicationThreadMembershipStatus;
GuildRecord guilds[2]{};
u16 guild_order[2]{};
ChannelRecord channels[8]{};
u16 channel_order[8]{};
u8 names[1024]{};
u8 account_marker{}, replacement_marker{};
u64 sequence = 100, accepted_id{}, cancelled_id{};
u32 submissions{}, cancellations{};
u32 metadata_submissions{};
bool last_metadata{};
ThreadActionRequest submitted{};
RestClientSubmitError admission_error{};

void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}
ChannelRecord* model_channel(u64 id = 20) noexcept {
    return const_cast<ChannelRecord*>(chat_state_store_find_channel(g_app.account_ui->chat_state, id));
}
RestClientSubmitResult WINAPI submit(void* instance, const ThreadActionRequest* request,
                                           u64 id) noexcept {
    if (instance != &account_marker || !id) return {RestClientSubmitError::invalid_input};
    if (!request) {
        ++cancellations; cancelled_id = id;
        return {id == accepted_id ? RestClientSubmitError::none : RestClientSubmitError::busy};
    }
    ++submissions;
    if (admission_error != RestClientSubmitError::none) return {admission_error};
    if (accepted_id || !thread_action_request_valid(*request)) return {RestClientSubmitError::busy};
    accepted_id = id; submitted = *request; last_metadata = false;
    return {};
}
RestClientSubmitResult WINAPI submit_metadata(void* instance, ChannelOverviewOperation operation,
    u64 thread_id, const ChannelOverviewPatch* patch, u64 id) noexcept {
    if (instance != &account_marker || !id || !thread_id || patch)
        return {RestClientSubmitError::invalid_input};
    if (operation != ChannelOverviewOperation::fetch) return {RestClientSubmitError::invalid_input};
    ++metadata_submissions;
    if (admission_error != RestClientSubmitError::none) return {admission_error};
    if (accepted_id) return {RestClientSubmitError::busy};
    accepted_id = id; last_metadata = true;
    return {};
}
bool reset(u8 type = 11, bool joined = false, bool known = false) noexcept {
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    g_app.details_request_id = 0;
    application_thread_actions_release();
    accepted_id = cancelled_id = 0;
    admission_error = RestClientSubmitError::none;
    fail_allocation = false;
    now = 1'000;
    g_app.account.instance = &account_marker;
    g_app.account.status.running = true;
    g_app.account.status.profile_id = 4;
    g_app.account.status.required_action = AccountRequirement::none;
    g_app.account.stop_failed = false;
    g_app.account.api.thread_action = submit;
    g_app.account.api.channel_overview = submit_metadata;
    g_app.plugin_account_scope_serial = 7;
    g_app.login.visible = false;
    auto& state = g_app.account_ui->chat_state;
    if (!chat_state_store_initialize(&state, {guilds, 2}, {guild_order, 2},
        {channels, 8}, {channel_order, 8}, {names, sizeof(names)})) return false;
    state.self_user_id = 5;
    if (!chat_state_store_upsert_guild(&state, {10, utf8_literal("Guild"), GuildMutationKind::create, 99}).ok() ||
        !chat_state_store_upsert_channel(&state, {11, 10, 0, utf8_literal("Parent"), 0, 0, 0}).ok()) return false;
    constexpr u64 thread_ids[]{20, 21};
    for (u64 id : thread_ids) {
        ChannelInput input{};
        input.id = id; input.guild_id = 10; input.parent_id = 11; input.name = utf8_literal("Thread"); input.type = type;
        input.thread_archived_known = true; input.thread_locked_known = true;
        input.thread_self_joined = joined; input.thread_self_joined_known = known;
        if (!chat_state_store_upsert_channel(&state, input).ok()) return false;
    }
    constexpr u64 permissions = kPermissionViewChannel | kPermissionReadMessageHistory |
        kPermissionSendMessagesInThreads;
    for (u32 i = 0; i < state.channel_count; ++i) {
        auto& value = state.channels[state.channel_order[i]];
        value.permissions_resolved = true; value.effective_permissions = permissions;
        value.actions = channel_is_thread(value.type)
            ? chat_state_store_thread_actions(state, value) : channel_actions(value.type, permissions, true);
    }
    g_app.account_ui->active_channel_id = 20;
    state.selected_channel_id = 20;
    return true;
}
ApplicationThreadActionTicket fetch(u64 thread_id = 20) noexcept {
    ApplicationThreadActionTicket ticket{};
    check(application_thread_actions_fetch(thread_id, &ticket) == Admission::accepted,
        "thread runtime: fetch queues a current actor/target receipt");
    return ticket;
}
bool send() noexcept {
    const u64 id = ++sequence;
    if (!application_thread_actions_submit(id, now)) return false;
    g_app.details_request_owner = MessageHistoryRequestOwner::thread_actions;
    g_app.details_request_id = id;
    return true;
}
bool receipt(u16 status, Utf8View body = {}, RestClientError error = RestClientError::none,
             u64 retry_at = 0, bool required_action = false) noexcept {
    RestClientResponse response{};
    response.request_id = accepted_id; response.status_code = status; response.body_bytes = body.bytes;
    response.error = error; response.retry_at_ms = retry_at; response.rate_limited = status == 429;
    if (required_action) response.rejection = {20026, true, false};
    accepted_id = 0;
    const bool handled = application_thread_actions_response(response, body);
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    g_app.details_request_id = 0;
    return handled;
}
constexpr auto joined_body = utf8_literal(
    R"({"id":"20","user_id":"5","join_timestamp":"2026-10-05T12:00:00Z","flags":0})");
void membership_tests() noexcept {
    check(reset(), "thread runtime: authoritative model fixture initialized");
    check(!application_thread_actions_metrics().owner_bytes && !application_thread_actions_next_wake(),
        "thread runtime: no owner before first explicit request");
    const auto ticket = fetch();
    const auto metrics = application_thread_actions_metrics();
    check(metrics.entry_count == 1 && metrics.owner_bytes <= 4096 && metrics.owner_committed_bytes == 4096 &&
        application_thread_actions_pending(now) && application_thread_actions_membership(ticket) == Membership::pending,
        "thread runtime: first use commits exactly one bounded lazy page");
    ApplicationThreadActionTicket coalesced{};
    check(application_thread_actions_fetch(20, &coalesced) == Admission::coalesced && coalesced.serial == ticket.serial,
        "thread runtime: repeated fetch shares existing intent serial");
    check(send() && submitted.thread_id == 20 && submitted.self_user_id == 5 && submitted.operation == Op::fetch_self &&
        !model_channel()->thread_self_joined_known && !application_thread_actions_next_wake(),
        "thread runtime: admission does not forge membership or busy-poll an occupied DETAILS owner");
    RestClientResponse wrong{}; wrong.request_id = accepted_id + 1; wrong.status_code = 200; wrong.body_bytes = joined_body.bytes;
    check(!application_thread_actions_response(wrong, joined_body) && application_thread_actions_metrics().flight_active,
        "thread runtime: unrelated response cannot drain current thread flight");
    check(receipt(200, joined_body) && model_channel()->thread_self_joined_known && model_channel()->thread_self_joined &&
        application_thread_actions_membership(ticket) == Membership::joined,
        "thread runtime: exact decoded200 establishes self membership through production model");
    const u32 before_frames = frames, before_alloc = allocations, before_calls = submissions;
    const auto before_metrics = application_thread_actions_metrics();
    const u32 before_revision = model_channel()->revision;
    for (u32 i = 0; i < 8; ++i) {
        application_thread_actions_synchronize();
        application_thread_actions_view(20);
        application_thread_actions_pending(now);
    }
    const auto quiet = application_thread_actions_metrics();
    check(frames == before_frames && allocations == before_alloc && submissions == before_calls &&
        quiet.publication_revision == before_metrics.publication_revision && quiet.owner_committed_bytes == 4096 &&
        model_channel()->revision == before_revision && !application_thread_actions_next_wake(),
        "thread runtime: unchanged passes neither allocate repaint submit mutate nor schedule wake");
    fetch(); check(send() && receipt(404) && model_channel()->thread_self_joined_known && !model_channel()->thread_self_joined,
        "thread runtime: fetch404 is an authoritative not-joined receipt");
    fetch(); check(send() && receipt(403) && !model_channel()->thread_self_joined_known &&
        application_thread_actions_view(20).phase == Phase::failed,
        "thread runtime: fetch403 remains unknown/error and cannot become not-joined");
    fetch(); check(send() && receipt(200, utf8_literal(
        R"({"id":"20","user_id":"6","join_timestamp":"2026-10-05T12:00:00Z","flags":0})")) &&
        !model_channel()->thread_self_joined_known && application_thread_actions_view(20).phase == Phase::failed,
        "thread runtime: foreign self member200 fails without publishing membership");
    fetch(); check(send() && receipt(200, utf8_literal(
        R"({"id":"20","user_id":"5","join_timestamp":"not-a-date","flags":0})")) &&
        !model_channel()->thread_self_joined_known && application_thread_actions_view(20).phase == Phase::failed,
        "thread runtime: malformed member200 cannot become authoritative membership");
    fetch(); check(send(), "thread runtime: body length fence request admitted");
    RestClientResponse truncated{};
    truncated.request_id = accepted_id; truncated.status_code = 200;
    truncated.body_bytes = joined_body.bytes + 1;
    accepted_id = 0;
    check(application_thread_actions_response(truncated, joined_body) &&
        !model_channel()->thread_self_joined_known && application_thread_actions_view(20).phase == Phase::failed,
        "thread runtime: member body/receipt length mismatch fails before decode publication");
    g_app.details_request_owner = MessageHistoryRequestOwner::none; g_app.details_request_id = 0;
    check(allocations == before_alloc, "thread runtime: settled failed and repeated requests reuse one owner page");
    reset(12);
    check(!application_thread_actions_channel_readable(20) &&
        (application_thread_actions_view(20).actions & thread_action_fetch) &&
        !(application_thread_actions_view(20).actions & (thread_action_join | thread_action_leave)),
        "thread runtime: private unknown membership permits only parent-authorized fetch, not private reads/join");
    fetch(); check(send() && receipt(200, joined_body) && application_thread_actions_channel_readable(20),
        "thread runtime: private matching member proof enables current read authority");
}
void mutation_tests() noexcept {
    reset(11, false, true);
    model_channel()->actions &= ~channel_action_read_history;
    model_channel()->effective_permissions &= ~kPermissionReadMessageHistory;
    model_channel(11)->actions &= ~channel_action_read_history;
    model_channel(11)->effective_permissions &= ~kPermissionReadMessageHistory;
    check(application_thread_actions_channel_sendable(20) && !application_thread_actions_channel_readable(20),
        "thread runtime: View plus Send permits sending without ReadHistory while reads remain blocked");
    reset(11, false, true);
    auto view = application_thread_actions_view(20);
    check(application_thread_actions_activate(view.ticket, Op::join) == Admission::accepted && send() &&
        !model_channel()->thread_self_joined && receipt(204) && model_channel()->thread_self_joined,
        "thread runtime: explicit Join204 sets membership only after its matching receipt");
    view = application_thread_actions_view(20);
    check(application_thread_actions_activate(view.ticket, Op::leave) == Admission::accepted && send() && receipt(204) &&
        model_channel()->thread_self_joined_known && !model_channel()->thread_self_joined,
        "thread runtime: explicit Leave204 establishes known absence");
    view = application_thread_actions_view(20);
    check(application_thread_actions_activate(view.ticket, Op::join) == Admission::accepted && send() && receipt(404) &&
        application_thread_actions_view(20).phase == Phase::failed && !model_channel()->thread_self_joined,
        "thread runtime: mutation404 is failure rather than a fetch membership receipt");
    view = application_thread_actions_view(20);
    check(application_thread_actions_retry(view.ticket) && send(), "thread runtime: explicit retry reauthorizes failed mutation");
    auto& state = g_app.account_ui->chat_state;
    check(chat_state_store_set_thread_self_membership(&state, 20, 5, true) &&
        chat_state_store_set_thread_self_membership(&state, 20, 5, false) && receipt(204) &&
        !model_channel()->thread_self_joined && application_thread_actions_view(20).operation == Op::fetch_self &&
        application_thread_actions_pending(now),
        "thread runtime: oldJoin204 cannot overwrite newer Gateway membership and queues exact self verification");
    check(send() && receipt(404) && !model_channel()->thread_self_joined,
        "thread runtime: verification preserves latest authoritative absence");
    reset();
    auto waiter = fetch(); check(send(), "thread runtime: stale fetch fixture admitted");
    check(chat_state_store_set_thread_self_membership(&g_app.account_ui->chat_state, 20, 5, true) &&
        receipt(404) && model_channel()->thread_self_joined && application_thread_actions_membership(waiter) == Membership::joined,
        "thread runtime: oldfetch404 cannot erase newer joined Gateway authority");
}
void cancellation_retry_tests() noexcept {
    reset(); fetch(); check(send(), "thread runtime: superseded old request admitted");
    const u64 old_id = accepted_id;
    const u32 before_cancel = cancellations;
    const auto replacement = fetch(21);
    check(cancellations == before_cancel + 1 && cancelled_id == old_id && !application_thread_actions_pending(now) &&
        application_thread_actions_metrics().flight_active,
        "thread runtime: newer choice cancels exact old flight and waits for immutable drain");
    check(receipt(429, {}, RestClientError::none, 5'000) &&
        application_thread_actions_membership(replacement) == Membership::pending &&
        !application_thread_actions_pending(4'999) && application_thread_actions_next_wake() == 5'000,
        "thread runtime:429 actor floor survives supersession and gates replacement before exact drain deadline");
    now = 5'000;
    check(send() && submitted.thread_id == 21 && receipt(404) && model_channel(21)->thread_self_joined_known,
        "thread runtime: replacement alone submits at preserved429 floor");
    reset(); fetch(); check(send(), "thread runtime: rate retry request admitted");
    check(receipt(429, {}, RestClientError::none, 0) && !application_thread_actions_pending(1'049) &&
        application_thread_actions_next_wake() == 1'050,
        "thread runtime: malformed or past retry time retains minimum50ms floor");
    now = 1'050; check(send() && receipt(429, {}, RestClientError::none, 9'000, true) &&
        application_thread_actions_view(20).phase == Phase::failed && !application_thread_actions_pending(20'000),
        "thread runtime: required-action429 is terminal and cannot silently retry");
    reset(); fetch(); admission_error = RestClientSubmitError::busy;
    const u32 before = submissions;
    check(!send() && !application_thread_actions_pending(1'049) && application_thread_actions_next_wake() == 1'050 &&
        submissions == before + 1, "thread runtime: SDKbusy backs off50ms without duplicate admission");
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    check(!application_thread_actions_next_wake(), "thread runtime: occupied other DETAILS owner suppresses wake spin");
    g_app.details_request_owner = MessageHistoryRequestOwner::none;
    admission_error = RestClientSubmitError::none; now = 1'050;
    check(send(), "thread runtime: SDKbusy resumes after explicit due and free lane");
    const auto cancel_ticket = application_thread_actions_view(20).ticket;
    check(application_thread_actions_cancel(cancel_ticket) && cancelled_id == accepted_id &&
        !application_thread_actions_pending(now), "thread runtime: current cancel retains old flight until matching drain");
    check(receipt(0, {}, RestClientError::cancelled) && !application_thread_actions_metrics().flight_active,
        "thread runtime: cancelled transport receipt drains exact immutable flight");
}
void terminal_freshness_tests() noexcept {
    for (u32 kind = 0; kind < 4; ++kind) {
        for (u32 truth = 0; truth < 2; ++truth) {
            reset(11, false, true);
            if (kind == 1) fetch();
            else check(application_thread_actions_activate(application_thread_actions_view(20).ticket, Op::join) == Admission::accepted,
                "thread runtime: terminal freshness mutation admitted");
            check(application_thread_actions_view(20).membership == Membership::not_joined,
                "thread runtime: queued check/mutation retains known non-membership while phase is busy");
            check(send(), "thread runtime: terminal freshness request submitted");
            auto& state = g_app.account_ui->chat_state;
            check(chat_state_store_set_thread_self_membership(&state, 20, 5, true) &&
                (truth || chat_state_store_set_thread_self_membership(&state, 20, 5, false)),
                "thread runtime: newer Gateway membership authority published");
            const u32 revision = model_channel()->revision, before_calls = submissions;
            check(application_thread_actions_view(20).membership == (truth ? Membership::joined : Membership::not_joined),
                "thread runtime: pending phase preserves current known positive or negative membership getter");
            RestClientResponse response{};
            response.request_id = accepted_id;
            if (kind == 0) {
                response.status_code = 429; response.rate_limited = true; response.retry_at_ms = 90'000;
                response.rejection = {40002, true, false};
            } else if (kind == 1) {
                response.status_code = 400; response.rejection.captcha = true;
            } else if (kind == 2) response.error = RestClientError::account_restricted;
            else response.status_code = 401;
            accepted_id = 0;
            const bool handled = application_thread_actions_response(response, {});
            g_app.details_request_owner = MessageHistoryRequestOwner::none; g_app.details_request_id = 0;
            const auto view = application_thread_actions_view(20);
            check(handled && view.phase == Phase::failed && view.error && view.status.bytes &&
                !application_thread_actions_pending(now) && !application_thread_actions_next_wake() &&
                model_channel()->thread_self_joined_known && model_channel()->thread_self_joined == (truth != 0) &&
                model_channel()->revision == revision && submissions == before_calls,
                kind == 3
                    ? "thread runtime: authentication401 precedes revision-refetch and preserves newer positive/negative membership"
                    : "thread runtime: mandatory action CAPTCHA or restricted failure precedes revision-refetch and preserves newer membership");
        }
    }
}
void actor_fence_tests() noexcept {
    for (u32 kind = 0; kind < 9; ++kind) {
        reset(); const auto ticket = fetch(); check(send(), "thread runtime: actor fence flight admitted");
        const u32 old_revision = model_channel()->revision;
        if (kind == 0) ++g_app.plugin_account_scope_serial;
        if (kind == 1) ++g_app.account.status.profile_id;
        if (kind == 2) ++g_app.account_ui->chat_state.self_user_id;
        if (kind == 3) g_app.account.instance = &replacement_marker;
        if (kind == 4) g_app.account.stop_failed = true;
        if (kind == 5) g_app.login.visible = true;
        if (kind == 6) g_app.account.status.running = false;
        if (kind == 7) ++model_channel()->generation;
        if (kind == 8) model_channel()->type = 0;
        check(!application_thread_actions_current(ticket) && !application_thread_actions_pending(now) &&
            receipt(200, joined_body) && !model_channel()->thread_self_joined_known &&
            model_channel()->revision == old_revision && !application_thread_actions_metrics().owner_bytes,
            "thread runtime: account/profile/self/instance/login/stop/generation/type retirement rejects late200 and frees after drain");
    }
    reset(); const auto stale = application_thread_actions_view(20).ticket;
    ++model_channel()->revision;
    check(application_thread_actions_activate(stale, Op::fetch_self) == Admission::stale,
        "thread runtime: stale rendered channel revision cannot activate a new action");
    reset(); fetch(); check(send(), "thread runtime: parent authority fence request admitted");
    model_channel(11)->actions &= ~channel_action_read_history;
    const u32 before_permission_cancel = cancellations;
    application_thread_actions_synchronize();
    check(cancellations == before_permission_cancel + 1 && cancelled_id == accepted_id &&
        receipt(200, joined_body) && !model_channel()->thread_self_joined_known &&
        application_thread_actions_view(20).phase == Phase::failed,
        "thread runtime: lost parent read authority cancels flight and discards late membership proof");
    reset(); fail_allocation = true;
    ApplicationThreadActionTicket failed{};
    check(application_thread_actions_fetch(20, &failed) == Admission::storage && !failed.serial &&
        !application_thread_actions_metrics().owner_bytes,
        "thread runtime: cold allocation failure leaves no owner and no false receipt");
    fail_allocation = false;
    fetch(); check(send(), "thread runtime: release drain fixture admitted");
    application_thread_actions_release();
    check(application_thread_actions_metrics().flight_active && receipt(200, joined_body) &&
        !application_thread_actions_metrics().owner_bytes && !model_channel()->thread_self_joined_known,
        "thread runtime: release keeps owning page alive until exact DETAILS drain without publishing late membership");
}
void unarchive_tests() noexcept {
    reset(11, true, true);
    auto& state = g_app.account_ui->chat_state;
    chat_state_store_set_thread_archived(&state, 20, 5, true);
    chat_state_store_set_thread_locked(&state, 20, 5, true);
    check(!(application_thread_actions_view(20).actions & (thread_action_join | thread_action_leave | thread_action_unarchive)) &&
        application_thread_actions_channel_readable(20) && !application_thread_actions_channel_sendable(20),
        "thread runtime: archived locked ordinary user retains read only; joining/leaving/unarchive are unavailable");
    model_channel()->effective_permissions |= kPermissionManageThreads;
    auto view = application_thread_actions_view(20);
    check((view.actions & thread_action_unarchive) && application_thread_actions_activate(view.ticket, Op::unarchive) == Admission::accepted && send(),
        "thread runtime: moderator explicitly admits locked Unarchive without joining");
    constexpr auto body = utf8_literal(
        R"({"id":"20","guild_id":"10","parent_id":"11","name":"Thread","type":11,"thread_metadata":{"archived":false},"member":{"id":"20","user_id":"5","join_timestamp":"2026-10-05T12:00:00Z","flags":0}})");
    check(receipt(200, body) && !model_channel()->thread_archived && model_channel()->thread_locked &&
        model_channel()->thread_locked_known && model_channel()->thread_self_joined && model_channel()->thread_self_joined_known,
        "thread runtime: strictUnarchive200 updates archived only and never forges locked/member fields from response");
    reset(11, false, true);
    chat_state_store_set_thread_archived(&state, 20, 5, true);
    view = application_thread_actions_view(20);
    check(application_thread_actions_activate(view.ticket, Op::unarchive) == Admission::accepted && send() && receipt(200, body) &&
        !model_channel()->thread_archived && !model_channel()->thread_self_joined && model_channel()->thread_self_joined_known &&
        !model_channel()->thread_locked,
        "thread runtime: unarchive response member subtree cannot silently join current user");
    constexpr Utf8View invalid_bodies[] = {
        utf8_literal(R"({"id":"21","guild_id":"10","parent_id":"11","name":"Thread","type":11,"thread_metadata":{"archived":false}})"),
        utf8_literal(R"({"id":"20","guild_id":"12","parent_id":"11","name":"Thread","type":11,"thread_metadata":{"archived":false}})"),
        utf8_literal(R"({"id":"20","guild_id":"10","parent_id":"12","name":"Thread","type":11,"thread_metadata":{"archived":false}})"),
        utf8_literal(R"({"id":"20","guild_id":"10","parent_id":"11","name":"Thread","type":12,"thread_metadata":{"archived":false}})"),
        utf8_literal(R"({"id":"20","guild_id":"10","parent_id":"11","name":"Thread","type":11,"thread_metadata":{"archived":true}})"),
        utf8_literal(R"({"id":"20","guild_id":"10","parent_id":"11","name":"Thread","type":11,"thread_metadata":{}})"),
    };
    for (Utf8View invalid : invalid_bodies) {
        reset(11, false, true);
        chat_state_store_set_thread_archived(&state, 20, 5, true);
        view = application_thread_actions_view(20);
        check(application_thread_actions_activate(view.ticket, Op::unarchive) == Admission::accepted && send() && receipt(200, invalid) &&
            model_channel()->thread_archived && !model_channel()->thread_self_joined &&
            application_thread_actions_view(20).phase == Phase::failed,
            "thread runtime: foreign/incomplete/archived Unarchive200 cannot mutate current authority");
    }
}
void metadata_tests() noexcept {
    reset(11, false, true);
    auto& state = g_app.account_ui->chat_state;
    chat_state_store_set_thread_archived(&state, 20, 5, false, false);
    chat_state_store_set_thread_locked(&state, 20, 5, false, false);
    const u32 before_metadata = metadata_submissions, before_members = submissions;
    const auto ticket = fetch();
    check(send() && last_metadata && metadata_submissions == before_metadata + 1 && submissions == before_members,
        "thread runtime: unknown archive/lock starts exact channel metadata GET before self fetch");
    constexpr auto metadata = utf8_literal(
        R"({"id":"20","guild_id":"10","parent_id":"11","name":"Thread","type":11,"thread_metadata":{"archived":true,"locked":true},"member":{"id":"20","user_id":"5","join_timestamp":"2026-10-05T12:00:00Z","flags":0}})");
    check(receipt(200, metadata) && model_channel()->thread_archived_known && model_channel()->thread_archived &&
        model_channel()->thread_locked_known && model_channel()->thread_locked &&
        model_channel()->thread_self_joined_known && !model_channel()->thread_self_joined &&
        application_thread_actions_view(20).ticket.serial == ticket.serial &&
        application_thread_actions_view(20).phase == Phase::queued && application_thread_actions_pending(now),
        "thread runtime: metadata200 learns provided archive/lock but preserves membership and same waiter serial");
    check(send() && !last_metadata && submitted.operation == Op::fetch_self && metadata_submissions == before_metadata + 1 &&
        receipt(200, joined_body) && model_channel()->thread_self_joined && model_channel()->thread_archived && model_channel()->thread_locked,
        "thread runtime: second stage alone establishes member proof without unarchiving or unlocking");
    reset();
    chat_state_store_set_thread_locked(&state, 20, 5, false, false);
    fetch(); check(send() && last_metadata && receipt(200, utf8_literal(
        R"({"id":"20","guild_id":"10","parent_id":"11","name":"Thread","type":11,"thread_metadata":{"archived":false}})")) &&
        !model_channel()->thread_locked_known && model_channel()->thread_archived_known &&
        !model_channel()->thread_self_joined_known,
        "thread runtime: omitted metadata lock/member stays unknown, never default false/joined");
    reset();
    chat_state_store_set_thread_locked(&state, 20, 5, false, false);
    fetch(); check(send() && last_metadata && receipt(404) && !model_channel()->thread_self_joined_known &&
        application_thread_actions_view(20).phase == Phase::failed,
        "thread runtime: metadata404 is failure, not a self-member absence receipt");
    reset();
    chat_state_store_set_thread_locked(&state, 20, 5, false, false);
    fetch(); check(send() && last_metadata, "thread runtime: metadata cancellation stage admitted");
    const auto cancelling = application_thread_actions_view(20).ticket;
    const u32 before_metadata_cancel = cancellations, before_metadata_calls = metadata_submissions;
    check(application_thread_actions_cancel(cancelling) && cancellations == before_metadata_cancel + 1 &&
        metadata_submissions == before_metadata_calls && cancelled_id == accepted_id &&
        receipt(0, {}, RestClientError::cancelled),
        "thread runtime: metadata stage uses canonical exact-ID cancellation on the same DETAILS worker");
}
void run() noexcept {
    g_app.account_ui = static_cast<AccountUiRuntime*>(::VirtualAlloc(nullptr, sizeof(AccountUiRuntime),
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.account_ui != nullptr, "thread runtime: independent native account-model fixture allocated");
    if (!g_app.account_ui) return;
    membership_tests(); mutation_tests(); cancellation_retry_tests(); terminal_freshness_tests();
    actor_fence_tests(); unarchive_tests(); metadata_tests();
    g_app.details_request_owner = MessageHistoryRequestOwner::none; g_app.details_request_id = 0;
    application_thread_actions_release();
    check(allocations == frees, "thread runtime: every lazy owner page freed at final release");
    ::VirtualFree(g_app.account_ui, 0, MEM_RELEASE); g_app.account_ui = nullptr;
}
} // namespace maicord::thread_actions_runtime_test

extern "C" __declspec(noreturn) void WINAPI maicord_application_thread_actions_runtime_entry() noexcept {
    maicord::thread_actions_runtime_test::run();
    ExitProcess(maicord::thread_actions_runtime_test::failures ? 1 : 0);
}
