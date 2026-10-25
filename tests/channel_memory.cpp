#include "maicord/app/channel_memory.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/guild_member_store.hpp"

namespace maicord {
namespace {
constexpr u32 kBlocked = kChannelObfuscatedFlag;

struct Fixture {
    ChatStateStore state;
    GuildRecord guilds[4];
    ChannelRecord channels[24];
    u16 guild_order[4], channel_order[24];
    u8 names[4096];
};

bool add_guild(Fixture& f, u64 id) noexcept {
    return chat_state_store_upsert_guild(&f.state, {id, utf8_literal("Guild"), GuildMutationKind::create, 1}).ok();
}
bool add(Fixture& f, u64 id, u64 guild, u8 type, u32 position, u64 parent = 0, u32 flags = 0) noexcept {
    return chat_state_store_upsert_channel(&f.state, {id, guild, parent, {}, position, flags, type}).ok();
}
// Guild 20: text 201 and 202, voice 203, thread 204 under 202. Guild 21: text 211 and 212. DMs 30 and 31.
bool populate(Fixture& f) noexcept {
    return add(f, 30, 0, 3, 0) && add(f, 31, 0, 3, 0) && add_guild(f, 20) && add_guild(f, 21) &&
           add(f, 201, 20, 0, 0) && add(f, 202, 20, 0, 1) && add(f, 203, 20, 2, 2) && add(f, 204, 20, 11, 0, 202) &&
           add(f, 211, 21, 0, 0) && add(f, 212, 21, 0, 1);
}
bool initialize(Fixture& f) noexcept {
    clear_bytes(&f, sizeof(f));
    const bool ok = chat_state_store_initialize(&f.state, {f.guilds, 4}, {f.guild_order, 4}, {f.channels, 24},
                                                {f.channel_order, 24}, {f.names, 4096});
    f.state.self_user_id = 1;
    return ok;
}
// What the host does on a guild click: recall, select, observe.
u64 visit_guild(Fixture& f, ChannelMemory& memory, u64 guild) noexcept {
    const ChannelMemoryEntry* found = channel_memory_find(memory, guild);
    chat_state_store_select_guild(&f.state, guild, found ? found->channel_id : 0, found ? found->parent_id : 0);
    channel_memory_observe(&memory, f.state);
    return f.state.selected_channel_id;
}
u64 visit_channel(Fixture& f, ChannelMemory& memory, u64 channel) noexcept {
    chat_state_store_select_channel(&f.state, channel);
    channel_memory_observe(&memory, f.state);
    return f.state.selected_channel_id;
}
bool temporary_path(wchar_t* path, u32 capacity) noexcept {
    const DWORD length = GetTempPathW(capacity, path);
    constexpr wchar_t name[] = L"maicord-channel-memory-test.bin";
    if (!length || length + sizeof(name) / sizeof(wchar_t) > capacity)
        return false;
    for (u32 i = 0; i < sizeof(name) / sizeof(wchar_t); ++i)
        path[length + i] = name[i];
    return true;
}
bool write_raw(const wchar_t* path, const void* bytes, u32 count) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool ok = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes, count, &written, nullptr);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    return ok;
}
} // namespace

