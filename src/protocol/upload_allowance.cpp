#include "maicord/protocol/upload_allowance.hpp"
#include "maicord/core/decimal.hpp"

#include <Windows.h>
namespace maicord {
namespace {
JsonValue find(const JsonIndexedDocument& document, JsonValue parent, Utf8View key) noexcept {
    struct Find { Utf8View key; JsonValue value; bool found; } context{key};
    const auto read = json_visit_indexed_object(document, parent, 64,
        +[](void* opaque, JsonString name, JsonValue value) noexcept {
            auto& found = *static_cast<Find*>(opaque);
            if (!json_string_equals_ascii(name, found.key)) return true;
            if (found.found) return false;
            found.found = true; found.value = value; return true;
        }, &context);
    return read.ok() ? context.value : JsonValue{};
}
struct Assignments { const JsonIndexedDocument* document; u32 variants[4]; u8 seen; };
bool assignment(void* opaque, u32, JsonValue value) noexcept {
    auto& context = *static_cast<Assignments*>(opaque);
    struct Fields { u64 hash, variant, flags; u8 seen; bool valid; } fields{0, 0, 0, 0, true};
    const auto parsed = json_visit_indexed_array(*context.document, value, 64,
        +[](void* opaque, u32 index, JsonValue value) noexcept {
            auto& f = *static_cast<Fields*>(opaque); if (index > 2) return true;
            u64 number = 0;
            if (index == 2 && value.kind == JsonValueKind::null_value) number = 0;
            else if (!json_value_to_u64(value, &number)) { f.valid = false; return true; }
            if (index == 0) f.hash = number; else if (index == 1) f.variant = number; else f.flags = number;
            f.seen |= static_cast<u8>(1u << index); return true;
        }, &fields);
    if (!parsed.ok()) return false;
    // Murmur3 hashes of the four assignment names the policy reads.
    constexpr u32 hashes[]{3942863072u, 587404649u, 981354709u, 3383307794u};
    for (u32 i = 0; i < 4; ++i) if (fields.hash == hashes[i]) {
        if (!fields.valid || (fields.seen & 3) != 3) return false;
        if (context.seen & (1u << i)) return false;
        context.seen |= static_cast<u8>(1u << i);
        // Eligibility-only assignments do not activate a client configuration.
        if (!(fields.flags & 8) && fields.variant <= MAXDWORD) context.variants[i] = static_cast<u32>(fields.variant);
    }
    return true;
}
}
bool upload_policy_state(JsonValue state, u64 user_id, Slice<u32> scratch, UploadPolicy* policy, bool* changed) noexcept {
    if (!policy || !changed || !user_id) return false;
    *changed = false;
    const auto scan = json_scan_structurals_avx1(state.source, scratch);
    if (!scan.ok()) return false;
    const JsonIndexedDocument document{state.source, {scratch.data, scan.count}};
    state.source_offset = 0;
    const auto apex = find(document, state, utf8_literal("apex_experiments"));
    if (!apex.source.bytes || apex.kind == JsonValueKind::null_value) return true;
    auto value = find(document, apex, utf8_literal("assignments"));
    if (value.kind != JsonValueKind::object) return false;
    value = find(document, value, utf8_literal("1"));
    if (!value.source.bytes) return true;
    char id[20]; const u32 id_bytes = decimal_write(id, user_id);
    value = find(document, value, {id, id_bytes});
    if (!value.source.bytes) return true;
    if (!upload_policy_decode(apex, user_id, scratch, policy)) return false;
    *changed = true; return true;
}
bool upload_policy_decode(JsonValue apex, u64 user_id, Slice<u32> scratch, UploadPolicy* policy) noexcept {
    if (!policy || !user_id) return false;
    *policy = {};
    if (!apex.source.bytes || apex.kind == JsonValueKind::null_value) return true;
    const auto scan = json_scan_structurals_avx1(apex.source, scratch);
    if (!scan.ok()) return false;
    const JsonIndexedDocument document{apex.source, {scratch.data, scan.count}};
    apex.source_offset = 0;
    auto value = find(document, apex, utf8_literal("assignments"));
    if (value.kind != JsonValueKind::object) return false;
    value = find(document, value, utf8_literal("1"));
    if (!value.source.bytes) return true;
    char id[20]; const u32 id_bytes = decimal_write(id, user_id);
    value = find(document, value, {id, id_bytes});
    if (!value.source.bytes) return true;
    value = find(document, value, utf8_literal("assignments"));
    if (value.kind != JsonValueKind::array) return false;
    Assignments assignments{&document};
    if (!json_visit_indexed_array(document, value, 64, assignment, &assignments).ok()) return false;
    const auto* variant = assignments.variants;
    if (variant[0] >= 1 && variant[0] <= 3) policy->floor_bytes = (10u + variant[0] * 5u) * kUploadMiB;
    if (variant[1] == 1) policy->floor_bytes = 20u * kUploadMiB;
    if (variant[2] == 1) policy->nitro_bytes = 1024u * kUploadMiB;
    policy->guild_250_disabled = variant[3] == 1;
    return true;
}
}
