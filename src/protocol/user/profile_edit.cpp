#include "maicord/protocol/user/profile_edit.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "../encoding/json_fields.hpp"
#include "../encoding/json_helpers.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/protocol/http/rest.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
using protocol_detail::JsonBody;
bool member_operation(ProfileEditOperation op) noexcept {
    return op == ProfileEditOperation::member_patch || op == ProfileEditOperation::member_profile_patch;
}
bool data_uri(Utf8View value) noexcept {
    constexpr auto prefix = utf8_literal("data:image/");
    return value.bytes > prefix.bytes && bytes_equal(value.data, prefix.data, prefix.bytes);
}
bool text_within(const Utf8View& value, u32 codepoints, u32 bytes) noexcept {
    const auto checked = utf8_validate(value);
    return checked.valid && checked.codepoints <= codepoints && value.bytes <= bytes;
}
bool image_valid(const Utf8View& value) noexcept {
    return !value.bytes || (data_uri(value) && utf8_validate(value).valid);
}
// An optional image whose description needs it.
bool md5_valid(Utf8View md5, const Utf8View& image) noexcept {
    if (!md5.bytes)
        return true;
    if (md5.bytes != 32 || !image.bytes)
        return false;
    for (u32 i = 0; i < 32; ++i)
        if (!ascii_is_lower_hex_digit(md5.data[i]))
            return false;
    return true;
}
bool avatar_valid(const Utf8View& avatar, const Utf8View& description, const Utf8View& md5) noexcept {
    return image_valid(avatar) && md5_valid(md5, avatar) && (!description.bytes || avatar.bytes) &&
           text_within(description, kProfileEditDescriptionBytes, kProfileEditDescriptionBytes);
}
bool style_valid(const ProfileNameStyle& s, bool sent) noexcept {
    if (!sent)
        return !s.font && !s.effect && !s.color_count;
    if (s.color_count > kProfileNameStyleColors || (profile_name_style_set(s) && !s.color_count) ||
        (!profile_name_style_set(s) && s.color_count))
        return false;
    for (u32 i = 0; i < s.color_count; ++i)
        if (s.colors[i] > 0xFFFFFF)
            return false;
    return true;
}
bool user_valid(const UserPatch& p) noexcept {
    if (!p.fields || (p.fields & ~31u))
        return false;
    if (!(p.fields & user_patch_avatar) && (p.avatar.bytes || p.avatar_description.bytes))
        return false;
    if (!(p.fields & user_patch_global_name) && p.global_name.bytes)
        return false;
    if (!(p.fields & user_patch_avatar_decoration) && p.avatar_decoration_sku)
        return false;
    if (!(p.fields & user_patch_nameplate) && p.nameplate_sku)
        return false;
    return avatar_valid(p.avatar, p.avatar_description, p.avatar_md5) &&
           style_valid(p.name_style, p.fields & user_patch_name_style) &&
           text_within(p.global_name, kProfileEditDisplayNameCodepoints, kProfileEditDisplayNameCodepoints * 4);
}
bool member_valid(const MemberEditPatch& p) noexcept {
    if (!p.fields || (p.fields & ~31u))
        return false;
    if (!(p.fields & member_edit_avatar) && (p.avatar.bytes || p.avatar_description.bytes))
        return false;
    if (!(p.fields & member_edit_nick) && p.nick.bytes)
        return false;
    if (!(p.fields & member_edit_avatar_decoration) && p.avatar_decoration_sku)
        return false;
    if (!(p.fields & member_edit_nameplate) && p.nameplate_sku)
        return false;
    return avatar_valid(p.avatar, p.avatar_description, p.avatar_md5) &&
           style_valid(p.name_style, p.fields & member_edit_name_style) &&
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
    return image_valid(p.banner) && md5_valid(p.banner_md5, p.banner) && text_within(p.bio, kProfileBioMaximumCodepoints, kProfileBioMaximumUtf8Bytes) &&
           text_within(p.pronouns, kProfileEditPronounsCodepoints, kProfileEditPronounsCodepoints * 4);
}
void escaped(const Utf8View& value, u32* bytes) noexcept {
    *bytes += 2 + value.bytes * 6;
}
// display_name_font_id, display_name_effect_id and display_name_colors: a style, or three nulls.
void write_style(JsonBody& b, const ProfileNameStyle& s) noexcept {
    if (!profile_name_style_set(s)) {
        b.member("display_name_font_id", "null");
        b.member("display_name_effect_id", "null");
        b.member("display_name_colors", "null");
        return;
    }
    b.member("display_name_font_id", static_cast<u32>(s.font));
    b.member("display_name_effect_id", static_cast<u32>(s.effect));
    b.key("display_name_colors");
    b.put('[');
    for (u32 i = 0; i < s.color_count; ++i) {
        if (i)
            b.put(',');
        b.put(s.colors[i]);
    }
    b.put(']');
}
void write_user(JsonBody& b, const UserPatch& p) noexcept {
    if (p.fields & user_patch_avatar) {
        b.member("avatar", nullable_json(p.avatar));
        b.member_if(p.avatar.bytes && p.avatar_description.bytes, "avatar_description",
                    json_text(p.avatar_description));
    }
    b.member_if(p.fields & user_patch_global_name, "global_name", json_text(p.global_name));
    b.member_if(p.fields & user_patch_avatar_decoration, "avatar_decoration_sku_id",
                nullable_snowflake(p.avatar_decoration_sku));
    b.member_if(p.fields & user_patch_nameplate, "nameplate_sku_id", nullable_snowflake(p.nameplate_sku));
    if (p.fields & user_patch_name_style)
        write_style(b, p.name_style);
}
void write_member(JsonBody& b, const MemberEditPatch& p) noexcept {
    b.member_if(p.fields & member_edit_nick, "nick", json_text(p.nick));
    if (p.fields & member_edit_avatar) {
        b.member("avatar", nullable_json(p.avatar));
        b.member_if(p.avatar.bytes && p.avatar_description.bytes, "avatar_description",
                    json_text(p.avatar_description));
    }
    b.member_if(p.fields & member_edit_avatar_decoration, "avatar_decoration_sku_id",
                nullable_snowflake(p.avatar_decoration_sku));
    if (p.fields & member_edit_nameplate) {
        // As the official client sends it: {"nameplate": {"sku_id": "..."}} or {"nameplate": null}.
        b.key("collectibles");
        if (p.nameplate_sku)
            b.put("{\"nameplate\":{\"sku_id\":\"", p.nameplate_sku, "\"}}");
        else
            b.put("{\"nameplate\":null}");
    }
    if (p.fields & member_edit_name_style)
        write_style(b, p.name_style);
}
void write_profile(JsonBody& b, const ProfilePatch& p) noexcept {
    b.member_if(p.fields & profile_patch_banner, "banner", nullable_json(p.banner));
    b.member_if(p.fields & profile_patch_bio, "bio", json_text(p.bio));
    b.member_if(p.fields & profile_patch_pronouns, "pronouns", json_text(p.pronouns));
    b.member_if(p.fields & profile_patch_accent, "accent_color", or_null(!p.accent_none, p.accent_color));
    if (p.fields & profile_patch_theme) {
        b.key("theme_colors");
        if (p.theme == ProfileThemePatch::none)
            b.put("null");
        else if (p.theme == ProfileThemePatch::reset)
            b.put("[null,null]");
        else
            b.put('[', p.theme_colors[0], ',', p.theme_colors[1], ']');
    }
    if (p.fields & profile_patch_collectibles) {
        b.begin_array("collectibles_sku_ids");
        for (u32 i = 0; i < p.collectible_count; ++i)
            b.element(nullable_snowflake(p.collectibles[i]));
        b.end_array();
    }
}

