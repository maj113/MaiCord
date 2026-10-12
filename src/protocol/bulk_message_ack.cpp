#include "maicord/protocol/bulk_message_ack.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/protocol/writer.hpp"

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
    const ConstSlice<u8> sources[]{{reinterpret_cast<const u8*>(entries.data),
                                    entries.count * static_cast<u32>(sizeof(BulkMessageAckEntry))},
                                   {reinterpret_cast<const u8*>(&entries), sizeof(entries)},
                                   {reinterpret_cast<const u8*>(&output), sizeof(output)}};
    const ConstSlice<u8> destination{output.data, output.count};
    if (!byte_range_valid(destination))
        return fail(BulkMessageAckWriteError::invalid_input);
    for (u32 i = 0; i < 3; ++i) {
        if (!byte_range_valid(sources[i]) || byte_ranges_overlap(sources[i], destination))
            return fail(BulkMessageAckWriteError::invalid_input);
        for (u32 j = 0; j < i; ++j)
            if (byte_ranges_overlap(sources[i], sources[j]))
                return fail(BulkMessageAckWriteError::invalid_input);
    }
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
    ProtocolWriter writer{output, 0};
    bool valid = protocol_writer_append_literal(&writer, "{\"read_states\":[");
    for (u32 i = 0; valid && i < entries.count; ++i) {
        const auto& entry = entries.data[i];
        valid = (!i || protocol_writer_append_literal(&writer, ",")) &&
                protocol_writer_append_literal(&writer, "{\"channel_id\":\"") &&
                protocol_writer_append_u64(&writer, entry.channel_id) &&
                protocol_writer_append_literal(&writer, "\",\"message_id\":\"") &&
                protocol_writer_append_u64(&writer, entry.message_id) &&
                protocol_writer_append_literal(&writer, "\",\"read_state_type\":0}");
    }
    valid = valid && protocol_writer_append_literal(&writer, "]}");
    return valid ? BulkMessageAckWriteResult{writer.used, BulkMessageAckWriteError::none}
                 : fail(BulkMessageAckWriteError::capacity);
}
} // namespace maicord
