#pragma once

#include "maicord/protocol/gateway_events.hpp"

namespace maicord::protocol_detail {

inline GatewayEventDecodeResult decode_failure(GatewayEventDecodeError error, u32 offset = 0,
                                               JsonScanError scan_error = JsonScanError::none,
                                               JsonReadError read_error = JsonReadError::none) noexcept {
    return GatewayEventDecodeResult{offset, error, scan_error, read_error};
}

// A rejected read: the error its visitor recorded, else `fallback`.
inline GatewayEventDecodeResult read_failure(
    GatewayEventDecodeError visitor_error, const JsonReadResult& read,
    GatewayEventDecodeError fallback = GatewayEventDecodeError::invalid_json) noexcept {
    return decode_failure(visitor_error != GatewayEventDecodeError::none ? visitor_error : fallback,
                          read.error_offset, JsonScanError::none, read.error);
}

// Marks a member as seen; a repeat is a duplicate_field error.
template <typename Seen>
inline bool mark_seen(Seen* seen, u64 bit, GatewayEventDecodeError* error) noexcept {
    if ((*seen & bit) != 0) {
        *error = GatewayEventDecodeError::duplicate_field;
        return false;
    }
    *seen |= static_cast<Seen>(bit);
    return true;
}

// mark_seen plus the member's validity: an invalid value records `invalid`.
template <typename Seen>
inline bool accept_member(Seen* seen, u64 bit, bool valid, GatewayEventDecodeError* error,
                          GatewayEventDecodeError invalid = GatewayEventDecodeError::invalid_data) noexcept {
    if (!mark_seen(seen, bit, error)) {
        return false;
    }
    if (!valid) {
        *error = invalid;
        return false;
    }
    return true;
}

} // namespace maicord::protocol_detail
