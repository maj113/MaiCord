#pragma once
#include "maicord/core/base.hpp"

namespace maicord {

inline constexpr u32 kGatewayClientEventNameCapacity = 64;
inline constexpr u32 kGatewayClientEventCapacity = 256;
// Ordinary typed records, the mailbox ring and the UI pop scratch.
inline constexpr u32 kGatewayClientDispatchPayloadCapacity = 128 * 1024;

} // namespace maicord
