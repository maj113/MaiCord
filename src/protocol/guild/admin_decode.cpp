#include "maicord/protocol/guild/admin.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/protocol/guild/invite.hpp"
#include "field_table.hpp"


namespace maicord {
namespace {

using namespace protocol_detail;
using Error = GuildAdminError;

constexpr u8 kDepth = 32;
constexpr ErrorCodes kCodes{static_cast<u8>(Error::duplicate), static_cast<u8>(Error::field),
                            static_cast<u8>(Error::json), static_cast<u8>(Error::identity),
                            static_cast<u8>(Error::capacity)};

// The first error of a decode is kept.
bool fail(u8* error, u8 value = kCodes.field) noexcept {
    if (!*error)
        *error = value;
    return false;
}

bool decode_u32(JsonValue value, u32* output, u64 maximum = 0xFFFFFFFFull) noexcept {
    u64 number = 0;
    if (!json_value_to_u64(value, &number) || number > maximum)
        return false;
    *output = static_cast<u32>(number);
    return true;
}

// The object `value` read through `spec` into `base`.
u8 read(const JsonIndexedDocument& document, const ObjectSpec& spec, void* base, const JsonValue& value) noexcept {
    ObjectReader r{{&document, 0}, &spec, static_cast<u8*>(base), 0};
    return read_object(r, value);
}

// User --------------------------------------------------------------------

// "0" (migrated username) or up to four digits.
bool decode_discriminator(JsonValue value, u16* output) noexcept {
    *output = 0;
    if (value.kind == JsonValueKind::null_value)
        return true;
    char digits[8];
    u16 bytes = 0;
    if (!decode_text(value, digits, sizeof(digits), &bytes, false) || !bytes || bytes > 4)
        return false;
    u32 parsed = 0;
    for (u16 i = 0; i < bytes; ++i) {
        if (digits[i] < '0' || digits[i] > '9')
            return false;
        parsed = parsed * 10 + static_cast<u32>(digits[i] - '0');
    }
    *output = static_cast<u16>(parsed);
    return true;
}

bool user_hook(ObjectReader& r, u32, const JsonValue& value) noexcept {
    return decode_discriminator(value, &reinterpret_cast<GuildAdminUser*>(r.base)->discriminator);
}
constexpr FieldDef kUserDefs[]{
    {"id", MAICORD_FIELD_SNOWFLAKE(GuildAdminUser, id)},
    {"username", MAICORD_FIELD_TEXT(GuildAdminUser, username, username_bytes)},
    {"global_name", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminUser, display, display_bytes)},
    {"avatar", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminUser, avatar, avatar_bytes)},
    {"discriminator", MAICORD_FIELD_HOOK},
};
MAICORD_FIELD_TABLE(kUserSpec, kUserDefs, user_hook, kCodes, kDepth);

bool decode_user(const JsonIndexedDocument& document, JsonValue value, GuildAdminUser* user, u8* error) noexcept {
    if (value.kind != JsonValueKind::object)
        return fail(error);
    reset_value(user);
    const u8 result = read(document, kUserSpec, user, value);
    return result == 0 ? user->id || fail(error, kCodes.identity) : fail(error, result);
}

// Widget and server profile -------------------------------------------------

constexpr FieldDef kWidgetDefs[]{
    {"enabled", MAICORD_FIELD_BOOL(GuildAdminWidget, enabled)},
    {"channel_id", MAICORD_FIELD_SNOWFLAKE_OR_NULL(GuildAdminWidget, channel_id)},
};
MAICORD_FIELD_TABLE(kWidgetSpec, kWidgetDefs, nullptr, kCodes, kDepth, 1);

struct TraitBuild : GuildAdminProfileTrait {
    u32 position;
};
constexpr FieldDef kTraitDefs[]{
    {"label", MAICORD_FIELD_TEXT_OR_NULL(TraitBuild, label, label_bytes)},
    {"position", MAICORD_FIELD_NUMBER(TraitBuild, position)},
    {"emoji_id", MAICORD_FIELD_SNOWFLAKE_OR_NULL(TraitBuild, emoji_id)},
    {"emoji_name", MAICORD_FIELD_TEXT_OR_NULL(TraitBuild, emoji_name, emoji_name_bytes)},
    {"emoji_animated", MAICORD_FIELD_BOOL_OR_NULL(TraitBuild, emoji_animated)},
};
MAICORD_FIELD_TABLE(kTraitSpec, kTraitDefs, nullptr, kCodes, kDepth);

