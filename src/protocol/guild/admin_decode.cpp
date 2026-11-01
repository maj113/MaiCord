#include "maicord/protocol/guild/admin.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "maicord/protocol/guild/invite.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "../encoding/json_helpers.hpp"
#include "../encoding/json_field_lists.hpp"


namespace maicord {
namespace {

using namespace protocol_detail;

// Decoder helpers -----------------------------------------------------------

constexpr u32 kDepth = 32;

bool decode_u32(JsonValue value, u32* output, u64 maximum = 0xFFFFFFFFull) noexcept {
    u64 number = 0;
    if (!json_value_to_u64(value, &number) || number > maximum)
        return false;
    *output = static_cast<u32>(number);
    return true;
}

bool decode_permissions(JsonValue value, PermissionSet* output) noexcept {
    JsonString text{};
    return json_value_as_string(value, &text) && json_string_to_u64(text, output);
}

// Field lookup with duplicate detection over a fixed key table.
struct Fields {
    const u8* names; // MemberNames bytes: the count, then the length-prefixed names
    u64 seen;
    GuildAdminError* error;

    // Returns the field index, count for unknown keys, or ~0u after a duplicate.
    __declspec(noinline) u32 find(JsonString name) noexcept {
        const u32 i = member_index(name, names);
        if (i == names[0])
            return i;
        if (seen & (1ull << i)) {
            *error = GuildAdminError::duplicate;
            return ~0u;
        }
        seen |= 1ull << i;
        return i;
    }
};

bool fail(GuildAdminError* error, GuildAdminError value = GuildAdminError::field) noexcept {
    if (*error == GuildAdminError::none)
        *error = value;
    return false;
}

// What every decode shares: the document being read and the first error.
struct ParseState {
    const JsonIndexedDocument* document;
    GuildAdminError error;
};
// A JSON object read through a fixed key table.
struct ObjectParse : ParseState {
    Fields fields;
};
// Items counted as they are stored.
struct CollectionParse : ParseState {
    u32 count;
};

// Visits `value` (an object) into `p`, with its keys in `names`; the first error, or none.
GuildAdminError visit_object_with(ObjectParse& p, void* opaque, const u8* names, JsonValue value,
                                  JsonMemberVisitor visitor) noexcept {
    p.fields = {names, 0, &p.error};
    if (json_visit_indexed_object(*p.document, value, kDepth, visitor, opaque).ok())
        return GuildAdminError::none;
    return p.error == GuildAdminError::none ? GuildAdminError::json : p.error;
}
template <typename Parse>
inline GuildAdminError visit_object(Parse& p, const u8* names, JsonValue value, JsonMemberVisitor visitor) noexcept {
    return visit_object_with(p, &p, names, value, visitor);
}

// User --------------------------------------------------------------------

struct UserParse : ObjectParse {
    GuildAdminUser* user;
};
constexpr const u8* kUserFields = MemberNames<"id", "username", "global_name", "avatar", "discriminator">::data.bytes;

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

bool visit_user(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<UserParse*>(opaque);
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    if (field == 0)
        valid = json_value_to_snowflake(value, &p.user->id);
    else if (field == 1)
        valid = decode_text(value, p.user->username, kGuildAdminUsernameBytes, &p.user->username_bytes, false);
    else if (field == 2)
        valid = decode_text(value, p.user->display, kGuildAdminUsernameBytes, &p.user->display_bytes, true);
    else if (field == 3) {
        u16 bytes = 0;
        valid = decode_text(value, p.user->avatar, kGuildAdminHashBytes, &bytes, true);
        p.user->avatar_bytes = static_cast<u8>(bytes);
    } else if (field == 4)
        valid = decode_discriminator(value, &p.user->discriminator);
    return valid || fail(&p.error);
}

bool decode_user(const JsonIndexedDocument& document, JsonValue value, GuildAdminUser* user,
                 GuildAdminError* error) noexcept {
    if (value.kind != JsonValueKind::object)
        return fail(error);
    reset_value(user);
    UserParse p{};
    p.document = &document;
    p.user = user;
    const auto result = visit_object(p, kUserFields, value, visit_user);
    return result == GuildAdminError::none ? user->id || fail(error, GuildAdminError::identity) : fail(error, result);
}

// Widget and server profile -------------------------------------------------

struct WidgetParse : ObjectParse {
    GuildAdminWidget value;
};
constexpr const u8* kWidgetFields = MemberNames<"enabled", "channel_id">::data.bytes;

bool visit_widget(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<WidgetParse*>(opaque);
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    const bool valid = field == 0   ? json_value_to_bool(value, &p.value.enabled)
                       : field == 1 ? json_value_to_nullable_snowflake(value, &p.value.channel_id)
                                    : true;
    return valid || fail(&p.error);
}

struct ProfileParse : ObjectParse {
    GuildAdminProfile* value;
};
constexpr const u8* kProfileFields = MemberNames<"visibility", "name", "description", "icon_hash", "custom_banner_hash",
                                                 "brand_color_primary", "traits", "game_application_ids",
                                                 "features">::data.bytes;

struct TraitParse : ObjectParse {
    GuildAdminProfileTrait trait;
    u32 position;
};
constexpr const u8* kTraitFields =
    MemberNames<"label", "position", "emoji_id", "emoji_name", "emoji_animated">::data.bytes;

bool visit_trait(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<TraitParse*>(opaque);
    auto& t = p.trait;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    switch (field) {
    case 0: valid = decode_text(value, t.label, kGuildAdminProfileTraitBytes, &t.label_bytes, true); break;
    case 1: valid = decode_u32(value, &p.position); break;
    case 2: valid = json_value_to_nullable_snowflake(value, &t.emoji_id); break;
    case 3: valid = decode_text(value, t.emoji_name, kGuildAdminProfileEmojiNameBytes, &t.emoji_name_bytes, true); break;
    case 4: {
        bool animated = false; // Validated, not kept.
        valid = value.kind == JsonValueKind::null_value || json_value_to_bool(value, &animated);
        break;
    }
    default: break;
    }
    return valid || fail(&p.error);
}

// Traits outside positions 0-4 are dropped, as the official client reads them.
bool visit_trait_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileParse*>(opaque);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    TraitParse trait{};
    trait.document = p.document;
    trait.position = ~0u;
    const auto result = visit_object(trait, kTraitFields, value, visit_trait);
    if (result != GuildAdminError::none)
        return fail(&p.error, result);
    if (trait.position < kGuildAdminProfileTraits)
        p.value->traits[trait.position] = trait.trait;
    return true;
}

bool visit_game_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileParse*>(opaque);
    auto& v = *p.value;
    if (v.game_count >= kGuildAdminProfileGames)
        return fail(&p.error, GuildAdminError::capacity);
    u64 id = 0;
    if (!json_value_to_snowflake(value, &id) || !id)
        return fail(&p.error);
    v.game_application_ids[v.game_count++] = id;
    return true;
}

