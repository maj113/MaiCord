#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {

// Server images, expressions and owner actions. One request describes one
// REST call (or a local image probe); the account layer prepares the picked
// image, sizes the body and serializes it through guild_asset_write_*.
enum class GuildAssetOperation : u8 {
    guild_icon,     // PATCH  /guilds/{g}             {"icon":data-uri|null}
    guild_banner,   // PATCH  /guilds/{g}             {"banner":data-uri|null}
    role_icon,      // PATCH  /guilds/{g}/roles/{r}   {"icon":data-uri|null}
    emoji_create,   // POST   /guilds/{g}/emojis      {"name","image"}
    emoji_update,   // PATCH  /guilds/{g}/emojis/{e}  {"name"}
    emoji_delete,   // DELETE /guilds/{g}/emojis/{e}
    sticker_list,   // GET    /guilds/{g}/stickers
    sticker_create, // POST   /guilds/{g}/stickers    multipart name/tags/description/file
    sticker_update, // PATCH  /guilds/{g}/stickers/{s}
    sticker_delete, // DELETE /guilds/{g}/stickers/{s}
    guild_mfa,      // POST   /guilds/{g}/mfa         {"level"}
    guild_delete,   // POST   /guilds/{g}/delete      (no body)
    image_probe,    // local: prepares `image` for `profile`, no request
    // POST /guilds {"name","icon":data-uri|null,"channels":[],"system_channel_id":null,
    // "guild_template_code"}: a new server from the blank template. No
    // guild ID; the image (optional) is prepared like a server icon.
    guild_create,
    count,
};
// The "Create My Own" template, sent with every blank new server.
inline constexpr char kGuildCreateTemplateCode[] = "2TffvPucqHkN";
inline constexpr u32 kGuildCreateNameMinimum = 2;
inline constexpr u32 kGuildCreateNameMaximum = 100;

// Size and dimension rules the service applies to each image use.
enum class GuildImageProfile : u8 {
    icon,
    banner,
    emoji,
    role_icon,
    sticker,
    // Profile banners (account or per-server): 17:6, the 680x240 banner shape.
    profile_banner,
    count,
};

enum class GuildImageKind : u8 {
    none,
    png,
    apng,
    gif,
    jpeg,
    webp,
    lottie,
};

enum class GuildImageError : u8 {
    none,
    input,
    // Not a PNG, APNG, GIF, JPEG, WebP or (stickers) Lottie JSON file.
    format,
    // A format this use does not accept (e.g. JPEG stickers, animated role icons).
    kind,
    // Too small, or not exactly 320x320 for a sticker.
    dimensions,
    // Over the use's byte limit and not a static PNG that can be resized.
    too_large,
    decode,
    memory,
};

// Outcome of preparing a picked file: the source shape, and the bytes that
// would be uploaded (the source itself, or a cropped/resized PNG).
struct GuildImageInfo {
    u32 source_width;
    u32 source_height;
    u32 source_bytes;
    u32 width;
    u32 height;
    u32 bytes;
    GuildImageKind source_kind;
    GuildImageKind kind;
    GuildImageError error;
    bool animated;
    bool resized;
    u8 reserved[3];
};

inline constexpr u64 kGuildAssetRouteBase = 0x4741535345540000ull;
inline constexpr u32 kGuildAssetMaximumTargetBytes = 96;
inline constexpr u32 kGuildEmojiNameMinimum = 2;
inline constexpr u32 kGuildEmojiNameMaximum = 32;
inline constexpr u32 kGuildStickerNameMinimum = 2;
inline constexpr u32 kGuildStickerNameMaximum = 30;
inline constexpr u32 kGuildStickerTagsMaximum = 200;
inline constexpr u32 kGuildStickerDescriptionMinimum = 2;
inline constexpr u32 kGuildStickerDescriptionMaximum = 100;
inline constexpr u32 kGuildStickerNameBytes = kGuildStickerNameMaximum * 4;
inline constexpr u32 kGuildStickerTagsBytes = kGuildStickerTagsMaximum * 4;
inline constexpr u32 kGuildStickerDescriptionBytes = kGuildStickerDescriptionMaximum * 4;
inline constexpr u32 kGuildEmojiImageMaximumBytes = 256 * 1024;
inline constexpr u32 kGuildRoleIconMaximumBytes = 256 * 1024;
inline constexpr u32 kGuildStickerMaximumBytes = 512 * 1024;
inline constexpr u32 kGuildStickerDimension = 320;
// Server icon and banner uploads; the base64 body stays under the large REST ceiling.
inline constexpr u32 kGuildImageMaximumBytes = 8 * 1024 * 1024;
// Picked source files, before any resize.
inline constexpr u32 kGuildImageSourceMaximumBytes = 16 * 1024 * 1024;
inline constexpr u32 kGuildAssetBoundaryBytes = 32;

enum GuildAssetField : u8 {
    guild_asset_field_name = 1u << 0,
    guild_asset_field_tags = 1u << 1,
    guild_asset_field_description = 1u << 2,
    guild_asset_field_all = (1u << 3) - 1,
};

