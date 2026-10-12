#include "maicord/protocol/dave_replay.hpp"


namespace maicord {
namespace {
u32 slot(const DaveReplay& replay, u32 index) noexcept {
    // Both terms are at most the ring size; one subtraction wraps the sum.
    const u32 at = replay.head + index;
    return at >= kDaveReplaySlots ? at - kDaveReplaySlots : at;
}
u32 position(const DaveReplay& replay, u64 counter) noexcept {
    u32 first = 0, end = replay.count;
    while (first < end) {
        const u32 middle = first + (end - first) / 2;
        if (replay.missing[slot(replay, middle)] < counter)
            first = middle + 1;
        else
            end = middle;
    }
    return first;
}
bool valid(const DaveReplay& replay, u64 counter) noexcept {
    return counter <= kDaveMaximumFrameCounter && replay.head < kDaveReplaySlots &&
           replay.count <= kDaveMissingNonces;
}
} // namespace
bool dave_frame_counter(u32 oldest_generation, u32 truncated_nonce, DaveFrameCounter* output) noexcept {
    if (!output)
        return false;
    const u32 low = truncated_nonce >> 24;
    u64 generation = static_cast<u64>(oldest_generation & ~u32{255}) + low;
    if (low < (oldest_generation & 255))
        generation += 256;
    if (generation > ~u32{0})
        return false;
    *output = {(generation << 24) | (truncated_nonce & 0xFFFFFF), static_cast<u32>(generation)};
    return true;
}
bool dave_replay_can_accept(const DaveReplay& replay, u64 counter) noexcept {
    if (!valid(replay, counter))
        return false;
    if (!replay.initialized || counter > replay.largest)
        return true;
    const u32 at = position(replay, counter);
    return at < replay.count && replay.missing[slot(replay, at)] == counter;
}
bool dave_replay_commit(DaveReplay* replay, u64 counter) noexcept {
    if (!replay || !valid(*replay, counter))
        return false;
    if (!replay->initialized) {
        replay->largest = counter;
        replay->initialized = true;
        return true;
    }
    if (counter > replay->largest) {
        const u64 difference = counter - replay->largest - 1;
        const u32 added =
            difference > kDaveMissingNonces ? kDaveMissingNonces : static_cast<u32>(difference);
        const u32 drop =
            replay->count + added > kDaveMissingNonces ? replay->count + added - kDaveMissingNonces : 0;
        replay->head = slot(*replay, drop);
        replay->count -= drop;
        for (u32 i = 0; i != added; ++i)
            replay->missing[slot(*replay, replay->count++)] = counter - added + i;
        replay->largest = counter;
        return true;
    }
    const u32 at = position(*replay, counter);
    if (at == replay->count || replay->missing[slot(*replay, at)] != counter)
        return false;
    // Erase by shifting the shorter side of the ordered ring.
    if (at < replay->count / 2) {
        for (u32 i = at; i; --i)
            replay->missing[slot(*replay, i)] = replay->missing[slot(*replay, i - 1)];
        replay->missing[replay->head] = 0;
        replay->head = slot(*replay, 1);
    } else {
        for (u32 i = at; i + 1 < replay->count; ++i)
            replay->missing[slot(*replay, i)] = replay->missing[slot(*replay, i + 1)];
        replay->missing[slot(*replay, replay->count - 1)] = 0;
    }
    --replay->count;
    return true;
}
} // namespace maicord
