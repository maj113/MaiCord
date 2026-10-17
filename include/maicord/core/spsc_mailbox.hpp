#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {

enum class SpscMailboxError : u8 {
    none,
    invalid_input,
    empty,
    event_capacity,
    payload_capacity,
    output_too_small,
    corrupt,
};

struct SpscMailboxResult {
    u32 payload_bytes;
    SpscMailboxError error;
    // The record's payload lives in the producer's external buffer, not in
    // the ring (see spsc_mailbox_push_external).
    bool external = false;

    constexpr bool ok() const noexcept {
        return error == SpscMailboxError::none;
    }
};

template <typename T> struct SpscMailboxRecord {
    T value;
    u64 payload_sequence;
    u32 payload_bytes;
    u32 flags;
};

// Record flag: payload_bytes describe a producer-owned external buffer that
// the ring does not hold. Rare oversized records use it so the ring itself
// stays sized for ordinary records.
inline constexpr u32 kSpscMailboxExternalPayload = 1;

template <typename T> struct SpscMailboxTypeIdentity {
    using type = T;
};

// One producer and one consumer; each sequence has one writer. Descriptor and
// byte sequences are monotonic and each descriptor covers one FIFO payload
// interval. The producer copies bytes before publishing the descriptor; the
// consumer releases the interval only after copying or discarding it.
// Acquire/release ordering publishes the descriptor and its bytes without a
// locked read-modify-write per counter access; the build pins x64
// /volatile:ms semantics.
template <typename T, u32 EventCapacity, u32 PayloadCapacity> struct SpscMailbox {
    static_assert(EventCapacity >= 2);
    static_assert((EventCapacity & (EventCapacity - 1)) == 0);
    static_assert(EventCapacity < 0x80000000u);
    static_assert(PayloadCapacity != 0);

    SpscMailboxRecord<T> records[EventCapacity];
    volatile LONG read_sequence;
    u8 read_padding[60];
    volatile LONG write_sequence;
    u8 write_padding[60];
    volatile LONG64 payload_read_sequence;
    u8 payload_read_padding[56];
    volatile LONG64 payload_write_sequence;
    u8 payload_write_padding[56];
    volatile LONG dropped;
    u8 dropped_padding[60];
    u8 payload[PayloadCapacity];
};

// Acquire loads and release stores as winnt.h's x64 ReadAcquire/WriteRelease
// implement them: plain volatile accesses, ordered under /volatile:ms.
inline u32 spsc_mailbox_load_u32(const volatile LONG* value) noexcept {
    return static_cast<u32>(*value);
}

inline u64 spsc_mailbox_load_u64(const volatile LONG64* value) noexcept {
    return static_cast<u64>(*value);
}

template <typename Sequence, typename Value>
inline void spsc_mailbox_store(volatile Sequence* sequence, Value value) noexcept {
    *sequence = static_cast<Sequence>(value);
}

template <u32 Capacity>
void spsc_mailbox_copy_into(u8 (&destination)[Capacity], u64 sequence,
                            ConstSlice<u8> source) noexcept {
    if (source.count == 0) {
        return;
    }
    const u32 offset = static_cast<u32>(sequence % Capacity);
    const u32 first = minimum(source.count, Capacity - offset);
    // A FIFO interval wraps at most once, so two copies cover it.
    copy_bytes(destination + offset, source.data, first);
    copy_bytes(destination, source.data + first, source.count - first);
}

template <u32 Capacity>
void spsc_mailbox_copy_out(const u8 (&source)[Capacity], u64 sequence,
                           Slice<u8> destination, u32 bytes) noexcept {
    if (bytes == 0) {
        return;
    }
    const u32 offset = static_cast<u32>(sequence % Capacity);
    const u32 first = minimum(bytes, Capacity - offset);
    copy_bytes(destination.data, source + offset, first);
    copy_bytes(destination.data + first, source, bytes - first);
}