// Traits outside positions 0-4 are dropped, as the official client reads them.
bool visit_trait_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ObjectReader*>(opaque);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    TraitBuild trait{};
    trait.position = ~0u;
    const u8 result = read(*p.document, kTraitSpec, &trait, value);
    if (result)
        return fail(&p.error, result);
    if (trait.position < kGuildAdminProfileTraits)
        reinterpret_cast<GuildAdminProfile*>(p.base)->traits[trait.position] = trait;
    return true;
}

bool visit_game_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ObjectReader*>(opaque);
    auto& v = *reinterpret_cast<GuildAdminProfile*>(p.base);
    if (v.game_count >= kGuildAdminProfileGames)
        return fail(&p.error, kCodes.capacity);
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id) || !id)
        return fail(&p.error);
    v.game_application_ids[v.game_count++] = id;
    return true;
}

bool visit_feature_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ObjectReader*>(opaque);
    JsonString name{};
    if (json_value_as_string(value, &name) && json_string_equals_ascii(name, utf8_literal("DISCOVERABLE")))
        reinterpret_cast<GuildAdminProfile*>(p.base)->discoverable = true;
    return true;
}

// An unknown or null visibility leaves it unknown; the other known flags follow what the key held.
bool profile_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    auto& v = *reinterpret_cast<GuildAdminProfile*>(r.base);
    const bool present = value.kind != JsonValueKind::null_value;
    u64 number = 0;
    JsonArrayVisitor items = nullptr;
    switch (index) {
    case 0:
        if (json_value_to_u64(value, &number) && number <= 255) {
            v.visibility = static_cast<u8>(number);
            v.visibility_known = true;
        }
        return true;
    case 1: v.name_known = present; return true;
    case 3: v.icon_known = true; return true;
    case 4: v.custom_banner_known = true; return true;
    case 12: v.badge_known = present; return true;
    case 6: items = visit_trait_item; break;
    case 7: items = visit_game_item; break;
    default: items = visit_feature_item; break;
    }
    return value.kind == JsonValueKind::array && json_visit_indexed_array(*r.document, value, kDepth, items, &r).ok();
}
constexpr FieldDef kProfileDefs[]{
    {"visibility", MAICORD_FIELD_HOOK_OR_NULL},
    {"name", MAICORD_TEXT_ROW(field_text, kFieldNull | kFieldNotify, GuildAdminProfile, name, name_bytes)},
    {"description", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminProfile, description, description_bytes)},
    {"icon_hash", MAICORD_TEXT_ROW(field_text, kFieldNull | kFieldNotify, GuildAdminProfile, icon, icon_bytes)},
    {"custom_banner_hash", MAICORD_TEXT_ROW(field_text, kFieldNull | kFieldNotify, GuildAdminProfile, custom_banner, custom_banner_bytes)},
    {"brand_color_primary", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminProfile, brand_color, brand_color_bytes)},
    {"traits", MAICORD_FIELD_HOOK_OR_NULL},
    {"game_application_ids", MAICORD_FIELD_HOOK_OR_NULL},
    {"features", MAICORD_FIELD_HOOK_OR_NULL},
    {"online_count", MAICORD_FIELD_NUMBER_OR_NULL(GuildAdminProfile, online_count)},
    {"member_count", MAICORD_FIELD_NUMBER_OR_NULL(GuildAdminProfile, member_count)},
    {"tag", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminProfile, tag, tag_bytes)},
    {"badge", MAICORD_ROW(field_number, kFieldNull | kFieldNotify, GuildAdminProfile, badge)},
    {"badge_color_primary", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminProfile, badge_color_primary, badge_color_primary_bytes)},
    {"badge_color_secondary", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminProfile, badge_color_secondary, badge_color_secondary_bytes)},
    {"badge_hash", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminProfile, badge_hash, badge_hash_bytes)},
};
MAICORD_FIELD_TABLE(kProfileSpec, kProfileDefs, profile_hook, kCodes, kDepth);

