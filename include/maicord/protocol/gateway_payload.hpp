#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

struct GatewayEnvelope {
    JsonValue data;
    JsonString event_name;
    u64 sequence;
    u32 opcode;
    bool has_sequence;
    bool has_event_name;
};

enum class GatewayDecodeError : u8 {
    none,
    scan_failed,
    invalid_json,
    duplicate_field,
    invalid_opcode,
    invalid_sequence,
    invalid_event_name,
    missing_opcode,
    missing_data,
};

struct GatewayDecodeResult {
    u32 error_offset;
    GatewayDecodeError error;
    JsonScanError scan_error;
    JsonReadError read_error;

    constexpr bool ok() const noexcept {
        return error == GatewayDecodeError::none;
    }
};

// The scratch span is caller-owned and receives structural offsets. No input
// bytes are copied; returned views remain valid only as long as input does.
GatewayDecodeResult gateway_decode_envelope(Utf8View input, Slice<u32> structural_scratch,
                                            u32 maximum_depth, GatewayEnvelope* output) noexcept;

} // namespace maicord