template <typename T, u32 EventCapacity, u32 PayloadCapacity>
SpscMailboxResult spsc_mailbox_push(
    SpscMailbox<T, EventCapacity, PayloadCapacity>* mailbox,
    const typename SpscMailboxTypeIdentity<T>::type& value,
    ConstSlice<u8> payload) noexcept {
    if (!mailbox || (!payload.data && payload.count != 0)) {
        return SpscMailboxResult{payload.count, SpscMailboxError::invalid_input};
    }
    const u32 write = spsc_mailbox_load_u32(&mailbox->write_sequence);
    const u32 read = spsc_mailbox_load_u32(&mailbox->read_sequence);
    if (write - read >= EventCapacity) {
        _InterlockedIncrement(&mailbox->dropped);
        return SpscMailboxResult{payload.count, SpscMailboxError::event_capacity};
    }
    const u64 payload_write = spsc_mailbox_load_u64(&mailbox->payload_write_sequence);
    const u64 payload_read = spsc_mailbox_load_u64(&mailbox->payload_read_sequence);
    if (payload_write < payload_read || payload_write - payload_read > PayloadCapacity) {
        _InterlockedIncrement(&mailbox->dropped);
        return SpscMailboxResult{payload.count, SpscMailboxError::corrupt};
    }
    if (payload.count > PayloadCapacity - (payload_write - payload_read)) {
        _InterlockedIncrement(&mailbox->dropped);
        return SpscMailboxResult{payload.count, SpscMailboxError::payload_capacity};
    }

    spsc_mailbox_copy_into(mailbox->payload, payload_write, payload);
    SpscMailboxRecord<T>& record = mailbox->records[write & (EventCapacity - 1)];
    record.value = value;
    record.payload_sequence = payload_write;
    record.payload_bytes = payload.count;
    record.flags = 0;
    spsc_mailbox_store(&mailbox->payload_write_sequence,
                   static_cast<LONG64>(payload_write + payload.count));
    spsc_mailbox_store(&mailbox->write_sequence, static_cast<LONG>(write + 1));
    return SpscMailboxResult{payload.count, SpscMailboxError::none};
}

// Queues a descriptor whose payload stays in a producer-owned buffer. The
// producer must not reuse that buffer until the consumer has popped or
// discarded this record; no ring bytes are used.
template <typename T, u32 EventCapacity, u32 PayloadCapacity>
SpscMailboxResult spsc_mailbox_push_external(
    SpscMailbox<T, EventCapacity, PayloadCapacity>* mailbox,
    const typename SpscMailboxTypeIdentity<T>::type& value, u32 payload_bytes) noexcept {
    if (!mailbox || !payload_bytes) {
        return SpscMailboxResult{payload_bytes, SpscMailboxError::invalid_input, true};
    }
    const u32 write = spsc_mailbox_load_u32(&mailbox->write_sequence);
    const u32 read = spsc_mailbox_load_u32(&mailbox->read_sequence);
    if (write - read >= EventCapacity) {
        _InterlockedIncrement(&mailbox->dropped);
        return SpscMailboxResult{payload_bytes, SpscMailboxError::event_capacity, true};
    }
    const u64 payload_write = spsc_mailbox_load_u64(&mailbox->payload_write_sequence);
    SpscMailboxRecord<T>& record = mailbox->records[write & (EventCapacity - 1)];
    record.value = value;
    record.payload_sequence = payload_write;
    record.payload_bytes = payload_bytes;
    record.flags = kSpscMailboxExternalPayload;
    spsc_mailbox_store(&mailbox->write_sequence, static_cast<LONG>(write + 1));
    return SpscMailboxResult{payload_bytes, SpscMailboxError::none, true};
}

template <typename T, u32 PayloadCapacity>
bool spsc_mailbox_record_valid(const SpscMailboxRecord<T>& record, u64 payload_read) noexcept {
    if (record.payload_sequence != payload_read || (record.flags & ~kSpscMailboxExternalPayload))
        return false;
    return (record.flags & kSpscMailboxExternalPayload) ? record.payload_bytes != 0
                                                        : record.payload_bytes <= PayloadCapacity;
}

// Consumer: whether the oldest visible record is external. Reading it
// acquires that record, so producer writes made before its push are visible.
template <typename T, u32 EventCapacity, u32 PayloadCapacity>
bool spsc_mailbox_head_external(const SpscMailbox<T, EventCapacity, PayloadCapacity>& mailbox) noexcept {
    const u32 read = spsc_mailbox_load_u32(&mailbox.read_sequence);
    const u32 write = spsc_mailbox_load_u32(&mailbox.write_sequence);
    return read != write &&
           (mailbox.records[read & (EventCapacity - 1)].flags & kSpscMailboxExternalPayload) != 0;
}

