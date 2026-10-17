#include "maicord/protocol/profile_edit.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/rest.hpp"

namespace maicord {
namespace {
bool member_operation(ProfileEditOperation op) noexcept {
    return op == ProfileEditOperation::member_patch || op == ProfileEditOperation::member_profile_patch;
}
bool data_uri(Utf8View value) noexcept {
    constexpr auto prefix = utf8_literal("data:image/");
    return value.bytes > prefix.bytes && bytes_equal(value.data, prefix.data, prefix.bytes);
}
bool text_within(Utf8View value, u32 codepoints, u32 bytes) noexcept {
    const auto checked = utf8_validate(value);
    return checked.valid && checked.codepoints <= codepoints && value.bytes <= bytes;
}
bool image_valid(Utf8View value) noexcept {
    return !value.bytes || (data_uri(value) && utf8_validate(value).valid);
}
bool user_valid(const UserPatch& p) noexcept {
    if (!p.fields || (p.fields & ~7u))
        return false;
    if (!(p.fields & user_patch_avatar) && (p.avatar.bytes || p.avatar_description.bytes))
        return false;
    if (!(p.fields & user_patch_global_name) && p.global_name.bytes)
        return false;
    if (!(p.fields & user_patch_avatar_decoration) && p.avatar_decoration_sku)
        return false;
    return image_valid(p.avatar) && (!p.avatar_description.bytes || p.avatar.bytes) &&
           text_within(p.avatar_description, kProfileEditDescriptionBytes, kProfileEditDescriptionBytes) &&
           text_within(p.global_name, kProfileEditDisplayNameCodepoints, kProfileEditDisplayNameCodepoints * 4);
}
bool member_valid(const MemberEditPatch& p) noexcept {
    if (!p.fields || (p.fields & ~7u))
        return false;
    if (!(p.fields & member_edit_avatar) && (p.avatar.bytes || p.avatar_description.bytes))
        return false;
    if (!(p.fields & member_edit_nick) && p.nick.bytes)
        return false;
    if (!(p.fields & member_edit_avatar_decoration) && p.avatar_decoration_sku)
        return false;
    return image_valid(p.avatar) && (!p.avatar_description.bytes || p.avatar.bytes) &&
           text_within(p.avatar_description, kProfileEditDescriptionBytes, kProfileEditDescriptionBytes) &&
           text_within(p.nick, kProfileEditNicknameCodepoints, kProfileEditNicknameCodepoints * 4);
}
bool profile_valid(const ProfilePatch& p, bool member) noexcept {
    if (!p.fields || (p.fields & ~63u) || p.collectible_count > kProfileEditCollectibleMaximum)
        return false;
    // Banner colours belong to the main profile only.
    if (member && (p.fields & profile_patch_accent))
        return false;
    if ((!(p.fields & profile_patch_banner) && p.banner.bytes) || (!(p.fields & profile_patch_bio) && p.bio.bytes) ||
        (!(p.fields & profile_patch_pronouns) && p.pronouns.bytes) ||
        (!(p.fields & profile_patch_collectibles) && p.collectible_count))
        return false;
    if (p.accent_color > 0xFFFFFF || p.theme_colors[0] > 0xFFFFFF || p.theme_colors[1] > 0xFFFFFF ||
        p.theme > ProfileThemePatch::reset || (!member && p.theme == ProfileThemePatch::reset))
        return false;
    for (u32 i = 0; i < p.collectible_count; ++i)
        if (!p.collectibles[i])
            return false;
    // The server enforces the bio limit (an experiment raises it to 300).
    return image_valid(p.banner) && text_within(p.bio, kProfileBioMaximumCodepoints, kProfileBioMaximumUtf8Bytes) &&
           text_within(p.pronouns, kProfileEditPronounsCodepoints, kProfileEditPronounsCodepoints * 4);
}
bool escaped(Utf8View value, u32* bytes) noexcept {
    *bytes += 2 + value.bytes * 6;
    return true;
}
struct Body {
    ProtocolWriter w;
    bool first = true;
    bool key(const char* name) noexcept {
        const bool comma = !first;
        first = false;
        return (!comma || protocol_writer_append_byte(&w, ',')) && protocol_writer_append_byte(&w, '"') &&
               protocol_writer_append_c_string(&w, name) && protocol_writer_append_literal(&w, "\":");
    }
    bool text(const char* name, Utf8View value) noexcept {
        return key(name) && protocol_writer_append_json_string(&w, value);
    }
    bool image(const char* name, Utf8View value) noexcept {
        return key(name) && (value.bytes ? protocol_writer_append_json_string(&w, value)
                                         : protocol_writer_append_literal(&w, "null"));
    }
    bool snowflake(u64 value) noexcept {
        return value ? protocol_writer_append_byte(&w, '"') && protocol_writer_append_u64(&w, value) &&
                           protocol_writer_append_byte(&w, '"')
                     : protocol_writer_append_literal(&w, "null");
    }
};
bool write_user(Body& b, const UserPatch& p) noexcept {
    if ((p.fields & user_patch_avatar) &&
        (!b.image("avatar", p.avatar) ||
         (p.avatar.bytes && p.avatar_description.bytes && !b.text("avatar_description", p.avatar_description))))
        return false;
    if ((p.fields & user_patch_global_name) && !b.text("global_name", p.global_name))
        return false;
    return !(p.fields & user_patch_avatar_decoration) ||
           (b.key("avatar_decoration_sku_id") && b.snowflake(p.avatar_decoration_sku));
}
bool write_member(Body& b, const MemberEditPatch& p) noexcept {
    if ((p.fields & member_edit_nick) && !b.text("nick", p.nick))
        return false;
    if ((p.fields & member_edit_avatar) &&
        (!b.image("avatar", p.avatar) ||
         (p.avatar.bytes && p.avatar_description.bytes && !b.text("avatar_description", p.avatar_description))))
        return false;
    return !(p.fields & member_edit_avatar_decoration) ||
           (b.key("avatar_decoration_sku_id") && b.snowflake(p.avatar_decoration_sku));
}
bool write_profile(Body& b, const ProfilePatch& p) noexcept {
    if ((p.fields & profile_patch_banner) && !b.image("banner", p.banner))
        return false;
    if ((p.fields & profile_patch_bio) && !b.text("bio", p.bio))
        return false;
    if ((p.fields & profile_patch_pronouns) && !b.text("pronouns", p.pronouns))
        return false;
    if (p.fields & profile_patch_accent) {
        if (!b.key("accent_color") || !(p.accent_none ? protocol_writer_append_literal(&b.w, "null")
                                                      : protocol_writer_append_u64(&b.w, p.accent_color)))
            return false;
    }
    if (p.fields & profile_patch_theme) {
        bool ok = b.key("theme_colors");
        if (p.theme == ProfileThemePatch::none)
            ok = ok && protocol_writer_append_literal(&b.w, "null");
        else if (p.theme == ProfileThemePatch::reset)
            ok = ok && protocol_writer_append_literal(&b.w, "[null,null]");
        else
            ok = ok && protocol_writer_append_byte(&b.w, '[') && protocol_writer_append_u64(&b.w, p.theme_colors[0]) &&
                 protocol_writer_append_byte(&b.w, ',') && protocol_writer_append_u64(&b.w, p.theme_colors[1]) &&
                 protocol_writer_append_byte(&b.w, ']');
        if (!ok)
            return false;
    }
    if (p.fields & profile_patch_collectibles) {
        if (!b.key("collectibles_sku_ids") || !protocol_writer_append_byte(&b.w, '['))
            return false;
        for (u32 i = 0; i < p.collectible_count; ++i)
            if ((i && !protocol_writer_append_byte(&b.w, ',')) || !b.snowflake(p.collectibles[i]))
                return false;
        if (!protocol_writer_append_byte(&b.w, ']'))
            return false;
    }
    return true;
}

// JSON helpers (bounded copies; malformed input fails the decode).
struct Field {
    Utf8View key;
    JsonValue value;
};
struct Fields {
    Field* data;
    u32 count;
};
bool collect(void* opaque, JsonString key, JsonValue value) noexcept {
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
bool present(JsonValue v) noexcept {
    return v.source.data && v.kind != JsonValueKind::null_value;
}
template <u32 N> bool object(const JsonIndexedDocument& doc, JsonValue value, Field (&fields)[N]) noexcept {
    if (!present(value))
        return true;
    if (value.kind != JsonValueKind::object)
        return false;
    Fields context{fields, N};
    return json_visit_indexed_object(doc, value, 64, collect, &context).ok();
}
bool number(JsonValue v, u64* n) noexcept {
    *n = 0;
    if (!present(v))
        return true;
    return v.kind == JsonValueKind::string ? json_value_to_snowflake(v, n) : json_value_to_u64(v, n);
}
// Copies a string, cut at a code point boundary when it is too long. One body
// for every ProfileString<N>: object is the whole string (data first), cleared
// like `*s = {}` before at most capacity bytes are copied into it.
__declspec(noinline) bool string_into(JsonValue v, char* object, u32 object_bytes, u32 capacity,
                                      u16* stored) noexcept {
    clear_bytes(object, object_bytes);
    if (!present(v))
        return true;
    JsonString input{};
    if (!json_value_as_string(v, &input))
        return false;
    char buffer[2048];
    const auto result = json_string_decode_utf8(input, {buffer, sizeof(buffer)});
    if (!result.ok())
        return false;
    u32 bytes = minimum<u32>(result.bytes, capacity);
    while (bytes && bytes < result.bytes && (static_cast<u8>(buffer[bytes]) & 0xC0) == 0x80)
        --bytes;
    copy_bytes(object, buffer, bytes);
    *stored = static_cast<u16>(bytes);
    return true;
}
template <u32 N> bool string(JsonValue v, ProfileString<N>* s) noexcept {
    return string_into(v, s->data, sizeof(*s), N, &s->bytes);
}
struct Colors {
    u32* values;
    u32 count;
};
bool color_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Colors*>(opaque);
    u64 n = 0;
    if (!present(value) || !number(value, &n) || n > 0xFFFFFF || c.count == 2)
        return false;
    c.values[c.count++] = static_cast<u32>(n);
    return true;
}
bool theme(const JsonIndexedDocument& doc, JsonValue v, u32* out, bool* has) noexcept {
    *has = false;
    if (!present(v))
        return true;
    Colors c{out, 0};
    // [null, null] (a reset per-server theme) reads as no theme.
    if (!json_visit_indexed_array(doc, v, 64, color_item, &c).ok())
        return true;
    *has = c.count == 2;
    return true;
}
struct Collectibles {
    const JsonIndexedDocument* doc;
    ProfileCollectibleRef* out;
    u8* count;
};
bool collectible_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Collectibles*>(opaque);
    Field f[] = {{utf8_literal("sku_id")}, {utf8_literal("type")}};
    u64 sku = 0, type = 0;
    if (!object(*c.doc, value, f) || !number(f[0].value, &sku) || !number(f[1].value, &type))
        return false;
    if (sku && type <= 3 && *c.count < kProfileEditCollectibleMaximum)
        c.out[(*c.count)++] = {sku, static_cast<ProfileCollectibleType>(type)};
    return true;
}
bool collectibles(const JsonIndexedDocument& doc, JsonValue v, ProfileCollectibleRef* out, u8* count) noexcept {
    *count = 0;
    if (!present(v))
        return true;
    Collectibles context{&doc, out, count};
    return json_visit_indexed_array(doc, v, 64, collectible_item, &context).ok();
}
bool decoration(const JsonIndexedDocument& doc, JsonValue v, ProfileString<64>* asset, u64* sku) noexcept {
    Field f[] = {{utf8_literal("asset")}, {utf8_literal("sku_id")}};
    return object(doc, v, f) && string(f[0].value, asset) && number(f[1].value, sku);
}
} // namespace