bool visit_feature_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileParse*>(opaque);
    JsonString name{};
    if (json_value_as_string(value, &name) && json_string_equals_ascii(name, utf8_literal("DISCOVERABLE")))
        p.value->discoverable = true;
    return true;
}

// An unknown or null visibility leaves it unknown.
bool visit_profile(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ProfileParse*>(opaque);
    auto& v = *p.value;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    const bool null = value.kind == JsonValueKind::null_value;
    u32 number = 0;
    u16 bytes = 0;
    bool valid = true;
    switch (field) {
    case 0:
        if (!null && decode_u32(value, &number, 255)) {
            v.visibility = static_cast<u8>(number);
            v.visibility_known = true;
        }
        break;
    case 1:
        valid = decode_text(value, v.name, kGuildAdminNameBytes, &v.name_bytes, true);
        v.name_known = !null;
        break;
    case 2: valid = decode_text(value, v.description, kGuildAdminProfileDescriptionBytes, &v.description_bytes, true); break;
    case 3:
        valid = decode_text(value, v.icon, kGuildAdminHashBytes, &bytes, true);
        v.icon_bytes = static_cast<u8>(bytes);
        v.icon_known = true;
        break;
    case 4:
        valid = decode_text(value, v.custom_banner, kGuildAdminHashBytes, &bytes, true);
        v.custom_banner_bytes = static_cast<u8>(bytes);
        v.custom_banner_known = true;
        break;
    case 5:
        valid = decode_text(value, v.brand_color, kGuildAdminBrandColorBytes, &bytes, true);
        v.brand_color_bytes = static_cast<u8>(bytes);
        break;
    case 6:
        valid = null || (value.kind == JsonValueKind::array &&
                         json_visit_indexed_array(*p.document, value, kDepth, visit_trait_item, &p).ok());
        break;
    case 7:
        valid = null || (value.kind == JsonValueKind::array &&
                         json_visit_indexed_array(*p.document, value, kDepth, visit_game_item, &p).ok());
        break;
    case 8:
        valid = null || (value.kind == JsonValueKind::array &&
                         json_visit_indexed_array(*p.document, value, kDepth, visit_feature_item, &p).ok());
        break;
    default: break;
    }
    return valid || fail(&p.error);
}

