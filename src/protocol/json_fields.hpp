#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord::protocol_detail {

// A member that exists and is not null.
inline bool filled(JsonValue v) noexcept {
    return v.source.data && v.kind != JsonValueKind::null_value;
}

// One named member of an object; value stays empty while the member is absent.
struct Field {
    Utf8View key;
    JsonValue value;
};
struct Fields {
    Field* data;
    u32 count;
};
__declspec(noinline) inline bool collect(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& f = *static_cast<Fields*>(opaque);
    for (u32 i = 0; i < f.count; ++i)
        if (json_string_equals_ascii(key, f.data[i].key)) {
            if (f.data[i].value.source.data)
                return false;
            f.data[i].value = value;
            break;
        }
    return true;
}
// Fills fields from an object in one pass; a repeated named member fails.
// An absent or null object leaves them empty.
template <u32 N> __declspec(noinline) bool object(const JsonIndexedDocument& doc, JsonValue value, Field (&fields)[N]) noexcept {
    if (!filled(value))
        return true;
    Fields context{fields, N};
    return json_visit_indexed_object(doc, value, 64, collect, &context).ok();
}
// An integer sent as a JSON number or as a snowflake string; absent or null is 0.
__declspec(noinline) inline bool number(JsonValue v, u64* n) noexcept {
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

} // namespace maicord::protocol_detail
