#include "gateway_payload_writer.hpp"

#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {

bool gateway_payload_append(GatewayPayloadWriter* writer, const void* source,
                            u32 bytes) noexcept {
    if (!writer || (!source && bytes != 0))
        return false;
    if (writer->used > writer->output.count || bytes > writer->output.count - writer->used) {
        writer->overflow = true;
        return false;
    }
    copy_bytes(writer->output.data + writer->used, source, bytes);
    writer->used += bytes;
    return true;
}

bool gateway_payload_append_u64(GatewayPayloadWriter* writer, u64 value) noexcept {
    u8 encoded[8];
    byte_store_le64(encoded, value);
    return gateway_payload_append(writer, encoded, sizeof(encoded));
}

} // namespace maicord
