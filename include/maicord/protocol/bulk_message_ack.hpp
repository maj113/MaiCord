#pragma once
#include "maicord/core/utf.hpp"

namespace maicord {
inline constexpr u32 kBulkMessageAckMaximumEntries = 100;
inline constexpr u32 kBulkMessageAckMaximumBodyBytes =
    sizeof("{\"read_states\":[]}") - 1 +
    kBulkMessageAckMaximumEntries *
        (sizeof("{\"channel_id\":\"\",\"message_id\":\"\",\"read_state_type\":0}") - 1 + 40) +
    kBulkMessageAckMaximumEntries - 1;

// Genuine captured cursors only. The protocol does not synthesize timestamps,
// tokens, alternate read-state types, or empty-channel cursor values.
struct BulkMessageAckEntry {
    u64 channel_id, message_id;
    u32 read_state_type;
};
static_assert(sizeof(BulkMessageAckEntry) == 24);
enum class BulkMessageAckWriteError : u8 {
    none,
    invalid_input,
    capacity
};
struct BulkMessageAckWriteResult {
    u32 bytes;
    BulkMessageAckWriteError error;
    constexpr bool ok() const noexcept {
        return error == BulkMessageAckWriteError::none;
    }
};
// Validates the entire batch before writing. Failure leaves every output byte
// unchanged. Entries, both slice descriptors, and output must not overlap.
BulkMessageAckWriteResult write_bulk_message_ack_body(const ConstSlice<BulkMessageAckEntry>& entries,
                                                      const Slice<u8>& output) noexcept;
} // namespace maicord