struct VanityParse : ObjectParse {
    GuildAdminVanity value;
};
constexpr const u8* kVanityFields = MemberNames<"code", "uses">::data.bytes;

bool visit_vanity(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<VanityParse*>(opaque);
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    if (field == 0)
        valid = decode_text(value, p.value.code, kGuildAdminVanityCodeBytes, &p.value.code_bytes, true);
    else if (field == 1)
        valid = value.kind == JsonValueKind::null_value || decode_u32(value, &p.value.uses);
    return valid || fail(&p.error);
}

// Roles -------------------------------------------------------------------

struct RoleParse : ObjectParse {
    GuildAdminRole value;
};
constexpr const u8* kRoleFields = MemberNames<"id", "name", "permissions", "position", "color", "hoist", "managed",
                                              "mentionable", "icon", "unicode_emoji", "flags">::data.bytes;

bool visit_role(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<RoleParse*>(opaque);
    auto& v = p.value;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    switch (field) {
    case 0: valid = json_value_to_snowflake(value, &v.id); break;
    case 1: valid = decode_text(value, v.name, kGuildAdminNameBytes, &v.name_bytes, false); break;
    case 2: valid = decode_permissions(value, &v.permissions); break;
    case 3: valid = decode_u32(value, &v.position); break;
    case 4: valid = decode_u32(value, &v.color, 0xFFFFFF); break;
    case 5: valid = json_value_to_bool(value, &v.hoist); break;
    case 6: valid = json_value_to_bool(value, &v.managed); break;
    case 7: valid = json_value_to_bool(value, &v.mentionable); break;
    case 8: valid = decode_text(value, v.icon, kGuildAdminHashBytes, &v.icon_bytes, true); break;
    case 9: valid = decode_text(value, v.emoji, kGuildAdminEmojiBytes, &v.emoji_bytes, true); break;
    case 10: valid = decode_u32(value, &v.flags); break;
    default: break;
    }
    return valid || fail(&p.error);
}

template <typename Item> struct ArrayParse : CollectionParse {
    Slice<Item> output;
};

// Role member counts: {"<role id>": <count>, ...}
bool visit_role_count(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<ArrayParse<GuildAdminRoleCount>*>(opaque);
    if (p.count >= p.output.count)
        return fail(&p.error, GuildAdminError::capacity);
    u64 id = 0;
    u32 members = 0;
    if (!json_string_to_u64(name, &id) || !id || !decode_u32(value, &members))
        return fail(&p.error);
    for (u32 i = 0; i < p.count; ++i)
        if (p.output[i].role_id == id)
            return fail(&p.error, GuildAdminError::duplicate);
    p.output[p.count++] = {id, members};
    return true;
}

// Bans --------------------------------------------------------------------

struct BanParse : ObjectParse {
    GuildAdminBan* value;
};
constexpr const u8* kBanFields = MemberNames<"user", "reason">::data.bytes;

bool visit_ban(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<BanParse*>(opaque);
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    if (field == 0)
        return decode_user(*p.document, value, &p.value->user, &p.error);
    if (field == 1)
        return decode_text(value, p.value->reason, kGuildAdminReasonBytes, &p.value->reason_bytes, true) ||
               fail(&p.error);
    return true;
}

