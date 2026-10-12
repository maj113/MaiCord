#include "maicord/app/gateway_thread_members.hpp"
#include "maicord/model/member_list.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace thread_probe {
struct Fixture {
    ThreadMemberStore threads;
    u64 banks[2][8];
    GuildMemberStore members;
    GuildMemberRecord member_records[8]; u32 member_order[8];
    UserStore users;
    UserRecord user_records[16]; u32 user_order[16]; u8 user_names[8192];
    ChatStateStore state;
    GuildRecord guilds[2]; u16 guild_order[2];
    ChannelRecord channels[2]; u16 channel_order[2]; u8 state_names[4096];
    MemberListView projection;
    MemberListRow rows[16]; u32 scratch[8];
};
Fixture fixture{};
u32 structurals[4096]; char packet[16384];
u64 full_banks[2][kThreadMemberMaximumIds];
struct ReserveProbe { u32 calls, required, fail_above; };
bool reserve_probe(void* context, u32 required) noexcept {
    auto& p = *static_cast<ReserveProbe*>(context); ++p.calls; p.required = required;
    return !p.fail_above || required <= p.fail_above;
}
bool initialize() noexcept {
    auto& f = fixture;
    return thread_member_store_initialize(&f.threads, {f.banks[0], 8}, {f.banks[1], 8}) &&
        thread_member_store_select(&f.threads, 7, 70) &&
        guild_member_store_initialize(&f.members, {f.member_records, 8}, {f.member_order, 8}) &&
        user_store_initialize(&f.users, {f.user_records, 16}, {f.user_order, 16}, {f.user_names, sizeof(f.user_names)}) &&
        chat_state_store_initialize(&f.state, {f.guilds, 2}, {f.guild_order, 2}, {f.channels, 2}, {f.channel_order, 2}, {f.state_names, sizeof(f.state_names)}) &&
        member_list_view_initialize(&f.projection, {f.rows, 16}, {f.scratch, 8});
}
bool decode(Utf8View source, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    GatewayEnvelope envelope{}; bool handled = false;
    if (!gateway_decode_envelope(source, {structurals, 4096}, 64, &envelope).ok()) return false;
    *event = {}; event->kind = GatewayClientEventKind::dispatch;
    return gateway_client_route_thread_member_dispatch(envelope, {structurals, 4096}, 64, {packet, sizeof(packet)}, event, payload, &handled).ok() && handled;
}
bool apply(GatewayClientEvent event, ConstSlice<u8> payload) noexcept {
    auto& f = fixture;
    return gateway_client_apply_thread_members(event, payload, &f.threads, &f.members, &f.users, f.state);
}
bool project() noexcept {
    auto& f = fixture;
    return member_list_view_thread(&f.projection, f.threads, f.members, f.users, f.state, 7, 70).ok();
}
}
void test_thread_members(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace thread_probe;
    auto& f = fixture; GatewayClientEvent event{}; ConstSlice<u8> payload{};
    check(initialize(), "thread fixtures initialize with independent identity banks");
    check(!thread_member_store_add(&f.threads, 1) && !f.threads.known, "delta does not establish a thread snapshot");
    check(decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"thread_id\":\"70\",\"members\":[{\"user_id\":\"1\"},{\"user_id\":\"2\",\"member\":{\"user\":{\"id\":\"2\",\"username\":\"Alpha\"},\"roles\":[],\"nick\":\"Zulu\"},\"presence\":{\"user\":{\"id\":\"2\"},\"status\":\"online\"}}]}}"), &event, &payload) && apply(event, payload), "thread snapshot preserves optional inline hydration and identities");
    check(f.threads.known && f.threads.count == 2 && thread_member_store_contains(f.threads, 1) && f.members.count == 1, "unknown thread identities do not consume profile records");
    check(project() && f.projection.member_count == 2 && f.projection.count == 2 && f.rows[1].user_id == 2, "unknown usernames are retained but skipped in thread projection");
    const u32 old_revision = f.threads.revision;
    check(thread_member_store_snapshot_begin(&f.threads, 7, 70) && thread_member_store_snapshot_append(&f.threads, 9) &&
        thread_member_store_contains(f.threads, 1) && f.threads.revision == old_revision, "staged snapshot is invisible until atomic commit");
    thread_member_store_snapshot_abort(&f.threads);
    check(decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBERS_UPDATE\",\"d\":{\"guild_id\":\"7\",\"id\":\"70\",\"member_count\":50,\"added_members\":[{\"user_id\":\"3\"},{\"user_id\":\"3\"}],\"removed_member_ids\":[\"1\",\"3\"]}}"), &event, &payload) && apply(event, payload) && f.threads.count == 1 && !thread_member_store_contains(f.threads, 3), "idempotent additions precede removals; approximate count creates no identities");
    check(!member_list_view_row(f.projection, 1), "thread identity revision invalidates retained rows immediately");
    const u32 users_revision = f.users.revision;
    check(!decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"thread_id\":\"70\",\"members\":[{\"user_id\":\"4\",\"member\":{\"user\":{\"id\":\"5\",\"username\":\"Wrong\"}}}]}}"), &event, &payload) && f.users.revision == users_revision && f.threads.count == 1, "outer and embedded user identity mismatch is rejected without state application");
    check(!decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"id\":\"70\",\"members\":[]}}"), &event, &payload), "snapshot requires thread_id and never uses normal member list identity");
    check(decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"thread_id\":\"71\",\"members\":[{\"user_id\":\"9\"}]}}"), &event, &payload) && apply(event, payload) && f.threads.count == 1, "late unowned snapshot is a validated no-op");
    check(decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"thread_id\":\"70\",\"members\":[]}}"), &event, &payload) && apply(event, payload) && f.threads.known && !f.threads.count && project() && !f.projection.count, "empty snapshot atomically establishes known empty membership");
    thread_member_store_snapshot_begin(&f.threads, 7, 70);
    for (u64 i = 1; i <= 9; ++i) thread_member_store_snapshot_append(&f.threads, i);
    check(thread_member_store_snapshot_commit(&f.threads) && f.threads.count == 8 && f.threads.incomplete, "identity overflow is bounded and explicitly incomplete");
    thread_member_store_remove(&f.threads, 1);
    check(f.threads.incomplete, "removing a retained identity cannot prove overflowed membership complete");
    const u32 retained = f.threads.count;
    thread_member_store_snapshot_begin(&f.threads, 7, 70);
    thread_member_store_snapshot_append(&f.threads, 99);
    check(!thread_member_store_snapshot_append(&f.threads, 0) &&
        !thread_member_store_snapshot_commit(&f.threads) && f.threads.count == retained,
        "invalid staged identity aborts replacement and preserves previous atomic snapshot");
    check(user_store_upsert(&f.users, 6, utf8_literal("Beta"), utf8_literal("Beta")).ok() &&
        guild_member_store_upsert(&f.members, f.state, 7, 6, {}, {}, false, GuildMemberStatus::offline).ok() &&
        user_store_upsert(&f.users, 99, utf8_literal("Unrelated"), utf8_literal("Unrelated")).ok() &&
        guild_member_store_upsert(&f.members, f.state, 7, 99, {}, {}, false, GuildMemberStatus::online).ok() && project(), "thread projection uses cached identities without guild positional membership");
    bool unrelated = false; for (u32 i = 0; i < f.projection.count; ++i) unrelated |= f.rows[i].user_id == 99;
    check(!unrelated && f.projection.incomplete, "thread projection cannot admit unrelated cached guild members");
    thread_member_store_select(&f.threads, 7, 71);
    check(!member_list_view_row(f.projection, 1), "scope generation invalidates retained row after thread selection");
    check(thread_member_store_select(&f.threads, 7, 70) &&
        decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"thread_id\":\"70\",\"members\":[{\"user_id\":\"2\"},{\"user_id\":\"6\"}]}}"), &event, &payload), "typed thread packet ready for whole-packet corruption test");
    const u32 before = f.threads.revision;
    if (payload.count >= 80) packet[40] = 0;
    check(!apply(event, payload) && f.threads.revision == before && !f.threads.known, "malformed later typed record cannot partially apply earlier membership");
    check(decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"thread_id\":\"70\",\"members\":[{\"user_id\":\"2\",\"member\":{\"user\":{\"id\":\"2\"}},\"presence\":null},{\"user_id\":\"4\"}],\"presences\":[{\"user\":{\"id\":\"4\",\"username\":\"Aaron\"},\"status\":\"idle\"}]}}"), &event, &payload) && apply(event, payload) && project(), "snapshot nullable inline presence and separate presence user hydration survive typed transport");
    const auto* preserved = guild_member_store_find(f.members, 7, 2);
    const auto* presence_cached = guild_member_store_find(f.members, 7, 4);
    check(preserved && preserved->roles_known && guild_member_store_nickname(*preserved).bytes == 4 &&
        user_store_find(f.users, 4) && presence_cached && presence_cached->status == GuildMemberStatus::idle &&
        !presence_cached->roles_known && f.threads.count == 2 && f.projection.count == 3,
        "absent roles and nickname preserve member data; independent presence hydration supplies a bounded partial profile");
    const u32 old_member_revision = f.members.revision;
    check(!decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"thread_id\":\"70\",\"members\":[{\"user_id\":\"2\",\"presence\":{\"user\":{\"id\":\"3\"},\"status\":\"online\"}}]}}"), &event, &payload) && f.members.revision == old_member_revision,
        "presence embedded user identity mismatch fails before appearance hydration");
    check(project(), "thread projection rebuilds before generation guard test");
    for (u32 i = 0; i < f.projection.count; ++i) {
        if (f.rows[i].user_id == 2 && f.rows[i].member_slot < f.members.capacity) {
            ++f.members.records[f.rows[i].member_slot].generation;
            check(!member_list_view_row(f.projection, i), "member slot generation prevents stale thread hit even without revision change");
        }
    }
    thread_member_store_reset_snapshot(&f.threads);
    check(!f.threads.known && thread_member_store_owns(f.threads, 7, 70), "fresh connection reset withdraws snapshot but preserves selected desired scope");
    check(user_store_upsert(&f.users, 50, utf8_literal("Named"), utf8_literal("Named")).ok() &&
        decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"thread_id\":\"70\",\"members\":[{\"user_id\":\"50\"}]}}"), &event, &payload) && apply(event, payload) && project() &&
        !guild_member_store_find(f.members, 7, 50) && f.projection.count == 2 && f.rows[1].member_slot == ~0u &&
        member_list_view_row(f.projection, 1), "cached username alone renders thread membership without fabricating a full guild profile");
    check(decode(utf8_literal("{\"op\":0,\"t\":\"THREAD_MEMBER_LIST_UPDATE\",\"d\":{\"guild_id\":\"7\",\"thread_id\":\"70\",\"members\":[{\"user_id\":\"2\"},{\"user_id\":\"50\"}]}}"), &event, &payload) && apply(event, payload), "two known thread usernames ready for compact projection capacity check");
    f.projection.member_scratch.count = 1;
    f.projection.source_thread_revision = 0;
    check(project() && f.projection.incomplete && f.projection.member_count == 2 && f.projection.count == 2,
        "compact projection capacity reports incomplete while independent identity store retains every member");
    ThreadMemberStore full{};
    check(thread_member_store_initialize(&full, {full_banks[0], kThreadMemberMaximumIds}, {full_banks[1], kThreadMemberMaximumIds}) &&
        thread_member_store_select(&full, 7, 70) && thread_member_store_snapshot_begin(&full, 7, 70),
        "maximum thread membership staging initializes without filling caller banks");
    bool reverse_ok = true;
    for (u64 id = kThreadMemberMaximumIds; id; --id) reverse_ok &= thread_member_store_snapshot_append(&full, id);
    check(reverse_ok && thread_member_store_snapshot_commit(&full) && full.count == kThreadMemberMaximumIds && !full.incomplete &&
        full.banks[full.active_bank][0] == 1 && full.banks[full.active_bank][kThreadMemberMaximumIds - 1] == kThreadMemberMaximumIds,
        "reverse maximum snapshot sorts once and retains every distinct identity");
    u64 tiny_banks[2][3]{}; ThreadMemberStore tiny{};
    check(thread_member_store_initialize(&tiny, {tiny_banks[0], 3}, {tiny_banks[1], 3}) &&
        thread_member_store_select(&tiny, 7, 70) && thread_member_store_snapshot_begin(&tiny, 7, 70),
        "tiny identity capacity ready for duplicate compaction");
    thread_member_store_snapshot_append(&tiny, 2);
    thread_member_store_snapshot_append(&tiny, 2);
    check(thread_member_store_snapshot_commit(&tiny) && tiny.count == 1 && !tiny.incomplete,
        "duplicates before staging fills compact at commit without false incompleteness");
    thread_member_store_snapshot_begin(&tiny, 7, 70);
    const u64 duplicate_sequence[] = {2, 2, 2, 1, 1, 3, 3, 2, 1, 3};
    bool duplicates_ok = true;
    for (const u64 id : duplicate_sequence) duplicates_ok &= thread_member_store_snapshot_append(&tiny, id);
    check(duplicates_ok && thread_member_store_snapshot_commit(&tiny) && tiny.count == 3 && !tiny.incomplete &&
        tiny.banks[tiny.active_bank][0] == 1 && tiny.banks[tiny.active_bank][2] == 3,
        "duplicates across full staging compact before true identity capacity is diagnosed");
    thread_member_store_snapshot_begin(&tiny, 7, 70);
    for (const u64 id : duplicate_sequence) thread_member_store_snapshot_append(&tiny, id);
    thread_member_store_snapshot_append(&tiny, 4);
    check(thread_member_store_snapshot_commit(&tiny) && tiny.count == 3 && tiny.incomplete,
        "distinct identity beyond compacted capacity marks snapshot incomplete");
    const u32 unchanged_count = tiny.count;
    ReserveProbe backing{0, 0, 2};
    check(thread_member_store_set_reserve(&tiny, reserve_probe, &backing) && !backing.calls && !tiny.backed_count &&
        thread_member_store_snapshot_begin(&tiny, 7, 70) && !backing.calls,
        "reserve callback is deferred until first bank write rather than scope selection");
    check(thread_member_store_snapshot_append(&tiny, 9) && thread_member_store_snapshot_append(&tiny, 8) &&
        !thread_member_store_snapshot_append(&tiny, 7) && !thread_member_store_snapshot_commit(&tiny) &&
        backing.calls == 3 && backing.required == 3 && tiny.count == unchanged_count && thread_member_store_contains(tiny, 1),
        "staging reserve failure aborts atomically and retains previous active identities");
    backing.fail_above = 0;
    thread_member_store_snapshot_begin(&tiny, 7, 70);
    thread_member_store_snapshot_append(&tiny, 9);
    thread_member_store_snapshot_append(&tiny, 8);
    check(thread_member_store_snapshot_commit(&tiny) && tiny.count == 2 && !tiny.incomplete && backing.calls == 3,
        "already backed highest staging indices do not request redundant reservation");
    backing.fail_above = 2;
    check(!thread_member_store_add(&tiny, 7) && tiny.count == 2 && tiny.incomplete && backing.required == 3,
        "delta reserve failure retains active identities and explicitly diagnoses incomplete membership");
    backing.fail_above = 0;
    check(thread_member_store_add(&tiny, 7) && tiny.count == 3 && backing.required == 3,
        "successful delta growth reserves both-bank backing before inserting new highest index");
    const u32 calls_before_zero = backing.calls;
    thread_member_store_snapshot_begin(&tiny, 7, 70);
    check(!thread_member_store_snapshot_append(&tiny, 0) && !thread_member_store_snapshot_commit(&tiny) &&
        backing.calls == calls_before_zero && tiny.count == 3, "zero staged identity aborts without requesting backing or changing active snapshot");
    ThreadMemberStore invalid{}; u64 overlapping[8]{};
    check(!thread_member_store_initialize(&invalid, {overlapping, 4}, {overlapping + 1, 4}) &&
        !thread_member_store_initialize(&invalid, {overlapping, 4}, {overlapping, 4}),
        "overlapping or identical identity banks cannot compromise atomic replacement");
    check(thread_member_store_select(&f.threads,7,70)&&
        thread_member_store_snapshot_begin(&f.threads,7,70)&&
        thread_member_store_snapshot_append(&f.threads,2)&&thread_member_store_snapshot_commit(&f.threads),
        "direct notification-tail participant fixture establishes original identity");
    GatewayEnvelope full_envelope{};GatewayClientEvent full_event{},ordinary_event{};ConstSlice<u8> full_payload{},ordinary_payload{};
    const auto full_delta=utf8_literal(R"({"op":0,"t":"THREAD_MEMBERS_UPDATE","d":{"guild_id":"7","id":"70","added_members":[{"user_id":"1","flags":2}],"removed_member_ids":["2"]}})");
    const bool full_routed=gateway_decode_envelope(full_delta,{structurals,4096},64,&full_envelope).ok()&&
        gateway_client_route_dispatch(full_envelope,{structurals,4096},64,{packet,sizeof(packet)},{},1,&full_event,&full_payload).ok()&&
        full_event.notification_payload_bytes&&gateway_client_notification_trim(full_event,full_payload,&ordinary_event,&ordinary_payload);
    check(full_routed&&apply(full_event,full_payload)&&f.threads.count==1&&thread_member_store_contains(f.threads,1)&&
        !thread_member_store_contains(f.threads,2),
        "direct thread participant admission preserves add-remove body while validating separate self policy tail");
    if(full_routed) {
        ThreadMemberStore ordinary_threads{};u64 ordinary_banks[2][8]{};
        const bool baseline=thread_member_store_initialize(&ordinary_threads,{ordinary_banks[0],8},{ordinary_banks[1],8})&&
            thread_member_store_select(&ordinary_threads,7,70)&&thread_member_store_snapshot_begin(&ordinary_threads,7,70)&&
            thread_member_store_snapshot_append(&ordinary_threads,2)&&thread_member_store_snapshot_commit(&ordinary_threads);
        check(baseline&&gateway_client_apply_thread_members(ordinary_event,ordinary_payload,&ordinary_threads,&f.members,&f.users,f.state)&&
            ordinary_threads.count==f.threads.count&&thread_member_store_contains(ordinary_threads,1)&&
            !thread_member_store_contains(ordinary_threads,2),
            "full routed thread and normalized ordinary participant event produce identical membership");
        GatewayClientNotificationSidecar metadata{};ConstSlice<u8> rows{};
        const bool copied=gateway_client_notification_sidecar({full_payload.data+ordinary_payload.count,full_event.notification_payload_bytes},&metadata,&rows);
        const u32 thread_revision=f.threads.revision,member_revision=f.members.revision,user_revision=f.users.revision;
        metadata.flags|=1u<<31;copy_bytes(packet+ordinary_payload.count,&metadata,sizeof(metadata));
        check(copied&&!apply(full_event,full_payload)&&f.threads.revision==thread_revision&&f.members.revision==member_revision&&f.users.revision==user_revision,
            "malformed notification tail cannot partially mutate direct thread participants or user profiles");
    }
}
} // namespace maicord
