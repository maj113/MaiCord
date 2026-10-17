#include "maicord/model/member_list.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/model/guild_member_store.hpp"

namespace maicord {
namespace sparse_probe {
struct Fixture {
    GuildMemberStore members;
    GuildMemberRecord member_records[512];
    u32 member_order[512];
    UserStore users;
    UserRecord user_records[512];
    u32 user_order[512];
    u8 user_names[65536];
    ChatStateStore state;
    GuildRecord guilds[2];
    ChannelRecord channels[2];
    u16 guild_order[2], channel_order[2];
    u8 state_names[4096];
    MemberListView projection;
    MemberListRow rows[768];
    u32 scratch[512];
};
Fixture fixture{};
char json[128 * 1024], packet[128 * 1024];
u32 structurals[32768];
bool initialize() noexcept {
    auto& f = fixture;
    return guild_member_store_initialize(&f.members, {f.member_records, 512}, {f.member_order, 512}) &&
        user_store_initialize(&f.users, {f.user_records, 512}, {f.user_order, 512}, {f.user_names, sizeof(f.user_names)}) &&
        chat_state_store_initialize(&f.state, {f.guilds, 2}, {f.guild_order, 2}, {f.channels, 2}, {f.channel_order, 2}, {f.state_names, sizeof(f.state_names)}) &&
        member_list_view_initialize(&f.projection, {f.rows, 768}, {f.scratch, 512});
}
bool member(u64 id, u32 index, u64 list = 70) noexcept {
    auto& f = fixture;
    return user_store_upsert(&f.users, id, id == 1 ? utf8_literal("Zebra") : utf8_literal("Alpha")).ok() &&
        guild_member_store_upsert(&f.members, f.state, 7, id, {}, {}, false,
            GuildMemberStatus::online, index, list).ok();
}
bool project(u64 list = 70) noexcept {
    auto& f = fixture;
    return member_list_view_rebuild(&f.projection, f.members, f.users, f.state, 7, list).ok();
}
u64 user_at(u32 index) noexcept {
    const auto* row = member_list_view_row(fixture.projection, index);
    return row ? row->user_id : 0;
}
u32 group_count_at(u32 index) noexcept {
    const auto* row = member_list_view_row(fixture.projection, index);
    return row ? row->group_count : 0;
}
u64 list_user(u64 list, u32 index) noexcept {
    u64 user = 0;
    return guild_member_store_list_users(fixture.members, 7, list, index, index, {&user, 1}) ? user : 0;
}
u32 list_position_count(u64 list) noexcept {
    static u64 users[kMemberListPositionCapacity];
    return guild_member_store_list_users(fixture.members, 7, list, 0, ~0u, {users, kMemberListPositionCapacity});
}
bool route(Utf8View source, GatewayClientEvent* event, ConstSlice<u8>* payload) noexcept {
    GatewayEnvelope envelope{};
    bool handled = false;
    if (!gateway_decode_envelope(source, {structurals, 32768}, 64, &envelope).ok() ||
        !gateway_client_route_member_list_dispatch(envelope, {structurals, 32768}, 64, {packet, sizeof(packet)}, event, payload, &handled).ok() || !handled) return false;
    event->kind = GatewayClientEventKind::dispatch;
    event->dispatch_payload_bytes = payload->count;
    return true;
}
}
void test_member_list_sparse_model(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace sparse_probe;
    auto& f = fixture;
    check(initialize(), "sparse member model fixtures initialize");
    check(member(1, 300001) && member(2, 300000) && project(), "legacy member projection still builds");
    check(!f.projection.authoritative && f.projection.count == 3 && f.rows[1].user_id == 2,
        "legacy projection keeps client sorted dense behavior until a wire list is observed");
    check(guild_member_store_observe_list(&f.members, 7, 70, 900000, 400000, true, true, true, 2, 400002) &&
        guild_member_store_set_group(&f.members, 7, 70, 0, kMemberListOnlineGroup, 400000) &&
        guild_member_store_group_summary(&f.members, 7, 70, 0, kMemberListOnlineGroup, 400000),
        "authoritative list retains supplied groups and independent guild counters");
    guild_member_store_finish_groups(&f.members, 7, 70);
    check(project() && f.projection.authoritative && f.projection.count == 400002 && f.projection.member_count == 900000 &&
        f.projection.resident_count == 3 && !member_list_view_row(f.projection, 300099),
        "huge wire extent uses only compact resident rows and holes");
    const auto* first = member_list_view_row(f.projection, 300000);
    const auto* second = member_list_view_row(f.projection, 300001);
    const auto* header = member_list_view_row(f.projection, 0);
    check(first && second && first->user_id == 2 && second->user_id == 1 && header && header->group_count == 400000,
        "wire positions and authoritative header counts survive name sorting pressure");
    check(guild_member_store_observe_list(&f.members, 7, 70, 0, 0, false, false, false, 0, 0) &&
        guild_member_store_shift_list(&f.members, 7, 70, 300000, true) &&
        guild_member_store_set_group(&f.members, 7, 70, 300000, 1234, 2) && project(),
        "inserting a group shifts resident members at their wire positions");
    first = member_list_view_row(f.projection, 300001);
    second = member_list_view_row(f.projection, 300002);
    check(f.projection.count == 400003 && first && second && first->user_id == 2 && second->user_id == 1,
        "insert without a new group snapshot adjusts the previous authoritative logical length");
    check(guild_member_store_invalidate_list_range(&f.members, 7, 300000, 300000, 70) &&
        guild_member_store_shift_list(&f.members, 7, 70, 300000, false) && project() &&
        user_at(300000) == 2,
        "delete shifts later rows even when a header row was withdrawn");
    check(guild_member_store_invalidate_list_range(&f.members, 7, 299999, 300000, 70) && project() &&
        !member_list_view_row(f.projection, 300000) && user_at(300001) == 1 &&
        guild_member_store_find(f.members, 7, 2),
        "invalidate clears a range across holes without shifting or discarding cached identity");
    check(member(3, 300001) && !member_list_view_row(f.projection, 300001) && project() &&
        user_at(300001) == 3 &&
        guild_member_store_find(f.members, 7, 1)->list_index == ~0u,
        "update replaces the position and stale projections cannot resolve recycled or replaced slots");
    check(guild_member_store_observe_list(&f.members, 7, 71, 3, 3, true, true, false, 0, 0) &&
        member(4, 1, 71) && guild_member_store_invalidate_list_range(&f.members, 7, 0, 400002, 71) && project() &&
        user_at(300001) == 3,
        "other visibility sets invalidate only their exact list scope");

    check(initialize() && guild_member_store_observe_list(&f.members, 7, 70, 100000, 100000, true, true, true, 1, 100001),
        "pin pressure fixture initializes");
    bool filled = true;
    for (u32 i = 0; i < 512; ++i) filled &= member(i + 1, 50000 + i);
    check(filled && guild_member_store_pin_range(&f.members, 7, 70, 50000, 50499),
        "five requested pages fit inside the retained member cache");
    for (u32 i = 0; i < 40; ++i) filled &= member(1000 + i, i, 71);
    bool pinned_resident = true;
    for (u32 i = 0; i < 500; ++i) {
        const auto* cached = guild_member_store_find(f.members, 7, i + 1);
        pinned_resident &= cached && cached->list_id == 70 && cached->list_index == 50000 + i;
    }
    check(filled && pinned_resident && f.members.count == 512,
        "late other-list and unrelated cache admissions cannot evict five pinned wire pages");
    check(member(1, 2, 71) && guild_member_store_find(f.members, 7, 1)->list_id == 70 &&
        guild_member_store_find(f.members, 7, 1)->list_index == 50000,
        "late inactive-list updates preserve the active pinned identity position");
    guild_member_store_clear_pins(&f.members);
    check(guild_member_store_pin_range(&f.members, 7, 70, 60000, 60099), "new viewport interest replaces pins");
    for (u32 i = 0; i < 100; ++i) filled &= member(2000 + i, 60000 + i);
    bool new_page = true;
    for (u32 i = 0; i < 100; ++i) {
        const auto* cached = guild_member_store_find(f.members, 7, 2000 + i);
        new_page &= cached && cached->list_index == 60000 + i;
    }
    check(filled && new_page && f.members.count == 512, "unpinning old pages lets distant viewport members enter the same bounded cache");
    for (u32 i = 0; i < 12; ++i)
        filled &= guild_member_store_observe_list(&f.members, 8 + i, 80 + i, 1, 1, true, true, false, 0, 0);
    check(filled && guild_member_store_list(f.members, 7, 70), "metadata scope eviction preserves the pinned active visibility list");

    check(initialize() && guild_member_store_observe_list(&f.members, 7, 70, 600, 600, true, true, true, 300, 900),
        "more root groups than resident header capacity remain valid");
    for (u32 i = 0; i < 300; ++i) filled &= guild_member_store_set_group(&f.members, 7, 70, i * 3, 1000 + i, 2);
    check(member(777, 0), "unretained header collision fixture caches an old member at a header position");
    for (u32 i = 0; i < 300; ++i) filled &= guild_member_store_group_summary(&f.members, 7, 70, i * 3, 1000 + i, 2);
    guild_member_store_finish_groups(&f.members, 7, 70);
    check(filled && project() && f.projection.count == 900 && f.projection.resident_count == 256 &&
        !member_list_view_row(f.projection, 0) && group_count_at(897) == 2,
        "shared header pool evicts excess residency to holes without truncating logical extent");
    const auto* collided_member = guild_member_store_find(f.members, 7, 777);
    check(collided_member && collided_member->list_index == ~0u && !member_list_view_row(f.projection, 0),
        "authoritative summary withdraws a member collision even when its header is outside resident capacity");
    const u64 retained_roles[] = {17};
    check(member(9999, 899) && guild_member_store_upsert(&f.members, f.state, 7, 9999,
        utf8_literal("Retained"), {retained_roles, 1}, true, GuildMemberStatus::idle, 899, 70).ok() &&
        guild_member_store_pin_range(&f.members, 7, 70, 800, 899),
        "fresh ready reset fixture retains a pinned guild identity");
    const auto* identity = guild_member_store_find(f.members, 7, 9999);
    const u32 identity_generation = identity ? identity->generation : 0;
    const u32 revision_before_reset = f.members.revision;
    const u32 identities_before_reset = f.members.count;
    guild_member_store_reset_lists(&f.members);
    identity = guild_member_store_find(f.members, 7, 9999);
    bool groups_cleared = true;
    for (const auto& group : f.members.groups) groups_cleared &= !group.resident;
    check(identity && identity->list_index == ~0u && identity->list_id == 0 &&
        identity->generation == identity_generation && identity->status == GuildMemberStatus::idle &&
        identity->nickname_bytes == 8 && bytes_equal(identity->nickname, "Retained", 8) && identity->roles_known && identity->role_count == 1 &&
        f.members.count == identities_before_reset && !f.members.pin_count && groups_cleared &&
        !guild_member_store_list(f.members, 7, 70) && f.members.revision > revision_before_reset &&
        project() && !f.projection.authoritative && !f.projection.count,
        "fresh ready withdraws list residency and pins without deleting guild identity or presence");
    const u32 reset_revision = f.members.revision;
    guild_member_store_reset_lists(&f.members);
    check(f.members.revision == reset_revision,
        "repeating an empty positional reset preserves the member revision");
    check(initialize() && guild_member_store_observe_list(&f.members, 7, 70, 300, 300, true, true, true, 300, 600),
        "protected shared header fixture initializes");
    bool protected_headers = true;
    for (u32 i = 0; i < 256; ++i) protected_headers &= guild_member_store_set_group(&f.members, 7, 70, i * 2, 4000 + i, 1);
    protected_headers &= guild_member_store_pin_range(&f.members, 7, 70, 0, 255) &&
        guild_member_store_pin_range(&f.members, 7, 70, 256, 511) &&
        guild_member_store_set_visible_range(&f.members, 7, 70, 0, 127) &&
        guild_member_store_observe_list(&f.members, 7, 71, 20, 20, true, true, true, 20, 40);
    for (u32 i = 0; i < 20; ++i) protected_headers &= guild_member_store_set_group(&f.members, 7, 71, i * 2, 8000 + i, 1);
    check(protected_headers && project() && f.projection.resident_count == 256 && group_count_at(0) == 1 && group_count_at(510) == 1,
        "inactive header admissions cannot evict a fully requested shared header pool");
    guild_member_store_clear_pins(&f.members);
    check(guild_member_store_pin_range(&f.members, 7, 70, 500, 599) &&
        guild_member_store_set_group(&f.members, 7, 70, 512, 9999, 1) && project() &&
        group_count_at(0) == 1 && group_count_at(126) == 1 && group_count_at(512) == 1,
        "new requested headers replace unpinned history while visible headers remain protected");
    check(sizeof(GuildMemberStore) < 8192 + sizeof(MemberListPositionTable) &&
              sizeof(MemberListPositionTable) <= kMemberListPositionCapacity * 16 + 8 && sizeof(MemberListRow) == 40,
        "list metadata, bounded per-list rows and sparse row records remain fixed storage");

    // Each member list keeps its own rows: a second list of the guild that
    // lists the same members (a channel switch) takes none from the first,
    // and the first shows its rows again at once.
    check(initialize() && guild_member_store_observe_list(&f.members, 7, 70, 3, 3, true, true, true, 0, 3) &&
              member(1, 0) && member(2, 1) && member(3, 2) &&
              guild_member_store_observe_list(&f.members, 7, 71, 2, 2, true, true, true, 0, 2) &&
              member(3, 0, 71) && member(1, 1, 71) && project(71) && f.projection.resident_count == 2 &&
              user_at(0) == 3 && user_at(1) == 1,
        "per-list rows: the second list presents its own rows");
    check(project(70) && f.projection.authoritative && f.projection.resident_count == 3 && user_at(0) == 1 &&
              user_at(1) == 2 && user_at(2) == 3,
        "per-list rows: returning to the first list shows every retained row");
    check(guild_member_store_shift_list(&f.members, 7, 71, 0, true) && member(4, 0, 71) && project(70) &&
              user_at(0) == 1 && user_at(2) == 3 && list_user(71, 1) == 3 && list_user(71, 2) == 1,
        "per-list rows: inserts and updates of one list never move another");
    check(guild_member_store_invalidate_list_range(&f.members, 7, 0, 1, 71) && project(70) &&
              f.projection.resident_count == 3 && !list_user(71, 0),
        "per-list rows: a withdrawn range of one list leaves the other intact");
    check(guild_member_store_remove(&f.members, 7, 2) && project(70) && f.projection.resident_count == 2 &&
              !list_user(70, 1),
        "per-list rows: a member leaving the guild leaves every list");
    const u64 kept = list_user(70, 2);
    u64 retained_rows[8]{};
    check(kept == 3 && guild_member_store_list_users(f.members, 7, 70, 0, 5, {retained_rows, 8}) == 2,
        "per-list rows: a list reports the users it holds in a range");

    // Bounded rows: a list keeps at most its page budget (rows on screen
    // first), and a full pool drops the least recently viewed other list.
    check(initialize() && guild_member_store_observe_list(&f.members, 7, 70, 1000, 1000, true, true, true, 0, 1000) &&
              guild_member_store_pin_range(&f.members, 7, 70, 0, 99),
        "position budget fixture initializes");
    bool budget = true;
    for (u32 i = 0; i < 600; ++i) budget &= member(10'000 + i, i);
    check(budget && list_position_count(70) == kMemberListPositionsPerList && list_user(70, 0) == 10'000 &&
              list_user(70, 599) == 10'599,
        "per-list budget keeps the pages on screen and the newest rows");
    for (u64 list = 71; list < 76; ++list) {
        budget &= guild_member_store_observe_list(&f.members, 7, list, 600, 600, true, true, true, 0, 600);
        for (u32 i = 0; i < 520; ++i) budget &= member(20'000 + (list - 71) * 1000 + i, i, list);
    }
    u32 held = 0;
    for (const auto& entry : f.members.positions.entries) held += entry.occupied ? 1u : 0u;
    check(budget && held == f.members.positions.count && held <= kMemberListPositionCapacity &&
              f.members.positions.evicted_lists >= 1 &&
              list_position_count(70) == kMemberListPositionsPerList &&
              list_position_count(75) == kMemberListPositionsPerList &&
              !guild_member_store_list(f.members, 7, 71),
        "full position pool drops the least recently viewed list and keeps the list on screen");
}
void test_member_list_visible_users(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace sparse_probe;
    auto& f = fixture;
    char display[128], login[128];
    for (u32 i = 0; i < 128; ++i) { display[i] = 'D'; login[i] = 'u'; }
    const Utf8View name{display, 128}, username{login, 128};
    check(initialize() && guild_member_store_observe_list(&f.members, 7, 70, 100000, 100000,
        true, true, true, 1, 100001), "visible profile name pressure fixture initializes");
    bool inserted = true;
    for (u32 i = 0; i < 64; ++i) {
        inserted &= user_store_upsert(&f.users, 100 + i, name, username).ok() &&
            user_store_set_avatar(&f.users, 100 + i, utf8_literal("0123456789abcdef0123456789abcdef")) &&
            guild_member_store_upsert(&f.members, f.state, 7, 100 + i, {}, {}, false,
                GuildMemberStatus::online, 50000 + i, 70).ok();
    }
    check(inserted && guild_member_store_set_visible_range(&f.members, 7, 70, 50000, 50063) &&
        guild_member_store_refresh_visible_users(f.members, &f.users) == 64,
        "only the admitted viewport pins global user profiles independently of requested member pages");
    for (u32 i = 0; i < 256; ++i) inserted &= user_store_upsert(&f.users, 1000 + i, name, username).ok();
    bool retained = true;
    for (u32 i = 0; i < 64; ++i) {
        const auto* record = user_store_find(f.users, 100 + i);
        retained &= record && record->pinned && record->name_bytes == 128 && record->username_bytes == 128 &&
            record->avatar_hash_bytes == 32 && bytes_equal(user_store_name(f.users, *record).data, display, 128) &&
            bytes_equal(user_store_username(f.users, *record).data, login, 128);
    }
    check(inserted && retained && f.users.evictions >= 192 && !user_store_find(f.users, 1000) && sizeof(UserRecord) == 64,
        "name-bank LRU evicts unprotected profiles while retaining all visible names usernames and avatars without record growth");
    const auto old_visible = f.members.visible;
    check(!guild_member_store_set_visible_range(&f.members, 7, 70, 0, 128) &&
        f.members.visible.first == old_visible.first && f.members.visible.last == old_visible.last,
        "visible-interest bounds reject more than128 rows before changing admitted interest");
    guild_member_store_clear_visible_range(&f.members);
    check(guild_member_store_refresh_visible_users(f.members, &f.users) == 0 &&
        !user_store_find(f.users, 100)->pinned,
        "clearing viewport interest releases global profile pins");

    UserStore tiny{};
    UserRecord records[3]{};
    u32 order[3]{};
    u8 backing[16]{};
    check(user_store_initialize(&tiny, {records, 3}, {order, 3}, {backing, sizeof(backing)}) &&
        user_store_upsert(&tiny, 1, utf8_literal("One"), utf8_literal("1")).ok() &&
        user_store_pin(&tiny, 1) && user_store_upsert(&tiny, 2, utf8_literal("Two"), utf8_literal("2")).ok(),
        "protected byte-bank exhaustion fixture initializes");
    const u32 count_before = tiny.count, evictions_before = tiny.evictions;
    const auto fail = user_store_upsert(&tiny, 3, utf8_literal("Large"));
    check(fail.error == UserStoreError::name_capacity && tiny.count == count_before && tiny.evictions == evictions_before &&
        user_store_find(tiny, 1) && user_store_find(tiny, 2) &&
        bytes_equal(user_store_name(tiny, *user_store_find(tiny, 1)).data, "One", 3),
        "insufficient room beside pinned names fails before deleting protected or unprotected rows");
    check(user_store_pin(&tiny, 2) && !user_store_upsert(&tiny, 1, utf8_literal("Longer")).ok() &&
        user_store_find(tiny, 1)->name_bytes == 3 && user_store_find(tiny, 2)->name_bytes == 3,
        "growing a pinned name cannot partially overwrite either protected identity");
    UserStore full{};
    UserRecord only[1]{};
    u32 one_order[1]{};
    u8 one_names[32]{};
    check(user_store_initialize(&full, {only, 1}, {one_order, 1}, {one_names, sizeof(one_names)}) &&
        user_store_upsert(&full, 7, utf8_literal("Kept")).ok() && user_store_pin(&full, 7) &&
        !user_store_upsert(&full, 8, utf8_literal("New")).ok() && full.count == 1 &&
        user_store_find(full, 7) && !user_store_find(full, 8),
        "record exhaustion never evicts a protected visible identity");
}
void test_member_list_sparse_wire(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace sparse_probe;
    GatewayClientEvent event{};
    ConstSlice<u8> payload{};
    const auto source = utf8_literal(R"({"op":0,"t":"GUILD_MEMBER_LIST_UPDATE","s":1,"d":{"guild_id":"7","id":"70","member_count":900000,"online_count":100000,"groups":[{"id":"online","count":100000},{"id":"offline","count":200000}],"ops":[{"op":"SYNC","range":[100001,100003],"items":[{"group":{"id":"offline","count":200000}},{"member":{"user":{"id":"1","username":"Zebra"}}},{"member":{"user":{"id":"2","username":"Alpha"}}}]}]}})");
    check(route(source, &event, &payload) && event.member_list.logical_count == 300002 && event.member_list.group_count == 2 &&
        event.member_list.member_count == 900000 && event.member_list.record_count == 6 && event.member_list.member_record_count == 2 && event.member_list.groups_known,
        "wire decoder preserves group rows and counts instead of deriving logical extent from guild counts");
    u32 cursor = 0, index = 0;
    bool valid = true;
    while (index < event.member_list.record_count) {
        GatewayClientMemberListMutation mutation{};
        ConstSlice<u8> roles{}; Utf8View nickname{}, display{}, avatar{};
        valid &= gateway_client_event_member_list_record(event, payload, &cursor, &index, &mutation, &roles, &nickname, &display, &avatar);
        if (!valid) break;
        if (index == 2) valid &= mutation.kind == GatewayClientMemberListItemKind::group && mutation.index == 100001 && mutation.user_id == ~0ull && mutation.range_end == 200000;
        if (index == 6) valid &= mutation.operation == GatewayClientMemberListOperation::group_summary && mutation.index == 100001;
    }
    check(valid && cursor == payload.count, "packed group and summary records retain exact wire positions");
    check(!route(utf8_literal(R"({"op":0,"t":"GUILD_MEMBER_LIST_UPDATE","s":1,"d":{"guild_id":"7","id":"70","groups":[{"id":"online","count":1},{"id":"online","count":2}],"ops":[]}})"), &event, &payload) &&
        !route(utf8_literal(R"({"op":0,"t":"GUILD_MEMBER_LIST_UPDATE","s":1,"d":{"guild_id":"7","id":"70","groups":[{"id":"online","count":4294967295}],"ops":[]}})"), &event, &payload) &&
        !route(utf8_literal(R"({"op":0,"t":"GUILD_MEMBER_LIST_UPDATE","s":1,"d":{"guild_id":"7","id":"70","ops":[{"op":"SYNC","range":[0,0],"items":[{"group":{"id":"online","count":1}},{"member":{"user":{"id":"1","username":"A"}}}]}]}})"), &event, &payload),
        "duplicate groups, overflowed extents, and sync items outside their range are rejected");
    ProtocolWriter writer{{reinterpret_cast<u8*>(json), sizeof(json)}, 0};
    protocol_writer_append_literal(&writer, "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"s\":2,\"d\":{\"guild_id\":\"7\",\"id\":\"70\",\"groups\":[");
    for (u32 i = 0; i < 300; ++i) {
        if (i) protocol_writer_append_byte(&writer, ',');
        protocol_writer_append_literal(&writer, "{\"id\":\"");
        protocol_writer_append_u64(&writer, 1000 + i);
        protocol_writer_append_literal(&writer, "\",\"count\":2}");
    }
    protocol_writer_append_literal(&writer, "],\"ops\":[]}}");
    check(route({json, writer.used}, &event, &payload) && event.member_list.group_count == 300 && event.member_list.logical_count == 900 && event.member_list.record_count == 300,
        "root group metadata streams beyond retained header capacity without a dense allocation");
    writer.used = 0;
    protocol_writer_append_literal(&writer, "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"s\":3,\"d\":{\"guild_id\":\"7\",\"id\":\"70\",\"member_count\":100000,\"groups\":[");
    for (u32 i = 0; i < 250; ++i) {
        if (i) protocol_writer_append_byte(&writer, ',');
        protocol_writer_append_literal(&writer, "{\"id\":\"");
        protocol_writer_append_u64(&writer, 1000 + i);
        protocol_writer_append_literal(&writer, "\",\"count\":");
        protocol_writer_append_u64(&writer, i ? 0 : 100000);
        protocol_writer_append_byte(&writer, '}');
    }
    protocol_writer_append_literal(&writer, "],\"ops\":[");
    for (u32 page = 0; page < 5; ++page) {
        if (page) protocol_writer_append_byte(&writer, ',');
        protocol_writer_append_literal(&writer, "{\"op\":\"SYNC\",\"range\":[");
        protocol_writer_append_u64(&writer, page * 100);
        protocol_writer_append_byte(&writer, ',');
        protocol_writer_append_u64(&writer, page * 100 + 99);
        protocol_writer_append_literal(&writer, "],\"items\":[");
        for (u32 item = 0; item < 100; ++item) {
            if (item) protocol_writer_append_byte(&writer, ',');
            const u32 wire_index = page * 100 + item;
            if (!wire_index) protocol_writer_append_literal(&writer, "{\"group\":{\"id\":\"1000\",\"count\":100000}}");
            else {
                protocol_writer_append_literal(&writer, "{\"member\":{\"user\":{\"id\":\"");
                protocol_writer_append_u64(&writer, 2000 + wire_index);
                protocol_writer_append_literal(&writer, "\",\"username\":\"E\"},\"roles\":[]}}");
            }
        }
        protocol_writer_append_literal(&writer, "]}");
    }
    protocol_writer_append_literal(&writer, "]}}");
    check(route({json, writer.used}, &event, &payload) && event.member_list.group_count == 250 &&
        event.member_list.logical_count == 100250 && event.member_list.record_count == 755 && event.member_list.member_record_count == 499,
        "five full wire pages and hundreds of group summaries exceed the former 512 packet bound safely");
    const auto group_source = [&](u32 group_limit, bool duplicate) noexcept {
        writer.used = 0;
        protocol_writer_append_literal(&writer, "{\"op\":0,\"t\":\"GUILD_MEMBER_LIST_UPDATE\",\"s\":4,\"d\":{\"guild_id\":\"7\",\"id\":\"70\",\"groups\":[");
        for (u32 group_index = 0; group_index < group_limit; ++group_index) {
            if (group_index) protocol_writer_append_byte(&writer, ',');
            protocol_writer_append_literal(&writer, "{\"id\":\"");
            protocol_writer_append_u64(&writer, duplicate && group_index + 1 == group_limit ? 1000 : 1000 + group_index);
            protocol_writer_append_literal(&writer, "\",\"count\":0}");
        }
        protocol_writer_append_literal(&writer, "],\"ops\":[]}}");
        return Utf8View{json, writer.used};
    };
    const auto rejects_duplicate_without_publish = [&](u32 group_limit) noexcept {
        const auto duplicate_source = group_source(group_limit, true);
        GatewayEnvelope envelope{};
        GatewayClientEvent unpublished{};
        unpublished.dispatch_kind = GatewayClientDispatchKind::unknown;
        unpublished.member_list.guild_id = 123;
        unpublished.member_list.record_count = 456;
        const u8 sentinel = 1;
        ConstSlice<u8> unpublished_payload{&sentinel, 1};
        bool handled = false;
        if (!gateway_decode_envelope(duplicate_source, {structurals, 32768}, 64, &envelope).ok()) return false;
        const auto rejected = gateway_client_route_member_list_dispatch(envelope, {structurals, 32768}, 64,
            {packet, sizeof(packet)}, &unpublished, &unpublished_payload, &handled);
        return handled && rejected.error == GatewayClientDispatchRouteError::decode &&
            rejected.event_error == GatewayEventDecodeError::duplicate_field &&
            unpublished.dispatch_kind == GatewayClientDispatchKind::unknown &&
            unpublished.member_list.guild_id == 123 && unpublished.member_list.record_count == 456 &&
            unpublished_payload.data == &sentinel && unpublished_payload.count == 1;
    };
    check(rejects_duplicate_without_publish(300),
        "decoder rejects cross-chunk group duplicates before publishing event or payload");
    check(route(group_source(2048, false), &event, &payload) && event.member_list.group_count == 2048 &&
        event.member_list.record_count == 2048 && event.member_list.logical_count == 2048,
        "decoder validates the maximum bounded group packet with compact duplicate scratch");
    check(rejects_duplicate_without_publish(2048) && !route(group_source(2049, false), &event, &payload),
        "maximum-packet cross-chunk duplicates and excess records remain rejected");
}
}