bool visit_ban_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ArrayParse<GuildAdminBan>*>(opaque);
    if (p.count >= p.output.count)
        return fail(&p.error, GuildAdminError::capacity);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    GuildAdminBan ban{};
    BanParse parse{};
    parse.document = p.document;
    parse.value = &ban;
    const auto result = visit_object(parse, kBanFields, value, visit_ban);
    if (result != GuildAdminError::none)
        return fail(&p.error, result);
    if (!(parse.fields.seen & 1u))
        return fail(&p.error, GuildAdminError::identity);
    for (u32 i = 0; i < p.count; ++i)
        if (p.output[i].user.id == ban.user.id)
            return fail(&p.error, GuildAdminError::duplicate);
    copy_value(&p.output[p.count++], ban);
    return true;
}

// Audit log ---------------------------------------------------------------

struct AuditEntryParse : ObjectParse {
    GuildAdminAuditEntry value;
};
constexpr const u8* kAuditEntryFields =
    MemberNames<"id", "user_id", "target_id", "action_type", "reason">::data.bytes;

bool visit_audit_entry(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<AuditEntryParse*>(opaque);
    auto& v = p.value;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    u32 number = 0;
    bool valid = true;
    switch (field) {
    case 0: valid = json_value_to_snowflake(value, &v.id); break;
    case 1: valid = json_value_to_nullable_snowflake(value, &v.user_id); break;
    case 2: valid = json_value_to_nullable_snowflake(value, &v.target_id); break;
    case 3: valid = decode_u32(value, &number, 0xFFFF); v.action_type = static_cast<u16>(number); break;
    case 4: valid = decode_text(value, v.reason, kGuildAdminReasonBytes, &v.reason_bytes, true); break;
    default: break;
    }
    return valid || fail(&p.error);
}

struct AuditParse : CollectionParse {
    GuildAdminAuditPage page;
    u64 seen;
};

bool visit_audit_entry_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<AuditParse*>(opaque);
    if (p.page.entry_count >= p.page.entries.count)
        return fail(&p.error, GuildAdminError::capacity);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    AuditEntryParse entry{};
    entry.document = p.document;
    const auto result = visit_object(entry, kAuditEntryFields, value, visit_audit_entry);
    if (result != GuildAdminError::none)
        return fail(&p.error, result);
    if (!(entry.fields.seen & 1u) || !(entry.fields.seen & 8u))
        return fail(&p.error, GuildAdminError::identity);
    for (u32 i = 0; i < p.page.entry_count; ++i)
        if (p.page.entries[i].id == entry.value.id)
            return fail(&p.error, GuildAdminError::duplicate);
    copy_value(&p.page.entries[p.page.entry_count++], entry.value);
    return true;
}

bool visit_audit_user_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<AuditParse*>(opaque);
    if (p.page.user_count >= p.page.users.count)
        return fail(&p.error, GuildAdminError::capacity);
    GuildAdminUser user;
    if (!decode_user(*p.document, value, &user, &p.error))
        return false;
    for (u32 i = 0; i < p.page.user_count; ++i)
        if (p.page.users[i].id == user.id)
            return fail(&p.error, GuildAdminError::duplicate);
    p.page.users[p.page.user_count++] = user;
    return true;
}

bool visit_audit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<AuditParse*>(opaque);
    const bool entries = json_string_equals_ascii(name, utf8_literal("audit_log_entries"));
    const bool users = !entries && json_string_equals_ascii(name, utf8_literal("users"));
    if (!entries && !users)
        return true;
    const u64 bit = entries ? 1u : 2u;
    if (p.seen & bit)
        return fail(&p.error, GuildAdminError::duplicate);
    p.seen |= bit;
    if (value.kind != JsonValueKind::array)
        return fail(&p.error);
    const auto read = json_visit_indexed_array(*p.document, value, kDepth,
                                               entries ? visit_audit_entry_item : visit_audit_user_item, &p);
    return read.ok() || fail(&p.error, GuildAdminError::json);
}

// Invites -----------------------------------------------------------------

struct NestedIdParse {
    u64 id;
    bool seen;
    GuildAdminError error;
};

bool visit_nested_id(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<NestedIdParse*>(opaque);
    if (!json_string_equals_ascii(name, utf8_literal("id")))
        return true;
    if (p.seen)
        return fail(&p.error, GuildAdminError::duplicate);
    p.seen = true;
    return json_value_to_snowflake(value, &p.id) || fail(&p.error);
}