RestMethod profile_edit_method(ProfileEditOperation op) noexcept {
    switch (op) {
    case ProfileEditOperation::profile_fetch:
    case ProfileEditOperation::collectibles:
    case ProfileEditOperation::collectible_product:
        return RestMethod::get;
    default:
        return RestMethod::patch;
    }
}

bool profile_edit_request_valid(const ProfileEditRequest& r) noexcept {
    for (const u8 b : r.reserved)
        if (b)
            return false;
    switch (r.operation) {
    case ProfileEditOperation::user_patch:
        return r.user && !r.member && !r.profile && !r.guild_id && !r.id && user_valid(*r.user);
    case ProfileEditOperation::user_profile_patch:
        return r.profile && !r.user && !r.member && !r.guild_id && !r.id && profile_valid(*r.profile, false);
    case ProfileEditOperation::member_patch:
        return r.member && !r.user && !r.profile && r.guild_id && !r.id && member_valid(*r.member);
    case ProfileEditOperation::member_profile_patch:
        return r.profile && !r.user && !r.member && r.guild_id && !r.id && profile_valid(*r.profile, true);
    case ProfileEditOperation::profile_fetch:
        return r.id && !r.user && !r.member && !r.profile;
    case ProfileEditOperation::collectibles:
        return !r.id && !r.guild_id && !r.user && !r.member && !r.profile;
    case ProfileEditOperation::collectible_product:
        return r.id && !r.guild_id && !r.user && !r.member && !r.profile;
    default:
        return false;
    }
}

