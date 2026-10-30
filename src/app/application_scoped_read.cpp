#include "application_scoped_read.hpp"
#include "application_state.hpp"
#include "application_read_state.hpp"
#include "application_account_presentation.hpp"
#include "maicord/app/bulk_read_ack_controller.hpp"
#include "maicord/protocol/bulk_message_ack.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "account_ui_runtime.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
struct ScopedTarget {
    ReadScopeTarget target;
    u64 action_serial, scope_serial;
    bool individual, completed, failed;
};
struct ScopedJob {
    ApplicationScopedReadTicket ticket;
    u32 total, pending, completed, failed;
};
struct ScopedFlightEntry {
    BulkReadAckIntent intent;
    ReadScopeSource source;
    BulkMessageAckEntry wire;
    ApplicationReadEchoProof proof;
};
} // namespace
struct ApplicationScopedRead {
    ApplicationScopedReadTicket actor;
    BulkReadAckController queue;
    StableStorage queued, admitted, targets, plan, candidates, jobs, flight;
    BulkReadAckTicket flight_ticket;
    ApplicationScopedReadTicket flight_actor;
    u64 admission_due;
    u32 target_count, job_count, flight_count, revision;
    u32 channels_epoch, permissions_epoch, guilds_epoch;
    u32 plan_builds, target_checks, encodings;
    bool flight_active, retired, cancel_requested, release_pending;
};
namespace {
u64 scoped_ticket_serial;
bool actor_live() noexcept {
    return g_app.account_ui && g_app.account.instance && g_app.account.api.message_ack &&
           g_app.account.api.bulk_message_ack && !g_app.account.stop_failed && !g_app.login.visible &&
           g_app.plugin_account_scope_serial && g_app.account.status.profile_id &&
           g_app.account_ui->chat_state.self_user_id;
}
bool actor_current(const ApplicationScopedReadTicket& t, bool read_epoch = true) noexcept {
    return actor_live() && t.account_ui == g_app.account_ui &&
           t.account_instance == g_app.account.instance &&
           t.account_scope == g_app.plugin_account_scope_serial &&
           t.profile_id == g_app.account.status.profile_id &&
           t.self_user_id == g_app.account_ui->chat_state.self_user_id &&
           (!read_epoch || t.read_generation == g_app.account_ui->read_state_store.generation);
}
bool reserve_queued(void* context, u32 count) noexcept {
    return stable_storage_reserve(&static_cast<ApplicationScopedRead*>(context)->queued, count);
}
bool reserve_admitted(void* context, u32 count) noexcept {
    return stable_storage_reserve(&static_cast<ApplicationScopedRead*>(context)->admitted, count);
}
void destroy(ApplicationScopedRead* o) noexcept {
    if (!o)
        return;
    stable_storage_release(&o->queued);
    stable_storage_release(&o->admitted);
    stable_storage_release(&o->targets);
    stable_storage_release(&o->plan);
    stable_storage_release(&o->candidates);
    stable_storage_release(&o->jobs);
    stable_storage_release(&o->flight);
    VirtualFree(o, 0, MEM_RELEASE);
    if (g_app.scoped_read == o)
        g_app.scoped_read = nullptr;
}
ApplicationScopedRead* ensure_owner(const ApplicationScopedReadTicket& actor) noexcept {
    if (g_app.scoped_read)
        return g_app.scoped_read;
    auto* o = static_cast<ApplicationScopedRead*>(
        VirtualAlloc(nullptr, sizeof(ApplicationScopedRead), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!o)
        return nullptr;
    if (!stable_storage_initialize(&o->queued, 4096, 0, sizeof(BulkReadAckIntent),
                                   alignof(BulkReadAckIntent)) ||
        !stable_storage_initialize(&o->admitted, 100, 0, sizeof(BulkReadAckIntent),
                                   alignof(BulkReadAckIntent)) ||
        !stable_storage_initialize(&o->targets, 4096, 0, sizeof(ScopedTarget), alignof(ScopedTarget)) ||
        !stable_storage_initialize(&o->plan, 4096, 0, sizeof(ReadScopeTarget), alignof(ReadScopeTarget)) ||
        !stable_storage_initialize(&o->candidates, 4096, 0, sizeof(BulkReadAckIntent),
                                   alignof(BulkReadAckIntent)) ||
        !stable_storage_initialize(&o->jobs, 4096, 0, sizeof(ScopedJob), alignof(ScopedJob)) ||
        !stable_storage_initialize(&o->flight, 100, 0, sizeof(ScopedFlightEntry),
                                   alignof(ScopedFlightEntry)) ||
        !bulk_read_ack_initialize(&o->queue, {stable_storage_data<BulkReadAckIntent>(o->queued), 4096},
                                  {stable_storage_data<BulkReadAckIntent>(o->admitted), 100},
                                  reserve_queued, reserve_admitted, o)) {
        destroy(o);
        return nullptr;
    }
    o->actor = actor;
    g_app.scoped_read = o;
    return o;
}
ScopedTarget* target(ApplicationScopedRead& o, u64 id) noexcept {
    auto* rows = stable_storage_data<ScopedTarget>(o.targets);
    for (u32 i = 0; i < o.target_count; ++i)
        if (rows[i].target.channel_id == id)
            return rows + i;
    return nullptr;
}
ScopedJob* job(ApplicationScopedRead& o, ReadScopeKind kind, u64 id) noexcept {
    auto* rows = stable_storage_data<ScopedJob>(o.jobs);
    for (u32 i = 0; i < o.job_count; ++i)
        if (rows[i].ticket.source.kind == kind && rows[i].ticket.source.id == id)
            return rows + i;
    return nullptr;
}
ScopedJob* serial_job(ApplicationScopedRead& o, u64 serial) noexcept {
    auto* rows = stable_storage_data<ScopedJob>(o.jobs);
    for (u32 i = 0; i < o.job_count; ++i)
        if (rows[i].ticket.serial == serial)
            return rows + i;
    return nullptr;
}
void publish(ApplicationScopedRead& o) noexcept {
    ++o.revision;
    if (!o.revision)
        ++o.revision;
    application_request_frame();
}
void recount(ApplicationScopedRead& o) noexcept {
    auto* jobs = stable_storage_data<ScopedJob>(o.jobs);
    for (u32 i = 0; i < o.job_count; ++i)
        jobs[i].total = jobs[i].pending = jobs[i].completed = jobs[i].failed = 0;
    const auto* rows = stable_storage_data<ScopedTarget>(o.targets);
    for (u32 i = 0; i < o.target_count; ++i)
        if (auto* j = serial_job(o, rows[i].scope_serial)) {
            ++j->total;
            if (rows[i].completed)
                ++j->completed;
            else if (rows[i].failed)
                ++j->failed;
            else
                ++j->pending;
        }
}
void cancel_flight(ApplicationScopedRead& o) noexcept {
    if (!o.flight_active)
        return;
    o.retired = true;
    if (!o.cancel_requested && actor_current(o.flight_actor, false)) {
        g_app.account.api.bulk_message_ack(g_app.account.instance, nullptr, 0, o.flight_ticket.request_id);
        o.cancel_requested = true;
    }
}
bool target_current(ApplicationScopedRead& o, const ScopedTarget& row) noexcept {
    const auto* j = serial_job(o, row.scope_serial);
    ++o.target_checks;
    return j && actor_current(j->ticket) &&
           read_scope_source_current(g_app.account_ui->chat_state, j->ticket.source) &&
           read_scope_target_current(g_app.account_ui->chat_state, j->ticket.source, row.target) &&
           application_read_state_action_current(row.target.channel_id, row.action_serial);
}
void remove_target(ApplicationScopedRead& o, u32 at) noexcept {
    auto* rows = stable_storage_data<ScopedTarget>(o.targets);
    bulk_read_ack_cancel(&o.queue, rows[at].target.channel_id, rows[at].action_serial);
    application_read_state_scoped_cancel(rows[at].target.channel_id, rows[at].action_serial);
    rows[at] = rows[--o.target_count];
}
bool install(const ApplicationScopedReadTicket& ticket, ConstSlice<ReadScopeTarget> plan,
             bool retry) noexcept {
    auto* o = ensure_owner(ticket);
    if (!o || o->release_pending || (o->flight_active && o->retired))
        return false;
    if (!actor_current(o->actor)) {
        application_scoped_read_reset();
        if (o->flight_active)
            return false;
        o->actor = ticket;
    }
    auto* existing = job(*o, ticket.source.kind, ticket.source.id);
    if (existing && retry && existing->ticket.serial != ticket.serial)
        return false;
    u32 new_targets = o->target_count;
    for (u32 i = 0; i < plan.count; ++i)
        if (!target(*o, plan[i].channel_id))
            ++new_targets;
    const u32 new_jobs = o->job_count + (existing ? 0 : 1);
    const bool individual = ticket.source.kind == ReadScopeKind::category;
    const bool bulk_work = !individual && (plan.count || o->queue.count);
    const u32 scratch_count = bulk_work ? maximum(plan.count, 100u) : plan.count;
    if (new_targets > 4096 || new_jobs > 4096 ||
        (new_targets && !stable_storage_reserve(&o->targets, new_targets)) ||
        !stable_storage_reserve(&o->jobs, new_jobs) ||
        (scratch_count && !stable_storage_reserve(&o->candidates, scratch_count)) ||
        (bulk_work && !stable_storage_reserve(&o->flight, 100)))
        return false;
    u64 first{};
    if (!application_read_state_scoped_prepare(plan, individual, &first))
        return false;
    auto* candidate = stable_storage_data<BulkReadAckIntent>(o->candidates);
    u32 outbound{};
    for (u32 i = 0; i < plan.count; ++i)
        if (plan[i].outbound && !individual)
            candidate[outbound++] = {
                plan[i].channel_id, plan[i].message_id, first + i, ticket.serial, 0, 0, false};
    if (!bulk_read_ack_prepare(&o->queue, {candidate, outbound}))
        return false;
    // All allocation and whole-plan validation precedes the shared state commit.
    // Main-thread enqueueing has no reentrant callbacks until the final commit.
    for (u32 i = 0; i < plan.count; ++i)
        if (!read_scope_target_current(g_app.account_ui->chat_state, ticket.source, plan[i]))
            return false;
    if (!bulk_read_ack_queue(&o->queue, {candidate, outbound}))
        return false;
    if (!existing)
        existing = stable_storage_data<ScopedJob>(o->jobs) + o->job_count++;
    if (!retry) {
        // Replace only this scope's unsent targets; other scopes stay queued.
        for (u32 i = 0; i < o->target_count;) {
            auto* j = serial_job(*o, stable_storage_data<ScopedTarget>(o->targets)[i].scope_serial);
            if (j == existing)
                remove_target(*o, i);
            else
                ++i;
        }
    }
    existing->ticket = ticket;
    auto* rows = stable_storage_data<ScopedTarget>(o->targets);
    for (u32 i = 0; i < plan.count; ++i) {
        auto* row = target(*o, plan[i].channel_id);
        if (!row)
            row = rows + o->target_count++;
        else {
            // Different scope kinds share one channel state. Cancel only
            // the prior serial; a newly staged bulk intent is already newer.
            bulk_read_ack_cancel(&o->queue, row->target.channel_id, row->action_serial);
            application_read_state_scoped_cancel(row->target.channel_id, row->action_serial);
        }
        *row = {plan[i], first + i, ticket.serial, individual, !plan[i].outbound, false};
    }
    if (!application_read_state_scoped_commit(plan, individual, first))
        return false;
    const auto& state = g_app.account_ui->chat_state;
    o->channels_epoch = state.channel_mutations;
    o->permissions_epoch = state.permission_mutations;
    o->guilds_epoch = state.guild_mutations;
    recount(*o);
    publish(*o);
    synchronize_navigation_view();
    return true;
}
} // namespace
bool application_scoped_read_capture(ReadScopeKind kind, u64 id,
                                     ApplicationScopedReadTicket* out) noexcept {
    if (!out || !actor_live() || scoped_ticket_serial == ~u64{0})
        return false;
    ApplicationScopedReadTicket t{};
    if (!read_scope_source(g_app.account_ui->chat_state, kind, id, &t.source))
        return false;
    t.account_ui = g_app.account_ui;
    t.account_instance = g_app.account.instance;
    t.account_scope = g_app.plugin_account_scope_serial;
    t.profile_id = g_app.account.status.profile_id;
    t.self_user_id = g_app.account_ui->chat_state.self_user_id;
    t.read_generation = g_app.account_ui->read_state_store.generation;
    t.serial = ++scoped_ticket_serial;
    *out = t;
    return true;
}
bool application_scoped_read_current(const ApplicationScopedReadTicket& t) noexcept {
    return t.serial && actor_current(t) &&
           read_scope_source_current(g_app.account_ui->chat_state, t.source);
}
bool application_scoped_read_begin(const ApplicationScopedReadTicket& t) noexcept {
    application_read_state_ready();
    if (!application_scoped_read_current(t))
        return false;
    auto* o = ensure_owner(t);
    if (!o)
        return false;
    const u64 action_clock = clock_unix_milliseconds();
    const auto size =
        read_scope_plan_size(g_app.account_ui->chat_state, t.source.kind, t.source.id, action_clock);
    if (!size.valid || (size.count && !stable_storage_reserve(&o->plan, size.count)))
        return false;
    ++o->plan_builds;
    const auto result =
        read_scope_plan(g_app.account_ui->chat_state, t.source.kind, t.source.id, action_clock,
                        {stable_storage_data<ReadScopeTarget>(o->plan), size.count});
    if (!result.valid || result.count != size.count || result.outbound_count != size.outbound_count)
        return false;
    return install(t, {stable_storage_data<ReadScopeTarget>(o->plan), result.count}, false);
}
bool application_scoped_read_retry(const ApplicationScopedReadTicket& t) noexcept {
    application_scoped_read_synchronize();
    auto* o = g_app.scoped_read;
    auto* j = o ? job(*o, t.source.kind, t.source.id) : nullptr;
    if (!j || j->ticket.serial != t.serial || !j->failed || !application_scoped_read_current(t) ||
        !stable_storage_reserve(&o->plan, j->failed))
        return false;
    auto* plan = stable_storage_data<ReadScopeTarget>(o->plan);
    u32 count{};
    auto* rows = stable_storage_data<ScopedTarget>(o->targets);
    for (u32 i = 0; i < o->target_count; ++i)
        if (rows[i].scope_serial == t.serial && rows[i].failed) {
            if (!target_current(*o, rows[i]))
                return false;
            plan[count++] = rows[i].target;
        }
    return count && install(t, {plan, count}, true);
}
ApplicationScopedReadView application_scoped_read_view(ReadScopeKind kind, u64 id) noexcept {
    ApplicationScopedReadView view{};
    if (!actor_live())
        return view;
    const auto* o = g_app.scoped_read;
    auto* j = o ? job(*g_app.scoped_read, kind, id) : nullptr;
    if (j && application_scoped_read_current(j->ticket)) {
        view.ticket = j->ticket;
        view.publication_revision = o->revision;
        view.total = j->total;
        view.pending = j->pending;
        view.completed = j->completed;
        view.failed = j->failed;
        view.available = true;
        view.running = j->pending != 0;
        view.error = j->failed != 0;
        view.can_retry = j->failed != 0;
        view.status = view.running ? utf8_literal("Marking as read…")
                      : view.error ? utf8_literal("Some channels could not be marked as read.")
                                   : utf8_literal("Marked as read.");
    } else {
        ReadScopeSource source{};
        view.available = read_scope_source(g_app.account_ui->chat_state, kind, id, &source);
    }
    return view;
}
void application_scoped_read_synchronize() noexcept {
    auto* o = g_app.scoped_read;
    if (!o)
        return;
    if (!actor_current(o->actor)) {
        application_scoped_read_reset();
        return;
    }
    const auto& state = g_app.account_ui->chat_state;
    if (o->channels_epoch == state.channel_mutations &&
        o->permissions_epoch == state.permission_mutations && o->guilds_epoch == state.guild_mutations)
        return;
    o->channels_epoch = state.channel_mutations;
    o->permissions_epoch = state.permission_mutations;
    o->guilds_epoch = state.guild_mutations;
    bool changed = false;
    for (u32 i = 0; i < o->target_count;) {
        if (!target_current(*o, stable_storage_data<ScopedTarget>(o->targets)[i])) {
            remove_target(*o, i);
            changed = true;
        } else
            ++i;
    }
    if (changed) {
        recount(*o);
        publish(*o);
    }
}
u64 application_scoped_read_next_wake() noexcept {
    const auto* o = g_app.scoped_read;
    if (!o || o->flight_active || !actor_current(o->actor))
        return 0;
    const u64 due = bulk_read_ack_next_wake(o->queue);
    return due ? maximum(due, o->admission_due) : 0;
}
bool application_scoped_read_submit(u64 request_id, u64 now) noexcept {
    application_scoped_read_synchronize();
    auto* o = g_app.scoped_read;
    if (!o || !request_id || o->flight_active || !actor_current(o->actor) || now < o->admission_due)
        return false;
    // The candidate bank is already allocated when the request is accepted. It
    // is independent of the in-flight transport request, so newer plans cannot
    // overwrite responses.
    auto* candidates = stable_storage_data<BulkReadAckIntent>(o->candidates);
    if (!stable_storage_reserve(&o->candidates, 100))
        return false;
    u32 count{};
    if (!bulk_read_ack_peek(&o->queue, now, {candidates, 100}, &count))
        return false;
    if (!stable_storage_reserve(&o->flight, count))
        return false;
    auto* rows = stable_storage_data<ScopedFlightEntry>(o->flight);
    // UI-thread scratch, filled before it is read; 100 entries each would otherwise cost this request a stack probe.
    static BulkMessageAckEntry wire[100];
    static ApplicationReadEchoProof proofs[100];
    for (u32 i = 0; i < count; ++i) {
        auto* t = target(*o, candidates[i].channel_id);
        auto* j = t ? serial_job(*o, t->scope_serial) : nullptr;
        if (t && (candidates[i].action_serial != t->action_serial ||
                  candidates[i].scope_serial != t->scope_serial ||
                  candidates[i].message_id != t->target.message_id || t->individual)) {
            bulk_read_ack_cancel(&o->queue, candidates[i].channel_id, candidates[i].action_serial);
            return false;
        }
        if (!t || !j || !target_current(*o, *t)) {
            bulk_read_ack_cancel(&o->queue, candidates[i].channel_id, candidates[i].action_serial);
            if (t)
                application_scoped_read_superseded(t->target.channel_id, 0);
            return false;
        }
        wire[i] = {t->target.channel_id, t->target.message_id, 0};
        proofs[i] = {t->target.channel_id, t->target.message_id, t->action_serial};
        rows[i] = {candidates[i], j->ticket.source, wire[i], proofs[i]};
    }
    if (!application_read_state_echo_prepare({proofs, count})) {
        // A different request can consume echo capacity after plan capture.
        // Fail only this batch once; a past-due retry would busy-loop.
        for (u32 i = 0; i < count; ++i) {
            auto* t = target(*o, candidates[i].channel_id);
            if (t && t->action_serial == candidates[i].action_serial)
                t->failed = true;
            bulk_read_ack_cancel(&o->queue, candidates[i].channel_id, candidates[i].action_serial);
        }
        recount(*o);
        publish(*o);
        return false;
    }
    ++o->encodings;
    const auto result = g_app.account.api.bulk_message_ack(g_app.account.instance, wire, count, request_id);
    if (!result.ok()) {
        o->admission_due = deadline_after(now, 125);
        if (result.error != RestClientSubmitError::busy) {
            for (u32 i = 0; i < count; ++i) {
                auto* t = target(*o, candidates[i].channel_id);
                if (t)
                    t->failed = true;
                bulk_read_ack_cancel(&o->queue, candidates[i].channel_id, candidates[i].action_serial);
            }
            recount(*o);
            publish(*o);
        }
        return false;
    }
    if (!bulk_read_ack_admit(&o->queue, {candidates, count}, request_id, &o->flight_ticket))
        return false;
    o->flight_actor = o->actor;
    o->flight_count = count;
    o->flight_active = true;
    o->retired = o->cancel_requested = false;
    o->admission_due = 0;
    application_read_state_echo_admitted({proofs, count}, request_id);
    g_app.primary_request_id = request_id;
    return true;
}
bool application_scoped_read_response(const RestClientResponse& response, Utf8View, u64 now) noexcept {
    auto* o = g_app.scoped_read;
    if (!o || !o->flight_active || response.request_id != o->flight_ticket.request_id)
        return false;
    const bool same_actor = actor_current(o->flight_actor, false);
    if (same_actor && response.transport_ok() && response.status_code == 429)
        bulk_read_ack_floor(&o->queue,
                            response.retry_at_ms ? response.retry_at_ms : deadline_after(now, 1000));
    const bool success =
        response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    if (same_actor && success)
        bulk_read_ack_floor(&o->queue, deadline_after(now, 1000));
    if (response.transport_ok() && response.status_code >= 400 && response.status_code < 500)
        application_read_state_echo_rejected(response.request_id);
    const bool transient =
        !response.transport_ok() || response.status_code >= 500 || response.status_code == 429;
    const bool retry = transient && !api_rejection_requires_action(response.rejection);
    application_scoped_read_synchronize();
    const bool current = !o->retired && actor_current(o->flight_actor);
    const u32 attempts = o->queue.flight_count ? o->queue.flight[0].attempts : 3;
    const u64 due = maximum(response.retry_at_ms, deadline_after(now, attempts < 2 ? 2000 : 4000));
    if (current) {
        auto* rows = stable_storage_data<ScopedFlightEntry>(o->flight);
        for (u32 i = 0; i < o->flight_count; ++i) {
            auto* t = target(*o, rows[i].intent.channel_id);
            if (!t || t->action_serial != rows[i].intent.action_serial ||
                t->scope_serial != rows[i].intent.scope_serial)
                continue;
            if (!target_current(*o, *t)) {
                application_scoped_read_superseded(t->target.channel_id, 0);
                continue;
            }
            if (success)
                t->completed = true;
            else if (!retry || o->queue.flight[i].attempts >= 3)
                t->failed = true;
        }
        bulk_read_ack_complete(&o->queue, o->flight_ticket, success, !success && retry, now, due);
        recount(*o);
        publish(*o);
    } else
        bulk_read_ack_reset(&o->queue, same_actor);
    o->flight_active = false;
    o->flight_count = 0;
    o->retired = o->cancel_requested = false;
    if (o->release_pending)
        destroy(o);
    return true;
}
void application_scoped_read_superseded(u64 channel, u64 serial) noexcept {
    auto* o = g_app.scoped_read;
    if (!o)
        return;
    auto* row = target(*o, channel);
    if (!row || row->action_serial == serial)
        return;
    remove_target(*o, static_cast<u32>(row - stable_storage_data<ScopedTarget>(o->targets)));
    recount(*o);
    publish(*o);
}
void application_scoped_read_individual_result(u64 channel, u64 serial, bool success, bool retry) noexcept {
    auto* o = g_app.scoped_read;
    if (!o)
        return;
    auto* t = target(*o, channel);
    if (!t || !t->individual || t->action_serial != serial || !target_current(*o, *t))
        return;
    if (retry)
        return;
    if (t->completed == success && t->failed == !success)
        return;
    t->completed = success;
    t->failed = !success;
    recount(*o);
    publish(*o);
}
void application_scoped_read_reset() noexcept {
    auto* o = g_app.scoped_read;
    if (!o)
        return;
    cancel_flight(*o);
    const bool changed = o->target_count || o->job_count || o->queue.count;
    const auto* targets = stable_storage_data<ScopedTarget>(o->targets);
    for (u32 i = 0; i < o->target_count; ++i)
        application_read_state_scoped_cancel(targets[i].target.channel_id, targets[i].action_serial);
    o->target_count = o->job_count = 0;
    if (o->flight_active) {
        while (o->queue.count)
            bulk_read_ack_cancel(&o->queue, o->queue.pending[0].channel_id);
    } else
        bulk_read_ack_reset(&o->queue, actor_current(o->actor, false));
    if (changed)
        publish(*o);
}
void application_scoped_read_release() noexcept {
    auto* o = g_app.scoped_read;
    if (!o)
        return;
    application_scoped_read_reset();
    if (o->flight_active && g_app.account.instance &&
        (g_app.account.status.running || g_app.account.stop_failed)) {
        o->release_pending = true;
        return;
    }
    destroy(o);
}
ApplicationScopedReadMetrics application_scoped_read_metrics() noexcept {
    ApplicationScopedReadMetrics m{};
    const auto* o = g_app.scoped_read;
    if (!o)
        return m;
    Win32MemoryInformation region{};
    VirtualQuery(o, region.native(), sizeof(region));
    m.owner_bytes = sizeof(*o);
    m.owner_reserved_bytes = static_cast<u32>(region.RegionSize);
    m.owner_committed_bytes = m.owner_reserved_bytes;
    m.queue_reserved_bytes = static_cast<u32>(o->queued.arena.reserved + o->admitted.arena.reserved);
    m.queue_committed_bytes = static_cast<u32>(o->queued.arena.committed + o->admitted.arena.committed);
    m.targets_reserved_bytes = static_cast<u32>(o->targets.arena.reserved);
    m.targets_committed_bytes = static_cast<u32>(o->targets.arena.committed);
    m.plan_reserved_bytes = static_cast<u32>(o->plan.arena.reserved);
    m.plan_committed_bytes = static_cast<u32>(o->plan.arena.committed);
    m.jobs_reserved_bytes = static_cast<u32>(o->jobs.arena.reserved);
    m.jobs_committed_bytes = static_cast<u32>(o->jobs.arena.committed);
    m.flight_reserved_bytes = static_cast<u32>(o->flight.arena.reserved);
    m.flight_committed_bytes = static_cast<u32>(o->flight.arena.committed);
    m.candidate_reserved_bytes = static_cast<u32>(o->candidates.arena.reserved);
    m.candidate_committed_bytes = static_cast<u32>(o->candidates.arena.committed);
    m.publication_revision = o->revision;
    m.flight_active = o->flight_active;
    m.plan_builds = o->plan_builds;
    m.target_checks = o->target_checks;
    m.encodings = o->encodings;
    m.commits = o->queued.arena.commit_count + o->admitted.arena.commit_count +
                o->targets.arena.commit_count + o->plan.arena.commit_count +
                o->candidates.arena.commit_count + o->jobs.arena.commit_count +
                o->flight.arena.commit_count + 1;
    return m;
}
} // namespace maicord::app_detail