struct InviteParse : ObjectParse {
    GuildAdminInvite value;
};
constexpr const u8* kInviteFields = MemberNames<"code", "channel", "inviter", "uses", "max_uses", "max_age",
                                                "temporary", "expires_at">::data.bytes;

bool visit_invite(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<InviteParse*>(opaque);
    auto& v = p.value;
    const u32 field = p.fields.find(name);
    if (field == ~0u)
        return false;
    bool valid = true;
    switch (field) {
    case 0:
        valid = decode_text(value, v.code, kGuildAdminInviteCodeBytes, &v.code_bytes, false) &&
                guild_invite_code_valid(v.code, v.code_bytes);
        break;
    case 1:
    case 2: {
        if (value.kind == JsonValueKind::null_value && field == 2)
            break;
        NestedIdParse nested{};
        valid = value.kind == JsonValueKind::object &&
                json_visit_indexed_object(*p.document, value, kDepth, visit_nested_id, &nested).ok() && nested.seen;
        (field == 1 ? v.channel_id : v.inviter_id) = nested.id;
        break;
    }
    case 3: valid = decode_u32(value, &v.uses); break;
    case 4: valid = decode_u32(value, &v.max_uses); break;
    case 5: valid = decode_u32(value, &v.max_age_seconds); break;
    case 6: valid = json_value_to_bool(value, &v.temporary); break;
    case 7:
        if (value.kind == JsonValueKind::null_value)
            v.expires_at_ms = 0;
        else
            valid = json_value_to_timestamp(value, &v.expires_at_ms);
        break;
    default: break;
    }
    return valid || fail(&p.error);
}

bool visit_invite_item(void* opaque, u32, JsonValue value) noexcept {
    auto& p = *static_cast<ArrayParse<GuildAdminInvite>*>(opaque);
    if (p.count >= p.output.count)
        return fail(&p.error, GuildAdminError::capacity);
    if (value.kind != JsonValueKind::object)
        return fail(&p.error);
    InviteParse parse{};
    parse.document = p.document;
    const auto result = visit_object(parse, kInviteFields, value, visit_invite);
    if (result != GuildAdminError::none)
        return fail(&p.error, result);
    if (!(parse.fields.seen & 1u))
        return fail(&p.error, GuildAdminError::identity);
    for (u32 i = 0; i < p.count; ++i)
        if (p.output[i].code_bytes == parse.value.code_bytes &&
            bytes_equal(p.output[i].code, parse.value.code, parse.value.code_bytes))
            return fail(&p.error, GuildAdminError::duplicate);
    p.output[p.count++] = parse.value;
    return true;
}

// Scans `body` and visits its root with `object` or `array`; the result's count is unset.
GuildAdminResult decode_scanned(Utf8View body, Slice<u32> scratch, ParseState& p, void* opaque,
                                JsonMemberVisitor object, JsonArrayVisitor array) noexcept {
    const auto s = scan_json<GuildAdminError>(body, scratch);
    if (s.error != GuildAdminError::none)
        return {0, 0, s.error};
    p.document = &s.document;
    const auto read = array ? json_visit_root_array(s.document.input, s.document.structurals, kDepth, array, opaque)
                            : json_visit_root_object(s.document.input, s.document.structurals, kDepth, object, opaque);
    if (!read.ok())
        return {0, 0, p.error == GuildAdminError::none ? GuildAdminError::json : p.error};
    return {body.bytes, 0, GuildAdminError::none};
}

// A root object of fixed fields; `required` is the mask of those that must appear.
template <typename Parse>
inline GuildAdminResult decode_root(Utf8View body, Slice<u32> scratch, Parse& p, const u8* names,
                                    JsonMemberVisitor visitor, u64 required) noexcept {
    p.fields = {names, 0, &p.error};
    auto result = decode_scanned(body, scratch, p, &p, visitor, nullptr);
    if (result.ok())
        result = (p.fields.seen & required) == required ? GuildAdminResult{body.bytes, 1, GuildAdminError::none}
                                                        : GuildAdminResult{0, 0, GuildAdminError::identity};
    return result;
}

