#include "maicord/protocol/client_status.hpp"

#include "maicord/core/json_scan.hpp"
#include "json_field_lists.hpp"

namespace maicord {
static_assert(kClientPlatformCount == 4);
namespace {

struct ClientStatusContext {
    u8 packed;
    u8 seen;
};

bool visit_platform(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& context = *static_cast<ClientStatusContext*>(opaque);
    const u32 platform = protocol_detail::member_index<"desktop", "mobile", "web", "embedded">(name);
    if (platform == kClientPlatformCount)
        return true;
    const u8 bit = static_cast<u8>(1u << platform);
    if (context.seen & bit)
        return false;
    context.seen |= bit;
    JsonString text{};
    if (!json_value_as_string(value, &text))
        return true;
    ClientPlatformStatus status = ClientPlatformStatus::offline;
    switch (protocol_detail::member_index<"online", "idle", "dnd">(text)) {
    case 0: status = ClientPlatformStatus::online; break;
    case 1: status = ClientPlatformStatus::idle; break;
    case 2: status = ClientPlatformStatus::dnd; break;
    default: break;
    }
    context.packed = client_status_with(context.packed, static_cast<ClientPlatform>(platform), status);
    return true;
}

} // namespace

bool client_status_decode(JsonValue value, u8* packed) noexcept {
    *packed = 0;
    if (value.kind != JsonValueKind::object || !value.source.data)
        return value.kind != JsonValueKind::object;
    // Four platforms and their values: a small fixed index suffices; a larger
    // object is not a client_status the server sends.
    u32 structurals[64];
    const JsonScanResult scan = json_scan_structurals_avx1(value.source, Slice<u32>{structurals, 64});
    if (!scan.ok())
        return false;
    ClientStatusContext context{};
    if (!json_visit_value_object(value, ConstSlice<u32>{structurals, scan.count}, 4, visit_platform, &context)
             .ok())
        return false;
    *packed = context.packed;
    return true;
}

} // namespace maicord