RestRouteKey profile_edit_route(const ProfileEditRequest& r) noexcept {
    const bool guild = member_operation(r.operation);
    return {kProfileEditRouteBase + static_cast<u64>(r.operation), guild ? r.guild_id : 0,
            profile_edit_method(r.operation), guild ? RestMajorParameter::guild : RestMajorParameter::none};
}

ProfileEditWriteResult profile_edit_write_target(const ProfileEditRequest& r, Slice<u8> output) noexcept {
    if (!profile_edit_request_valid(r))
        return {};
    ProtocolWriter w{output, 0};
    bool ok = false;
    switch (r.operation) {
    case ProfileEditOperation::user_patch:
        ok = protocol_writer_append_literal(&w, "/api/v10/users/@me");
        break;
    case ProfileEditOperation::user_profile_patch:
        ok = protocol_writer_append_literal(&w, "/api/v10/users/@me/profile");
        break;
    case ProfileEditOperation::member_patch:
    case ProfileEditOperation::member_profile_patch:
        ok = protocol_writer_append_literal(&w, "/api/v10/guilds/") && protocol_writer_append_u64(&w, r.guild_id) &&
             (r.operation == ProfileEditOperation::member_patch ? protocol_writer_append_literal(&w, "/members/@me")
                                                                : protocol_writer_append_literal(&w, "/profile/@me"));
        break;
    case ProfileEditOperation::profile_fetch:
        ok = protocol_writer_append_literal(&w, "/api/v10/users/") && protocol_writer_append_u64(&w, r.id) &&
             protocol_writer_append_literal(
                 &w, "/profile?with_mutual_guilds=false&with_mutual_friends=false&with_mutual_friends_count=false") &&
             (!r.guild_id ||
              (protocol_writer_append_literal(&w, "&guild_id=") && protocol_writer_append_u64(&w, r.guild_id)));
        break;
    case ProfileEditOperation::collectibles:
        ok = protocol_writer_append_literal(&w, "/api/v10/users/@me/collectibles-purchases?variants_return_style=2");
        break;
    case ProfileEditOperation::collectible_product:
        ok = protocol_writer_append_literal(&w, "/api/v10/collectibles-products/") && protocol_writer_append_u64(&w, r.id);
        break;
    default:
        break;
    }
    return ok ? ProfileEditWriteResult{w.used, true} : ProfileEditWriteResult{};
}