// Games ---------------------------------------------------------------------

// An image of a game: a hash string, or {"type":"hash"|"url","value"}. One
// that does not fit or has another type is left out, never an error.
struct GameImage {
    char* text;
    u16* bytes;
    bool* url;
};
struct GameAssetReader : ObjectReader {
    GameImage image;
    u32 type; // 0 none, 1 hash, 2 url, 3 other
    bool value;
};
bool game_asset_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    auto& p = static_cast<GameAssetReader&>(r);
    if (index == 0) {
        JsonString type{};
        p.type = !json_value_as_string(value, &type)                  ? 3
                 : json_string_equals_ascii(type, utf8_literal("hash")) ? 1
                 : json_string_equals_ascii(type, utf8_literal("url"))  ? 2
                                                                        : 3;
    } else {
        u16 bytes = 0;
        p.value = value.kind != JsonValueKind::null_value &&
                  decode_text(value, p.image.text, kGuildAdminGameImageBytes, &bytes, false);
        if (p.value)
            *p.image.bytes = bytes;
    }
    return true;
}
constexpr FieldDef kGameAssetDefs[]{{"type", MAICORD_FIELD_HOOK}, {"value", MAICORD_FIELD_HOOK}};
MAICORD_FIELD_TABLE(kGameAssetSpec, kGameAssetDefs, game_asset_hook, kCodes, kDepth);

void decode_game_image(const JsonIndexedDocument& document, JsonValue value, GameImage image) noexcept {
    if (value.kind == JsonValueKind::string) {
        u16 bytes = 0;
        if (decode_text(value, image.text, kGuildAdminGameImageBytes, &bytes, false)) {
            *image.bytes = bytes;
            *image.url = false;
        }
        return;
    }
    if (value.kind != JsonValueKind::object)
        return;
    char text[kGuildAdminGameImageBytes];
    u16 bytes = 0;
    GameAssetReader p{{{&document, 0}, &kGameAssetSpec, nullptr, 0}, {text, &bytes, nullptr}, 0, false};
    if (read_object(p, value) != 0 || !p.value || !bytes || (p.type != 1 && p.type != 2))
        return;
    copy_bytes(image.text, text, bytes);
    *image.bytes = bytes;
    *image.url = p.type == 2;
}

bool game_media_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    auto& g = *reinterpret_cast<GuildAdminGame*>(r.base);
    // The documented hashes win over the newer media.
    if (index == 0 && !g.icon_bytes)
        decode_game_image(*r.document, value, {g.icon, &g.icon_bytes, &g.icon_url});
    else if (index == 1 && !g.cover_bytes)
        decode_game_image(*r.document, value, {g.cover, &g.cover_bytes, &g.cover_url});
    return true;
}
constexpr FieldDef kGameMediaDefs[]{{"icon", MAICORD_FIELD_HOOK}, {"cover", MAICORD_FIELD_HOOK}};
MAICORD_FIELD_TABLE(kGameMediaSpec, kGameMediaDefs, game_media_hook, kCodes, kDepth);

bool game_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    auto& g = *reinterpret_cast<GuildAdminGame*>(r.base);
    switch (index) {
    case 0: return json_value_to_snowflake(value, &g.id) || json_value_to_u64(value, &g.id);
    case 1: g.name_bytes = static_cast<u8>(clip_copy(value, g.name, kGuildAdminGameNameBytes, 512)); break;
    case 2:
    case 5:
        if (value.kind == JsonValueKind::string) {
            g.icon_bytes = 0;
            decode_game_image(*r.document, value, {g.icon, &g.icon_bytes, &g.icon_url});
        } else if (index == 5 && !g.icon_bytes)
            decode_game_image(*r.document, value, {g.icon, &g.icon_bytes, &g.icon_url});
        break;
    case 3:
        if (value.kind == JsonValueKind::string) {
            g.cover_bytes = 0;
            decode_game_image(*r.document, value, {g.cover, &g.cover_bytes, &g.cover_url});
        }
        break;
    default:
        if (value.kind == JsonValueKind::object)
            read(*r.document, kGameMediaSpec, &g, value);
        break;
    }
    return true;
}
constexpr FieldDef kGameDefs[]{
    {"id", MAICORD_FIELD_HOOK},         {"name", MAICORD_FIELD_HOOK}, {"icon_hash", MAICORD_FIELD_HOOK},
    {"cover_image_hash", MAICORD_FIELD_HOOK}, {"media", MAICORD_FIELD_HOOK}, {"icon", MAICORD_FIELD_HOOK},
};
MAICORD_FIELD_TABLE(kGameSpec, kGameDefs, game_hook, kCodes, kDepth);