// An external record copies from `external`, the producer's buffer for it;
// without one (or when it is shorter than the record) the record is corrupt.
template <typename T, u32 EventCapacity, u32 PayloadCapacity>
SpscMailboxResult spsc_mailbox_pop(
    SpscMailbox<T, EventCapacity, PayloadCapacity>* mailbox, T* value,
    Slice<u8> payload_output, ConstSlice<u8> external = {}) noexcept {
    if (!mailbox || !value || (!payload_output.data && payload_output.count != 0)) {
        return SpscMailboxResult{0, SpscMailboxError::invalid_input};
    }
    const u32 read = spsc_mailbox_load_u32(&mailbox->read_sequence);
    const u32 write = spsc_mailbox_load_u32(&mailbox->write_sequence);
    if (read == write) {
        return SpscMailboxResult{0, SpscMailboxError::empty};
    }
    const SpscMailboxRecord<T>& record = mailbox->records[read & (EventCapacity - 1)];
    const u64 payload_read = spsc_mailbox_load_u64(&mailbox->payload_read_sequence);
    const bool outside = (record.flags & kSpscMailboxExternalPayload) != 0;
    if (!spsc_mailbox_record_valid<T, PayloadCapacity>(record, payload_read) ||
        (outside && (!external.data || external.count < record.payload_bytes))) {
        return SpscMailboxResult{record.payload_bytes, SpscMailboxError::corrupt, outside};
    }
    if (record.payload_bytes > payload_output.count) {
        return SpscMailboxResult{record.payload_bytes, SpscMailboxError::output_too_small, outside};
    }
    if (outside)
        copy_bytes(payload_output.data, external.data, record.payload_bytes);
    else
        spsc_mailbox_copy_out(mailbox->payload, payload_read, payload_output, record.payload_bytes);
    *value = record.value;
    // Copy results before advancing read_sequence: the producer may then reuse the slot.
    const u32 payload_bytes = record.payload_bytes;
    if (!outside)
        spsc_mailbox_store(&mailbox->payload_read_sequence,
                       static_cast<LONG64>(payload_read + payload_bytes));
    spsc_mailbox_store(&mailbox->read_sequence, static_cast<LONG>(read + 1));
    return SpscMailboxResult{payload_bytes, SpscMailboxError::none, outside};
}

template <typename T, u32 EventCapacity, u32 PayloadCapacity>
SpscMailboxResult spsc_mailbox_discard(
    SpscMailbox<T, EventCapacity, PayloadCapacity>* mailbox, T* value) noexcept {
    if (!mailbox || !value) {
        return SpscMailboxResult{0, SpscMailboxError::invalid_input};
    }
    const u32 read = spsc_mailbox_load_u32(&mailbox->read_sequence);
    const u32 write = spsc_mailbox_load_u32(&mailbox->write_sequence);
    if (read == write) {
        return SpscMailboxResult{0, SpscMailboxError::empty};
    }
    const SpscMailboxRecord<T>& record = mailbox->records[read & (EventCapacity - 1)];
    const u64 payload_read = spsc_mailbox_load_u64(&mailbox->payload_read_sequence);
    const bool outside = (record.flags & kSpscMailboxExternalPayload) != 0;
    if (!spsc_mailbox_record_valid<T, PayloadCapacity>(record, payload_read)) {
        return SpscMailboxResult{record.payload_bytes, SpscMailboxError::corrupt, outside};
    }
    *value = record.value;
    // Copy results before advancing read_sequence: the producer may then reuse the slot.
    const u32 payload_bytes = record.payload_bytes;
    if (!outside)
        spsc_mailbox_store(&mailbox->payload_read_sequence,
                       static_cast<LONG64>(payload_read + payload_bytes));
    spsc_mailbox_store(&mailbox->read_sequence, static_cast<LONG>(read + 1));
    return SpscMailboxResult{payload_bytes, SpscMailboxError::none, outside};
}

template <typename T, u32 EventCapacity, u32 PayloadCapacity>
u32 spsc_mailbox_size(const SpscMailbox<T, EventCapacity, PayloadCapacity>& mailbox) noexcept {
    return spsc_mailbox_load_u32(&mailbox.write_sequence) -
           spsc_mailbox_load_u32(&mailbox.read_sequence);
}

template <typename T, u32 EventCapacity, u32 PayloadCapacity>
u32 spsc_mailbox_payload_used(
    const SpscMailbox<T, EventCapacity, PayloadCapacity>& mailbox) noexcept {
    const u64 write = spsc_mailbox_load_u64(&mailbox.payload_write_sequence);
    const u64 read = spsc_mailbox_load_u64(&mailbox.payload_read_sequence);
    const u64 used = write >= read ? write - read : ~0ull;
    return used <= PayloadCapacity ? static_cast<u32>(used) : ~0u;
}

template <typename T, u32 EventCapacity, u32 PayloadCapacity>
u32 spsc_mailbox_dropped(
    const SpscMailbox<T, EventCapacity, PayloadCapacity>& mailbox) noexcept {
    return spsc_mailbox_load_u32(&mailbox.dropped);
}

} // namespace maicord