u32 profile_edit_body_capacity(const ProfileEditRequest& r) noexcept {
    if (!profile_edit_request_valid(r) || profile_edit_method(r.operation) == RestMethod::get)
        return 0;
    u32 bytes = 192;
    if (r.user) {
        escaped(r.user->avatar, &bytes);
        escaped(r.user->avatar_description, &bytes);
        escaped(r.user->global_name, &bytes);
    }
    if (r.member) {
        escaped(r.member->nick, &bytes);
        escaped(r.member->avatar, &bytes);
        escaped(r.member->avatar_description, &bytes);
    }
    if (r.profile) {
        escaped(r.profile->banner, &bytes);
        escaped(r.profile->bio, &bytes);
        escaped(r.profile->pronouns, &bytes);
        bytes += r.profile->collectible_count * 24;
    }
    return bytes;
}

ProfileEditWriteResult profile_edit_write_body(const ProfileEditRequest& r, Slice<u8> output) noexcept {
    if (!profile_edit_request_valid(r) || profile_edit_method(r.operation) == RestMethod::get || !output.data)
        return {};
    Body b{{output, 0}};
    bool ok = protocol_writer_append_byte(&b.w, '{');
    if (ok && r.user)
        ok = write_user(b, *r.user);
    if (ok && r.member)
        ok = write_member(b, *r.member);
    if (ok && r.profile)
        ok = write_profile(b, *r.profile);
    ok = ok && protocol_writer_append_byte(&b.w, '}');
    if (!ok) {
        clear_bytes(output.data, b.w.used);
        return {};
    }
    return {b.w.used, true};
}

