#include "maicord/core/byte_queue.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

void compact(ByteQueue* queue) noexcept {
    if (queue->begin == 0) {
        return;
    }
    const u32 bytes = queue->end - queue->begin;
    copy_bytes(queue->data, queue->data + queue->begin, bytes);
    queue->begin = 0;
    queue->end = bytes;
}

} // namespace

bool byte_queue_initialize(ByteQueue* queue, Slice<u8> storage) noexcept {
    if (!queue || (!storage.data && storage.count != 0)) {
        return false;
    }
    *queue = ByteQueue{storage.data, storage.count, 0, 0};
    return true;
}

u32 byte_queue_size(const ByteQueue& queue) noexcept {
    return queue.end - queue.begin;
}

u32 byte_queue_available(const ByteQueue& queue) noexcept {
    return queue.capacity - byte_queue_size(queue);
}

ConstSlice<u8> byte_queue_readable(const ByteQueue& queue) noexcept {
    return ConstSlice<u8>{queue.data ? queue.data + queue.begin : nullptr, byte_queue_size(queue)};
}

Slice<u8> byte_queue_writable(ByteQueue* queue) noexcept {
    if (!queue || !queue->data) {
        return Slice<u8>{nullptr, 0};
    }
    if (queue->end == queue->capacity && queue->begin != 0) {
        compact(queue);
    }
    return Slice<u8>{queue->data + queue->end, queue->capacity - queue->end};
}

bool byte_queue_append(ByteQueue* queue, ConstSlice<u8> input) noexcept {
    if (!queue || (!input.data && input.count != 0) || input.count > byte_queue_available(*queue)) {
        return false;
    }
    if (queue->capacity - queue->end < input.count) {
        compact(queue);
    }
    copy_bytes(queue->data + queue->end, input.data, input.count);
    queue->end += input.count;
    return true;
}

bool byte_queue_produce(ByteQueue* queue, u32 bytes) noexcept {
    if (!queue || bytes > queue->capacity - queue->end) {
        return false;
    }
    queue->end += bytes;
    return true;
}

bool byte_queue_consume(ByteQueue* queue, u32 bytes) noexcept {
    if (!queue || bytes > byte_queue_size(*queue)) {
        return false;
    }
    queue->begin += bytes;
    if (queue->begin == queue->end) {
        queue->begin = 0;
        queue->end = 0;
    }
    return true;
}

void byte_queue_clear(ByteQueue* queue, bool erase) noexcept {
    if (!queue) {
        return;
    }
    if (erase && queue->data) {
        volatile u8* bytes = queue->data;
        for (u32 index = 0; index < queue->end; ++index) {
            bytes[index] = 0;
        }
    }
    queue->begin = 0;
    queue->end = 0;
}

} // namespace maicord
