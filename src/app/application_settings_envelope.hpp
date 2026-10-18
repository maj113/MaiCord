#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
// Zeroes and frees a committed block; null is ignored.
inline void dispose(void* data, u32 bytes) noexcept {
    if (data) {
        secure_clear_bytes(data, bytes);
        VirtualFree(data, 0, MEM_RELEASE);
    }
}
template <class T> T* committed_allocate(u32 bytes) noexcept {
    return static_cast<T*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}

using SettingsDecode = bool (*)(void* context, Utf8View settings) noexcept;
// Parses a user settings response, {"settings": <base64 proto>, "out_of_date": bool}, and passes the
// unescaped settings text to decode. False when the envelope is malformed or decode fails.
inline bool application_settings_envelope(Utf8View body, bool* out_of_date, SettingsDecode decode,
                                          void* context) noexcept {
    if (!body.data || !body.bytes || body.bytes > kRestClientMaximumResponseBytes)
        return false;
    struct Envelope {
        JsonString settings;
        bool settings_seen, out_of_date_seen, out_of_date;
    } envelope{};
    const auto member = +[](void* opaque, JsonString name, JsonValue value) noexcept {
        auto& found = *static_cast<Envelope*>(opaque);
        if (json_string_equals_ascii(name, utf8_literal("settings"))) {
            if (found.settings_seen || !json_value_as_string(value, &found.settings))
                return false;
            found.settings_seen = true;
        } else if (json_string_equals_ascii(name, utf8_literal("out_of_date"))) {
            if (found.out_of_date_seen || !json_value_to_bool(value, &found.out_of_date))
                return false;
            found.out_of_date_seen = true;
        }
        return true;
    };
    u32 indexes[256]{};
    const auto scanned = json_scan_structurals_avx1(body, {indexes, 256});
    if (!scanned.ok() || !json_visit_root_object(body, {indexes, scanned.count}, 16, member, &envelope).ok() ||
        !envelope.settings_seen)
        return false;
    *out_of_date = envelope.out_of_date;
    if (!envelope.settings.has_escapes)
        return decode(context, envelope.settings.encoded);
    const auto measured = json_string_decode_utf8(envelope.settings, {});
    if (measured.error != JsonStringDecodeError::output_too_small || !measured.bytes ||
        measured.bytes > kRestClientMaximumResponseBytes)
        return false;
    char* temporary = committed_allocate<char>(measured.bytes);
    if (!temporary)
        return false;
    const auto decoded = json_string_decode_utf8(envelope.settings, {temporary, measured.bytes});
    const bool ok = decoded.ok() && decode(context, {temporary, decoded.bytes});
    dispose(temporary, measured.bytes);
    return ok;
}
} // namespace maicord::app_detail