bool profile_edit_decode_profile(Utf8View body, Slice<u32> structurals, u64 user, u64 guild,
                                 ProfileEditSaved* p) noexcept {
    if (!p || !user)
        return false;
    *p = {};
    const auto scan = json_scan_structurals_avx1(body, structurals);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{body, {structurals.data, scan.count}};
    Field f[] = {{utf8_literal("user")},
                 {utf8_literal("user_profile")},
                 {utf8_literal("guild_member")},
                 {utf8_literal("guild_member_profile")},
                 {utf8_literal("premium_type")}};
    if (!object(doc, {body, 0, JsonValueKind::object}, f) || !present(f[0].value))
        return false;
    Field u[] = {{utf8_literal("id")},
                 {utf8_literal("username")},
                 {utf8_literal("global_name")},
                 {utf8_literal("avatar")},
                 {utf8_literal("avatar_decoration_data")},
                 {utf8_literal("banner")}};
    u64 premium = 0;
    if (!object(doc, f[0].value, u) || !number(u[0].value, &p->user_id) || p->user_id != user ||
        !string(u[1].value, &p->username) || !string(u[2].value, &p->global_name) || !string(u[3].value, &p->avatar) ||
        !decoration(doc, u[4].value, &p->decoration_asset, &p->decoration_sku) || !number(f[4].value, &premium) ||
        premium > 255)
        return false;
    p->premium_type = static_cast<u8>(premium);
    p->guild_id = guild;
    Field profile[] = {{utf8_literal("bio")},          {utf8_literal("pronouns")},     {utf8_literal("banner")},
                       {utf8_literal("accent_color")}, {utf8_literal("theme_colors")}, {utf8_literal("collectibles")}};
    u64 accent = 0;
    if (!object(doc, f[1].value, profile) || !string(profile[0].value, &p->bio) ||
        !string(profile[1].value, &p->pronouns) || !string(profile[2].value, &p->banner) ||
        !number(profile[3].value, &accent) || accent > 0xFFFFFF ||
        !theme(doc, profile[4].value, p->theme_colors, &p->has_theme) ||
        !collectibles(doc, profile[5].value, p->collectibles, &p->collectible_count))
        return false;
    p->has_accent = present(profile[3].value);
    p->accent_color = static_cast<u32>(accent);
    // Some responses carry the account banner on the user object only.
    if (!p->banner.bytes && !string(u[5].value, &p->banner))
        return false;
    if (present(f[2].value)) {
        p->has_member = true;
        Field m[] = {{utf8_literal("nick")}, {utf8_literal("avatar")}, {utf8_literal("avatar_decoration_data")}};
        if (!object(doc, f[2].value, m) || !string(m[0].value, &p->nick) || !string(m[1].value, &p->member_avatar) ||
            !decoration(doc, m[2].value, &p->member_decoration_asset, &p->member_decoration_sku))
            return false;
    }
    Field gm[] = {{utf8_literal("bio")},
                  {utf8_literal("pronouns")},
                  {utf8_literal("banner")},
                  {utf8_literal("theme_colors")},
                  {utf8_literal("collectibles")}};
    return object(doc, f[3].value, gm) && string(gm[0].value, &p->member_bio) &&
           string(gm[1].value, &p->member_pronouns) && string(gm[2].value, &p->member_banner) &&
           theme(doc, gm[3].value, p->member_theme_colors, &p->has_member_theme) &&
           collectibles(doc, gm[4].value, p->member_collectibles, &p->member_collectible_count);
}