// Items without an id are skipped.
bool visit_game_object(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ItemReader*>(opaque);
    if (value.kind != JsonValueKind::object)
        return true;
    GuildAdminGame game{};
    if (read(*p.document, kGameSpec, &game, value) != 0 || !game.id)
        return true;
    if (p.count >= p.capacity)
        return fail(&p.error, kCodes.capacity);
    copy_bytes(p.rows + p.count++ * sizeof(game), &game, sizeof(game));
    return true;
}

constexpr FieldDef kActivityDefs[]{
    {"game_application_id", MAICORD_FIELD_SNOWFLAKE(GuildAdminGameActivity, application_id)},
    {"activity_level", MAICORD_FIELD_NUMBER_OR_NULL(GuildAdminGameActivity, level)},
    {"activity_score", MAICORD_FIELD_NUMBER_OR_NULL(GuildAdminGameActivity, score)},
};
MAICORD_FIELD_TABLE(kActivitySpec, kActivityDefs, nullptr, kCodes, kDepth);

bool visit_activity_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ItemReader*>(opaque);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    GuildAdminGameActivity item{};
    const u8 result = read(*p.document, kActivitySpec, &item, value);
    if (result)
        return fail(&p.error, result);
    if (!item.application_id)
        return fail(&p.error, kCodes.identity);
    // The server's top 20; more than the caller holds keeps the first ones.
    if (p.count < p.capacity)
        copy_bytes(p.rows + p.count++ * sizeof(item), &item, sizeof(item));
    return true;
}
bool visit_top_games(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ItemReader*>(opaque);
    if (!name_is(name, "top_games"))
        return true;
    return value.kind == JsonValueKind::null_value ||
           (value.kind == JsonValueKind::array &&
            json_visit_indexed_array(*p.document, value, kDepth, visit_activity_item, &p).ok()) ||
           fail(&p.error);
}

constexpr FieldDef kVanityDefs[]{
    {"code", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminVanity, code, code_bytes)},
    {"uses", MAICORD_FIELD_NUMBER_OR_NULL(GuildAdminVanity, uses)},
};
MAICORD_FIELD_TABLE(kVanitySpec, kVanityDefs, nullptr, kCodes, kDepth);

// Roles -------------------------------------------------------------------

// permissions (a decimal string), and the colour, which holds 24 bits.
bool role_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    auto& role = *reinterpret_cast<GuildAdminRole*>(r.base);
    JsonString text{};
    return index == 4 ? role.color <= 0xFFFFFFu
                      : json_value_as_string(value, &text) && json_string_to_u64(text, &role.permissions);
}
constexpr FieldDef kRoleDefs[]{
    {"id", MAICORD_FIELD_SNOWFLAKE(GuildAdminRole, id)},
    {"name", MAICORD_FIELD_TEXT(GuildAdminRole, name, name_bytes)},
    {"permissions", MAICORD_FIELD_HOOK},
    {"position", MAICORD_FIELD_NUMBER(GuildAdminRole, position)},
    {"color", MAICORD_ROW(field_number, kFieldNotify, GuildAdminRole, color)},
    {"hoist", MAICORD_FIELD_BOOL(GuildAdminRole, hoist)},
    {"managed", MAICORD_FIELD_BOOL(GuildAdminRole, managed)},
    {"mentionable", MAICORD_FIELD_BOOL(GuildAdminRole, mentionable)},
    {"icon", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminRole, icon, icon_bytes)},
    {"unicode_emoji", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminRole, emoji, emoji_bytes)},
    {"flags", MAICORD_FIELD_NUMBER(GuildAdminRole, flags)},
};
MAICORD_FIELD_TABLE(kRoleSpec, kRoleDefs, role_hook, kCodes, kDepth, 1 | 2 | 4 | 8);

