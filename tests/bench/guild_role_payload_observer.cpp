#include "guild_role_payload_observer.hpp"

namespace maicord::guild_role_payload_bench {
__declspec(noinline) u64 observe_role(u64 hash, u32 ordinal,
    const GatewayClientRoleMutation& role, Utf8View name) noexcept {
    const u64 edge = name.bytes ? static_cast<u8>(name.data[0]) |
        (static_cast<u64>(static_cast<u8>(name.data[name.bytes - 1])) << 8) : 0;
    return (hash ^ ordinal ^ role.id ^ role.permissions ^ role.position ^
        (static_cast<u64>(role.color) << 32) ^ role.flags ^ name.bytes ^ edge) * 1099511628211ull;
}
__declspec(noinline) u64 observe_offsets(const GatewayClientEvent& event, ConstSlice<u8> payload,
    ConstSlice<u32> offsets) noexcept {
    // Opaque pointer escape keeps every table entry externally observable under
    // LTCG; cheap edge consumption avoids timing a second complete table hash.
    const u64 header = kObservationSeed ^ event.state.role_count ^ payload.count;
    return offsets.count ? header ^ offsets.count ^ offsets[0] ^
        (static_cast<u64>(offsets[offsets.count - 1]) << 32) : header;
}
}
