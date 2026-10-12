#include "maicord/ui/navigation_view.hpp"
#include "maicord/core/time.hpp"

namespace maicord {
bool benchmark_private_order(Clock clock, ChatStateStore& state,
    void (*write)(const char*, u64, const char*) noexcept) noexcept {
    if (state.channel_count != 1000 || state.channel_capacity != 1000) return false;
    Arena backing{};
    const usize bytes = 1000 * (sizeof(ChannelViewRow) + sizeof(ChannelSortKey) + sizeof(u16) + sizeof(u8));
    if (!arena_initialize(&backing, bytes, bytes, 4096)) return false;
    const auto fail = [&]() noexcept { arena_release(&backing); return false; };
    auto* rows = arena_allocate<ChannelViewRow>(&backing, 1000);
    auto* keys = arena_allocate<ChannelSortKey>(&backing, 1000);
    auto* order = arena_allocate<u16>(&backing, 1000);
    auto* visible = arena_allocate<u8>(&backing, 1000);
    GuildViewRow guild{};
    NavigationView view{};
    if (!navigation_view_initialize(&view, {&guild, 1}, {rows, 1000}, {order, 1000}, {visible, 1000}, {keys, 1000}))
        return fail();
    const i64 begin = clock_now();
    for (u32 i = 0; i < 1000; ++i) {
        const u64 id = 1000 + i;
        if (!chat_state_store_note_message(&state, id, (1000ull + i) << 22, 2, false, false) ||
            !navigation_view_rebuild(&view, state).ok() || view.channel_count != 1000 || rows[0].id != id)
            return fail();
    }
    write("private_order_update_projection_1000", clock_microseconds(clock, begin, clock_now()) / 1000, "us/update");
    write("private_order_projection_backing", backing.committed, "bytes");
    arena_release(&backing);
    return true;
}
}
