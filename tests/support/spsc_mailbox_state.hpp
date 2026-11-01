#pragma once
#include "maicord/core/spsc_mailbox.hpp"

namespace maicord {
// Occupancy readings for mailbox tests; production code only pushes and pops.
template <typename T, u32 EventCapacity, u32 PayloadCapacity>
u32 spsc_mailbox_size(const SpscMailbox<T, EventCapacity, PayloadCapacity>& mailbox) noexcept {
    return spsc_mailbox_load_u32(&mailbox.write_sequence) -
           spsc_mailbox_load_u32(&mailbox.read_sequence);
}

template <typename T, u32 EventCapacity, u32 PayloadCapacity>
u32 spsc_mailbox_payload_used(const SpscMailbox<T, EventCapacity, PayloadCapacity>& mailbox) noexcept {
    return static_cast<u32>(spsc_mailbox_load_u64(&mailbox.payload_write_sequence) -
                            spsc_mailbox_load_u64(&mailbox.payload_read_sequence));
}
} // namespace maicord
