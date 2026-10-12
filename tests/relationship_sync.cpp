#include "maicord/protocol/relationships.hpp"
#include "maicord/app/gateway_client.hpp"
#include "../src/app/gateway_relationship_dispatch.hpp"

namespace maicord {
namespace {
struct Capture {
    RelationshipStore active, pending;
    RelationshipRecord active_rows[8], pending_rows[8];
    u32 calls, fail_after, received;
    bool staging, completed;
    RelationshipMutation last;
};
struct AdmissionCapture {
    u32 candidate_calls, active_calls, candidate_count, active_count;
    bool reject_candidate, reject_active;
};
// The gateway path: decode one dispatch record, then apply it to the store.
RelationshipStoreResult apply_event(RelationshipStore* store, RelationshipEvent event, Utf8View input,
                                    Slice<u32> scratch) noexcept {
    RelationshipMutation mutation{};
    if (!relationship_decode_record(input, scratch, event, &mutation).ok())
        return {RelationshipStoreError::invalid_input, false};
    return relationship_apply_decoded(store, event, mutation);
}
bool admit_relationships(void* opaque, u32 count, bool active) noexcept {
    auto& capture = *static_cast<AdmissionCapture*>(opaque);
    if (active) { ++capture.active_calls; capture.active_count = count; return !capture.reject_active; }
    ++capture.candidate_calls; capture.candidate_count = count; return !capture.reject_candidate;
}
bool capture(void* opaque, const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    auto& capture = *static_cast<Capture*>(opaque);
    if (capture.fail_after && ++capture.calls == capture.fail_after) return false;
    switch (event.dispatch_kind) {
    case GatewayClientDispatchKind::relationship_snapshot_begin:
        relationship_store_replace(&capture.pending, {});
        capture.staging = true; capture.completed = false; capture.received = 0;
        return true;
    case GatewayClientDispatchKind::relationship_snapshot_item: {
        u32 cursor = 0;
        if (!capture.staging || !gateway_client_relationship_record(payload, &cursor, &capture.last) || cursor != payload.count ||
            relationship_store_find(capture.pending, capture.last.record.user_id)) return false;
        if (!relationship_store_upsert(&capture.pending, capture.last.record).ok()) return false;
        ++capture.received;
        return true;
    }
    case GatewayClientDispatchKind::presence_update: {
        using Status = GatewayClientMemberListStatus;
        const auto presence = event.presence.status == Status::online ? RelationshipPresence::online :
            event.presence.status == Status::idle ? RelationshipPresence::idle :
            event.presence.status == Status::dnd ? RelationshipPresence::dnd :
            event.presence.status == Status::offline ? RelationshipPresence::offline : RelationshipPresence::unknown;
        return !event.presence.guild_id && relationship_store_presence(capture.staging ? &capture.pending : &capture.active,
            event.presence.user_id, presence).ok();
    }
    case GatewayClientDispatchKind::relationship_snapshot_complete:
        if (!capture.staging || event.state.record_count != capture.received) return false;
        if (!relationship_store_replace(&capture.active, {capture.pending.records.data, capture.pending.count}).ok()) return false;
        capture.staging = false; capture.completed = true;
        return true;
    default: return false;
    }
}
struct Producer {
    GatewayClientMailbox* mailbox;
    HANDLE space, cancel, notification, entered;
    GatewayClientEvent event;
    ConstSlice<u8> payload;
    u64 deadline;
    bool published;
};
DWORD WINAPI producer(void* opaque) noexcept {
    auto& task = *static_cast<Producer*>(opaque);
    SetEvent(task.entered);
    task.published = gateway_mailbox_publish(task.mailbox, task.space, task.cancel, task.notification,
        task.deadline, task.event, task.payload);
    return 0;
}
void test_backpressure(void (*check)(bool, const char*) noexcept) noexcept {
    auto* mailbox = static_cast<GatewayClientMailbox*>(VirtualAlloc(nullptr, sizeof(GatewayClientMailbox), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* body = static_cast<u8*>(VirtualAlloc(nullptr, kGatewayClientDispatchPayloadCapacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    HANDLE space = CreateEventW(nullptr, FALSE, FALSE, nullptr), cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr),
        notification = CreateEventW(nullptr, FALSE, FALSE, nullptr), entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct Cleanup {
        void* mailbox; void* body; HANDLE space, cancel, notification, entered;
        ~Cleanup() noexcept {
            if (entered) CloseHandle(entered); if (notification) CloseHandle(notification);
            if (cancel) CloseHandle(cancel); if (space) CloseHandle(space);
            if (body) VirtualFree(body, 0, MEM_RELEASE); if (mailbox) VirtualFree(mailbox, 0, MEM_RELEASE);
        }
    } cleanup{mailbox, body, space, cancel, notification, entered};
    check(mailbox && body && space && cancel && notification && entered, "friends Gateway: bounded real-mailbox fixture and control handles allocate");
    if (!mailbox || !body || !space || !cancel || !notification || !entered) return;
    GatewayClientEvent event{};
    event.kind = GatewayClientEventKind::dispatch;
    const u8 byte = 0x5A;
    auto start = [&](Producer* task) noexcept {
        ResetEvent(entered);
        task->deadline = GetTickCount64() + 2000;
        return CreateThread(nullptr, 0, producer, task, 0, nullptr);
    };
    bool filled = true;
    for (u32 i = 0; i < kGatewayClientEventCapacity; ++i) filled = spsc_mailbox_push(mailbox, event, {}).ok() && filled;
    Producer task{mailbox, space, cancel, notification, entered, event, {&byte, 1}};
    HANDLE thread = start(&task);
    check(filled && thread && WaitForSingleObject(entered, 1000) == WAIT_OBJECT_0 && WaitForSingleObject(thread, 20) == WAIT_TIMEOUT,
        "friends Gateway: actual producer blocks on descriptor capacity without dropping a record");
    if (!thread) return;
    check(spsc_mailbox_discard(mailbox, &event).ok(), "friends Gateway: actual consumer returns descriptor ownership");
    SetEvent(space);
    const bool joined = WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0;
    check(joined && task.published && spsc_mailbox_size(*mailbox) == kGatewayClientEventCapacity && !spsc_mailbox_dropped(*mailbox),
        "friends Gateway: consumer space wakes the real producer and preserves FIFO without capacity drops");
    if (!joined) { SetEvent(cancel); WaitForSingleObject(thread, 3000); }
    CloseHandle(thread);
    u8 copied = 0;
    for (u32 i = 0; i < kGatewayClientEventCapacity; ++i) spsc_mailbox_pop(mailbox, &event, {&copied, 1});
    check(copied == byte && !spsc_mailbox_size(*mailbox), "friends Gateway: waited producer payload survives at the FIFO tail");

    SecureZeroMemory(mailbox, sizeof(*mailbox)); ResetEvent(space); ResetEvent(cancel);
    check(spsc_mailbox_push(mailbox, event, {body, kGatewayClientDispatchPayloadCapacity}).ok(),
        "friends Gateway: one event can exhaust the actual bounded payload ring");
    task.published = false;
    thread = start(&task);
    check(thread && WaitForSingleObject(entered, 1000) == WAIT_OBJECT_0 && WaitForSingleObject(thread, 20) == WAIT_TIMEOUT,
        "friends Gateway: real producer waits for payload bytes even with free descriptor slots");
    if (!thread) return;
    spsc_mailbox_discard(mailbox, &event); SetEvent(space);
    const bool payload_joined = WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0;
    check(payload_joined && task.published && !spsc_mailbox_dropped(*mailbox) && spsc_mailbox_payload_used(*mailbox) == 1,
        "friends Gateway: discarding payload ownership releases pressure without losing the next record");
    if (!payload_joined) { SetEvent(cancel); WaitForSingleObject(thread, 3000); }
    CloseHandle(thread);

    SecureZeroMemory(mailbox, sizeof(*mailbox)); ResetEvent(space); ResetEvent(cancel);
    for (u32 i = 0; i < kGatewayClientEventCapacity; ++i) spsc_mailbox_push(mailbox, event, {});
    task.published = false;
    thread = start(&task);
    check(thread && WaitForSingleObject(entered, 1000) == WAIT_OBJECT_0 && WaitForSingleObject(thread, 20) == WAIT_TIMEOUT,
        "friends Gateway: cancellation fixture starts a blocked real producer");
    if (!thread) return;
    SetEvent(cancel);
    const bool cancelled = WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0;
    check(cancelled && !task.published && !spsc_mailbox_dropped(*mailbox) && spsc_mailbox_size(*mailbox) == kGatewayClientEventCapacity,
        "friends Gateway: stop cancellation wakes blocked publication promptly and preserves existing FIFO ownership");
    if (!cancelled) WaitForSingleObject(thread, 3000);
    CloseHandle(thread);
    ResetEvent(cancel);
    spsc_mailbox_discard(mailbox, &event);
    SetEvent(space); // Consumer wins before producer enters its wait.
    task.published = false;
    thread = start(&task);
    const bool no_lost_wake = thread && WaitForSingleObject(thread, 1000) == WAIT_OBJECT_0;
    check(no_lost_wake && task.published && !spsc_mailbox_dropped(*mailbox),
        "friends Gateway: space published before the wait cannot be lost at the full-mailbox boundary");
    if (thread) { if (!no_lost_wake) { SetEvent(cancel); WaitForSingleObject(thread, 3000); } CloseHandle(thread); }
    ResetEvent(cancel);
    check(!gateway_mailbox_publish(mailbox, space, cancel, notification, GetTickCount64(), event, {&byte, 1}) &&
        !spsc_mailbox_dropped(*mailbox), "friends Gateway: a full mailbox honors its bounded deadline without a lossy push");
}
} // namespace

void test_relationship_sync(void (*check)(bool, const char*) noexcept) noexcept {
    RelationshipRecord records[8]{}, candidates[8]{};
    RelationshipStore store{};
    RelationshipUser snapshots[8]{};
    u32 scratch[1024]{}, object_scratch[512]{};
    check(relationship_store_initialize(&store, {records, 8}), "friends: caller-owned sorted relationship storage initializes");
    RelationshipRecord record{};
    record.user_id = 20; record.type = RelationshipType::friend_; record.presence = RelationshipPresence::online;
    check(relationship_store_upsert(&store, record).changed && relationship_store_upsert(&store, record).ok() &&
        !relationship_store_upsert(&store, record).changed && store.count == 1,
        "friends: duplicate semantic upsert is unchanged and owns no allocation");
    const u32 revision = store.revision;
    check(!relationship_store_presence(&store, 20, RelationshipPresence::online).changed &&
        !relationship_store_remove(&store, 999).changed && store.revision == revision,
        "friends: unchanged presence and absent removal preserve the projection revision");
    record.user_id = 10; record.type = RelationshipType::incoming;
    check(relationship_store_upsert(&store, record).ok() && store.records[0].user_id == 10 && store.records[1].user_id == 20,
        "friends: IDs stay sorted for bounded binary lookup and stable filtered indices");
    check(relationship_matches(records[1], RelationshipFilter::all) &&
        relationship_matches(records[1], RelationshipFilter::online) &&
        relationship_matches(records[0], RelationshipFilter::pending), "friends: friend, online and pending filters reflect actual relationship types");
    record.spam = true;
    check(!relationship_matches(record, RelationshipFilter::pending), "friends: ordinary pending excludes current-client spam requests");
    record.spam = false; record.ignored = true;
    check(!relationship_matches(record, RelationshipFilter::pending), "friends: ordinary pending excludes ignored incoming requests");
    {
        struct StrangerFixture { Utf8View input; bool stranger, known; };
        const StrangerFixture fixtures[]{
            {utf8_literal(R"({"id":"42","type":3,"is_stranger_request":true})"), true, true},
            {utf8_literal(R"({"id":"42","type":3,"is_stranger_request":false})"), false, true},
            {utf8_literal(R"({"id":"42","type":3})"), false, false}};
        for (const auto& fixture : fixtures) {
            RelationshipMutation decoded{};
            const auto result = relationship_decode_record(fixture.input, {scratch, 1024}, RelationshipEvent::update, &decoded);
            check(result.ok() && decoded.record.stranger == fixture.stranger && decoded.record.stranger_known == fixture.known,
                "friends stranger: explicit true, explicit false and absent flag remain distinct bounded decoder metadata");
        }
        RelationshipRecord tri_records[1]{};
        RelationshipStore tri_store{};
        relationship_store_initialize(&tri_store, {tri_records, 1});
        RelationshipRecord tri_record{}; tri_record.user_id = 42; tri_record.type = RelationshipType::incoming;
        relationship_store_upsert(&tri_store, tri_record);
        const u32 unknown_revision = tri_store.revision;
        tri_record.stranger_known = true;
        check(relationship_store_upsert(&tri_store, tri_record).changed && tri_store.revision > unknown_revision &&
            !tri_records[0].stranger && tri_records[0].stranger_known,
            "friends stranger: unknown to authoritative known false revises projection even when stranger boolean stays false");
        const u32 known_revision = tri_store.revision;
        check(!relationship_store_upsert(&tri_store, tri_record).changed && tri_store.revision == known_revision,
            "friends stranger: unchanged known metadata does not manufacture a projection update");
        check(apply_event(&tri_store, RelationshipEvent::update, fixtures[2].input, {scratch, 1024}).changed &&
            !tri_records[0].stranger_known && !tri_records[0].stranger,
            "friends stranger: authoritative update with omitted flag invalidates known state without fabricating known false");
        check(apply_event(&tri_store, RelationshipEvent::add, fixtures[0].input, {scratch, 1024}).changed &&
            tri_records[0].stranger_known && tri_records[0].stranger,
            "friends stranger: explicit ADD metadata restores known stranger state");
    }

    const auto array = utf8_literal(R"([{"id":"30","type":4,"user":{"id":"30","username":"outgoing","global_name":null,"avatar":null}},{"id":"20","type":1,"nickname":"Friend \u2603","user":{"id":"20","username":"friend","global_name":"Friendly","avatar":"0123456789abcdef0123456789abcdef"}},{"id":"10","type":2}])");
    const auto decoded = relationships_replace(&store, array, {scratch, 1024}, {candidates, 8}, {snapshots, 8});
    check(decoded.ok() && decoded.changed && decoded.count == 3 && decoded.user_count == 2 &&
        records[0].user_id == 10 && records[0].type == RelationshipType::blocked && records[1].nickname_bytes == 10 &&
        records[1].presence == RelationshipPresence::online && records[2].type == RelationshipType::outgoing,
        "friends: full decoded snapshot commits sorted owned rows, escaped nickname and preserved known presence");
    const u32 accepted_revision = store.revision;
    check(relationships_replace(&store, array, {scratch, 1024}, {candidates, 8}, {}).ok() && store.revision == accepted_revision,
        "friends: unchanged snapshot validates users without retaining duplicate name arrays or bumping revision");
    UserRecord users_storage[8]{}; u32 user_order[8]{}; u8 names[2048]{};
    UserStore users{};
    check(user_store_initialize(&users, {users_storage, 8}, {user_order, 8}, {names, 2048}) &&
        relationships_hydrate(&users, array, {scratch, 1024}).ok() &&
        utf8_equal(relationship_display_name(records[1], users), utf8_literal("Friend ☃")) &&
        utf8_equal(user_store_username(users, *user_store_find(users, 30)), utf8_literal("outgoing")),
        "friends: streaming hydration reuses existing user store while relationship nickname owns display precedence");
    RelationshipRecord accepted[8]{};
    copy_bytes(accepted, records, sizeof(records));
    const Utf8View bad[]{
        utf8_literal(R"([{"id":"1","type":1},{"id":"1","type":2}])"),
        utf8_literal(R"([{"id":"1","type":1},{"id":"2","type":99}])"),
        utf8_literal(R"([{"id":"1","type":1},{"id":"2","type":1,"user":{"id":"3","username":"wrong"}}])"),
        utf8_literal(R"([{"id":"1","type":1},{"id":"2","type":1,"user":{"id":"2","username":"bad","avatar":"not-a-hash"}}])"),
        utf8_literal(R"([{"id":"1","type":1},{"id":"2","type":1,"type":2}])"),
        utf8_literal(R"([{"id":"1","type":1},])")};
    for (const auto input : bad) check(!relationships_replace(&store, input, {scratch, 1024}, {candidates, 8}, {}).ok() &&
        bytes_equal(accepted, records, sizeof(records)) && store.revision == accepted_revision,
        "friends: late malformed, duplicate, unknown type and mismatched user snapshots leave active rows unchanged");
    check(relationships_replace(&store, array, {scratch, 1024}, {candidates, 1}, {}).error == RelationshipsDecodeError::capacity &&
        relationships_replace(&store, array, {scratch, 1}, {candidates, 8}, {}).error == RelationshipsDecodeError::scan &&
        relationships_replace(&store, array, {scratch, 1024}, {records, 8}, {}).error == RelationshipsDecodeError::invalid_input &&
        bytes_equal(accepted, records, sizeof(records)), "friends: candidate/parser exhaustion and active-scratch alias fail without partial replacement");
    check(apply_event(&store, RelationshipEvent::add, utf8_literal(R"({"id":"20","type":1,"nickname":null})"),
        {scratch, 1024}).ok() && records[1].nickname_bytes == 10 && records[1].presence == RelationshipPresence::online,
        "friends: ADD preserves current nickname for missing/null nickname and retains known presence");
    check(apply_event(&store, RelationshipEvent::update, utf8_literal(R"({"id":"20","type":1})"),
        {scratch, 1024}).changed && !records[1].nickname_bytes && records[1].presence == RelationshipPresence::online,
        "friends: UPDATE clears omitted nickname while retaining independently tracked presence");
    check(apply_event(&store, RelationshipEvent::remove, utf8_literal(R"({"id":"30"})"), {scratch, 1024}).changed &&
        !apply_event(&store, RelationshipEvent::remove, utf8_literal(R"({"id":"30"})"), {scratch, 1024}).changed,
        "friends: REMOVE is idempotent and accepts the confirmed ID-only dispatch shape");
    check(relationships_replace(&store, utf8_literal("[]"), {scratch, 1024}, {candidates, 8}, {}).changed && !store.count,
        "friends: an explicitly complete empty snapshot clears active relationships");
    {
        RelationshipRecord active[2]{}, staged[2]{};
        RelationshipStore admitted_store{};
        relationship_store_initialize(&admitted_store, {active, 2});
        auto previous = record; previous.user_id = 77; previous.type = RelationshipType::friend_;
        relationship_store_upsert(&admitted_store, previous);
        const u32 previous_revision = admitted_store.revision;
        staged[0].user_id = 99;
        AdmissionCapture capture{}; capture.reject_candidate = true;
        const auto input = utf8_literal(R"([{"id":"2","type":3},{"id":"1","type":4}])");
        auto admitted = relationships_replace(&admitted_store, input, {scratch, 1024}, {staged, 2}, {}, {&capture, admit_relationships});
        check(admitted.error == RelationshipsDecodeError::capacity && capture.candidate_calls == 1 &&
            capture.candidate_count == 2 && !capture.active_calls && staged[0].user_id == 99 &&
            admitted_store.count == 1 && admitted_store.revision == previous_revision && active[0].user_id == 77,
            "friends admission: rejected exact candidate prefix is requested before any candidate write and preserves active storage");
        capture = {}; capture.reject_active = true;
        admitted = relationships_replace(&admitted_store, input, {scratch, 1024}, {staged, 2}, {}, {&capture, admit_relationships});
        check(admitted.error == RelationshipsDecodeError::capacity && capture.candidate_count == 2 &&
            capture.active_calls == 1 && capture.active_count == 2 && staged[0].user_id == 1 &&
            admitted_store.count == 1 && admitted_store.revision == previous_revision && active[0].user_id == 77,
            "friends admission: destination refusal after sorted validation leaves active records, count and revision unchanged");
        const Utf8View malformed_snapshots[]{utf8_literal(R"([{"id":"1","type":1},{"id":"1","type":2}])"),
                utf8_literal(R"([{"id":"1","type":1},{"id":"2","type":99}])"),
                utf8_literal(R"([{"id":"1","type":1},])")};
        for (const auto malformed : malformed_snapshots) {
            capture = {};
            check(!relationships_replace(&admitted_store, malformed, {scratch, 1024}, {staged, 2}, {},
                    {&capture, admit_relationships}).ok() && !capture.active_calls && active[0].user_id == 77 &&
                admitted_store.count == 1 && admitted_store.revision == previous_revision,
                "friends admission: malformed and duplicate snapshots never admit or alter the active destination");
        }
        capture = {};
        check(relationships_replace(&admitted_store, input, {scratch, 1024}, {staged, 2}, {},
                {&capture, admit_relationships}).ok() && admitted_store.count == 2 && active[0].user_id == 1 &&
            capture.candidate_count == 2 && capture.active_count == 2,
            "friends admission: exact capacity snapshot admits only its two prefixes and commits sorted records");
        capture = {};
        check(relationships_replace(&admitted_store, utf8_literal("[]"), {scratch, 1024}, {staged, 2}, {},
                {&capture, admit_relationships}).ok() && !admitted_store.count && !active[0].user_id && !active[1].user_id &&
            capture.candidate_count == 0 && capture.active_count == 2,
            "friends admission: empty replacement admits no candidate rows and retains the previous active tail for clearing");
        capture = {};
        check(relationships_replace(&admitted_store, utf8_literal("[]"), {scratch, 1024}, {staged, 2}, {},
                {&capture, admit_relationships}).ok() && capture.candidate_calls == 1 && capture.active_calls == 1 &&
            !capture.candidate_count && !capture.active_count,
            "friends admission: already empty snapshot requests zero prefixes and needs no committed row capacity");
    }
    UserRecord small_users[2]{}; u32 small_order[2]{}; u8 small_names[4]{}; UserStore small{};
    user_store_initialize(&small, {small_users, 2}, {small_order, 2}, {small_names, 4});
    check(!relationship_users_apply(&small, {snapshots, decoded.user_count}).ok() &&
        relationship_users_apply(&small, {snapshots, decoded.user_count}).applied == 0,
        "friends: bounded user-name hydration reports explicit failure rather than dropping relationship rows");

    u8 output[128]{};
    const auto target = write_relationship_target(RelationshipOperation::accept, ~u64{0}, {output, 128});
    check(target.ok() && utf8_equal({reinterpret_cast<const char*>(output), target.bytes},
        utf8_literal("/api/v10/users/@me/relationships/18446744073709551615")) &&
        relationship_method(RelationshipOperation::accept) == RestMethod::put &&
        relationship_method(RelationshipOperation::remove) == RestMethod::delete_method,
        "friends: ID routes preserve full snowflakes and normalized confirmed PUT/DELETE methods");
    auto written = write_relationship_body(RelationshipOperation::request, utf8_literal("some.user"), {output, 128});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(output), written.bytes},
        utf8_literal("{\"username\":\"some.user\",\"discriminator\":null}")), "friends: modern username request uses current-client POST body fields");
    written = write_relationship_body(RelationshipOperation::accept, {}, {output, 128});
    check(written.ok() && written.bytes == 2 && bytes_equal(output, "{}", 2), "friends: accepting uses minimal type-omitted PUT body");
    const auto stranger_target = write_relationship_target(RelationshipOperation::accept_stranger, 42, {output, 128});
    check(stranger_target.ok() && utf8_equal({reinterpret_cast<const char*>(output), stranger_target.bytes},
        utf8_literal("/api/v10/users/@me/relationships/42")) &&
        relationship_method(RelationshipOperation::accept_stranger) == RestMethod::put &&
        static_cast<u8>(RelationshipOperation::block) == 4 && static_cast<u8>(RelationshipOperation::accept_stranger) == 5,
        "friends: explicit stranger consent appends operation without changing existing wire enum values or user-ID route");
    written = write_relationship_body(RelationshipOperation::accept_stranger, {}, {output, 128});
    check(written.ok() && written.bytes == 33 && bytes_equal(output, "{\"confirm_stranger_request\":true}", written.bytes),
        "friends: confirmed stranger accept uses observed first-party consent body without inventing relationship type");
    written = write_relationship_body(RelationshipOperation::block, {}, {output, 128});
    check(written.ok() && bytes_equal(output, "{\"type\":2}", written.bytes), "friends: block uses the verified relationship type2");
    check(write_relationship_body(RelationshipOperation::remove, {}, {}).ok() &&
        write_relationship_target(RelationshipOperation::list, 1, {output, 128}).error == RelationshipWriteError::invalid_input &&
        write_relationship_body(RelationshipOperation::request, utf8_literal("quote\"name"), {output, 128}).error == RelationshipWriteError::invalid_input &&
        write_relationship_body(RelationshipOperation::block, utf8_literal("extra"), {output, 128}).error == RelationshipWriteError::invalid_input,
        "friends: bodyless removal, ID scope and bounded username validation prevent guessed or malformed requests");
    for (auto& byte : output) byte = 0xCC;
    check(write_relationship_body(RelationshipOperation::block, {}, {output, 1}).error == RelationshipWriteError::output_too_small &&
        output[0] == 0xCC, "friends: failed request writes are atomic even at exact buffer capacity boundaries");
    const auto invalid_operation = static_cast<RelationshipOperation>(10);
    check(write_relationship_target(RelationshipOperation::accept_stranger, 0, {output, 128}).error == RelationshipWriteError::invalid_input &&
        write_relationship_body(RelationshipOperation::accept_stranger, utf8_literal("extra"), {output, 128}).error == RelationshipWriteError::invalid_input &&
        write_relationship_target(invalid_operation, 42, {output, 128}).error == RelationshipWriteError::invalid_input &&
        write_relationship_body(invalid_operation, {}, {output, 128}).error == RelationshipWriteError::invalid_input &&
        write_relationship_body(RelationshipOperation::accept_stranger, {}, {output, 32}).error == RelationshipWriteError::output_too_small &&
        output[0] == 0xCC && output[32] == 0xCC,
        "friends: consent writer rejects missing identity, username scope, unknown operation and short buffer atomically");
    written = write_relationship_body(RelationshipOperation::accept_stranger, {}, {output, 33});
    check(written.ok() && written.bytes == 33 && output[33] == 0xCC,
        "friends: consent body succeeds at exact capacity without writing a terminator or trailing storage");
    check(static_cast<u8>(RelationshipOperation::ignore) == 6 && static_cast<u8>(RelationshipOperation::unignore) == 7 &&
        static_cast<u8>(RelationshipOperation::clear_incoming) == 8 && static_cast<u8>(RelationshipOperation::clear_hidden) == 9,
        "friends: ignore and bulk operation values append after existing stranger consent");
    const RelationshipOperation ignores[]{RelationshipOperation::ignore, RelationshipOperation::unignore};
    for (const auto operation : ignores) {
        const auto ignore_target = write_relationship_target(operation, ~u64{0}, {output, 128});
        check(ignore_target.ok() && utf8_equal({reinterpret_cast<const char*>(output), ignore_target.bytes},
                utf8_literal("/api/v10/users/@me/relationships/18446744073709551615/ignore")) &&
            relationship_method(operation) == (operation == RelationshipOperation::ignore ? RestMethod::put : RestMethod::delete_method) &&
            write_relationship_body(operation, {}, {}).ok() &&
            write_relationship_target(operation, 0, {output, 128}).error == RelationshipWriteError::invalid_input,
            "friends: ignore and unignore use observed individual ignore route with bodyless PUT and DELETE");
    }
    const RelationshipOperation clears[]{RelationshipOperation::clear_incoming, RelationshipOperation::clear_hidden};
    for (const auto operation : clears) {
        const auto clear_target = write_relationship_target(operation, 0, {output, 128});
        check(clear_target.ok() && utf8_equal({reinterpret_cast<const char*>(output), clear_target.bytes},
                utf8_literal("/api/v10/users/@me/relationships?relationship_type=3")) &&
            relationship_method(operation) == RestMethod::delete_method &&
            write_relationship_target(operation, 1, {output, 128}).error == RelationshipWriteError::invalid_input,
            "friends: clear actions use collection DELETE constrained to incoming relationship type");
    }
    check(write_relationship_body(RelationshipOperation::clear_incoming, {}, {}).ok(),
        "friends: clearing all incoming requests has no JSON body");
    written = write_relationship_body(RelationshipOperation::clear_hidden, {}, {output, 17});
    check(written.ok() && written.bytes == 17 && bytes_equal(output, "{\"filters\":[1,2]}", 17),
        "friends: hidden clear sends observed spam and ignored filter enum values in exact JSON body");
    for (auto& byte : output) byte = 0xCC;
    check(write_relationship_body(RelationshipOperation::clear_hidden, {}, {output, 16}).error == RelationshipWriteError::output_too_small &&
        output[0] == 0xCC && output[16] == 0xCC &&
        write_relationship_target(RelationshipOperation::clear_hidden, 0, {output, 1}).error == RelationshipWriteError::output_too_small &&
        output[0] == 0xCC &&
        write_relationship_body(RelationshipOperation::ignore, utf8_literal("extra"), {output, 128}).error == RelationshipWriteError::invalid_input,
        "friends: hidden filters and new operation scope failures preserve output atomically");

