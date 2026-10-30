#pragma once

#include "maicord/core/json_reader.hpp"

namespace maicord {

struct GatewayEnvelope {
    JsonValue data;
    JsonString event_name;
    // The whole frame and its structural index (the caller's scratch). Valid only until something scans into
    // that scratch again (a route that scans before it declines an event invalidates it for the next route);
    // empty for an envelope built by hand.
    JsonIndexedDocument document;
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

// As gateway_decode_envelope for JSON whose structural index the caller already holds (etf_to_json_indexed),
// which is valid by construction: nothing is scanned or revalidated. Depth, the index and the envelope's own
// fields are still checked.
GatewayDecodeResult gateway_decode_envelope_indexed(Utf8View input, ConstSlice<u32> structurals,
                                                    u32 maximum_depth, GatewayEnvelope* output) noexcept;

} // namespace maicord
