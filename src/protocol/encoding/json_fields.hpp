#pragma once

#include "json_field_lists.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"

namespace maicord::protocol_detail {

// A member that exists and is not null.
inline bool filled(JsonValue v) noexcept {
    return v.source.data && v.kind != JsonValueKind::null_value;
}

// Fills a FieldSet from an object in one pass; a repeated named member fails.
// An absent or null object leaves every field empty (no source text).
__declspec(noinline) inline bool object_list(const JsonIndexedDocument& doc, const JsonValue& value, FieldValue* out,
                                             const u8* list) noexcept {
    if (!filled(value)) {
        for (u32 i = 0; i < list[0]; ++i)
            out[i] = FieldValue{};
        return true;
    }
    return fields_n(doc, value, 64, out, list[0], list + 1);
}
template <MemberName... Names>
__forceinline bool object(const JsonIndexedDocument& doc, const JsonValue& value, FieldSet<Names...>& set) noexcept {
    return object_list(doc, value, set.fields, MemberNames<Names...>::data.bytes);
}
// An integer sent as a JSON number or as a snowflake string; absent or null is 0.
__declspec(noinline) inline bool number(const JsonValue& v, u64* n) noexcept {
    *n = 0;
    if (!filled(v))
        return true;
    return v.kind == JsonValueKind::string ? json_value_to_snowflake(v, n) : json_value_to_u64(v, n);
}

// The settings-proto PATCH body {"settings":"<base64>","required_data_version":N}:
// seen bit 1 is settings (kept undecoded in encoded), bit 2 the version.
struct SettingsPatchEnvelope {
    Utf8View encoded;
    u8 seen;
};
inline bool settings_patch_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& result = *static_cast<SettingsPatchEnvelope*>(opaque);
    if (json_string_equals_ascii(key, utf8_literal("settings"))) {
        JsonString encoded{};
        if ((result.seen & 1) || !json_value_as_string(value, &encoded) || encoded.has_escapes)
            return false;
        result.seen |= 1;
        result.encoded = encoded.encoded;
        return true;
    }
    if (json_string_equals_ascii(key, utf8_literal("required_data_version"))) {
        u64 version = 0;
        if ((result.seen & 2) || !json_value_to_u64(value, &version) || version > ~u32{0})
            return false;
        result.seen |= 2;
        return true;
    }
    return false;
}
// The member bits of a body that holds only those two members (0 when it holds anything else or is malformed)
// and the undecoded settings.
__declspec(noinline) inline u8 settings_patch_members(Utf8View json, Utf8View* encoded) noexcept {
    u32 structurals[16];
    const auto scan = json_scan_structurals_avx1(json, {structurals, 16});
    SettingsPatchEnvelope parsed{};
    if (!scan.ok() || !json_visit_root_object(json, {structurals, scan.count}, 2, settings_patch_member, &parsed).ok())
        return 0;
    *encoded = parsed.encoded;
    return parsed.seen;
}

} // namespace maicord::protocol_detail