    Capture stream{};
    relationship_store_initialize(&stream.active, {stream.active_rows, 8});
    relationship_store_initialize(&stream.pending, {stream.pending_rows, 8});
    record = {}; record.user_id = 99; record.type = RelationshipType::friend_;
    relationship_store_upsert(&stream.active, record);
    const auto merged = utf8_literal(R"({"friends":[null,{"user_id":"20","status":"idle"}]})");
    GatewayReady ready{};
    const auto ready_json = utf8_literal(R"({"v":9,"user":{"id":"1"},"guilds":[],"session_id":"fixture","resume_gateway_url":"wss://gateway.discord.gg","relationships":[{"id":"20","type":1}],"merged_presences":{"friends":[{"user_id":"20","status":"online"}]}})");
    check(gateway_decode_ready({ready_json, 0, JsonValueKind::object}, {scratch, 1024}, 64, &ready).ok() &&
        ready.relationships.kind == JsonValueKind::array && ready.merged_presences.kind == JsonValueKind::object &&
        gateway_client_publish_relationship_snapshot(ready.relationships, ready.merged_presences,
            {scratch, 1024}, {object_scratch, 512}, 9, 100, capture, &stream).ok() && stream.active.count == 1 &&
        relationship_store_find(stream.active, 20)->presence == RelationshipPresence::online,
        "friends Gateway: real READY decoder retains relationship and presence fields for atomic typed projection");
    const Utf8View bad_ready[]{
        utf8_literal(R"({"v":9,"user":{"id":"1"},"guilds":[],"session_id":"fixture","resume_gateway_url":"wss://gateway.discord.gg","relationships":[],"relationships":[]})"),
        utf8_literal(R"({"v":9,"user":{"id":"1"},"guilds":[],"session_id":"fixture","resume_gateway_url":"wss://gateway.discord.gg","merged_presences":[]})")};
    for (const auto input : bad_ready) check(!gateway_decode_ready({input, 0, JsonValueKind::object}, {scratch, 1024}, 64, &ready).ok(),
        "friends Gateway: READY rejects duplicate relationship fields and malformed merged presence shape");
    check(gateway_client_publish_relationship_snapshot({array, 0, JsonValueKind::array}, {merged, 0, JsonValueKind::object},
        {scratch, 1024}, {object_scratch, 512}, 10, 100, capture, &stream).ok() && stream.completed && stream.active.count == 3 &&
        relationship_store_find(stream.active, 20)->presence == RelationshipPresence::idle,
        "friends Gateway: pointer-free READY stream commits only after complete relationships and compressed initial presence");
    const auto supplemental_json = utf8_literal(R"({"merged_presences":{"friends":[{"user":{"id":"20"},"status":"dnd"}]}})");
    JsonValue supplemental_merged{};
    const u32 supplemental_count = stream.active.count;
    check(gateway_relationship_merged_presences({supplemental_json, 0, JsonValueKind::object}, {scratch, 1024}, &supplemental_merged).ok() &&
        gateway_client_publish_friend_presences(supplemental_merged, {scratch, 1024}, {object_scratch, 512}, 10, 101, capture, &stream).ok() &&
        !stream.staging && stream.completed && stream.active.count == supplemental_count &&
        relationship_store_find(stream.active, 20)->presence == RelationshipPresence::dnd,
        "friends Gateway: supplemental nested-user presence changes active status without resetting relationships");
    check(gateway_client_publish_friend_presences(supplemental_merged, {scratch, 1024}, {scratch, 1024}, 10, 101, capture, &stream).error ==
        GatewayClientDispatchRouteError::invalid_input,
        "friends Gateway: supplemental stream rejects aliased structural scratch before publication");
    const u32 stream_revision = stream.active.revision;
    stream.fail_after = 3; stream.calls = 0;
    check(!gateway_client_publish_relationship_snapshot({array, 0, JsonValueKind::array}, {}, {scratch, 1024}, {object_scratch, 512},
        11, 100, capture, &stream).ok() && !stream.completed && stream.active.revision == stream_revision && stream.active.count == 3,
        "friends Gateway: publisher failure after a staged item cannot replace the last complete snapshot");
    stream.fail_after = 0;
    check(!gateway_client_publish_relationship_snapshot({bad[1], 0, JsonValueKind::array}, {}, {scratch, 1024}, {object_scratch, 512},
        12, 100, capture, &stream).ok() && !stream.completed && stream.active.revision == stream_revision,
        "friends Gateway: late invalid relationship prevents the completion marker and preserves active state");
    GatewayEnvelope envelope{};
    const auto live = utf8_literal(R"({"id":"55","type":3,"user":{"id":"55","username":"requester"}})");
    envelope.event_name = {utf8_literal("RELATIONSHIP_ADD"), 0, false}; envelope.data = {live, 0, JsonValueKind::object};
    envelope.has_event_name = true; envelope.has_sequence = true; envelope.sequence = 15;
    char payload_storage[512]{}; GatewayClientEvent routed{}; ConstSlice<u8> payload{};
    check(gateway_client_route_dispatch(envelope, {scratch, 1024}, 64, {payload_storage, 512}, {}, 1, &routed, &payload).ok() &&
        routed.dispatch_kind == GatewayClientDispatchKind::relationship_add && routed.state.record_count == 1 && payload.count == 456,
        "friends Gateway: live first-party relationship event routes to bounded typed owned payload");
    RelationshipMutation mutation{}; u32 cursor = 0;
    check(gateway_client_relationship_record(payload, &cursor, &mutation) && cursor == 456 && mutation.record.user_id == 55 &&
        mutation.record.type == RelationshipType::incoming && mutation.user.username_bytes == 9 && !mutation.record.stranger_known,
        "friends Gateway: typed payload keeps user identity and owned username after JSON buffer reuse");
    {
        auto known = mutation;
        known.record.stranger_known = true;
        u8 raw[sizeof(known)]{};
        copy_bytes(raw, &known, sizeof(known));
        u32 known_cursor = 0;
        RelationshipMutation copied{};
        check(sizeof(RelationshipRecord) == 144 && sizeof(known) == 456 && raw[12] == 0 && raw[13] == 1 &&
            gateway_client_relationship_record({raw, sizeof(raw)}, &known_cursor, &copied) && known_cursor == 456 &&
            !copied.record.stranger && copied.record.stranger_known && copied.record.user_id == 55 &&
            copied.user.id == 55 && copied.user.username_bytes == 9,
            "friends Gateway: known false occupies former padding byte13 while record, user and mutation offsets stay unchanged");
        raw[12] = 1; known_cursor = 0;
        check(gateway_client_relationship_record({raw, sizeof(raw)}, &known_cursor, &copied) && copied.record.stranger && copied.record.stranger_known,
            "friends Gateway: exact raw known true metadata survives the typed mailbox boundary");
        const auto accepted_known = copied;
        raw[13] = 2; known_cursor = 0;
        check(!gateway_client_relationship_record({raw, sizeof(raw)}, &known_cursor, &copied) && !known_cursor &&
            bytes_equal(&copied, &accepted_known, sizeof(copied)),
            "friends Gateway: malformed raw known boolean is rejected before materialization without cursor or output mutation");
    }
    cursor = 0;
    check(!gateway_client_relationship_record({payload.data, 455}, &cursor, &mutation) && !cursor,
        "friends Gateway: truncated typed payload never advances ownership cursor");
    auto invalid_mutation = mutation;
    invalid_mutation.user.avatar_bytes = 1;invalid_mutation.user.avatar[0] = 'z';
    check(!gateway_client_relationship_record({reinterpret_cast<const u8*>(&invalid_mutation), sizeof(invalid_mutation)}, &cursor, &mutation) && !cursor,
        "friends Gateway: malformed owned avatar metadata cannot cross the typed consumer boundary");
    test_backpressure(check);
}
} // namespace maicord
