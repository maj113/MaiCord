#include "maicord/protocol/guild_assets.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "json_write.hpp"

#include <Windows.h>

namespace maicord {
namespace {

using Op = GuildAssetOperation;

bool view_valid(Utf8View text) noexcept {
    return byte_range_valid({reinterpret_cast<const u8*>(text.data), text.bytes});
}

// Single-line UTF-8 with a codepoint range and no controls.
bool line_valid(Utf8View text, u32 minimum_codepoints, u32 maximum_codepoints) noexcept {
    if (!view_valid(text) || text.bytes > maximum_codepoints * 4)
        return false;
    const auto validation = utf8_validate(text);
    if (!validation.valid || validation.codepoints < minimum_codepoints || validation.codepoints > maximum_codepoints)
        return false;
    bool visible = !minimum_codepoints;
    for (u32 i = 0; i < text.bytes; ++i) {
        const u8 byte = static_cast<u8>(text.data[i]);
        if (byte < 0x20 || byte == 0x7F)
            return false;
        visible = visible || byte != ' ';
    }
    return visible;
}

bool targets_item(Op op) noexcept {
    return op == Op::role_icon || op == Op::emoji_update || op == Op::emoji_delete || op == Op::sticker_update ||
           op == Op::sticker_delete;
}

}

u32 guild_image_limit(GuildImageProfile profile) noexcept {
    switch (profile) {
    case GuildImageProfile::emoji: return kGuildEmojiImageMaximumBytes;
    case GuildImageProfile::role_icon: return kGuildRoleIconMaximumBytes;
    case GuildImageProfile::sticker: return kGuildStickerMaximumBytes;
    case GuildImageProfile::icon:
    case GuildImageProfile::banner:
    case GuildImageProfile::profile_banner: return kGuildImageMaximumBytes;
    default: return 0;
    }
}

bool guild_image_kind_allowed(GuildImageProfile profile, GuildImageKind kind) noexcept {
    switch (profile) {
    case GuildImageProfile::sticker:
        return kind == GuildImageKind::png || kind == GuildImageKind::apng || kind == GuildImageKind::gif ||
               kind == GuildImageKind::lottie;
    case GuildImageProfile::role_icon:
        return kind == GuildImageKind::png || kind == GuildImageKind::jpeg || kind == GuildImageKind::webp;
    case GuildImageProfile::icon:
    case GuildImageProfile::banner:
    case GuildImageProfile::profile_banner:
    case GuildImageProfile::emoji:
        return kind == GuildImageKind::png || kind == GuildImageKind::apng || kind == GuildImageKind::gif ||
               kind == GuildImageKind::jpeg || kind == GuildImageKind::webp;
    default: return false;
    }
}

namespace {

bool reason_valid(const GuildAssetRequest& request) noexcept {
    if (!view_valid(request.reason))
        return false;
    if (!request.reason.bytes)
        return true;
    if (request.operation == Op::image_probe || guild_asset_method(request.operation) == RestMethod::get)
        return false;
    u8 scratch[kRestAuditReasonMaximumEncodedBytes];
    const bool ok = rest_encode_audit_reason(request.reason, {scratch, sizeof(scratch)}).ok;
    SecureZeroMemory(scratch, sizeof(scratch));
    return ok;
}

// Byte sink that either writes or only measures, so sizing and writing share
// one serializer.
struct Sink {
    u8* data;
    u32 capacity, used;
    bool ok;
    bool room(u32 count) noexcept {
        if (!ok)
            return false;
        if (data ? count > capacity - used : count > ~0u - used)
            ok = false;
        return ok;
    }
    void bytes(const void* value, u32 count) noexcept {
        if (!room(count))
            return;
        if (data && count)
            copy_bytes(data + used, value, count);
        used += count;
    }
    template <usize N> void literal(const char (&text)[N]) noexcept {
        bytes(text, static_cast<u32>(N - 1));
    }
    void text(Utf8View value) noexcept {
        bytes(value.data, value.bytes);
    }
    void number(u64 value) noexcept {
        char digits[20];
        bytes(digits, decimal_write(digits, value));
    }
    void json_string(Utf8View value) noexcept {
        u32 encoded = 0;
        if (!ok || !protocol_detail::measure_json_string(value, &encoded)) {
            ok = false;
            return;
        }
        if (!room(encoded))
            return;
        if (data)
            protocol_detail::write_json_string(data + used, value);
        used += encoded;
    }
    void data_uri(GuildPreparedImage image) noexcept {
        const u32 needed = guild_image_data_uri_bytes(image.kind, image.bytes.count);
        if (!needed) {
            ok = false;
            return;
        }
        if (!room(needed))
            return;
        if (data && guild_image_write_data_uri(image.kind, image.bytes, {data + used, needed}) != needed) {
            ok = false;
            return;
        }
        used += needed;
    }
};

template <usize N> void image_object(Sink& s, const char (&key)[N], GuildPreparedImage image) noexcept {
    s.literal("{\"");
    s.literal(key);
    s.literal("\":");
    if (image.bytes.count) {
        s.literal("\"");
        s.data_uri(image);
        s.literal("\"");
    } else
        s.literal("null");
    s.literal("}");
}

Utf8View sticker_extension(GuildImageKind kind) noexcept {
    return kind == GuildImageKind::gif      ? utf8_literal("gif")
           : kind == GuildImageKind::lottie ? utf8_literal("json")
                                            : utf8_literal("png");
}

template <usize N> void part(Sink& s, ConstSlice<u8> boundary, const char (&name)[N], Utf8View value) noexcept {
    s.literal("--");
    s.bytes(boundary.data, boundary.count);
    s.literal("\r\nContent-Disposition: form-data; name=\"");
    s.literal(name);
    s.literal("\"\r\n\r\n");
    s.text(value);
    s.literal("\r\n");
}

void serialize(const GuildAssetRequest& r, GuildPreparedImage image, ConstSlice<u8> boundary, Sink& s) noexcept {
    switch (r.operation) {
    case Op::guild_icon:
    case Op::role_icon: image_object(s, "icon", image); break;
    case Op::guild_banner: image_object(s, "banner", image); break;
    case Op::emoji_create:
        s.literal("{\"name\":");
        s.json_string(r.name);
        s.literal(",\"image\":\"");
        s.data_uri(image);
        s.literal("\"}");
        break;
    case Op::emoji_update:
        s.literal("{\"name\":");
        s.json_string(r.name);
        s.literal("}");
        break;
    case Op::sticker_update: {
        bool first = true;
        const auto key = [&](Utf8View name) noexcept {
            s.bytes(first ? "{" : ",", 1);
            first = false;
            s.literal("\"");
            s.text(name);
            s.literal("\":");
        };
        if (r.fields & guild_asset_field_name) {
            key(utf8_literal("name"));
            s.json_string(r.name);
        }
        if (r.fields & guild_asset_field_tags) {
            key(utf8_literal("tags"));
            s.json_string(r.tags);
        }
        if (r.fields & guild_asset_field_description) {
            key(utf8_literal("description"));
            s.json_string(r.description);
        }
        s.literal("}");
        break;
    }
    case Op::sticker_create:
        part(s, boundary, "name", r.name);
        part(s, boundary, "tags", r.tags);
        part(s, boundary, "description", r.description);
        s.literal("--");
        s.bytes(boundary.data, boundary.count);
        s.literal("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"sticker.");
        s.text(sticker_extension(image.kind));
        s.literal("\"\r\nContent-Type: ");
        s.text(guild_image_mime(image.kind));
        s.literal("\r\n\r\n");
        s.bytes(image.bytes.data, image.bytes.count);
        s.literal("\r\n--");
        s.bytes(boundary.data, boundary.count);
        s.literal("--\r\n");
        break;
    case Op::guild_mfa:
        s.literal("{\"level\":");
        s.number(r.mfa_level);
        s.literal("}");
        break;
    case Op::guild_create:
        s.literal("{\"name\":");
        s.json_string(r.name);
        s.literal(",\"icon\":");
        if (image.bytes.count) {
            s.literal("\"");
            s.data_uri(image);
            s.literal("\"");
        } else
            s.literal("null");
        s.literal(",\"channels\":[],\"system_channel_id\":null,\"guild_template_code\":\"");
        s.literal(kGuildCreateTemplateCode);
        s.literal("\"}");
        break;
    default: s.ok = false; break;
    }
}

// The prepared image must suit the operation: required for creation,
// optional (clear) for icons, absent otherwise.
bool image_suits(const GuildAssetRequest& r, GuildPreparedImage image) noexcept {
    if (!byte_range_valid(image.bytes))
        return false;
    if (!guild_asset_has_image(r.operation) || r.operation == Op::image_probe)
        return !image.bytes.count && image.kind == GuildImageKind::none;
    const auto profile = guild_asset_profile(r);
    if (!image.bytes.count)
        return image.kind == GuildImageKind::none && r.operation != Op::emoji_create &&
               r.operation != Op::sticker_create;
    return image.bytes.count <= guild_image_limit(profile) && guild_image_kind_allowed(profile, image.kind);
}

bool contains(ConstSlice<u8> haystack, ConstSlice<u8> needle) noexcept {
    if (needle.count > haystack.count)
        return false;
    for (u32 i = 0; i + needle.count <= haystack.count; ++i)
        if (haystack[i] == needle[0] && bytes_equal(haystack.data + i, needle.data, needle.count))
            return true;
    return false;
}

// Decoders

constexpr u32 kDepth = 32;

template <typename Size> bool decode_text(JsonValue value, char* output, u32 capacity, Size* bytes) noexcept {
    if (value.kind == JsonValueKind::null_value) {
        *bytes = 0;
        return true;
    }
    JsonString encoded{};
    if (!json_value_as_string(value, &encoded))
        return false;
    const auto decoded = json_string_decode_utf8(encoded, {output, capacity});
    if (!decoded.ok() || !utf8_validate({output, decoded.bytes}).valid)
        return false;
    *bytes = static_cast<Size>(decoded.bytes);
    return true;
}

constexpr const char* kStickerFields[] = {"id", "name", "tags", "description", "format_type", "available"};
constexpr u32 kStickerFieldCount = sizeof(kStickerFields) / sizeof(kStickerFields[0]);

struct StickerParse {
    GuildStickerRecord value;
    u32 seen;
    GuildAssetError error;
};

bool field_name(JsonString name, const char* expected) noexcept {
    u32 bytes = 0;
    while (expected[bytes])
        ++bytes;
    return json_string_equals_ascii(name, {expected, bytes});
}

bool visit_sticker(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<StickerParse*>(opaque);
    u32 field = kStickerFieldCount;
    for (u32 i = 0; i < kStickerFieldCount && field == kStickerFieldCount; ++i)
        if (field_name(name, kStickerFields[i]))
            field = i;
    if (field == kStickerFieldCount)
        return true;
    if (p.seen & (1u << field)) {
        p.error = GuildAssetError::duplicate;
        return false;
    }
    p.seen |= 1u << field;
    auto& v = p.value;
    u64 number = 0;
    bool valid = true;
    switch (field) {
    case 0: valid = json_value_to_snowflake(value, &v.id); break;
    case 1: valid = decode_text(value, v.name, kGuildStickerNameBytes, &v.name_bytes) && v.name_bytes; break;
    case 2: valid = decode_text(value, v.tags, kGuildStickerTagsBytes, &v.tags_bytes); break;
    case 3: valid = decode_text(value, v.description, kGuildStickerDescriptionBytes, &v.description_bytes); break;
    case 4:
        valid = json_value_to_u64(value, &number) && number >= 1 && number <= 4;
        v.format_type = static_cast<u8>(number);
        break;
    case 5: valid = json_value_to_bool(value, &v.available); break;
    default: break;
    }
    if (!valid)
        p.error = GuildAssetError::field;
    return valid;
}

struct StickerList {
    const JsonIndexedDocument* document;
    Slice<GuildStickerRecord> output;
    u32 count;
    GuildAssetError error;
};

bool visit_sticker_item(void* opaque, u32, JsonValue value) noexcept {
    auto& list = *static_cast<StickerList*>(opaque);
    if (list.count >= list.output.count) {
        list.error = GuildAssetError::capacity;
        return false;
    }
    if (value.kind != JsonValueKind::object) {
        list.error = GuildAssetError::field;
        return false;
    }
    StickerParse parse{};
    parse.value.available = true;
    const auto read = json_visit_indexed_object(*list.document, value, kDepth, visit_sticker, &parse);
    if (!read.ok()) {
        list.error = parse.error != GuildAssetError::none ? parse.error : GuildAssetError::json;
        return false;
    }
    if (!(parse.seen & 1u) || !(parse.seen & 2u) || !parse.value.id) {
        list.error = GuildAssetError::identity;
        return false;
    }
    for (u32 i = 0; i < list.count; ++i)
        if (list.output[i].id == parse.value.id) {
            list.error = GuildAssetError::duplicate;
            return false;
        }
    list.output[list.count++] = parse.value;
    return true;
}

struct CodeParse {
    u64 code;
};

bool visit_code(void* opaque, JsonString name, JsonValue value) noexcept {
    if (json_string_equals_ascii(name, utf8_literal("code")))
        json_value_to_u64(value, &static_cast<CodeParse*>(opaque)->code);
    return true;
}

} // namespace

RestMethod guild_asset_method(GuildAssetOperation op) noexcept {
    switch (op) {
    case Op::guild_icon:
    case Op::guild_banner:
    case Op::role_icon:
    case Op::emoji_update:
    case Op::sticker_update: return RestMethod::patch;
    case Op::emoji_create:
    case Op::sticker_create:
    case Op::guild_mfa:
    case Op::guild_delete:
    case Op::guild_create: return RestMethod::post;
    case Op::emoji_delete:
    case Op::sticker_delete: return RestMethod::delete_method;
    default: return RestMethod::get;
    }
}

bool guild_asset_has_body(GuildAssetOperation op) noexcept {
    const auto method = guild_asset_method(op);
    // Deleting a server POSTs with no body.
    return op != Op::image_probe && op != Op::guild_delete && op < Op::count &&
           (method == RestMethod::patch || method == RestMethod::post);
}

bool guild_asset_has_image(GuildAssetOperation op) noexcept {
    return op == Op::guild_icon || op == Op::guild_banner || op == Op::role_icon || op == Op::emoji_create ||
           op == Op::sticker_create || op == Op::image_probe || op == Op::guild_create;
}

GuildImageProfile guild_asset_profile(const GuildAssetRequest& r) noexcept {
    switch (r.operation) {
    case Op::guild_icon:
    case Op::guild_create: return GuildImageProfile::icon;
    case Op::guild_banner: return GuildImageProfile::banner;
    case Op::role_icon: return GuildImageProfile::role_icon;
    case Op::emoji_create: return GuildImageProfile::emoji;
    case Op::sticker_create: return GuildImageProfile::sticker;
    case Op::image_probe: return r.profile < GuildImageProfile::count ? r.profile : GuildImageProfile::count;
    default: return GuildImageProfile::count;
    }
}

bool guild_asset_request_valid(const GuildAssetRequest& r) noexcept {
    const Op op = r.operation;
    if (op >= Op::count || r.reserved[0] || r.reserved[1] || r.reserved[2] || r.reserved[3] || !reason_valid(r) ||
        !view_valid(r.name) || !view_valid(r.tags) || !view_valid(r.description) || !byte_range_valid(r.image) ||
        !byte_range_valid({r.data_uri.data, r.data_uri.count}) || (op != Op::image_probe && r.data_uri.count))
        return false;
    if ((op == Op::image_probe) ? r.profile >= GuildImageProfile::count : r.profile != GuildImageProfile::icon)
        return false;
    // A new server has no ID yet; probes need none; everything else names its server.
    if (op == Op::guild_create ? r.guild_id != 0 : op != Op::image_probe && !r.guild_id)
        return false;
    if (targets_item(op) != (r.target_id != 0))
        return false;
    if ((op != Op::guild_mfa && r.mfa_level) || r.mfa_level > 1)
        return false;
    if (guild_asset_has_image(op)) {
        if (r.image.count > kGuildImageSourceMaximumBytes ||
            ((op == Op::emoji_create || op == Op::sticker_create || op == Op::image_probe) && !r.image.count))
            return false;
    } else if (r.image.count)
        return false;
    const bool named = op == Op::emoji_create || op == Op::emoji_update || op == Op::sticker_create ||
                       (op == Op::sticker_update && (r.fields & guild_asset_field_name));
    const bool emoji = op == Op::emoji_create || op == Op::emoji_update;
    if (op == Op::guild_create) {
        if (!line_valid(r.name, kGuildCreateNameMinimum, kGuildCreateNameMaximum))
            return false;
    } else if (named ? !(emoji ? guild_emoji_name_valid(r.name) : guild_sticker_name_valid(r.name))
                     : r.name.bytes != 0)
        return false;
    const bool tagged = op == Op::sticker_create || (op == Op::sticker_update && (r.fields & guild_asset_field_tags));
    if (tagged ? !guild_sticker_tags_valid(r.tags) : r.tags.bytes != 0)
        return false;
    const bool described =
        op == Op::sticker_create || (op == Op::sticker_update && (r.fields & guild_asset_field_description));
    if (described ? !guild_sticker_description_valid(r.description) : r.description.bytes != 0)
        return false;
    if (op == Op::sticker_update ? (!r.fields || (r.fields & ~guild_asset_field_all)) : r.fields != 0)
        return false;
    return true;
}

RestRouteKey guild_asset_route(const GuildAssetRequest& r) noexcept {
    const bool unscoped = r.operation == Op::guild_create;
    return {kGuildAssetRouteBase + static_cast<u64>(r.operation), unscoped ? 0 : r.guild_id,
            guild_asset_method(r.operation),
            unscoped ? RestMajorParameter::none : RestMajorParameter::guild};
}

GuildAssetResult guild_asset_write_target(const GuildAssetRequest& r, Slice<u8> output) noexcept {
    if (!guild_asset_request_valid(r) || r.operation == Op::image_probe || !byte_range_valid({output.data, output.count}))
        return {0, 0, GuildAssetError::input};
    u8 target[kGuildAssetMaximumTargetBytes];
    Sink s{target, sizeof(target), 0, true};
    s.literal("/api/v10/guilds");
    if (r.operation != Op::guild_create) {
        s.literal("/");
        s.number(r.guild_id);
    }
    switch (r.operation) {
    case Op::role_icon:
        s.literal("/roles/");
        s.number(r.target_id);
        break;
    case Op::emoji_create: s.literal("/emojis"); break;
    case Op::emoji_update:
    case Op::emoji_delete:
        s.literal("/emojis/");
        s.number(r.target_id);
        break;
    case Op::sticker_list:
    case Op::sticker_create: s.literal("/stickers"); break;
    case Op::sticker_update:
    case Op::sticker_delete:
        s.literal("/stickers/");
        s.number(r.target_id);
        break;
    case Op::guild_mfa: s.literal("/mfa"); break;
    case Op::guild_delete: s.literal("/delete"); break;
    default: break;
    }
    if (!s.ok)
        return {0, 0, GuildAssetError::capacity};
    if (output.count < s.used)
        return {s.used, 0, GuildAssetError::capacity};
    copy_bytes(output.data, target, s.used);
    return {s.used, 0, GuildAssetError::none};
}

GuildAssetResult guild_asset_write_reason(const GuildAssetRequest& r, Slice<u8> output) noexcept {
    if (!guild_asset_request_valid(r) || !byte_range_valid({output.data, output.count}))
        return {0, 0, GuildAssetError::input};
    if (!r.reason.bytes)
        return {0, 0, GuildAssetError::none};
    const auto encoded = rest_encode_audit_reason(r.reason, output);
    return encoded.ok ? GuildAssetResult{encoded.bytes, 0, GuildAssetError::none}
                      : GuildAssetResult{encoded.bytes, 0, GuildAssetError::capacity};
}

u32 guild_asset_body_bytes(const GuildAssetRequest& r, GuildPreparedImage image, ConstSlice<u8> boundary) noexcept {
    if (!guild_asset_request_valid(r) || !guild_asset_has_body(r.operation) || !image_suits(r, image) ||
        ((r.operation == Op::sticker_create) != (boundary.count != 0)) ||
        (boundary.count && !rest_multipart_boundary_valid(boundary)))
        return 0;
    Sink s{nullptr, 0, 0, true};
    serialize(r, image, boundary, s);
    return s.ok ? s.used : 0;
}

GuildAssetResult guild_asset_write_body(const GuildAssetRequest& r, GuildPreparedImage image, ConstSlice<u8> boundary,
                                        Slice<u8> output) noexcept {
    if (!guild_asset_request_valid(r) || !byte_range_valid({output.data, output.count}) || !image_suits(r, image) ||
        ((r.operation == Op::sticker_create) != (boundary.count != 0)) ||
        (boundary.count && !rest_multipart_boundary_valid(boundary)))
        return {0, 0, GuildAssetError::input};
    if (!guild_asset_has_body(r.operation))
        return {0, 0, GuildAssetError::none};
    if (!output.data || !output.count)
        return {0, 0, GuildAssetError::capacity};
    Sink s{output.data, output.count, 0, true};
    serialize(r, image, boundary, s);
    if (!s.ok) {
        SecureZeroMemory(output.data, minimum(s.used, output.count));
        return {0, 0, GuildAssetError::capacity};
    }
    return {s.used, 0, GuildAssetError::none};
}

bool guild_asset_choose_boundary(const GuildAssetRequest& r, GuildPreparedImage image, u64 seed,
                                 Slice<u8> output) noexcept {
    constexpr char prefix[] = "MaiCordFormBoundary";
    constexpr u32 prefix_bytes = sizeof(prefix) - 1;
    static_assert(prefix_bytes + 13 == kGuildAssetBoundaryBytes);
    if (!output.data || output.count < kGuildAssetBoundaryBytes || !byte_range_valid(image.bytes))
        return false;
    for (u32 attempt = 0; attempt < 32; ++attempt) {
        copy_bytes(output.data, prefix, prefix_bytes);
        u64 value = seed;
        for (u32 i = 0; i < 13; ++i) {
            output[prefix_bytes + 12 - i] = static_cast<u8>("0123456789abcdef"[value & 15]);
            value >>= 4;
        }
        const ConstSlice<u8> boundary{output.data, kGuildAssetBoundaryBytes};
        const auto bytes = [](Utf8View text) noexcept {
            return ConstSlice<u8>{reinterpret_cast<const u8*>(text.data), text.bytes};
        };
        if (!contains(image.bytes, boundary) && !contains(bytes(r.name), boundary) &&
            !contains(bytes(r.tags), boundary) && !contains(bytes(r.description), boundary))
            return true;
        // Odd multiplier: a different 52-bit tail on every attempt.
        seed = seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    }
    return false;
}

Utf8View guild_image_mime(GuildImageKind kind) noexcept {
    switch (kind) {
    case GuildImageKind::png:
    case GuildImageKind::apng: return utf8_literal("image/png");
    case GuildImageKind::gif: return utf8_literal("image/gif");
    case GuildImageKind::jpeg: return utf8_literal("image/jpeg");
    case GuildImageKind::webp: return utf8_literal("image/webp");
    case GuildImageKind::lottie: return utf8_literal("application/json");
    default: return {};
    }
}

u32 guild_image_data_uri_bytes(GuildImageKind kind, u32 image_bytes) noexcept {
    const auto mime = guild_image_mime(kind);
    const u32 encoded = base64_encoded_size(image_bytes);
    constexpr u32 framing = sizeof("data:;base64,") - 1;
    if (!mime.bytes || !image_bytes || !encoded || encoded > ~0u - framing - mime.bytes)
        return 0;
    return framing + mime.bytes + encoded;
}

u32 guild_image_write_data_uri(GuildImageKind kind, ConstSlice<u8> image, Slice<u8> output) noexcept {
    const u32 needed = guild_image_data_uri_bytes(kind, image.count);
    if (!needed || !byte_range_valid(image) || !output.data || output.count < needed)
        return 0;
    const auto mime = guild_image_mime(kind);
    Sink s{output.data, output.count, 0, true};
    s.literal("data:");
    s.text(mime);
    s.literal(";base64,");
    if (!s.ok)
        return 0;
    const auto encoded =
        base64_encode(image, {reinterpret_cast<char*>(output.data + s.used), output.count - s.used});
    if (!encoded.ok() || s.used + encoded.bytes != needed) {
        SecureZeroMemory(output.data, needed);
        return 0;
    }
    return needed;
}

bool guild_emoji_name_valid(Utf8View name) noexcept {
    if (!view_valid(name) || name.bytes < kGuildEmojiNameMinimum || name.bytes > kGuildEmojiNameMaximum)
        return false;
    for (u32 i = 0; i < name.bytes; ++i) {
        const char c = name.data[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

bool guild_sticker_name_valid(Utf8View name) noexcept {
    return line_valid(name, kGuildStickerNameMinimum, kGuildStickerNameMaximum);
}

bool guild_sticker_tags_valid(Utf8View tags) noexcept {
    return line_valid(tags, 1, kGuildStickerTagsMaximum);
}

bool guild_sticker_description_valid(Utf8View description) noexcept {
    return !description.bytes ? view_valid(description)
                              : line_valid(description, kGuildStickerDescriptionMinimum,
                                           kGuildStickerDescriptionMaximum);
}

GuildAssetResult guild_asset_decode_stickers(Utf8View body, Slice<u32> scratch,
                                             Slice<GuildStickerRecord> output) noexcept {
    if ((!output.data && output.count) || !view_valid(body) || !body.bytes || !scratch.data || !scratch.count ||
        !utf8_validate(body).valid)
        return {0, 0, GuildAssetError::input};
    const auto scanned = json_scan_structurals_avx1(body, scratch);
    if (!scanned.ok())
        return {0, 0, scanned.error == JsonScanError::output_overflow ? GuildAssetError::capacity
                                                                      : GuildAssetError::json};
    const JsonIndexedDocument document{body, {scratch.data, scanned.count}};
    StickerList list{&document, output, 0, GuildAssetError::none};
    const auto read = json_visit_root_array(body, document.structurals, kDepth, visit_sticker_item, &list);
    if (!read.ok())
        return {0, 0, list.error != GuildAssetError::none ? list.error : GuildAssetError::json};
    return {body.bytes, list.count, GuildAssetError::none};
}

u64 guild_asset_error_code(Utf8View body, Slice<u32> scratch) noexcept {
    if (!view_valid(body) || !body.bytes || !scratch.data || !scratch.count || !utf8_validate(body).valid)
        return 0;
    const auto scanned = json_scan_structurals_avx1(body, scratch);
    if (!scanned.ok())
        return 0;
    CodeParse parse{};
    if (!json_visit_root_object(body, {scratch.data, scanned.count}, kDepth, visit_code, &parse).ok())
        return 0;
    return parse.code;
}

} // namespace maicord
