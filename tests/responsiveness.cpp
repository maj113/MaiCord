#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/presence_coalescer.hpp"
#include "maicord/app/wake_coalescing.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/member_list.hpp"
#include "maicord/protocol/client_status.hpp"
#include "maicord/model/user_profile.hpp"
#include "../src/app/gateway_relationship_dispatch.hpp"

namespace maicord {
namespace {

// Startup ordering: the account worker connects before the UI exists. Its
// Gateway producer publishes into the bounded mailbox while nothing consumes,
// blocks at capacity, and the UI later drains every event in order.
struct FakeWorker {
    GatewayClientMailbox* mailbox;
    HANDLE space, cancel, notification;
    u32 events;
    volatile LONG published;
    bool failed;
};

DWORD WINAPI fake_worker(void* context) noexcept {
    auto& worker = *static_cast<FakeWorker*>(context);
    for (u32 index = 0; index < worker.events; ++index) {
        GatewayClientEvent event{};
        event.kind = GatewayClientEventKind::dispatch;
        event.sequence = index + 1;
        event.has_sequence = true;
        const u64 body = 0x5EED0000ull + index;
        if (!gateway_mailbox_publish(worker.mailbox, worker.space, worker.cancel, worker.notification,
                                     GetTickCount64() + 5'000, event,
                                     {reinterpret_cast<const u8*>(&body), sizeof(body)})) {
            worker.failed = true;
            return 1;
        }
        InterlockedIncrement(&worker.published);
    }
    return 0;
}

void test_startup_buffered_worker(void (*check)(bool, const char*) noexcept) noexcept {
    auto* mailbox = static_cast<GatewayClientMailbox*>(
        VirtualAlloc(nullptr, sizeof(GatewayClientMailbox), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    FakeWorker worker{mailbox, CreateEventW(nullptr, FALSE, FALSE, nullptr),
                      CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr),
                      kGatewayClientEventCapacity * 2 + 7, 0, false};
    const HANDLE thread = mailbox && worker.space && worker.cancel && worker.notification
                              ? CreateThread(nullptr, 64 * 1024, fake_worker, &worker, 0, nullptr)
                              : nullptr;
    check(thread != nullptr, "startup: fake account worker starts before the UI");
    if (!thread) {
        if (mailbox)
            VirtualFree(mailbox, 0, MEM_RELEASE);
        return;
    }
    // The UI is still initializing: the producer fills the mailbox and waits. Every push and the
    // full-mailbox path signal the notification after `published` advances, so no wake is lost.
    while (ReadAcquire(&worker.published) < LONG{kGatewayClientEventCapacity} &&
           WaitForSingleObject(worker.notification, 5'000) == WAIT_OBJECT_0) {
    }
    check(ReadAcquire(&worker.published) == LONG{kGatewayClientEventCapacity} && mailbox->dropped == 0 &&
              WaitForSingleObject(thread, 0) == WAIT_TIMEOUT,
          "startup: worker buffers up to capacity and waits without dropping events");
    // Attach: the UI drains in order, signaling space like the Gateway pop.
    bool ordered = true;
    u32 received = 0;
    while (received < worker.events) {
        GatewayClientEvent event{};
        u64 body = 0;
        const auto popped = spsc_mailbox_pop(mailbox, &event, {reinterpret_cast<u8*>(&body), sizeof(body)});
        if (popped.error == SpscMailboxError::empty) {
            if (WaitForSingleObject(worker.notification, 5'000) != WAIT_OBJECT_0)
                break;
            continue;
        }
        ordered = ordered && popped.ok() && event.sequence == received + 1 && body == 0x5EED0000ull + received;
        ++received;
        SetEvent(worker.space);
    }
    const bool joined = WaitForSingleObject(thread, 5'000) == WAIT_OBJECT_0;
    check(joined && !worker.failed && ordered && received == worker.events && mailbox->dropped == 0,
          "startup: buffered dispatches reach the UI complete and in order");
    if (!joined) {
        SetEvent(worker.cancel);
        WaitForSingleObject(thread, INFINITE);
    }
    CloseHandle(thread);
    CloseHandle(worker.space);
    CloseHandle(worker.cancel);
    CloseHandle(worker.notification);
    VirtualFree(mailbox, 0, MEM_RELEASE);
}

struct PresenceFixture {
    MessageRecord records[4];
    u32 order[4];
    u8 text[128];
    u32 heights[4];
    u64 fenwick[4];
    MessageStore store;
    UiVariableList list;
    MessageTimeline timeline;
    MessageEventPump pump;
    GuildMemberRecord member_records[64];
    u32 member_order[64];
    GuildMemberStore members;
    UserRecord user_records[64];
    u32 user_order[64];
    u8 user_names[4096];
    UserStore users;
    GuildRecord guilds[2];
    ChannelRecord channels[2];
    u16 guild_order[2], channel_order[2];
    u8 state_names[1024];
    ChatStateStore state;
    MemberListView projection;
    MemberListRow rows[96];
    u32 scratch[64];
    u64 visible_user;
};
PresenceFixture g_presence{};

bool visible_user(void* context, u64, u64 user_id) noexcept {
    return static_cast<PresenceFixture*>(context)->visible_user == user_id;
}

MessageEventPumpConsumeResult presence(PresenceFixture& f, u64 user, GatewayClientMemberListStatus status,
                                       Utf8View activity, u8 client_status = 0) noexcept {
    UserAppearance appearance{};
    appearance.user_id = user;
    appearance.guild_id = 7;
    appearance.fields = appearance_activity;
    appearance.activity.bytes = static_cast<u16>(activity.bytes);
    copy_bytes(appearance.activity.data, activity.data, activity.bytes);
    u8 payload[512]{};
    const u32 bytes = user_appearance_write(appearance, {payload, sizeof(payload)});
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    event.dispatch_kind = GatewayClientDispatchKind::presence_update;
    event.dispatch_payload_bytes = bytes;
    event.presence = {7, user, status, client_status};
    return message_event_pump_consume(&f.pump, event, {payload, bytes});
}

void test_presence_visibility(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = g_presence;
    f = {};
    bool ready = message_store_initialize(&f.store, {f.records, 4}, {f.order, 4}, {f.text, 128}) &&
                 message_store_upsert(&f.store, MessageInput{300, 7, 11, 300, 0, utf8_literal("seed"), 0}).ok() &&
                 ui_variable_list_initialize(&f.list, {f.heights, 4}, {f.fenwick, 4}, 1, 100) &&
                 message_timeline_initialize(&f.timeline, &f.store, &f.list, 100, 50, false) &&
                 guild_member_store_initialize(&f.members, {f.member_records, 64}, {f.member_order, 64}) &&
                 user_store_initialize(&f.users, {f.user_records, 64}, {f.user_order, 64},
                                               {f.user_names, sizeof(f.user_names)}) &&
                 chat_state_store_initialize(&f.state, {f.guilds, 2}, {f.guild_order, 2}, {f.channels, 2},
                                                {f.channel_order, 2}, {f.state_names, sizeof(f.state_names)}) &&
                 member_list_view_initialize(&f.projection, {f.rows, 96}, {f.scratch, 64});
    // Forty online members in one dense (client-sorted) list; rows 0..40.
    char name[8] = "user00";
    for (u64 id = 1; ready && id <= 40; ++id) {
        name[4] = static_cast<char>('0' + id / 10);
        name[5] = static_cast<char>('0' + id % 10);
        ready = user_store_upsert(&f.users, id, {name, 6}).ok() &&
                guild_member_store_upsert(&f.members, f.state, 7, id, {}, {}, false,
                                                  GuildMemberStatus::online, static_cast<u32>(id), 70)
                    .ok();
    }
    ready = ready && member_list_view_rebuild(&f.projection, f.members, f.users, f.state, 7, 70).ok();
    f.pump.timeline = &f.timeline;
    f.pump.users = &f.users;
    f.pump.members = &f.members;
    message_event_pump_set_presence_visibility(&f.pump, visible_user, &f);
    check(ready && f.projection.count == 41 && f.projection.rebuilds == 1, "presence: dense member fixture");

    // Off-screen appearance and status changes update the model only.
    f.visible_user = 3;
    const u32 user_revision = f.users.revision, member_revision = f.members.revision;
    const auto hidden = presence(f, 30, GatewayClientMemberListStatus::idle, utf8_literal("Playing A"));
    const auto* stored = guild_member_store_find(f.members, 7, 30);
    const auto* activity = user_appearance_find(f.users.appearance, 30, 7);
    check(hidden.ok() && !hidden.repaint && !f.pump.presence_visible_dirty && f.pump.presence_offscreen == 1 &&
              f.users.revision == user_revision && f.members.revision == member_revision &&
              f.members.presence_revision == 1 && stored && stored->status == GuildMemberStatus::idle &&
              activity && activity->activity.bytes == 9,
          "presence: off-screen member applies to the model without invalidating text");

    // A visible author's appearance change is deferred to the coalesced flush.
    const auto shown = presence(f, 3, GatewayClientMemberListStatus::online, utf8_literal("Playing B"));
    check(shown.ok() && !shown.repaint && f.pump.presence_visible_dirty && f.users.revision == user_revision,
          "presence: visible appearance marks the coalesced repaint instead of repainting");

    presence(f, 30, GatewayClientMemberListStatus::dnd, utf8_literal("Playing C"));
    // In-place rows: idle/dnd stay in the online group, so no rebuild; only a
    // change inside the visible window publishes a projection revision.
    const u32 revision = f.projection.revision;
    auto refreshed = member_list_view_refresh_presence(&f.projection, f.members, 0, 10);
    check(refreshed.updated == 1 && !refreshed.visible && !refreshed.regroup && f.rows[30].user_id == 30 &&
              f.rows[30].status == GuildMemberStatus::dnd && f.projection.revision == revision &&
              member_list_view_rebuild(&f.projection, f.members, f.users, f.state, 7, 70).ok() &&
              f.projection.rebuilds == 1,
          "presence: off-screen status updates its row in place without a rebuild or revision");
    presence(f, 2, GatewayClientMemberListStatus::idle, {});
    refreshed = member_list_view_refresh_presence(&f.projection, f.members, 0, 10);
    check(refreshed.updated == 1 && refreshed.visible && f.rows[2].status == GuildMemberStatus::idle &&
              f.projection.revision == revision + 1 && f.projection.rebuilds == 1,
          "presence: visible in-place update publishes one projection revision");
    presence(f, 5, GatewayClientMemberListStatus::offline, {});
    refreshed = member_list_view_refresh_presence(&f.projection, f.members, 0, 10);
    const bool regrouped = refreshed.regroup &&
                           member_list_view_rebuild(&f.projection, f.members, f.users, f.state, 7, 70)
                               .ok() &&
                           f.projection.rebuilds == 2 && f.rows[f.projection.count - 1].user_id == 5 &&
                           f.rows[f.projection.count - 2].kind == MemberListRowKind::group;
    check(regrouped, "presence: leaving the online group requests one sorted rebuild");

    // Per-platform presence (client_status) follows the same policy: an
    // off-screen change updates the user only, a visible one joins the flush.
    const u8 mobile = client_status_with(0, ClientPlatform::mobile, ClientPlatformStatus::online);
    presence(f, 31, GatewayClientMemberListStatus::online, utf8_literal("Playing E"));
    f.pump.presence_visible_dirty = false;
    const u32 offscreen = f.pump.presence_offscreen, platform_users = f.users.revision;
    const auto hidden_platform =
        presence(f, 31, GatewayClientMemberListStatus::online, utf8_literal("Playing E"), mobile);
    check(hidden_platform.ok() && !hidden_platform.repaint && !f.pump.presence_visible_dirty &&
              f.pump.presence_offscreen == offscreen + 1 && f.users.revision == platform_users &&
              user_store_client_status(f.users, 31) == mobile,
          "client status: an off-screen platform change updates the user without a repaint");
    presence(f, 3, GatewayClientMemberListStatus::online, utf8_literal("Playing B"));
    f.pump.presence_visible_dirty = false;
    const auto shown_platform = presence(f, 3, GatewayClientMemberListStatus::online, utf8_literal("Playing B"), mobile);
    const bool marked = shown_platform.ok() && !shown_platform.repaint && f.pump.presence_visible_dirty &&
                        f.users.revision == platform_users && user_store_client_status(f.users, 3) == mobile;
    f.pump.presence_visible_dirty = false;
    presence(f, 3, GatewayClientMemberListStatus::online, utf8_literal("Playing B"), mobile);
    check(marked && !f.pump.presence_visible_dirty,
          "client status: a visible platform change joins the coalesced flush once; a repeat is no change");
    presence(f, 31, GatewayClientMemberListStatus::offline, {}, mobile);
    f.pump.state = &f.state;
    f.state.self_user_id = 4;
    presence(f, 4, GatewayClientMemberListStatus::online, {}, mobile);
    check(!user_store_client_status(f.users, 31) && !user_store_client_status(f.users, 4) &&
              user_store_client_status(f.users, 3) == mobile,
          "client status: offline clears every platform and the current user keeps none");
    f.pump.state = nullptr;
    f.state.self_user_id = 0;

    // Without a predicate (legacy owners), presence still repaints directly.
    message_event_pump_set_presence_visibility(&f.pump, nullptr, nullptr);
    const auto legacy = presence(f, 30, GatewayClientMemberListStatus::idle, utf8_literal("Playing D"));
    check(legacy.ok() && legacy.repaint && f.users.revision == user_revision + 1,
          "presence: owners without visibility keep immediate repaint");
    user_appearance_release(&f.users.appearance);
}

void test_presence_coalescing(void (*check)(bool, const char*) noexcept) noexcept {
    PresenceCoalescer coalescer{};
    check(!presence_coalescer_take(&coalescer, 1'000), "coalescer: idle window has no flush");
    presence_coalescer_note(&coalescer, 1'000);
    presence_coalescer_note(&coalescer, 1'040);
    presence_coalescer_note(&coalescer, 1'149);
    check(coalescer.flush_at_ms == 1'000 + kPresenceCoalesceMilliseconds && coalescer.noted == 3 &&
              !presence_coalescer_take(&coalescer, 1'149),
          "coalescer: changes inside the window join the first deadline");
    check(presence_coalescer_take(&coalescer, 1'150) && coalescer.flush_at_ms == 0 && coalescer.flushes == 1 &&
              !presence_coalescer_take(&coalescer, 1'151),
          "coalescer: one flush per window");
    presence_coalescer_note(&coalescer, 2'000);
    check(coalescer.flush_at_ms == 2'150, "coalescer: the next change opens a new window");
}

void test_wake_coalescing(void (*check)(bool, const char*) noexcept) noexcept {
    check(wake_tolerance_ms(5'000, WakeContext::foreground) == 0 &&
              wake_tolerance_ms(INFINITE, WakeContext::hidden) == 0,
          "wake: focused and infinite waits are exact");
    check(wake_tolerance_ms(40, WakeContext::background) == 0 &&
              wake_tolerance_ms(999, WakeContext::background) == 0 &&
              wake_tolerance_ms(2'000, WakeContext::background) == 250 &&
              wake_tolerance_ms(60'000, WakeContext::background) == 1'000,
          "wake: visible unfocused windows only relax long timers");
    check(wake_tolerance_ms(40, WakeContext::hidden) == 0 && wake_tolerance_ms(150, WakeContext::hidden) == 37 &&
              wake_tolerance_ms(30'000, WakeContext::hidden) == 1'000,
          "wake: minimized windows relax every nonvisual timer, bounded to one second");
    // The tolerant timer path fires once after its due time.
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    LARGE_INTEGER due{};
    due.QuadPart = -1 * 10'000;
    const bool armed = timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr,
                                                   wake_tolerance_ms(1, WakeContext::hidden) + 5);
    const bool fired = armed && WaitForSingleObject(timer, 2'000) == WAIT_OBJECT_0;
    check(fired && WaitForSingleObject(timer, 0) == WAIT_TIMEOUT, "wake: tolerant synchronization timer fires once");
    if (timer)
        CloseHandle(timer);
}

} // namespace

void test_responsiveness(void (*check)(bool, const char*) noexcept) noexcept {
    test_startup_buffered_worker(check);
    test_presence_visibility(check);
    test_presence_coalescing(check);
    test_wake_coalescing(check);
}

} // namespace maicord
