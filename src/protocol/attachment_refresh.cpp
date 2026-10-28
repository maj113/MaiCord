#include "maicord/protocol/attachment_refresh.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"

namespace maicord {
namespace {

bool reference(Utf8View url, Slice<u8> target, AttachmentUrlResult* result,
               u32* path_bytes) noexcept {
    const auto parsed = parse_media_url(url, target);
    if (!parsed.ok() || (parsed.host != CdnHost::cdn && parsed.host != CdnHost::media)) return false;
    u32 end = 0;
    while (end < parsed.target_bytes && target[end] != '?') ++end;
    u32 slash = 1;
    while (slash < end && target[slash] != '/') ++slash;
    const Utf8View prefix{reinterpret_cast<const char*>(target.data), slash};
    if (!utf8_equal(prefix, utf8_literal("/attachments")) &&
        !utf8_equal(prefix, utf8_literal("/ephemeral-attachments"))) return false;
    ++slash;
    while (slash < end && target[slash] != '/') ++slash;
    u64 id = 0;
    for (++slash; slash < end && target[slash] != '/'; ++slash) {
        const u8 digit = static_cast<u8>(target[slash] - '0');
        if (digit > 9 || id > (~0ull - digit) / 10) return false;
        id = id * 10 + digit;
    }
    *result = parse_signed_attachment_url(url, id, target);
    *path_bytes = end;
    return result->ok();
}

struct RefreshJson {
    JsonString original;
    JsonString refreshed;
    bool list_seen;
    bool original_seen;
    bool refreshed_seen;
};

bool visit_pair(void* context, JsonString name, JsonValue value) noexcept {
    auto& pair = *static_cast<RefreshJson*>(context);
    if (json_string_equals_ascii(name, utf8_literal("original"))) {
        if (pair.original_seen) return false;
        pair.original_seen = true;
        return json_value_as_string(value, &pair.original);
    }
    if (json_string_equals_ascii(name, utf8_literal("refreshed"))) {
        if (pair.refreshed_seen) return false;
        pair.refreshed_seen = true;
        return json_value_as_string(value, &pair.refreshed);
    }
    return true;
}

bool visit_item(void* context, u32 index, JsonValue value) noexcept {
    if (index != 0 || value.kind != JsonValueKind::object) return false;
    u32 structural[64];
    const auto scan = json_scan_structurals_avx1(value.source, {structural, 64});
    return scan.ok() && json_visit_value_object(value, {structural, scan.count}, 8,
                                               visit_pair, context).ok();
}

bool visit_root(void* context, JsonString name, JsonValue value) noexcept {
    if (!json_string_equals_ascii(name, utf8_literal("refreshed_urls"))) return true;
    auto& pair = *static_cast<RefreshJson*>(context);
    if (pair.list_seen || value.kind != JsonValueKind::array) return false;
    pair.list_seen = true;
    u32 structural[96];
    const auto scan = json_scan_structurals_avx1(value.source, {structural, 96});
    return scan.ok() && json_visit_value_array(value, {structural, scan.count}, 8,
                                              visit_item, context).ok();
}
bool expiring(const AttachmentUrlResult& url, u64 now_seconds) noexcept {
    return url.expires_at_seconds <= now_seconds || url.expires_at_seconds - now_seconds <= 3600;
}
} // namespace

bool attachment_refresh_needed(Utf8View url, u64 now_seconds) noexcept {
    u8 target[kCdnMaximumTargetBytes];
    AttachmentUrlResult parsed{};
    u32 path = 0;
    return reference(url, {target, sizeof(target)}, &parsed, &path) && expiring(parsed, now_seconds);
}

u32 attachment_refresh_body(Utf8View url, Slice<u8> output) noexcept {
    u8 target[kCdnMaximumTargetBytes];
    AttachmentUrlResult parsed{};
    u32 path = 0;
    if (!reference(url, {target, sizeof(target)}, &parsed, &path)) return 0;
    ProtocolWriter writer{output, 0};
    return protocol_writer_append_literal(&writer, "{\"attachment_urls\":[") &&
           protocol_writer_append_json_string(&writer, url) &&
           protocol_writer_append_literal(&writer, "]}") ? writer.used : 0;
}

u32 attachment_refresh_response(Utf8View body, Utf8View original,
                                u64 now_seconds, Slice<char> output) noexcept {
    if (body.bytes > 16 * 1024 || !output.data) return 0;
    u32 structural[128];
    const auto scan = json_scan_structurals_avx1(body, {structural, 128});
    RefreshJson pair{};
    if (!scan.ok() || !json_visit_root_object(body, {structural, scan.count}, 8, visit_root, &pair).ok() ||
        !pair.original_seen || !pair.refreshed_seen) return 0;
    char decoded_original[kCdnMaximumTargetBytes];
    const auto decoded = json_string_decode_utf8(pair.original, {decoded_original, sizeof(decoded_original)});
    if (!decoded.ok() || !utf8_equal(original, {decoded_original, decoded.bytes})) return 0;
    const auto refreshed = json_string_decode_utf8(pair.refreshed, output);
    if (!refreshed.ok()) return 0;
    u8 before[kCdnMaximumTargetBytes], after[kCdnMaximumTargetBytes];
    AttachmentUrlResult old_url{}, new_url{};
    u32 old_path = 0, new_path = 0;
    if (!reference(original, {before, sizeof(before)}, &old_url, &old_path) ||
        !reference({output.data, refreshed.bytes}, {after, sizeof(after)}, &new_url, &new_path) ||
        expiring(new_url, now_seconds) ||
        !utf8_equal({reinterpret_cast<const char*>(before), old_path},
               {reinterpret_cast<const char*>(after), new_path})) return 0;
    return refreshed.bytes;
}
} // namespace maicord