u64 profile_edit_collectible(ConstSlice<ProfileCollectibleRef> list, ProfileCollectibleType type) noexcept {
    for (u32 i = 0; i < list.count; ++i)
        if (list[i].type == type)
            return list[i].sku;
    return 0;
}

namespace {
struct Owned {
    const JsonIndexedDocument* doc;
    Slice<ProfileCollectible> out;
    u32 count;
    ProfileString<96> name;
    u64 purchase_sku;
};
bool owned_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Owned*>(opaque);
    Field f[] = {{utf8_literal("type")},  {utf8_literal("sku_id")}, {utf8_literal("asset")},
                 {utf8_literal("label")}, {utf8_literal("title")},  {utf8_literal("thumbnailPreviewSrc")}};
    u64 type = 0, sku = 0;
    if (!object(*c.doc, value, f) || !number(f[0].value, &type) || !number(f[1].value, &sku))
        return false;
    if (type > 1 || c.count >= c.out.count)
        return true;
    ProfileCollectible item{};
    item.sku = sku ? sku : c.purchase_sku;
    item.type = static_cast<ProfileCollectibleType>(type);
    if (!string(type ? f[4].value : f[3].value, &item.label) || !string(type ? f[5].value : f[2].value, &item.asset))
        return false;
    if (!item.label.bytes)
        item.label = c.name;
    if (!item.sku)
        return true;
    // A sku owned twice (variants, bundles) lists once.
    for (u32 i = 0; i < c.count; ++i)
        if (c.out[i].sku == item.sku && c.out[i].type == item.type)
            return true;
    c.out[c.count++] = item;
    return true;
}
bool owned_purchase(void* opaque, u32, JsonValue value) noexcept;
bool owned_variants(Owned& c, JsonValue variants) noexcept {
    return !present(variants) || json_visit_indexed_array(*c.doc, variants, 64, owned_purchase, &c).ok();
}
bool owned_purchase(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Owned*>(opaque);
    Field f[] = {{utf8_literal("sku_id")}, {utf8_literal("name")}, {utf8_literal("items")}, {utf8_literal("variants")}};
    if (!object(*c.doc, value, f) || !number(f[0].value, &c.purchase_sku) || !string(f[1].value, &c.name))
        return false;
    const auto name = c.name;
    const u64 sku = c.purchase_sku;
    if (present(f[2].value) && !json_visit_indexed_array(*c.doc, f[2].value, 64, owned_item, &c).ok())
        return false;
    if (!owned_variants(c, f[3].value))
        return false;
    c.name = name;
    c.purchase_sku = sku;
    return true;
}
} // namespace

