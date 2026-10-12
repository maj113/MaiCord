#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

struct GatewayPayloadWriter {
    Slice<char> output;
    u32 used;
    // Set when an append failed only because the output was full, so record
    // decoders can tell a capacity failure from a malformed record.
    bool overflow = false;
};

bool gateway_payload_append(GatewayPayloadWriter* writer, const void* source,
                            u32 bytes) noexcept;
bool gateway_payload_append_u64(GatewayPayloadWriter* writer, u64 value) noexcept;

} // namespace maicord
