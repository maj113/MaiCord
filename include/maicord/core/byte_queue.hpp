#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct ByteQueue {
    u8* data;
    u32 capacity;
    u32 begin;
    u32 end;
};

bool byte_queue_initialize(ByteQueue* queue, Slice<u8> storage) noexcept;
u32 byte_queue_size(const ByteQueue& queue) noexcept;
u32 byte_queue_available(const ByteQueue& queue) noexcept;
ConstSlice<u8> byte_queue_readable(const ByteQueue& queue) noexcept;
Slice<u8> byte_queue_writable(ByteQueue* queue) noexcept;
bool byte_queue_append(ByteQueue* queue, ConstSlice<u8> input) noexcept;
bool byte_queue_produce(ByteQueue* queue, u32 bytes) noexcept;
bool byte_queue_consume(ByteQueue* queue, u32 bytes) noexcept;
void byte_queue_clear(ByteQueue* queue, bool erase) noexcept;

} // namespace maicord
