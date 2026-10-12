#pragma once
#include "maicord/app/gateway_client.hpp"

namespace maicord::guild_role_payload_bench {
inline constexpr u64 kObservationSeed = 14695981039346656037ull;
// Compile this implementation separately with /GL- and do not unity-build it.
// Exact complete field/name equality is checked outside timing by the driver.
u64 observe_role(u64 hash, u32 ordinal, const GatewayClientRoleMutation&, Utf8View) noexcept;
u64 observe_offsets(const GatewayClientEvent&, ConstSlice<u8>, ConstSlice<u32>) noexcept;
}
