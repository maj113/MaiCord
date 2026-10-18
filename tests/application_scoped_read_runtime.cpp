// One executable for both read runtimes: the read-state suite is compiled in
// here and runs when the command line carries --read-state.
#define maicord_read_state_runtime_entry maicord_scoped_read_runtime_reused_entry
#include "application_read_state_runtime.cpp"
#undef maicord_read_state_runtime_entry
#include "application_scoped_read.hpp"
#include "maicord/protocol/bulk_message_ack.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord::scoped_read_runtime_test {
using namespace app_detail;
using namespace read_state_runtime_test;
struct BulkSeen {
    BulkMessageAckEntry entries[100];
    u32 calls, count, cancels;
    u64 request;
    RestClientSubmitError admission;
} bulk{};
RestClientSubmitResult WINAPI send_bulk(void* instance, const BulkMessageAckEntry* entries, u32 count,
                                        u64 request) noexcept {
    if (instance != &fixture.account_sentinel || !request)
        return {RestClientSubmitError::invalid_input};
    if (!entries && !count) {
        ++bulk.cancels;
        return {};
    }
    if (!entries || !count || count > 100)
        return {RestClientSubmitError::invalid_input};
    ++bulk.calls;
    bulk.count = count;
    bulk.request = request;
    copy_bytes(bulk.entries, entries, count * sizeof(*entries));
    return {bulk.admission};
}
void fresh() noexcept {
    g_app.account.status.running = false;
    application_scoped_read_release();
    fresh_owner();
    g_app.account.api.bulk_message_ack = send_bulk;
    bulk = {};
    for (u32 i = 0; i < 4; ++i) {
        channel(i).guild_id = 7;
        channel(i).parent_id = 0;
        channel(i).type = 0;
        channel(i).last_message_id = 1000 + i;
        channel(i).last_read_message_id = 900;
        channel(i).mention_count = 1;
    }
    channel(1).type = 11;
    channel(1).parent_id = 70;
    channel(1).thread_active_known = channel(1).thread_listed_active = true;
    channel(1).thread_archived_known = channel(1).thread_self_joined_known = channel(1).thread_self_joined =
        true;
    channel(1).thread_archived = false;
    channel(2).type = 15;
    channel(2).last_message_id = 0;
    channel(3).type = 4;
    channel(3).last_message_id = 0;
    g_app.account_ui->chat_state.channel_mutations++;
}
void fresh_category() noexcept {
    fresh();
    channel(0).parent_id = 73;
    channel(2).type = 0;
    channel(2).parent_id = 73;
    channel(2).last_message_id = 0;
}
bool scope_reply(u16 status, u64 now, u64 retry_at = 0,
                 RestClientError error = RestClientError::none) noexcept {
    RestClientResponse r{};
    r.request_id = bulk.request;
    r.status_code = status;
    r.retry_at_ms = retry_at;
    r.error = error;
    return application_scoped_read_response(r, {}, now);
}
void run() noexcept {
    check(initialize(), "scoped runtime: actual Core and ordinary broker initialize");
    if (!g_app.account_ui)
        return;
    ApplicationScopedReadTicket ticket{};
    fresh();
    channel(1).thread_archived_known = false; // Active/listed/joined is positive authority.

    check(application_scoped_read_capture(ReadScopeKind::guild, 7, &ticket) &&
              application_scoped_read_current(ticket) && application_scoped_read_begin(ticket),
          "scoped runtime: captured guild admits one fully reserved optimistic plan");
    auto view = application_scoped_read_view(ReadScopeKind::guild, 7);
    check(view.total == 3 && view.pending == 3 && channel(0).last_read_message_id == 1000 &&
              channel(2).last_read_message_id > 0 && channel(2).mention_count == 0,
          "scoped runtime: text joined thread and forum action clock clear locally with immutable "
          "boundaries");
    check(application_scoped_read_submit(10001, 1) && bulk.count == 3 && bulk.entries[0].channel_id == 70 &&
              bulk.entries[2].channel_id == 72 &&
              bulk.entries[2].message_id == channel(2).last_read_message_id,
          "scoped runtime: actual callback receives zero-state entries and genuine forum captured cursor");
    const u64 captured = channel().last_read_message_id;
    channel().last_message_id = 2000;
    channel().mention_count = 4;
    g_app.account_ui->chat_state.channel_mutations++;
    check(scope_reply(204, 100) && channel().last_read_message_id == captured &&
              channel().mention_count == 4 &&
              application_scoped_read_view(ReadScopeKind::guild, 7).completed == 3,
          "scoped runtime: later messages do not widen admitted boundary or clear later mentions");
    const auto metrics = application_scoped_read_metrics();
    const auto broker = application_read_state_metrics();
    for (u32 warm = 0; warm < 8; ++warm) {
        application_scoped_read_synchronize();
        check(!application_scoped_read_next_wake() &&
                  application_scoped_read_view(ReadScopeKind::guild, 7).completed == 3,
              "scoped runtime: completed idle status remains stable");
    }
    const auto warmed = application_scoped_read_metrics();
    const auto broker_warm = application_read_state_metrics();
    check(metrics.publication_revision == warmed.publication_revision &&
              metrics.plan_builds == warmed.plan_builds && metrics.target_checks == warmed.target_checks &&
              metrics.encodings == warmed.encodings && metrics.commits == warmed.commits &&
              broker.commits == broker_warm.commits && broker.action_serial == broker_warm.action_serial,
          "scoped runtime: 8 warm polls do no planning target scanning encoding backing or authority "
          "mutation");
    check(metrics.owner_committed_bytes >= metrics.owner_bytes && metrics.candidate_reserved_bytes &&
              broker.owner_committed_bytes >= broker.owner_bytes && broker.echoes_committed_bytes &&
              broker.actions_committed_bytes,
          "scoped runtime: metrics include base pages and duplicate immutable and echo banks");

    fresh();
    // These compact fixture IDs have no Snowflake millisecond component.
    // Seed real authoritative predecessors; native unread never subtracts one
    // from a Snowflake when the predecessor is absent from retained history.
    channel(1).last_message_id = 2001;
    check(seed(999, 70) && seed(2000, 71),
          "scoped runtime: retained authoritative predecessors support exact read/unread race boundaries");
    check(application_scoped_read_capture(ReadScopeKind::guild, 7, &ticket) &&
              application_scoped_read_begin(ticket) && application_scoped_read_submit(10002, 1),
          "scoped runtime: older bulk echo source admitted");
    check(application_read_state_mark_channel_unread(70) && channel().last_read_message_id == 999 &&
              ack_event(1000, false) && channel().last_read_message_id == 999 &&
              channel().read_auto_ack_suppressed && scope_reply(204, 50),
          "scoped runtime: older bulk Gateway echo and HTTP cannot erase newer manual unread");
    check(admit() && seen.message_id == 999 && reply(204),
          "scoped runtime: newer unread remains independently queued after bulk completion");
    // The bulk HTTP response has already completed; retained echo evidence is
    // still needed when a different newer manual action follows it.
    const bool selected_suppression = channel().read_auto_ack_suppressed;
    Body off_selected_body{};
    check(application_read_state_mark_channel_unread(71) && channel(1).last_read_message_id == 2000 &&
              !channel(1).read_auto_ack_suppressed && ack_event(2001, false, 71) &&
              channel(1).last_read_message_id == 2000 && !channel(1).read_auto_ack_suppressed &&
              channel().read_auto_ack_suppressed == selected_suppression &&
              application_read_state_next_wake() && admit() && seen.channel_id == 71 &&
              seen.message_id == 2000 && manual_body(off_selected_body) && reply(204),
          "scoped runtime: off-selected late owned bulk echo preserves exact unread and queued manual ACK "
          "after HTTP without fabricating selected suppression");

    fresh();
    check(application_scoped_read_capture(ReadScopeKind::guild, 7, &ticket) &&
              application_scoped_read_begin(ticket),
          "scoped runtime: unsent authority fixture admitted");
    channel(1).thread_self_joined = false;
    g_app.account_ui->chat_state.channel_mutations++;
    application_scoped_read_synchronize();
    check(application_scoped_read_view(ReadScopeKind::guild, 7).total == 2 &&
              application_scoped_read_submit(10003, 1) && bulk.count == 2,
          "scoped runtime: losing joined membership removes only that unsent target");
    const u32 old_generation = g_app.account_ui->read_state_store.generation;
    ++g_app.account_ui->read_state_store.generation;
    application_read_state_ready();
    check(bulk.cancels == 1 && !application_scoped_read_next_wake() && scope_reply(429, 100, 5000) &&
              !application_scoped_read_view(ReadScopeKind::guild, 7).running,
          "scoped runtime: new READY retires exact flight but drains late rate limit without publication");
    check(application_scoped_read_capture(ReadScopeKind::guild, 7, &ticket) &&
              application_scoped_read_begin(ticket) && application_scoped_read_next_wake() >= 5000 &&
              !application_scoped_read_submit(10004, 4999),
          "scoped runtime: same actor late 429 constrains new READY work before stale receipt checks");
    g_app.account_ui->read_state_store.generation = old_generation;

    fresh_category();
    check(application_scoped_read_capture(ReadScopeKind::category, 73, &ticket) &&
              application_scoped_read_begin(ticket),
          "scoped runtime: forced category plan admits real individual queue plus null local target");
    view = application_scoped_read_view(ReadScopeKind::category, 73);
    check(view.total == 3 && view.completed == 1 && view.pending == 2 &&
              !application_scoped_read_next_wake() && channel(2).last_read_message_id == 0 &&
              !channel(2).mention_count && admit() && seen.channel_id == 70,
          "scoped runtime: null category cursor is local-only and first child uses ordinary ACK");
    check(reply(200, utf8_literal(R"({"token":"scoped-category"})")) && admit() && seen.channel_id == 71,
          "scoped runtime: category rotates ordinary token and retains joined thread next");
    Body body{};
    check(submitted_body(&body) && equals(body.token, body.token_bytes, utf8_literal("scoped-category")) &&
              reply(403),
          "scoped runtime: category opaque token rotation is independent of bulk callback");
    view = application_scoped_read_view(ReadScopeKind::category, 73);
    check(view.failed == 1 && view.completed == 2 && view.can_retry &&
              application_scoped_read_retry(view.ticket) && admit() && seen.channel_id == 71 &&
              reply(204) && application_scoped_read_view(ReadScopeKind::category, 73).completed == 3,
          "scoped runtime: explicit Retry sends only current failed thread without replaying successful or "
          "null targets");

    // Scope changes share the same per-channel serial ledger.
    fresh_category();
    check(application_scoped_read_capture(ReadScopeKind::guild, 7, &ticket) &&
              application_scoped_read_begin(ticket),
          "scoped runtime: guild-to-category overlap first queues real bulk targets");
    channel(0).last_message_id = 1100;
    channel(1).last_message_id = 1101;
    ApplicationScopedReadTicket category_ticket{};
    check(application_scoped_read_capture(ReadScopeKind::category, 73, &category_ticket) &&
              application_scoped_read_begin(category_ticket) && !application_scoped_read_next_wake() &&
              !application_scoped_read_submit(10100, 1) && bulk.calls == 0 && admit() &&
              seen.message_id == 1100 && reply(204) && admit() && seen.message_id == 1101 && reply(204),
          "scoped runtime: newer category cancels old bulk serials and sends only new individual cursors");
    fresh_category();
    check(application_scoped_read_capture(ReadScopeKind::category, 73, &category_ticket) &&
              application_scoped_read_begin(category_ticket),
          "scoped runtime: category-to-guild overlap first queues real individual targets");
    channel(0).last_message_id = 1200;
    channel(1).last_message_id = 1201;
    check(application_scoped_read_capture(ReadScopeKind::guild, 7, &ticket) &&
              application_scoped_read_begin(ticket) && !application_read_state_next_wake() &&
              application_scoped_read_submit(10101, 1) && bulk.count == 2 &&
              bulk.entries[0].message_id == 1200 && bulk.entries[1].message_id == 1201 &&
              scope_reply(204, 10),
          "scoped runtime: newer guild cancels individual old serials and sends only captured new bulk "
          "cursors");
    fresh_category();
    check(application_scoped_read_capture(ReadScopeKind::category, 73, &category_ticket) &&
              application_scoped_read_begin(category_ticket),
          "scoped runtime: category reparent invalidation captures child and joined thread");
    channel(0).parent_id = 0;
    g_app.account_ui->chat_state.channel_mutations++;
    application_scoped_read_synchronize();
    check(!application_read_state_next_wake() && !admit() &&
              application_scoped_read_view(ReadScopeKind::category, 73).pending == 0,
          "scoped runtime: parent movement cancels exact individual child and thread intents before wire");
    fresh_category();
    check(application_scoped_read_capture(ReadScopeKind::category, 73, &category_ticket) &&
              application_scoped_read_begin(category_ticket),
          "scoped runtime: category permission invalidation captures individual intents");
    channel(3).effective_permissions = 0;
    g_app.account_ui->chat_state.permission_mutations++;
    application_scoped_read_synchronize();
    check(!application_read_state_next_wake() && !admit() &&
              !application_scoped_read_view(ReadScopeKind::category, 73).available,
          "scoped runtime: source permission loss retires individually queued category work");
    channel(3).effective_permissions = ~u64{0};
    fresh();
    for (u32 i = 0; i < 3; ++i) {
        channel(i).type = 0;
        channel(i).parent_id = 73;
        channel(i).last_message_id = 0;
    }
    check(application_scoped_read_capture(ReadScopeKind::category, 73, &category_ticket) &&
              application_scoped_read_begin(category_ticket) &&
              application_scoped_read_view(ReadScopeKind::category, 73).completed == 3 &&
              !application_read_state_next_wake() && !application_scoped_read_next_wake() && !bulk.calls,
          "scoped runtime: all-null category clears locally with no outgoing or zero-prefix allocation "
          "failure");
    fresh();
    channel(2).type = 0;
    for (u32 i = 0; i < 3; ++i) {
        channel(i).last_message_id = 0;
        channel(i).mention_count = 0;
    }
    check(
        application_scoped_read_capture(ReadScopeKind::guild, 7, &ticket) &&
            application_scoped_read_begin(ticket) &&
            application_scoped_read_view(ReadScopeKind::guild, 7).total == 0 &&
            !application_scoped_read_next_wake() && !bulk.calls,
        "scoped runtime: empty eligible guild is a valid no-wire action with no synthetic ordinary cursor");

    fresh();
    check(application_scoped_read_capture(ReadScopeKind::guild, 7, &ticket) &&
              application_scoped_read_begin(ticket) && application_scoped_read_submit(10005, 1) &&
              scope_reply(500, 100) && application_scoped_read_next_wake() == 2100 &&
              !application_scoped_read_submit(10006, 2099) && application_scoped_read_submit(10006, 2100) &&
              scope_reply(503, 2200) && application_scoped_read_next_wake() == 6200 &&
              application_scoped_read_submit(10007, 6200) && scope_reply(500, 6300),
          "scoped runtime: actual transient flights use bounded three attempts with 2s and 4s delays");
    view = application_scoped_read_view(ReadScopeKind::guild, 7);
    check(view.failed == 3 && !view.running && view.can_retry && !application_scoped_read_next_wake() &&
              application_scoped_read_retry(view.ticket) && application_scoped_read_submit(10008, 6400) &&
              scope_reply(204, 6500),
          "scoped runtime: terminal failed targets stay optimistic and require explicit failed-only retry");
    fresh();
    check(application_scoped_read_capture(ReadScopeKind::guild, 7, &ticket) &&
              application_scoped_read_begin(ticket),
          "scoped runtime: echo-capacity fixture admits bulk plan before shared ledger fills");
    bool filled = true;
    for (u32 i = 0; i < 4096 && filled; ++i) {
        channel().last_read_message_id = channel().last_message_id;
        channel().mention_count = 0;
        filled = application_read_state_mark_channel_unread(70) && admit() && reply(204);
    }
    check(filled && application_scoped_read_view(ReadScopeKind::guild, 7).pending == 2,
          "scoped runtime: 4096 actual admitted manual ACKs retain omitted Gateway echo proofs without "
          "losing other targets");
    const u32 before_capacity_calls = bulk.calls;
    check(!application_scoped_read_submit(10200, 1) && bulk.calls == before_capacity_calls &&
              application_scoped_read_view(ReadScopeKind::guild, 7).failed == 2 &&
              !application_scoped_read_next_wake(),
          "scoped runtime: full shared echo ledger fails exact pending batch visibly without POST or "
          "immediate wake");
    const auto capacity_metrics = application_scoped_read_metrics();
    for (u32 warm = 0; warm < 8; ++warm) {
        application_scoped_read_synchronize();
        check(!application_scoped_read_next_wake(),
              "scoped runtime: capacity failure has no past-due polling wake");
    }
    const auto capacity_warm = application_scoped_read_metrics();
    check(capacity_metrics.publication_revision == capacity_warm.publication_revision &&
              capacity_metrics.encodings == capacity_warm.encodings &&
              capacity_metrics.commits == capacity_warm.commits &&
              capacity_metrics.target_checks == capacity_warm.target_checks,
          "scoped runtime: capacity failure remains stable across 8 polls without frames encoding scans "
          "or commits");
    application_scoped_read_release();
    release();
}
} // namespace maicord::scoped_read_runtime_test
extern "C" __declspec(noreturn) void WINAPI maicord_scoped_read_runtime_entry() noexcept {
    constexpr wchar_t mode[] = L"--read-state";
    for (const wchar_t* line = GetCommandLineW(); *line; ++line) {
        maicord::u32 matched = 0;
        while (mode[matched] && line[matched] == mode[matched])
            ++matched;
        if (!mode[matched])
            maicord_scoped_read_runtime_reused_entry();
    }
    maicord::scoped_read_runtime_test::run();
    const bool failed = maicord::read_state_runtime_test::seen.failures != 0;
    constexpr char pass[] = "PASS scoped read runtime\n", fail[] = "FAIL scoped read runtime\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
