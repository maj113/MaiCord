#pragma once

#include "maicord/core/base.hpp"

// The queue behind window_post (Linux): any number of threads push {cause, a, b}, the UI thread drains. A bounded
// ring of slots each carrying a sequence number (the multi-producer bounded queue of D. Vyukov): a producer claims a
// position with one compare-and-swap, writes its slot and publishes it with a release store; the consumer reads a slot
// only once it is published. Nothing allocates and nothing blocks; a full queue refuses the post.

namespace maicord::wayland {

inline constexpr u32 kPostCapacity = 256;  // a power of two
static_assert((kPostCapacity & (kPostCapacity - 1)) == 0);

struct PostEntry {
    u32 cause;
    u64 a, b;
};

struct PostQueue {
    struct Slot {
        u32 sequence;
        PostEntry entry;
    };
    Slot slots[kPostCapacity];
    u32 enqueue_position;
    u32 dequeue_position;  // UI thread only
};

// Sets every slot free; call once before any thread posts.
inline void post_queue_init(PostQueue* queue) noexcept {
    for (u32 i = 0; i < kPostCapacity; ++i)
        queue->slots[i].sequence = i;
    queue->enqueue_position = 0;
    queue->dequeue_position = 0;
}

// Any thread. False when the queue is full.
inline bool post_queue_push(PostQueue* queue, const PostEntry& entry) noexcept {
    u32 position = __atomic_load_n(&queue->enqueue_position, __ATOMIC_RELAXED);
    for (;;) {
        PostQueue::Slot& slot = queue->slots[position & (kPostCapacity - 1)];
        const u32 sequence = __atomic_load_n(&slot.sequence, __ATOMIC_ACQUIRE);
        const i32 difference = static_cast<i32>(sequence - position);
        if (difference == 0) {
            if (__atomic_compare_exchange_n(&queue->enqueue_position, &position, position + 1, true, __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
                slot.entry = entry;
                __atomic_store_n(&slot.sequence, position + 1, __ATOMIC_RELEASE);
                return true;
            }
        } else if (difference < 0) {
            return false;
        } else {
            position = __atomic_load_n(&queue->enqueue_position, __ATOMIC_RELAXED);
        }
    }
}

// The UI thread: the oldest published entry, or false when there is none.
inline bool post_queue_pop(PostQueue* queue, PostEntry* entry) noexcept {
    const u32 position = queue->dequeue_position;
    PostQueue::Slot& slot = queue->slots[position & (kPostCapacity - 1)];
    const u32 sequence = __atomic_load_n(&slot.sequence, __ATOMIC_ACQUIRE);
    if (static_cast<i32>(sequence - (position + 1)) < 0)
        return false;
    *entry = slot.entry;
    __atomic_store_n(&slot.sequence, position + kPostCapacity, __ATOMIC_RELEASE);
    queue->dequeue_position = position + 1;
    return true;
}

} // namespace maicord::wayland