using namespace protocol_detail;
// Copies a string, cut at a code point boundary when it is too long. One body
// for every ProfileString<N>: object is the whole string (data first), cleared
// like `*s = {}` before at most capacity bytes are copied into it.
__declspec(noinline) bool string_into(const JsonValue& v, char* object, u32 object_bytes, u32 capacity,
                                      u16* stored) noexcept {
    clear_bytes(object, object_bytes);
    if (!filled(v))
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
template <u32 N> bool string(const JsonValue& v, ProfileString<N>* s) noexcept {
    return string_into(v, s->data, sizeof(*s), N, &s->bytes);
}
struct Colors {
    u32* values;
    u32 count, capacity;
};
bool color_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Colors*>(opaque);
    u64 n = 0;
    if (!filled(value) || !number(value, &n) || n > 0xFFFFFF || c.count == c.capacity)
        return false;
    c.values[c.count++] = static_cast<u32>(n);
    return true;
}
bool theme(const JsonIndexedDocument& doc, JsonValue v, u32* out, bool* has) noexcept {
    *has = false;
    if (!filled(v))
        return true;
    Colors c{out, 0, 2};
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
    FieldSet<"sku_id", "type"> f;
    u64 sku = 0, type = 0;
    if (!object(*c.doc, value, f) || !number(f[0].value, &sku) || !number(f[1].value, &type))
        return false;
    if (sku && type <= 3 && *c.count < kProfileEditCollectibleMaximum)
        c.out[(*c.count)++] = {sku, static_cast<ProfileCollectibleType>(type)};
    return true;
}
bool collectibles(const JsonIndexedDocument& doc, JsonValue v, ProfileCollectibleRef* out, u8* count) noexcept {
    *count = 0;
    if (!filled(v))
        return true;
    Collectibles context{&doc, out, count};
    return json_visit_indexed_array(doc, v, 64, collectible_item, &context).ok();
}
bool decoration(const JsonIndexedDocument& doc, JsonValue v, ProfileString<64>* asset, u64* sku) noexcept {
    FieldSet<"asset", "sku_id"> f;
    return object(doc, v, f) && string(f[0].value, asset) && number(f[1].value, sku);
}
// display_name_styles: {font_id, effect_id, colors}; null or absent is no style. The first two colours are kept.
bool name_style(const JsonIndexedDocument& doc, JsonValue v, ProfileNameStyle* out) noexcept {
    reset_value(out);
    if (!filled(v))
        return true;
    FieldSet<"font_id", "effect_id", "colors"> f;
    u64 font = 0, effect = 0;
    if (!object(doc, v, f) || !number(f[0].value, &font) || !number(f[1].value, &effect) || font > 0xFFFF ||
        effect > 0xFFFF)
        return false;
    out->font = static_cast<u16>(font);
    out->effect = static_cast<u16>(effect);
    if (filled(f[2].value)) {
        // Up to five (Prism); a longer list keeps its first five.
        Colors c{out->colors, 0, kProfileNameStyleColors};
        json_visit_indexed_array(doc, f[2].value, 64, color_item, &c);
        out->color_count = static_cast<u8>(c.count);
    }
    return true;
}
// collectibles: {nameplate: {asset, sku_id, ...}}; absent or null is none.
bool nameplate(const JsonIndexedDocument& doc, JsonValue v, ProfileString<128>* asset, u64* sku) noexcept {
    if (!filled(v))
        return true;
    FieldSet<"nameplate"> c;
    if (!object(doc, v, c))
        return false;
    if (!filled(c[0].value))
        return true;
    FieldSet<"asset", "sku_id"> n;
    return object(doc, c[0].value, n) && string(n[0].value, asset) && number(n[1].value, sku);
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
    ProtocolWriter w{output, 0};
    bool ok = false;
    switch (r.operation) {
    case ProfileEditOperation::user_patch:
        ok = protocol_write(&w, "/api/v9/users/@me");
        break;
    case ProfileEditOperation::user_profile_patch:
        ok = protocol_write(&w, "/api/v9/users/@me/profile");
        break;
    case ProfileEditOperation::member_patch:
    case ProfileEditOperation::member_profile_patch:
        ok = protocol_write(&w, "/api/v9/guilds/", r.guild_id,
                            r.operation == ProfileEditOperation::member_patch ? "/members/@me" : "/profile/@me");
        break;
    case ProfileEditOperation::profile_fetch:
        ok = protocol_write(&w, "/api/v9/users/", r.id,
                            "/profile?with_mutual_guilds=false&with_mutual_friends=false&with_mutual_friends_count=false") &&
             (!r.guild_id || protocol_write(&w, "&guild_id=", r.guild_id));
        break;
    case ProfileEditOperation::collectibles:
        ok = protocol_write(&w, "/api/v9/users/@me/collectibles-purchases?variants_return_style=2");
        break;
    case ProfileEditOperation::collectible_product:
        ok = protocol_write(&w, "/api/v9/collectibles-products/", r.id);
        break;
    default:
        break;
    }
    return ok ? ProfileEditWriteResult{w.used, true} : ProfileEditWriteResult{};
}