// Role member counts: {"<role id>": <count>, ...}
bool visit_role_count(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ItemReader*>(opaque);
    if (p.count >= p.capacity)
        return fail(&p.error, kCodes.capacity);
    u64 id = 0;
    u32 members = 0;
    if (!json_string_to_u64(name, &id) || !id || !decode_u32(value, &members))
        return fail(&p.error);
    auto* rows = reinterpret_cast<GuildAdminRoleCount*>(p.rows);
    for (u32 i = 0; i < p.count; ++i)
        if (rows[i].role_id == id)
            return fail(&p.error, kCodes.duplicate);
    rows[p.count++] = {id, members};
    return true;
}

// Bans --------------------------------------------------------------------

bool ban_hook(ObjectReader& r, u32, const JsonValue& value) noexcept {
    return decode_user(*r.document, value, &reinterpret_cast<GuildAdminBan*>(r.base)->user, &r.error);
}
constexpr FieldDef kBanDefs[]{
    {"user", MAICORD_FIELD_HOOK},
    {"reason", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminBan, reason, reason_bytes)},
};
// Decoded straight into the next slot; it counts only once accepted.
MAICORD_FIELD_TABLE(kBanSpec, kBanDefs, ban_hook, kCodes, kDepth, 1, sizeof(GuildAdminBan),
                    static_cast<u16>(__builtin_offsetof(GuildAdminBan, user.id)), 8);

// Audit log ---------------------------------------------------------------

constexpr FieldDef kAuditEntryDefs[]{
    {"id", MAICORD_FIELD_SNOWFLAKE(GuildAdminAuditEntry, id)},
    {"user_id", MAICORD_FIELD_SNOWFLAKE_OR_NULL(GuildAdminAuditEntry, user_id)},
    {"target_id", MAICORD_FIELD_SNOWFLAKE_OR_NULL(GuildAdminAuditEntry, target_id)},
    {"action_type", MAICORD_FIELD_NUMBER(GuildAdminAuditEntry, action_type)},
    {"reason", MAICORD_FIELD_TEXT_OR_NULL(GuildAdminAuditEntry, reason, reason_bytes)},
};
MAICORD_FIELD_TABLE(kAuditEntrySpec, kAuditEntryDefs, nullptr, kCodes, kDepth, 1 | 8, sizeof(GuildAdminAuditEntry),
                    static_cast<u16>(__builtin_offsetof(GuildAdminAuditEntry, id)), 8);

// The root: its two arrays, read into the caller's page.
struct AuditReader : ObjectReader {
    GuildAdminAuditPage page;
};

bool visit_audit_user_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<AuditReader*>(opaque);
    if (p.page.user_count >= p.page.users.count)
        return fail(&p.error, kCodes.capacity);
    GuildAdminUser user;
    if (!decode_user(*p.document, value, &user, &p.error))
        return false;
    for (u32 i = 0; i < p.page.user_count; ++i)
        if (p.page.users[i].id == user.id)
            return fail(&p.error, kCodes.duplicate);
    p.page.users[p.page.user_count++] = user;
    return true;
}

bool audit_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    auto& p = static_cast<AuditReader&>(r);
    if (value.kind != JsonValueKind::array)
        return false;
    if (index == 1)
        return json_visit_indexed_array(*r.document, value, kDepth, visit_audit_user_item, &r).ok() ||
               fail(&r.error, kCodes.json);
    ItemReader items{{r.document, 0}, &kAuditEntrySpec, reinterpret_cast<u8*>(p.page.entries.data),
                     p.page.entries.count, p.page.entry_count};
    const bool read = json_visit_indexed_array(*r.document, value, kDepth, read_item, &items).ok();
    p.page.entry_count = items.count;
    if (items.error)
        fail(&r.error, items.error);
    return read || fail(&r.error, kCodes.json);
}
constexpr FieldDef kAuditDefs[]{{"audit_log_entries", MAICORD_FIELD_HOOK}, {"users", MAICORD_FIELD_HOOK}};
MAICORD_FIELD_TABLE(kAuditSpec, kAuditDefs, audit_hook, kCodes, kDepth);