u32 profile_edit_decode_collectibles(Utf8View body, Slice<u32> structurals, Slice<ProfileCollectible> output) noexcept {
    const auto scan = json_scan_structurals_avx1(body, structurals);
    if (!scan.ok())
        return ~0u;
    const JsonIndexedDocument doc{body, {structurals.data, scan.count}};
    Owned context{&doc, output, 0, {}, 0};
    const JsonValue root{body, 0, JsonValueKind::array};
    if (!json_visit_indexed_array(doc, root, 64, owned_purchase, &context).ok())
        return ~0u;
    return context.count;
}

namespace {
struct ErrorContext {
    const JsonIndexedDocument* doc;
    ProfileEditErrors* out;
};
struct MessageContext {
    const JsonIndexedDocument* doc;
    ProfileString<192>* out;
};
bool message_item(void* opaque, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<MessageContext*>(opaque);
    if (index)
        return true;
    Field f[] = {{utf8_literal("message")}};
    return object(*c.doc, value, f) && string(f[0].value, c.out);
}
bool error_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& c = *static_cast<ErrorContext*>(opaque);
    constexpr Utf8View names[] = {utf8_literal("global_name"), utf8_literal("nick"),   utf8_literal("pronouns"),
                                  utf8_literal("avatar"),      utf8_literal("banner"), utf8_literal("bio")};
    u32 slot = static_cast<u32>(ProfileEditErrorField::misc);
    if (json_string_equals_ascii(key, utf8_literal("username"))) {
        // A per-server username error is reported under nick.
        c.out->username = true;
        slot = static_cast<u32>(ProfileEditErrorField::nick);
    } else if (!json_string_equals_ascii(key, utf8_literal("_errors")))
        for (u32 i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (json_string_equals_ascii(key, names[i])) {
                slot = i;
                break;
            }
    auto& target = c.out->fields[slot];
    if (target.bytes)
        return true;
    if (value.kind == JsonValueKind::array) {
        // Top-level _errors lists messages directly.
        MessageContext m{c.doc, &target};
        return json_string_equals_ascii(key, utf8_literal("_errors"))
                   ? json_visit_indexed_array(*c.doc, value, 64, message_item, &m).ok()
                   : true;
    }
    Field f[] = {{utf8_literal("_errors")}};
    if (!object(*c.doc, value, f))
        return false;
    if (present(f[0].value)) {
        MessageContext m{c.doc, &target};
        return json_visit_indexed_array(*c.doc, f[0].value, 64, message_item, &m).ok();
    }
    return true;
}
} // namespace

bool profile_edit_decode_errors(Utf8View body, Slice<u32> structurals, ProfileEditErrors* output) noexcept {
    if (!output)
        return false;
    *output = {};
    const auto scan = json_scan_structurals_avx1(body, structurals);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{body, {structurals.data, scan.count}};
    Field f[] = {{utf8_literal("code")}, {utf8_literal("message")}, {utf8_literal("errors")}, {utf8_literal("username")}};
    if (!object(doc, {body, 0, JsonValueKind::object}, f) || !number(f[0].value, &output->code) ||
        !string(f[1].value, &output->message))
        return false;
    output->username = output->username || present(f[3].value);
    if (present(f[2].value)) {
        if (f[2].value.kind != JsonValueKind::object)
            return false;
        ErrorContext context{&doc, output};
        if (!json_visit_indexed_object(doc, f[2].value, 64, error_member, &context).ok())
            return false;
    }
    return true;
}

} // namespace maicord