u32 profile_edit_write_md5(const ProfileEditRequest& r, Slice<u8> output) noexcept {
    const bool member = member_operation(r.operation);
    Utf8View md5{};
    const char* key = nullptr;
    if (r.user && (r.user->fields & user_patch_avatar)) {
        md5 = r.user->avatar_md5;
        key = "user_default_profile_avatar";
    } else if (r.member && (r.member->fields & member_edit_avatar)) {
        md5 = r.member->avatar_md5;
        key = "user_guild_profile_avatar";
    } else if (r.profile && (r.profile->fields & profile_patch_banner)) {
        md5 = r.profile->banner_md5;
        key = member ? "user_guild_profile_banner" : "user_default_profile_banner";
    }
    if (!md5.bytes)
        return 0;
    ProtocolWriter w{output, 0};
    return protocol_write(&w, key, "=\"", Utf8View{md5.data, 32}, '"') ? w.used : 0;
}

u32 profile_edit_body_capacity(const ProfileEditRequest& r) noexcept {
    if (profile_edit_method(r.operation) == RestMethod::get)
        return 0;
    // Keys, ids, colours and a name style's three members.
    u32 bytes = 384;
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
    if (profile_edit_method(r.operation) == RestMethod::get || !output.data)
        return {};
    JsonBody b{output};
    b.begin_object();
    if (r.user)
        write_user(b, *r.user);
    if (r.member)
        write_member(b, *r.member);
    if (r.profile)
        write_profile(b, *r.profile);
    b.end_object();
    if (!b.ok) {
        clear_bytes(output.data, b.writer.used);
        return {};
    }
    return {b.writer.used, true};
}