// A root array (`array`) or object (`object`) of items counted in p.count.
template <typename Parse>
inline GuildAdminResult decode_collection(Utf8View body, Slice<u32> scratch, Parse& p, JsonArrayVisitor array,
                                          JsonMemberVisitor object) noexcept {
    auto result = decode_scanned(body, scratch, p, &p, object, array);
    result.count = p.count;
    return result;
}

template <typename Item>
GuildAdminResult decode_array(Utf8View body, Slice<u32> scratch, Slice<Item> output,
                              JsonArrayVisitor visitor) noexcept {
    if (!output.data && output.count)
        return {0, 0, GuildAdminError::input};
    ArrayParse<Item> p{};
    p.output = output;
    return decode_collection(body, scratch, p, visitor, nullptr);
}

} // namespace

GuildAdminResult guild_admin_decode_widget(Utf8View body, Slice<u32> scratch, GuildAdminWidget* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    WidgetParse p{};
    const auto result = decode_root(body, scratch, p, kWidgetFields, visit_widget, 1u);
    if (result.ok())
        *output = p.value;
    return result;
}

GuildAdminResult guild_admin_decode_profile(Utf8View body, Slice<u32> scratch, GuildAdminProfile* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    GuildAdminProfile value{};
    ProfileParse p{};
    p.value = &value;
    const auto result = decode_root(body, scratch, p, kProfileFields, visit_profile, 0);
    if (result.ok())
        *output = value;
    return result;
}

GuildAdminResult guild_admin_decode_vanity(Utf8View body, Slice<u32> scratch, GuildAdminVanity* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    VanityParse p{};
    const auto result = decode_root(body, scratch, p, kVanityFields, visit_vanity, 0);
    if (result.ok())
        *output = p.value;
    return result;
}

GuildAdminResult guild_admin_decode_role(Utf8View body, Slice<u32> scratch, GuildAdminRole* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    RoleParse p{};
    const auto result = decode_root(body, scratch, p, kRoleFields, visit_role, 1u | 2u | 4u | 8u);
    if (!result.ok() || !p.value.id)
        return result.ok() ? GuildAdminResult{0, 0, GuildAdminError::identity} : result;
    copy_value(output, p.value);
    return result;
}

GuildAdminResult guild_admin_decode_role_counts(Utf8View body, Slice<u32> scratch,
                                                Slice<GuildAdminRoleCount> output) noexcept {
    if (!output.data && output.count)
        return {0, 0, GuildAdminError::input};
    ArrayParse<GuildAdminRoleCount> p{};
    p.output = output;
    return decode_collection(body, scratch, p, nullptr, visit_role_count);
}

GuildAdminResult guild_admin_decode_bans(Utf8View body, Slice<u32> scratch, Slice<GuildAdminBan> output) noexcept {
    return decode_array(body, scratch, output, visit_ban_item);
}

GuildAdminResult guild_admin_decode_audit_log(Utf8View body, Slice<u32> scratch, GuildAdminAuditPage* page) noexcept {
    if (!page || (!page->entries.data && page->entries.count) || (!page->users.data && page->users.count))
        return {0, 0, GuildAdminError::input};
    AuditParse p{};
    p.page = {page->entries, page->users, 0, 0};
    const auto result = decode_collection(body, scratch, p, nullptr, visit_audit_root);
    if (!result.ok() || !(p.seen & 1u))
        return result.ok() ? GuildAdminResult{0, 0, GuildAdminError::identity} : result;
    page->entry_count = p.page.entry_count;
    page->user_count = p.page.user_count;
    return {body.bytes, p.page.entry_count, GuildAdminError::none};
}

GuildAdminResult guild_admin_decode_invites(Utf8View body, Slice<u32> scratch,
                                            Slice<GuildAdminInvite> output) noexcept {
    return decode_array(body, scratch, output, visit_invite_item);
}

GuildAdminResult guild_admin_decode_invite(Utf8View body, Slice<u32> scratch, GuildAdminInvite* output) noexcept {
    if (!output)
        return {0, 0, GuildAdminError::input};
    InviteParse p{};
    const auto result = decode_root(body, scratch, p, kInviteFields, visit_invite, 1u);
    if (result.ok())
        *output = p.value;
    return result;
}
} // namespace maicord
