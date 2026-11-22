#include "maicord/protocol/messages/bulk_message_ack.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/protocol/encoding/writer.hpp"

namespace maicord {
namespace {
BulkMessageAckWriteResult fail(BulkMessageAckWriteError error) noexcept {
    return {0, error};
}
} // namespace
BulkMessageAckWriteResult write_bulk_message_ack_body(const ConstSlice<BulkMessageAckEntry>& entries,
                                                      const Slice<u8>& output) noexcept {
    if (!entries.data || !entries.count || entries.count > kBulkMessageAckMaximumEntries || !output.data)
        return fail(BulkMessageAckWriteError::invalid_input);
    // The only caller writes into its own stack buffer, so the entries never overlap the output.
    constexpr u32 envelope_bytes = sizeof("{\"read_states\":[]}") - 1;
    constexpr u32 entry_literal_bytes =
        sizeof("{\"channel_id\":\"\",\"message_id\":\"\",\"read_state_type\":0}") - 1;
    u32 required = envelope_bytes + entries.count - 1;
    for (u32 i = 0; i < entries.count; ++i) {
        const auto& entry = entries.data[i];
        if (!entry.channel_id || !entry.message_id || entry.read_state_type)
            return fail(BulkMessageAckWriteError::invalid_input);
        for (u32 j = 0; j < i; ++j)
            if (entries.data[j].channel_id == entry.channel_id)
                return fail(BulkMessageAckWriteError::invalid_input);
        required +=
            entry_literal_bytes + decimal_digits(entry.channel_id) + decimal_digits(entry.message_id);
    }
    if (required > output.count)
        return fail(BulkMessageAckWriteError::capacity);
    // The measured length fits, so no append below fails.
    ProtocolWriter writer{output, 0};
    protocol_write(&writer, "{\"read_states\":[");
    for (u32 i = 0; i < entries.count; ++i) {
        const auto& entry = entries.data[i];
        if (i)
            protocol_write(&writer, ',');
        protocol_write(&writer, "{\"channel_id\":", snowflake_text(entry.channel_id), ",\"message_id\":",
                       snowflake_text(entry.message_id), ",\"read_state_type\":0}");
    }
    protocol_write(&writer, "]}");
    return {writer.used, BulkMessageAckWriteError::none};
}
} // namespace maicord