bool profile_edit_decode_profile(Utf8View body, Slice<u32> structurals, u64 user, u64 guild,
                                 ProfileEditSaved* p) noexcept {
    if (!user)
        return false;
    reset_value(p);
    const auto scan = json_scan_structurals_avx1(body, structurals);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{body, {structurals.data, scan.count}};
    FieldSet<"user", "user_profile", "guild_member", "guild_member_profile", "premium_type"> f;
    if (!object(doc, {body, 0, JsonValueKind::object}, f) || !filled(f[0].value))
        return false;
    FieldSet<"id", "username", "global_name", "avatar", "avatar_decoration_data", "banner", "display_name_styles",
             "collectibles">
        u;
    u64 premium = 0;
    if (!object(doc, f[0].value, u) || !number(u[0].value, &p->user_id) || p->user_id != user ||
        !string(u[1].value, &p->username) || !string(u[2].value, &p->global_name) || !string(u[3].value, &p->avatar) ||
        !decoration(doc, u[4].value, &p->decoration_asset, &p->decoration_sku) || !number(f[4].value, &premium) ||
        premium > 255 || !name_style(doc, u[6].value, &p->name_style) ||
        !nameplate(doc, u[7].value, &p->nameplate_asset, &p->nameplate_sku))
        return false;
    p->premium_type = static_cast<u8>(premium);
    p->guild_id = guild;
    FieldSet<"bio", "pronouns", "banner", "accent_color", "theme_colors", "collectibles"> profile;
    u64 accent = 0;
    if (!object(doc, f[1].value, profile) || !string(profile[0].value, &p->bio) ||
        !string(profile[1].value, &p->pronouns) || !string(profile[2].value, &p->banner) ||
        !number(profile[3].value, &accent) || accent > 0xFFFFFF ||
        !theme(doc, profile[4].value, p->theme_colors, &p->has_theme) ||
        !collectibles(doc, profile[5].value, p->collectibles, &p->collectible_count))
        return false;
    p->has_accent = filled(profile[3].value);
    p->accent_color = static_cast<u32>(accent);
    // Some responses carry the account banner on the user object only.
    if (!p->banner.bytes && !string(u[5].value, &p->banner))
        return false;
    if (filled(f[2].value)) {
        FieldSet<"nick", "avatar", "avatar_decoration_data", "display_name_styles", "collectibles"> m;
        if (!object(doc, f[2].value, m) || !string(m[0].value, &p->nick) || !string(m[1].value, &p->member_avatar) ||
            !decoration(doc, m[2].value, &p->member_decoration_asset, &p->member_decoration_sku) ||
            !name_style(doc, m[3].value, &p->member_name_style) ||
            !nameplate(doc, m[4].value, &p->member_nameplate_asset, &p->member_nameplate_sku))
            return false;
    }
    FieldSet<"bio", "pronouns", "banner", "theme_colors", "collectibles"> gm;
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
    FieldSet<"type", "sku_id", "asset", "label", "title", "thumbnailPreviewSrc"> f;
    u64 type = 0, sku = 0;
    if (!object(*c.doc, value, f) || !number(f[0].value, &type) || !number(f[1].value, &sku))
        return false;
    if (type > 2 || c.count >= c.out.count)
        return true;
    // Built in the next free slot; it only counts once it is kept.
    ProfileCollectible& item = c.out[c.count];
    reset_value(&item);
    item.sku = sku ? sku : c.purchase_sku;
    item.type = static_cast<ProfileCollectibleType>(type);
    // Effects carry title and thumbnailPreviewSrc; decorations and nameplates label and asset.
    const bool effect = type == 1;
    if (!string(effect ? f[4].value : f[3].value, &item.label) || !string(effect ? f[5].value : f[2].value, &item.asset))
        return false;
    if (!item.label.bytes)
        item.label = c.name;
    if (!item.sku)
        return true;
    // A sku owned twice (variants, bundles) lists once.
    for (u32 i = 0; i < c.count; ++i)
        if (c.out[i].sku == item.sku && c.out[i].type == item.type)
            return true;
    ++c.count;
    return true;
}
bool owned_purchase(void* opaque, u32, JsonValue value) noexcept;
bool owned_variants(Owned& c, JsonValue variants) noexcept {
    return !filled(variants) || json_visit_indexed_array(*c.doc, variants, 64, owned_purchase, &c).ok();
}
bool owned_purchase(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Owned*>(opaque);
    FieldSet<"sku_id", "name", "items", "variants"> f;
    if (!object(*c.doc, value, f) || !number(f[0].value, &c.purchase_sku) || !string(f[1].value, &c.name))
        return false;
    const auto name = c.name;
    const u64 sku = c.purchase_sku;
    if (filled(f[2].value) && !json_visit_indexed_array(*c.doc, f[2].value, 64, owned_item, &c).ok())
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
    FieldSet<"message"> f;
    return object(*c.doc, value, f) && string(f[0].value, c.out);
}
bool error_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& c = *static_cast<ErrorContext*>(opaque);
    static constexpr Utf8View names[] = {
        utf8_literal("global_name"),          utf8_literal("nick"),
        utf8_literal("pronouns"),             utf8_literal("avatar"),
        utf8_literal("banner"),               utf8_literal("bio"),
        utf8_literal("display_name_font_id"), utf8_literal("display_name_effect_id"),
        utf8_literal("display_name_colors"),  utf8_literal("nameplate_sku_id"),
        utf8_literal("collectibles")};
    using F = ProfileEditErrorField;
    static constexpr F slots[] = {F::global_name, F::nick,       F::pronouns,   F::avatar,    F::banner,   F::bio,
                                  F::name_style,  F::name_style, F::name_style, F::nameplate, F::nameplate};
    static_assert(sizeof(names) / sizeof(names[0]) == sizeof(slots) / sizeof(slots[0]));
    u32 slot = static_cast<u32>(ProfileEditErrorField::misc);
    if (json_string_equals_ascii(key, utf8_literal("username"))) {
        // A per-server username error is reported under nick.
        c.out->username = true;
        slot = static_cast<u32>(ProfileEditErrorField::nick);
    } else if (!json_string_equals_ascii(key, utf8_literal("_errors")))
        for (u32 i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (json_string_equals_ascii(key, names[i])) {
                slot = static_cast<u32>(slots[i]);
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
    FieldSet<"_errors"> f;
    if (!object(*c.doc, value, f))
        return false;
    if (filled(f[0].value)) {
        MessageContext m{c.doc, &target};
        return json_visit_indexed_array(*c.doc, f[0].value, 64, message_item, &m).ok();
    }
    return true;
}
} // namespace

bool profile_edit_decode_errors(Utf8View body, Slice<u32> structurals, ProfileEditErrors* output) noexcept {
    reset_value(output);
    const auto scan = json_scan_structurals_avx1(body, structurals);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{body, {structurals.data, scan.count}};
    FieldSet<"code", "message", "errors", "username"> f;
    if (!object(doc, {body, 0, JsonValueKind::object}, f) || !number(f[0].value, &output->code) ||
        !string(f[1].value, &output->message))
        return false;
    output->username = output->username || filled(f[3].value);
    if (filled(f[2].value)) {
        if (f[2].value.kind != JsonValueKind::object)
            return false;
        ErrorContext context{&doc, output};
        if (!json_visit_indexed_object(doc, f[2].value, 64, error_member, &context).ok())
            return false;
    }
    return true;
}

} // namespace maicord