void test_channel_memory(void (*check)(bool, const char*) noexcept) noexcept {
    ChannelMemory memory{};
    check(channel_memory_remember(&memory, 20, 201, 0) && channel_memory_remember(&memory, 21, 211, 0) &&
              memory.count == 2 && channel_memory_find(memory, 20)->channel_id == 201 &&
              channel_memory_latest(memory)->guild_id == 21 && !channel_memory_find(memory, 22),
          "channel memory remembers one channel per guild and leads with the latest");
    const u32 revision = memory.revision;
    check(!channel_memory_remember(&memory, 21, 211, 0) && memory.revision == revision &&
              !channel_memory_remember(&memory, 22, 0, 0) && !channel_memory_remember(&memory, 22, 5, 5) &&
              memory.count == 2,
          "an unchanged lead, a zero channel or a self-parented channel records nothing");
    check(channel_memory_remember(&memory, 20, 202, 0) && memory.count == 2 &&
              channel_memory_latest(memory)->channel_id == 202 && memory.entries[1].guild_id == 21,
          "a new channel replaces the guild's entry and moves it to the front");
    check(channel_memory_remember(&memory, 21, 211, 0) && memory.entries[0].guild_id == 21 &&
              memory.entries[1].guild_id == 20 && memory.revision != revision,
          "revisiting a guild with the same channel only refreshes its recency");

    ChannelMemory lru{};
    for (u64 guild = 1; guild <= kChannelMemoryCapacity; ++guild)
        channel_memory_remember(&lru, guild, guild * 10, 0);
    check(lru.count == kChannelMemoryCapacity && channel_memory_find(lru, 1) &&
              lru.entries[0].guild_id == kChannelMemoryCapacity,
          "channel memory fills to its fixed capacity");
    channel_memory_remember(&lru, 1, 10, 0); // Touch the oldest so the next oldest goes.
    channel_memory_remember(&lru, 1000, 5000, 0);
    check(lru.count == kChannelMemoryCapacity && channel_memory_find(lru, 1) && !channel_memory_find(lru, 2) &&
              channel_memory_find(lru, 3) && channel_memory_latest(lru)->guild_id == 1000 &&
              lru.entries[1].guild_id == 1,
          "a full table evicts the least recently used guild");

    Fixture f;
    ChannelMemory visits{};
    check(initialize(f) && populate(f), "channel memory fixture builds");
    check(visit_guild(f, visits, 20) == 201 && visit_channel(f, visits, 202) == 202 &&
              visit_guild(f, visits, 21) == 211 && visit_channel(f, visits, 212) == 212,
          "a guild without a memory lands on its default channel and visits are recorded");
    check(visit_guild(f, visits, 20) == 202 && visit_guild(f, visits, 21) == 212 && visit_guild(f, visits, 20) == 202,
          "returning to a guild restores its last channel");
    check(visit_channel(f, visits, 30) == 30 && channel_memory_find(visits, 0)->channel_id == 30 &&
              visit_channel(f, visits, 31) == 31 && chat_state_store_select_home(&f.state).ok() &&
              f.state.selected_channel_id == 31,
          "Home remembers its last conversation");
    check(visit_guild(f, visits, 20) == 202 && f.state.selected_guild_id == 20,
          "guild selection leaves Home's conversation intact");

    // Voice chat is remembered when chosen, but never becomes a guild's default.
    check(visit_channel(f, visits, 203) == 203 && visit_guild(f, visits, 21) == 212 && visit_guild(f, visits, 20) == 203,
          "an explicitly selected voice channel's chat is restored");
    // Threads restore, and fall back to the parent when they are no longer reachable.
    check(visit_channel(f, visits, 204) == 204 && channel_memory_find(visits, 20)->parent_id == 202 &&
              visit_guild(f, visits, 21) == 212 && visit_guild(f, visits, 20) == 204,
          "a thread is remembered with its parent and restored");
    check(add(f, 204, 20, 11, 0, 202, kBlocked) && visit_guild(f, visits, 21) == 212 &&
              visit_guild(f, visits, 20) == 202,
          "an unreachable thread falls back to its parent channel");
    check(chat_state_store_select_guild(&f.state, 20, 999).ok() && f.state.selected_channel_id == 201,
          "a deleted channel falls back to the guild's default channel");
    check(chat_state_store_select_guild(&f.state, 21, 999, 998).ok() && f.state.selected_channel_id == 211,
          "a missing thread and parent fall back to the default channel");
    check(chat_state_store_select_guild(&f.state, 21, 201).ok() && f.state.selected_channel_id == 211,
          "a remembered channel of another guild is ignored");
    check(add(f, 202, 20, 0, 1, 0, kBlocked) && chat_state_store_select_guild(&f.state, 20, 202).ok() &&
              f.state.selected_channel_id == 201,
          "an unreadable channel falls back to the default channel");
    check(chat_state_store_select_guild(&f.state, 21, 1).ok() && chat_state_store_select_guild(&f.state, 20, 204, 202).ok() &&
              f.state.selected_channel_id == 201,
          "an unreadable thread and parent fall back to the default channel");

    // Observation skips unchosen selections.
    Fixture pending;
    ChannelMemory observed{};
    check(initialize(pending) && !channel_memory_observe(&observed, pending.state) && observed.count == 0,
          "nothing is recorded without a selection");
    check(populate(pending) && pending.state.selected_channel_id != 0 &&
              (pending.state.restore_channel_id = 211, !channel_memory_observe(&observed, pending.state)) &&
              observed.count == 0,
          "nothing is recorded while a restoration is pending");

    // Persistence.
    u8 record[kChannelMemoryFileBytes];
    const u32 bytes = channel_memory_encode(visits, {record, sizeof(record)});
    ChannelMemory decoded{};
    bool same = bytes && channel_memory_decode({record, bytes}, &decoded) && decoded.count == visits.count;
    for (u32 i = 0; same && i < visits.count; ++i)
        same = decoded.entries[i].guild_id == visits.entries[i].guild_id &&
               decoded.entries[i].channel_id == visits.entries[i].channel_id &&
               decoded.entries[i].parent_id == visits.entries[i].parent_id;
    check(same && bytes == 10 + visits.count * kChannelMemoryEntryBytes, "channel memory round-trips in order");
    // Keep the on-disk byte order independent of the matching decoder.
    const u64 wire_ids[]{0x0123456789abcdefull,0xfedcba9876543210ull,0x1020304050607080ull};
    ChannelMemory wide{};channel_memory_remember(&wide,wire_ids[0],wire_ids[1],wire_ids[2]);
    u8 unaligned[36]{};unaligned[0]=0xa5;unaligned[35]=0x5a;
    const u32 wide_bytes=channel_memory_encode(wide,{unaligned+1,34});
    bool exact=wide_bytes==34&&unaligned[0]==0xa5&&unaligned[35]==0x5a;
    for(u32 field=0;field<3;++field)for(u32 byte=0;byte<8;++byte)
        exact=exact&&unaligned[7+field*8+byte]==static_cast<u8>(wire_ids[field]>>(byte*8));
    u32 expected_hash=2166136261u;
    for(u32 byte=1;byte<=30;++byte)expected_hash=(expected_hash^unaligned[byte])*16777619u;
    for(u32 byte=0;byte<4;++byte)
        exact=exact&&unaligned[31+byte]==static_cast<u8>(expected_hash>>(byte*8));
    check(exact&&channel_memory_decode({unaligned+1,34},&decoded)&&decoded.count==1&&
              decoded.entries[0].guild_id==wire_ids[0]&&decoded.entries[0].channel_id==wire_ids[1]&&
              decoded.entries[0].parent_id==wire_ids[2],
          "channel memory preserves exact unaligned 64-bit fields, checksum and adjacent bytes");
    ChannelMemory full{};
    for (u64 guild = 1; guild <= kChannelMemoryCapacity; ++guild)
        channel_memory_remember(&full, guild, guild + 1000, guild % 2 ? guild + 2000 : 0);
    u8 big[kChannelMemoryFileBytes];
    const u32 big_bytes = channel_memory_encode(full, {big, sizeof(big)});
    check(big_bytes == kChannelMemoryFileBytes && channel_memory_decode({big, big_bytes}, &decoded) &&
              decoded.count == kChannelMemoryCapacity,
          "a full table fits the bounded file size");

    ChannelMemory untouched{};
    channel_memory_remember(&untouched, 7, 70, 0);
    record[8] ^= 1;
    check(!channel_memory_decode({record, bytes}, &untouched) && untouched.count == 1 &&
              untouched.entries[0].channel_id == 70,
          "a checksum failure is rejected and leaves the table untouched");
    record[8] ^= 1;
    check(!channel_memory_decode({record, bytes - 1}, &untouched) && !channel_memory_decode({record, 5}, &untouched) &&
              !channel_memory_decode({record, 0}, &untouched) &&
              !channel_memory_decode({big, kChannelMemoryFileBytes + 1}, &untouched) && untouched.count == 1,
          "short, empty and oversized records are rejected");
    // Well-formed records with invalid content are rejected as well.
    ChannelMemory bad{};
    bad.count = 2;
    bad.entries[0] = {5, 50, 0};
    bad.entries[1] = {5, 51, 0};
    u8 bad_record[kChannelMemoryFileBytes];
    u32 bad_bytes = channel_memory_encode(bad, {bad_record, sizeof(bad_record)});
    check(bad_bytes && !channel_memory_decode({bad_record, bad_bytes}, &untouched), "a duplicated guild is rejected");
    bad.entries[1] = {6, 0, 0};
    bad_bytes = channel_memory_encode(bad, {bad_record, sizeof(bad_record)});
    check(bad_bytes && !channel_memory_decode({bad_record, bad_bytes}, &untouched), "a zero channel is rejected");
    bad.entries[1] = {6, 60, 0};
    bad_bytes = channel_memory_encode(bad, {bad_record, sizeof(bad_record)});
    check(bad_bytes && channel_memory_decode({bad_record, bad_bytes}, &decoded), "a valid two-entry record decodes");
    bad_record[4] = 2;
    check(!channel_memory_decode({bad_record, bad_bytes}, &untouched) && untouched.count == 1,
          "an unknown version is rejected");

    wchar_t path[512];
    ChannelMemory loaded{};
    const bool have_path = temporary_path(path, 512);
    check(have_path && channel_memory_save_file(path, visits) && channel_memory_load_file(path, &loaded) &&
              loaded.count == visits.count &&
              channel_memory_latest(loaded)->channel_id == channel_memory_latest(visits)->channel_id,
          "channel memory persists to a file");
    if (have_path) {
        DeleteFileW(path);
        check(write_raw(path, "MCCM\x01\x05garbage", 13) && !channel_memory_load_file(path, &loaded) && loaded.count == 0,
              "a corrupt file loads an empty table");
        check(write_raw(path, "MC", 2) && !channel_memory_load_file(path, &loaded) && loaded.count == 0,
              "a short file loads an empty table");
        DeleteFileW(path);
        check(channel_memory_load_file(path, &loaded) && loaded.count == 0, "a missing file is an empty table");
    }
    wchar_t profile[512];
    wchar_t other[512];
    const bool built = channel_memory_path(0x1234, profile, 512) && channel_memory_path(0x1235, other, 512);
    const u32 profile_length = built ? wide_length(profile, 512) : 0;
    check(built && profile_length > 0 && profile_length == wide_length(other, 512) &&
              !bytes_equal(profile, other, profile_length * 2) && !channel_memory_path(0, other, 512),
          "the file is keyed by profile");

    // Startup restoration through the READY pump.
    ChannelMemory previous{};
    channel_memory_remember(&previous, 0, 30, 0);
    channel_memory_remember(&previous, 21, 212, 0);
    Fixture startup;
    MessageTimeline timeline{};
    MessageEventPump pump{};
    u64 ids[4]{};
    check(initialize(startup), "startup fixture builds");
    pump.state = &startup.state;
    pump.timeline = &timeline;
    pump.guild_id_scratch = {ids, 4};
    channel_memory_seed_restore(previous, &startup.state);
    check(startup.state.restore_channel_id == 212 && startup.state.restore_guild_id == 21 &&
              !startup.state.home_selected && startup.state.last_private_channel_id == 30 &&
              !startup.state.selected_channel_id,
          "the latest selection becomes the restoration target before the first frame");
    const auto consume = [&](GatewayClientEventKind kind, GatewayClientDispatchKind dispatch, u64 time) noexcept {
        GatewayClientEvent e{};
        e.kind = kind;
        e.state.id = 1;
        e.dispatch_kind = dispatch;
        e.timestamp_ms = time;
        return message_event_pump_consume(&pump, e, {}).ok();
    };
    check(consume(GatewayClientEventKind::ready, GatewayClientDispatchKind::unknown, 100) &&
              startup.state.restore_channel_id == 212 && add_guild(startup, 21) && add(startup, 211, 21, 0, 0) &&
              !startup.state.selected_channel_id && add(startup, 212, 21, 0, 1) &&
              startup.state.selected_channel_id == 212 && startup.state.selected_guild_id == 21 &&
              !startup.state.restore_channel_id,
          "READY places the remembered channel when it arrives");
    check(add(startup, 30, 0, 3, 0) && chat_state_store_select_home(&startup.state).ok() &&
              startup.state.selected_channel_id == 30,
          "Home reopens the last conversation remembered by the previous run");

    Fixture missing;
    pump.state = &missing.state;
    check(initialize(missing), "fallback fixture builds");
    channel_memory_seed_restore(previous, &missing.state);
    check(consume(GatewayClientEventKind::ready, GatewayClientDispatchKind::unknown, 200) && add_guild(missing, 21) &&
              add(missing, 211, 21, 0, 0) && !missing.state.selected_channel_id &&
              consume(GatewayClientEventKind::dispatch, GatewayClientDispatchKind::ready_state_complete, 300) &&
              missing.state.selected_channel_id == 211 && !missing.state.restore_channel_id,
          "a remembered channel that no longer exists falls back to its guild's default");

    Fixture dm;
    pump.state = &dm.state;
    ChannelMemory home_last{};
    channel_memory_remember(&home_last, 20, 201, 0);
    channel_memory_remember(&home_last, 0, 31, 0);
    check(initialize(dm), "home fixture builds");
    channel_memory_seed_restore(home_last, &dm.state);
    check(dm.state.home_selected && dm.state.restore_guild_id == 0 &&
              consume(GatewayClientEventKind::ready, GatewayClientDispatchKind::unknown, 400) &&
              add(dm, 30, 0, 3, 0) && !dm.state.selected_channel_id && add(dm, 31, 0, 3, 0) &&
              dm.state.selected_channel_id == 31 && dm.state.home_selected,
          "a last conversation on Home is restored at startup");

    Fixture empty;
    check(initialize(empty), "empty fixture builds");
    channel_memory_seed_restore(ChannelMemory{}, &empty.state);
    check(!empty.state.restore_channel_id && !empty.state.home_selected && !empty.state.last_private_channel_id,
          "an empty memory leaves the default startup selection alone");
}
} // namespace maicord