// Invites -----------------------------------------------------------------

struct NestedIdParse {
    u64 id;
    bool seen;
    u8 error;
};

bool visit_nested_id(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<NestedIdParse*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("id")))
        return true;
    if (p.seen)
        return fail(&p.error, kCodes.duplicate);
    p.seen = true;
    return json_value_to_snowflake(value, &p.id) || fail(&p.error);
}

bool invite_hook(ObjectReader& r, u32 index, const JsonValue& value) noexcept {
    auto& v = *reinterpret_cast<GuildAdminInvite*>(r.base);
    if (index == 0)
        return guild_invite_code_valid(v.code, v.code_bytes);
    NestedIdParse nested{};
    const bool valid = value.kind == JsonValueKind::object &&
                       json_visit_indexed_object(*r.document, value, kDepth, visit_nested_id, &nested).ok() &&
                       nested.seen;
    (index == 1 ? v.channel_id : v.inviter_id) = nested.id;
    return valid;
}
constexpr FieldDef kInviteDefs[]{
    {"code", MAICORD_TEXT_ROW(field_text, kFieldNotify, GuildAdminInvite, code, code_bytes)},
    {"channel", MAICORD_FIELD_HOOK},
    {"inviter", MAICORD_FIELD_HOOK_OR_NULL},
    {"uses", MAICORD_FIELD_NUMBER(GuildAdminInvite, uses)},
    {"max_uses", MAICORD_FIELD_NUMBER(GuildAdminInvite, max_uses)},
    {"max_age", MAICORD_FIELD_NUMBER(GuildAdminInvite, max_age_seconds)},
    {"temporary", MAICORD_FIELD_BOOL(GuildAdminInvite, temporary)},
    {"expires_at", MAICORD_FIELD_TIMESTAMP_OR_NULL(GuildAdminInvite, expires_at_ms)},
};
// A code is a run of letters and digits, so equal codes are equal bytes of the zero-filled array.
MAICORD_FIELD_TABLE(kInviteSpec, kInviteDefs, invite_hook, kCodes, kDepth, 1, sizeof(GuildAdminInvite),
                    static_cast<u16>(__builtin_offsetof(GuildAdminInvite, code)), kGuildAdminInviteCodeBytes);

// Scans `body` and visits its root with `object` or `array`; the result's count is unset.
GuildAdminResult decode_scanned(Utf8View body, Slice<u32> scratch, ReadState& p, void* opaque,
                                JsonMemberVisitor object, JsonArrayVisitor array) noexcept {
    const auto s = scan_json<GuildAdminError>(body, scratch);
    if (s.error != GuildAdminError::none)
        return {0, 0, s.error};
    p.document = &s.document;
    const auto read = array ? json_visit_root_array(s.document.input, s.document.structurals, kDepth, array, opaque)
                            : json_visit_root_object(s.document.input, s.document.structurals, kDepth, object, opaque);
    if (!read.ok())
        return {0, 0, p.error == 0 ? GuildAdminError::json : static_cast<GuildAdminError>(p.error)};
    return {body.bytes, 0, GuildAdminError::none};
}

// A root object read through `spec` into `base`.
GuildAdminResult decode_object(Utf8View body, Slice<u32> scratch, const ObjectSpec& spec, void* base) noexcept {
    ObjectReader r{{nullptr, 0}, &spec, static_cast<u8*>(base), 0};
    auto result = decode_scanned(body, scratch, r, &r, read_member, nullptr);
    if (result.ok())
        result = (r.seen & spec.required) == spec.required ? GuildAdminResult{body.bytes, 1, GuildAdminError::none}
                                                           : GuildAdminResult{0, 0, GuildAdminError::identity};
    return result;
}

