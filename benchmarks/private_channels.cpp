#include "maicord/core/time.hpp"
#include "maicord/ui/private_channel_title.hpp"

namespace maicord {
bool benchmark_private_order(Clock clock, ChatStateStore& state,
    void (*write)(const char*, u64, const char*) noexcept) noexcept;
bool benchmark_private_channels(Clock clock, void (*write)(const char*, u64, const char*) noexcept) noexcept {
    PrivateChannelStore private_store{};
    if (!private_channel_store_initialize_owned(&private_store, 4096, 65'536)) return false;
    const auto fail = [&]() noexcept { private_channel_store_release(&private_store); return false; };
    const u64 recipients[] = {2, 3, 4, 5};
    i64 begin = clock_now();
    for (u32 i = 0; i < 1000; ++i) {
        if (!private_channel_store_replace(&private_store, {1000 + i, 2, {recipients, 4}, {}, true, true, false})) return fail();
    }
    const u64 hydration_us = clock_microseconds(clock, begin, clock_now());
    const u32 revision = private_store.revision;
    begin = clock_now();
    for (u32 i = 0; i < 100'000; ++i) {
        if (!private_channel_store_replace(&private_store, {1000 + i % 1000, 2, {recipients, 4}, {}, true, true, false})) return fail();
    }
    const u64 unchanged_us = clock_microseconds(clock, begin, clock_now());
    if (private_store.revision != revision || private_store.recipient_count != 4000) return fail();
    UserStore users{};
    UserRecord records[8]{};
    u32 order[8]{};
    u8 names[2048]{};
    if (!user_store_initialize(&users, {records, 8}, {order, 8}, {names, 2048})) return fail();
    for (u64 id : recipients) if (!user_store_upsert(&users, id, utf8_literal("Peer")).ok()) return fail();
    ChatStateStore state{};
    state.private_channels = &private_store;
    state.self_user_id = 1;
    ChannelRecord channel{};
    channel.occupied = true;
    channel.type = 3;
    char title[512];
    u64 checksum = 0;
    begin = clock_now();
    for (u32 i = 0; i < 200'000; ++i) {
        channel.id = 1000 + i % 1000;
        const auto text = private_channel_title(state, &users, channel, {title, sizeof(title)});
        if (text.bytes != 22) return fail();
        checksum += text.bytes;
    }
    const u64 title_us = clock_microseconds(clock, begin, clock_now());
    if (!hydration_us || !unchanged_us || !title_us || !checksum) return fail();
    write("private_metadata_insert_1000", hydration_us, "us");
    write("private_metadata_unchanged", 100'000'000'000ull / unchanged_us, "runs/s");
    write("private_title_four_recipients", 200'000'000'000ull / title_us, "runs/s");
    write("private_metadata_committed_1000", private_store.record_backing.committed + private_store.recipient_backing.committed + private_store.nickname_backing.committed, "bytes");
    write("private_metadata_reserved", private_store.record_backing.reserved + private_store.recipient_backing.reserved + private_store.nickname_backing.reserved, "bytes");
    write("private_nickname_record_size", sizeof(PrivateNicknameRecord), "bytes");
    write("private_metadata_record_size", sizeof(PrivateChannelRecord), "bytes");
    const PrivateNicknameInput nick{2, utf8_literal("Nick")};
    begin = clock_now();
    for (u32 i = 0; i < 1000; ++i) {
        if (!private_channel_store_replace_nicknames(&private_store, 1000 + i, {&nick, 1})) return fail();
    }
    const u64 nick_us = clock_microseconds(clock, begin, clock_now());
    begin = clock_now();
    for (u32 i = 0; i < 200'000; ++i) {
        channel.id = 1000 + i % 1000;
        const auto text = private_channel_title(state, &users, channel, {title, sizeof(title)});
        if (text.bytes != 22 || text.data[0] != 'N') return fail();
    }
    const u64 nick_title_us = clock_microseconds(clock, begin, clock_now());
    if (!nick_us || !nick_title_us) return fail();
    write("private_nicknames_insert_1000", nick_us, "us");
    write("private_title_with_nickname", 200'000'000'000ull / nick_title_us, "runs/s");
    write("private_nicknames_committed_1000", private_store.nickname_backing.committed, "bytes");
    Arena reset_backing{};
    const usize reset_bytes = 1000 * (sizeof(ChannelRecord) + sizeof(u16)) + 256;
    if (!arena_initialize(&reset_backing, reset_bytes, reset_bytes, 4096)) return fail();
    const auto reset_fail = [&]() noexcept { arena_release(&reset_backing); return fail(); };
    auto* reset_channels = static_cast<ChannelRecord*>(arena_allocate(&reset_backing, 1000 * sizeof(ChannelRecord), alignof(ChannelRecord)));
    auto* reset_order = static_cast<u16*>(arena_allocate(&reset_backing, 1000 * sizeof(u16), alignof(u16)));
    auto* reset_names = static_cast<u8*>(arena_allocate(&reset_backing, 256, 1));
    GuildRecord reset_guild{};
    u16 reset_guild_order{};
    ChatStateStore reset_state{};
    if (!chat_state_store_initialize(&reset_state, {&reset_guild, 1}, {&reset_guild_order, 1},
        {reset_channels, 1000}, {reset_order, 1000}, {reset_names, 256})) return reset_fail();
    reset_state.private_channels = &private_store;
    reset_state.self_user_id = 1;
    for (u32 variant = 0; variant < 2; ++variant) {
        i64 elapsed = 0;
        for (u32 run = 0; run < 20; ++run) {
            for (u32 i = 0; i < 1000; ++i) {
                if (!chat_state_store_upsert_channel(&reset_state, {1000 + i, 0, 0, {}, 0, 0, 3}).ok() ||
                    !private_channel_store_replace(&private_store, {1000 + i, 2, {recipients, 4}, {}, true, true, false}) ||
                    !private_channel_store_replace_nicknames(&private_store, 1000 + i, {&nick, 1})) return reset_fail();
            }
            if (variant == 0 && run == 0 && !benchmark_private_order(clock, reset_state, write)) return reset_fail();
            begin = clock_now();
            if (variant == 0) {
                while (reset_state.channel_count) {
                    const u64 id = reset_state.channels[reset_state.channel_order[reset_state.channel_count - 1]].id;
                    if (!chat_state_store_remove_channel(&reset_state, id)) return reset_fail();
                }
            } else if (chat_state_store_clear_private_channels(&reset_state) != 1000) return reset_fail();
            elapsed += clock_now() - begin;
            if (reset_state.channel_count || private_store.count || private_store.nickname_count || private_store.recipient_count)
                return reset_fail();
        }
        write(variant == 0 ? "private_reset_1000_individual" : "private_reset_1000_bulk",
            clock_microseconds(clock, 0, elapsed) / 20, "us");
    }
    arena_release(&reset_backing);
    private_channel_store_release(&private_store);
    return true;
}
}