// ABI request. `image` holds the picked file's bytes for image operations
// (empty clears an icon, banner or role icon); the account layer prepares it.
// All views are borrowed for the call only.
struct GuildAssetRequest {
    GuildAssetOperation operation;
    // image_probe only.
    GuildImageProfile profile;
    // guild_mfa only: 0 none, 1 elevated.
    u8 mfa_level;
    // sticker_update: which of name/tags/description are sent.
    u8 fields;
    u8 reserved[4];
    u64 guild_id;
    // Role, emoji or sticker depending on the operation.
    u64 target_id;
    Utf8View name;
    Utf8View tags;
    Utf8View description;
    // Recorded in the audit log for mutations; empty sends no header.
    Utf8View reason;
    ConstSlice<u8> image;
    // Optional; filled by image operations and image_probe.
    GuildImageInfo* info;
    // image_probe only, optional: receives the prepared image as a data URI
    // of exactly guild_image_data_uri_bytes(info.kind, info.bytes) bytes.
    Slice<u8> data_uri;
};

// Prepared upload bytes.
struct GuildPreparedImage {
    ConstSlice<u8> bytes;
    GuildImageKind kind;
};

enum class GuildAssetError : u8 {
    none,
    input,
    capacity,
    json,
    duplicate,
    field,
    identity,
};

struct GuildAssetResult {
    u32 bytes;
    u32 count;
    GuildAssetError error;
    constexpr bool ok() const noexcept {
        return error == GuildAssetError::none;
    }
};

RestMethod guild_asset_method(GuildAssetOperation) noexcept;
bool guild_asset_has_body(GuildAssetOperation) noexcept;
bool guild_asset_has_image(GuildAssetOperation) noexcept;
// The image profile an operation uploads with; count when it carries none.
GuildImageProfile guild_asset_profile(const GuildAssetRequest&) noexcept;
bool guild_asset_request_valid(const GuildAssetRequest&) noexcept;
RestRouteKey guild_asset_route(const GuildAssetRequest&) noexcept;
GuildAssetResult guild_asset_write_target(const GuildAssetRequest&, Slice<u8> output) noexcept;
GuildAssetResult guild_asset_write_reason(const GuildAssetRequest&, Slice<u8> output) noexcept;
// Exact body size for this request and prepared image; zero when bodyless.
u32 guild_asset_body_bytes(const GuildAssetRequest&, GuildPreparedImage image,
                           ConstSlice<u8> boundary) noexcept;
// JSON (or multipart for sticker_create, which needs a boundary). Partial
// output is cleared on failure.
GuildAssetResult guild_asset_write_body(const GuildAssetRequest&, GuildPreparedImage image,
                                        ConstSlice<u8> boundary, Slice<u8> output) noexcept;
// A boundary of kGuildAssetBoundaryBytes derived from seed that occurs in
// none of the request's multipart values.
bool guild_asset_choose_boundary(const GuildAssetRequest&, GuildPreparedImage image, u64 seed,
                                 Slice<u8> output) noexcept;

// Upload byte limit and accepted kinds of each image use.
u32 guild_image_limit(GuildImageProfile profile) noexcept;
bool guild_image_kind_allowed(GuildImageProfile profile, GuildImageKind kind) noexcept;

// "data:<mime>;base64,<payload>". Returns bytes written, zero on failure.
u32 guild_image_data_uri_bytes(GuildImageKind kind, u32 image_bytes) noexcept;
u32 guild_image_write_data_uri(GuildImageKind kind, ConstSlice<u8> image, Slice<u8> output) noexcept;
Utf8View guild_image_mime(GuildImageKind kind) noexcept;

// Emoji names: 2-32 of [A-Za-z0-9_]. Sticker names 2-30 characters, tags
// 1-200, description empty or 2-100, all single-line.
bool guild_emoji_name_valid(Utf8View) noexcept;
bool guild_sticker_name_valid(Utf8View) noexcept;
bool guild_sticker_tags_valid(Utf8View) noexcept;
bool guild_sticker_description_valid(Utf8View) noexcept;

// GET /guilds/{g}/stickers. Strings are owned, not NUL-terminated.
struct GuildStickerRecord {
    u64 id;
    u16 name_bytes;
    u16 tags_bytes;
    u16 description_bytes;
    // 1 PNG, 2 APNG, 3 Lottie, 4 GIF.
    u8 format_type;
    bool available;
    char name[kGuildStickerNameBytes];
    char tags[kGuildStickerTagsBytes];
    char description[kGuildStickerDescriptionBytes];
};
// Rows beyond the first `count` are unspecified after failure.
GuildAssetResult guild_asset_decode_stickers(Utf8View body, Slice<u32> scratch,
                                             Slice<GuildStickerRecord> output) noexcept;
// Error code ("code") of a JSON error body; zero when absent or invalid.
u64 guild_asset_error_code(Utf8View body, Slice<u32> scratch) noexcept;

static_assert(sizeof(GuildImageInfo) == 32);
static_assert(sizeof(GuildAssetRequest) == 128);

} // namespace maicord