// A root array (`array`) or object (`object`) of items counted in the result.
GuildAdminResult decode_items(Utf8View body, Slice<u32> scratch, const ObjectSpec* spec, u8* rows, u32 capacity,
                              JsonArrayVisitor array, JsonMemberVisitor object) noexcept {
    if (!rows && capacity)
        return {0, 0, GuildAdminError::input};
    ItemReader p{{nullptr, 0}, spec, rows, capacity, 0};
    auto result = decode_scanned(body, scratch, p, &p, object, array);
    result.count = p.count;
    return result;
}
template <typename Row>
GuildAdminResult decode_rows(Utf8View body, Slice<u32> scratch, const ObjectSpec* spec, Slice<Row> output,
                             JsonArrayVisitor array, JsonMemberVisitor object) noexcept {
    return decode_items(body, scratch, spec, reinterpret_cast<u8*>(output.data), output.count, array, object);
}

} // namespace

GuildAdminResult guild_admin_decode_widget(Utf8View body, Slice<u32> scratch, GuildAdminWidget* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    GuildAdminWidget value{};
    const auto result = decode_object(body, scratch, kWidgetSpec, &value);
    if (result.ok())
        *output = value;
    return result;
}

GuildAdminResult guild_admin_decode_profile(const Utf8View& body, const Slice<u32>& scratch,
                                            GuildAdminProfile* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    GuildAdminProfile value{};
    const auto result = decode_object(body, scratch, kProfileSpec, &value);
    if (result.ok())
        *output = value;
    return result;
}

GuildAdminResult guild_admin_decode_vanity(const Utf8View& body, const Slice<u32>& scratch,
                                           GuildAdminVanity* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    GuildAdminVanity value{};
    const auto result = decode_object(body, scratch, kVanitySpec, &value);
    if (result.ok())
        *output = value;
    return result;
}

GuildAdminResult guild_admin_decode_role(Utf8View body, Slice<u32> scratch, GuildAdminRole* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    GuildAdminRole value{};
    const auto result = decode_object(body, scratch, kRoleSpec, &value);
    if (result.ok())
        copy_value(output, value);
    return result;
}

GuildAdminResult guild_admin_decode_role_counts(Utf8View body, Slice<u32> scratch,
                                                Slice<GuildAdminRoleCount> output) noexcept {
    return decode_rows(body, scratch, nullptr, output, nullptr, visit_role_count);
}

GuildAdminResult guild_admin_decode_bans(Utf8View body, Slice<u32> scratch, Slice<GuildAdminBan> output) noexcept {
    return decode_rows(body, scratch, &kBanSpec, output, read_item, nullptr);
}

GuildAdminResult guild_admin_decode_audit_log(Utf8View body, Slice<u32> scratch, GuildAdminAuditPage* page) noexcept {
    if (!page || (!page->entries.data && page->entries.count) || (!page->users.data && page->users.count))
        return {0, 0, GuildAdminError::input};
    AuditReader p{{{nullptr, 0}, &kAuditSpec, nullptr, 0}, {page->entries, page->users, 0, 0}};
    auto result = decode_scanned(body, scratch, p, &p, read_member, nullptr);
    if (!result.ok() || !(p.seen & 1u))
        return result.ok() ? GuildAdminResult{0, 0, GuildAdminError::identity} : result;
    page->entry_count = p.page.entry_count;
    page->user_count = p.page.user_count;
    return {body.bytes, p.page.entry_count, GuildAdminError::none};
}

GuildAdminResult guild_admin_decode_invites(Utf8View body, Slice<u32> scratch,
                                            Slice<GuildAdminInvite> output) noexcept {
    return decode_rows(body, scratch, &kInviteSpec, output, read_item, nullptr);
}

GuildAdminResult guild_admin_decode_invite(const Utf8View& body, const Slice<u32>& scratch,
                                           GuildAdminInvite* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    GuildAdminInvite value{};
    const auto result = decode_object(body, scratch, kInviteSpec, &value);
    if (result.ok())
        *output = value;
    return result;
}

GuildAdminResult guild_admin_decode_games(Utf8View body, Slice<u32> scratch, Slice<GuildAdminGame> output) noexcept {
    return decode_rows(body, scratch, nullptr, output, visit_game_object, nullptr);
}

GuildAdminResult guild_admin_decode_top_games(Utf8View body, Slice<u32> scratch,
                                              Slice<GuildAdminGameActivity> output) noexcept {
    return decode_rows(body, scratch, nullptr, output, nullptr, visit_top_games);
}
} // namespace maicord
