#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Identify's qos_token (discord_protos.qos_token.v1.QosToken), as the desktop
// client builds it: client_provided {is_active} and, once a READY carried
// one, derived (the server's DerivedQosData) re-encoded as protobuf-ts writes
// it: claims, signature and key_id when set, then unknown fields in order.
// Both are credentials of the session: never log them.
inline constexpr u32 kGatewayQosDerivedMaximumBytes = 512;
inline constexpr u32 kGatewayQosTokenMaximumBytes = 4 * ((kGatewayQosDerivedMaximumBytes + 12 + 2) / 3);

// READY's base64 qos_token re-encoded as DerivedQosData into output (*bytes,
// possibly zero for an empty message); false when it does not decode (the
// client then sends no derived part) or does not fit.
bool gateway_qos_derived_from_ready(Utf8View base64, Slice<u8> output, u32* bytes) noexcept;
// The base64 token, with derived when has_derived; zero when output is too small.
u32 gateway_qos_token_write(bool active, bool has_derived, ConstSlice<u8> derived, Slice<char> output) noexcept;

} // namespace maicord
