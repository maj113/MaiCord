#include "maicord/protocol/gateway/client_status.hpp"

#include "../encoding/json_field_lists.hpp"

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

bool client_status_decode(const JsonIndexedDocument& document, JsonValue value, u8* packed) noexcept {
    *packed = 0;
    if (value.kind != JsonValueKind::object || !value.source.data)
        return value.kind != JsonValueKind::object;
    ClientStatusContext context{};
    if (!json_visit_indexed_object(document, value, 4, visit_platform, &context).ok())
        return false;
    *packed = context.packed;
    return true;
}

} // namespace maicord
