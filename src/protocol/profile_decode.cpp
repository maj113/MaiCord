#include "maicord/model/user_profile.hpp"
#include "maicord/model/user_activity.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/json_ids.hpp"

namespace maicord {
namespace {
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
template <u32 N> bool object(const JsonIndexedDocument& doc, JsonValue value, Field (&fields)[N]) noexcept {
    if (!value.source.data || value.kind == JsonValueKind::null_value)
        return true;
    Fields context{fields, N};
    return json_visit_indexed_object(doc, value, 64, collect, &context).ok();
}
bool present(JsonValue v) noexcept {
    return v.source.data != nullptr;
}
bool number(JsonValue v, u64* n) noexcept {
    if (!present(v) || v.kind == JsonValueKind::null_value) {
        *n = 0;
        return true;
    }
    return v.kind == JsonValueKind::string ? json_value_to_snowflake(v, n)
                                           : json_value_to_u64(v, n);
}
bool boolean(JsonValue v, bool* b) noexcept {
    if (!present(v) || v.kind == JsonValueKind::null_value) {
        *b = false;
        return true;
    }
    return json_value_to_bool(v, b);
}
// One body for every ProfileString<N>: object is the whole string (data first),
// cleared like `*s = {}` before decoding into its first capacity bytes.
__declspec(noinline) bool string_into(JsonValue v, char* object, u32 object_bytes, u32 capacity,
                                      u16* bytes) noexcept {
    clear_bytes(object, object_bytes);
    if (!present(v) || v.kind == JsonValueKind::null_value)
        return true;
    JsonString input{};
    if (!json_value_as_string(v, &input))
        return false;
    const auto result = json_string_decode_utf8(input, {object, capacity});
    if (!result.ok())
        return false;
    *bytes = static_cast<u16>(result.bytes);
    return true;
}
template <u32 N> bool string(JsonValue v, ProfileString<N>* s) noexcept {
    return string_into(v, s->data, sizeof(*s), N, &s->bytes);
}
struct Colors {
    u32* values;
    u32 capacity, count;
};
bool color_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Colors*>(opaque);
    u64 n = 0;
    if (!number(value, &n) || n > 0xFFFFFF || c.count == c.capacity)
        return false;
    c.values[c.count++] = static_cast<u32>(n);
    return true;
}
bool colors(const JsonIndexedDocument& doc, JsonValue v, u32* out, u32 capacity, u32* count) noexcept {
    *count = 0;
    if (!present(v) || v.kind == JsonValueKind::null_value)
        return true;
    Colors c{out, capacity, 0};
    if (!json_visit_indexed_array(doc, v, 64, color_item, &c).ok())
        return false;
    *count = c.count;
    return true;
}
bool style(const JsonIndexedDocument& doc, JsonValue v, NameStyle* out) noexcept {
    Field f[] = {{utf8_literal("font_id")}, {utf8_literal("effect_id")}, {utf8_literal("colors")}};
    if (!object(doc, v, f))
        return false;
    u64 font = 0, effect = 0;
    u32 count = 0;
    if (!number(f[0].value, &font) || !number(f[1].value, &effect) || font > 65535 || effect > 65535 ||
        !colors(doc, f[2].value, out->colors, 8, &count))
        return false;
    out->font_id = static_cast<u16>(font);
    out->effect_id = static_cast<u16>(effect);
    out->color_count = static_cast<u8>(count);
    return true;
}
struct Activities {
    const JsonIndexedDocument* doc;
    UserAppearance* out;
    bool custom;
};
bool activity_item(void* opaque, u32, JsonValue v) noexcept {
    auto& c = *static_cast<Activities*>(opaque);
    if (v.kind != JsonValueKind::object)
        return true;
    Field f[] = {{utf8_literal("type")},
                 {utf8_literal("name")},
                 {utf8_literal("state")},
                 {utf8_literal("emoji")},
                 {utf8_literal("details")}};
    if (!object(*c.doc, v, f))
        return false;
    u64 type = 0;
    if (!number(f[0].value, &type))
        return false;
    if (c.custom || (type != 4 && c.out->activity.bytes) || type > 5)
        return true;
    if (type != 4) {
        // One-line status such as "Playing X" or "Listening to X".
        ProfileString<256> name{}, details{};
        if (!string(f[1].value, &name) || !name.bytes)
            return true; // Oversized or missing names do not invalidate the presence.
        string(f[4].value, &details);
        const u32 bytes = user_activity_summary(static_cast<UserActivityType>(type), name.view(), details.view(),
                                                {c.out->activity.data, sizeof(c.out->activity.data)});
        c.out->activity.bytes = static_cast<u16>(bytes);
        return true;
    }
    ProfileString<256> text{};
    if (!string(f[2].value, &text))
        return false;
    c.out->activity = text;
    c.custom = true;
    Field emoji[] = {{utf8_literal("id")}, {utf8_literal("name")}, {utf8_literal("animated")}};
    if (!object(*c.doc, f[3].value, emoji) || !number(emoji[0].value, &c.out->status_emoji_id) ||
        !string(emoji[1].value, &c.out->status_emoji) || !boolean(emoji[2].value, &c.out->emoji_animated))
        return false;
    return true;
}
struct ProfileContext {
    const JsonIndexedDocument* doc;
    UserProfile* profile;
};
bool badge_item(void* opaque, u32, JsonValue v) noexcept {
    auto& c = *static_cast<ProfileContext*>(opaque);
    if (c.profile->badge_count >= 24)
        return true;
    Field f[] = {{utf8_literal("id")}, {utf8_literal("icon")}, {utf8_literal("description")}};
    auto& b = c.profile->badges[c.profile->badge_count];
    if (!object(*c.doc, v, f) || !string(f[0].value, &b.id) || !string(f[1].value, &b.icon) ||
        !string(f[2].value, &b.description))
        return false;
    ++c.profile->badge_count;
    return true;
}
bool connection_item(void* opaque, u32 i, JsonValue v) noexcept {
    auto& c = *static_cast<ProfileContext*>(opaque);
    if (i >= 16)
        return true;
    Field f[] = {{utf8_literal("type")}, {utf8_literal("name")}, {utf8_literal("verified")},
                 {utf8_literal("metadata")}};
    auto& b = c.profile->connections[c.profile->connection_count];
    if (!object(*c.doc, v, f) || !string(f[0].value, &b.type) || !string(f[1].value, &b.name) ||
        !boolean(f[2].value, &b.verified))
        return false;
    // Platform metadata is loosely typed; only a string created_at is read.
    Field m[] = {{utf8_literal("created_at")}};
    if (f[3].value.kind == JsonValueKind::object && object(*c.doc, f[3].value, m) &&
        m[0].value.kind == JsonValueKind::string && !string(m[0].value, &b.created_at))
        b.created_at = {};
    ++c.profile->connection_count;
    return true;
}
struct PeerContext {
    const JsonIndexedDocument* doc;
    ProfilePeer* peers;
    u16* count;
    bool friends;
};
bool peer_item(void* opaque, u32 i, JsonValue v) noexcept {
    auto& c = *static_cast<PeerContext*>(opaque);
    if (i >= 100)
        return true;
    Field f[] = {{utf8_literal("id")},       {utf8_literal("nick")},        {utf8_literal("name")},
                 {utf8_literal("username")}, {utf8_literal("global_name")}, {utf8_literal("avatar")},
                 {utf8_literal("icon")}};
    auto& b = c.peers[*c.count];
    if (!object(*c.doc, v, f) || !number(f[0].value, &b.id) || !b.id)
        return false;
    JsonValue name = c.friends ? f[4].value : f[2].value;
    if (!present(name) || name.kind == JsonValueKind::null_value)
        name = c.friends ? f[3].value : JsonValue{};
    if (!string(name, &b.name) || !string(c.friends ? f[5].value : f[6].value, &b.icon))
        return false;
    ++*c.count;
    return true;
}
bool role_item(void* opaque, u32 i, JsonValue v) noexcept {
    auto& p = *static_cast<UserProfile*>(opaque);
    if (i >= 128)
        return true;
    if (!number(v, &p.roles[p.role_count]) || !p.roles[p.role_count])
        return false;
    ++p.role_count;
    return true;
}
struct EffectLayerContext {
    const JsonIndexedDocument* doc;
    ProfileEffect* output;
};
bool signed_number(JsonValue value, i32* output) noexcept {
    if (!present(value) || value.kind == JsonValueKind::null_value)
        return true;
    const auto raw = value.source;
    if (!raw.bytes)
        return false;
    u32 i = raw.data[0] == '-' ? 1 : 0;
    const bool negative = i != 0;
    u64 n = 0;
    if (i == raw.bytes)
        return false;
    for (; i < raw.bytes; ++i) {
        if (raw.data[i] < '0' || raw.data[i] > '9')
            return false;
        n = n * 10 + static_cast<u32>(raw.data[i] - '0');
        if (n > 4096)
            return false;
    }
    *output = negative ? -static_cast<i32>(n) : static_cast<i32>(n);
    return true;
}
bool profile_effect_layer(void* opaque, u32 index, JsonValue value) noexcept {
    auto& context = *static_cast<EffectLayerContext*>(opaque);
    if (index >= 8)
        return true;
    Field f[] = {{utf8_literal("src")},      {utf8_literal("id")},        {utf8_literal("start")},
                 {utf8_literal("duration")}, {utf8_literal("loopDelay")}, {utf8_literal("zIndex")},
                 {utf8_literal("loop")},     {utf8_literal("position")}};
    auto& layer = context.output->layers[context.output->count];
    if (!object(*context.doc, value, f) || !string(f[0].value, &layer.source) ||
        !string(f[1].value, &layer.id) || !boolean(f[6].value, &layer.loop))
        return false;
    u32* numbers[] = {&layer.start_ms, &layer.duration_ms, &layer.loop_delay_ms, &layer.z_index};
    for (u32 i = 0; i < 4; ++i) {
        u64 n = 0;
        if (!number(f[2 + i].value, &n) || n > 3600000)
            return false;
        *numbers[i] = static_cast<u32>(n);
    }
    Field position[] = {{utf8_literal("x")}, {utf8_literal("y")}};
    if (!object(*context.doc, f[7].value, position) || !signed_number(position[0].value, &layer.x) ||
        !signed_number(position[1].value, &layer.y))
        return false;
    if (!layer.source.bytes && !layer.id.bytes)
        return false;
    ++context.output->count;
    return true;
}
bool profile_effect_item(void* opaque, u32, JsonValue value) noexcept {
    auto& context = *static_cast<EffectLayerContext*>(opaque);
    Field f[] = {{utf8_literal("type")}, {utf8_literal("effects")}, {utf8_literal("reducedMotionSrc")}};
    u64 type = 0;
    if (!object(*context.doc, value, f) || !number(f[0].value, &type))
        return false;
    if (type != 1)
        return true;
    return string(f[2].value, &context.output->reduced_source) &&
           (!present(f[1].value) ||
            json_visit_indexed_array(*context.doc, f[1].value, 64, profile_effect_layer, &context).ok());
}
struct Effect {
    const JsonIndexedDocument* doc;
    u64* sku;
};
bool effect_item(void* opaque, u32, JsonValue v) noexcept {
    auto& e = *static_cast<Effect*>(opaque);
    Field f[] = {{utf8_literal("type")}, {utf8_literal("sku_id")}};
    u64 type = 0, sku = 0;
    if (!object(*e.doc, v, f) || !number(f[0].value, &type) || !number(f[1].value, &sku))
        return false;
    if (type == 1)
        *e.sku = sku;
    return true;
}
bool profile_details(const JsonIndexedDocument& doc, JsonValue value, UserProfile* p,
                     bool guild) noexcept {
    Field f[] = {{utf8_literal("banner")},       {utf8_literal("bio")},
                 {utf8_literal("pronouns")},     {utf8_literal("theme_colors")},
                 {utf8_literal("accent_color")}, {utf8_literal("collectibles")}};
    if (!object(doc, value, f) || !string(f[0].value, guild ? &p->guild_banner : &p->banner) ||
        !string(f[1].value, guild ? &p->guild_bio : &p->bio) ||
        !string(f[2].value, guild ? &p->guild_pronouns : &p->pronouns))
        return false;
    u32 count = 0;
    u64 accent = 0;
    if (!colors(doc, f[3].value, guild ? p->guild_theme_colors : p->theme_colors, 2, &count) ||
        !number(f[4].value, &accent) || accent > 0xFFFFFF)
        return false;
    if (guild)
        p->has_guild_theme = count == 2;
    else {
        p->has_theme = count == 2;
        p->accent_color = static_cast<u32>(accent);
    }
    Effect effect{&doc, guild ? &p->guild_effect_sku_id : &p->effect_sku_id};
    return !present(f[5].value) || f[5].value.kind == JsonValueKind::null_value ||
           json_visit_indexed_array(doc, f[5].value, 64, effect_item, &effect).ok();
}
} // namespace
bool user_appearance_decode(const JsonIndexedDocument& doc, JsonValue user, u64 id, u64 guild,
                               UserAppearance* output) noexcept {
    if (!output || !id)
        return false;
    UserAppearance a{};
    a.user_id = id;
    a.guild_id = guild;
    Field f[] = {{utf8_literal("bot")},
                 {utf8_literal("public_flags")},
                 {utf8_literal("display_name_styles")},
                 {utf8_literal("avatar_decoration_data")},
                 {utf8_literal("collectibles")},
                 {utf8_literal("primary_guild")},
                 {utf8_literal("avatar")}};
    if (!object(doc, user, f))
        return false;
    if (present(f[0].value)) {
        a.fields |= appearance_bot;
        if (!boolean(f[0].value, &a.bot))
            return false;
    }
    if (present(f[1].value)) {
        a.fields |= appearance_flags;
        if (!number(f[1].value, &a.public_flags))
            return false;
    }
    if (present(f[2].value)) {
        a.fields |= appearance_name;
        if (!style(doc, f[2].value, &a.name))
            return false;
    }
    if (present(f[3].value)) {
        a.fields |= appearance_decoration;
        Field d[] = {{utf8_literal("asset")}};
        if (!object(doc, f[3].value, d) || !string(d[0].value, &a.decoration))
            return false;
    }
    if (present(f[4].value)) {
        Field c[] = {{utf8_literal("nameplate")}};
        if (!object(doc, f[4].value, c))
            return false;
        if (present(c[0].value) || f[4].value.kind == JsonValueKind::null_value) {
            a.fields |= appearance_nameplate;
            Field n[] = {{utf8_literal("asset")}, {utf8_literal("sku_id")}};
            if (!object(doc, c[0].value, n) || !string(n[0].value, &a.nameplate) ||
                !number(n[1].value, &a.nameplate_sku))
                return false;
        }
    }
    if (present(f[5].value)) {
        a.fields |= appearance_guild_tag;
        Field g[] = {{utf8_literal("identity_guild_id")},
                     {utf8_literal("identity_enabled")},
                     {utf8_literal("tag")},
                     {utf8_literal("badge")}};
        if (!object(doc, f[5].value, g) || !number(g[0].value, &a.primary_guild_id) ||
            !boolean(g[1].value, &a.primary_enabled) || !string(g[2].value, &a.primary_tag) ||
            !string(g[3].value, &a.primary_badge))
            return false;
    }
    if (guild && present(f[6].value)) {
        a.fields |= appearance_avatar;
        if (!string(f[6].value, &a.avatar))
            return false;
    }
    *output = a;
    return true;
}
bool activity_decode(const JsonIndexedDocument& doc, JsonValue presence,
                             UserAppearance* output) noexcept {
    if (!output)
        return false;
    Field f[] = {{utf8_literal("activities")}};
    if (!object(doc, presence, f))
        return false;
    if (!present(f[0].value))
        return true;
    output->fields |= appearance_activity;
    output->activity = {};
    output->status_emoji = {};
    output->status_emoji_id = 0;
    output->emoji_animated = false;
    Activities context{&doc, output, false};
    return f[0].value.kind == JsonValueKind::null_value ||
           json_visit_indexed_array(doc, f[0].value, 64, activity_item, &context).ok();
}
bool user_profile_decode(Utf8View json, Slice<u32> scratch, u64 expected, u64 guild,
                            UserProfile* p) noexcept {
    if (!p || !expected)
        return false;
    *p = {};
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Field f[] = {{utf8_literal("user")},
                 {utf8_literal("user_profile")},
                 {utf8_literal("guild_member")},
                 {utf8_literal("guild_member_profile")},
                 {utf8_literal("badges")},
                 {utf8_literal("connected_accounts")},
                 {utf8_literal("mutual_guilds")},
                 {utf8_literal("mutual_friends")},
                 {utf8_literal("mutual_friends_count")},
                 {utf8_literal("premium_type")},
                 {utf8_literal("premium_since")},
                 {utf8_literal("premium_guild_since")},
                 {utf8_literal("private")},
                 {utf8_literal("guild_badges")}};
    if (!object(doc, {json, 0, JsonValueKind::object}, f))
        return false;
    Field u[] = {{utf8_literal("id")},
                 {utf8_literal("username")},
                 {utf8_literal("global_name")},
                 {utf8_literal("avatar")},
                 {utf8_literal("banner")}};
    if (!present(f[0].value) || !object(doc, f[0].value, u) || !number(u[0].value, &p->user_id) ||
        p->user_id != expected || !string(u[1].value, &p->username) || !p->username.bytes ||
        !string(u[2].value, &p->display_name) || !string(u[3].value, &p->avatar))
        return false;
    if (!p->display_name.bytes)
        p->display_name = p->username;
    p->guild_id = guild;
    if (!user_appearance_decode(doc, f[0].value, expected, 0, &p->user_appearance) ||
        !profile_details(doc, f[1].value, p, false) || !profile_details(doc, f[3].value, p, true))
        return false;
    // Some profile responses carry the global banner on the user object.
    if (!p->banner.bytes && !string(u[4].value, &p->banner))
        return false;
    if (present(f[2].value) && f[2].value.kind != JsonValueKind::null_value) {
        Field m[] = {{utf8_literal("nick")}, {utf8_literal("roles")}, {utf8_literal("joined_at")}};
        if (!object(doc, f[2].value, m) || !string(m[0].value, &p->nickname) ||
            !string(m[2].value, &p->joined_at) ||
            !user_appearance_decode(doc, f[2].value, expected, guild, &p->guild_appearance))
            return false;
        if (present(m[1].value) && !json_visit_indexed_array(doc, m[1].value, 64, role_item, p).ok())
            return false;
    }
    ProfileContext context{&doc, p};
    if (present(f[4].value) && f[4].value.kind != JsonValueKind::null_value &&
        !json_visit_indexed_array(doc, f[4].value, 64, badge_item, &context).ok())
        return false;
    if (present(f[13].value) && f[13].value.kind != JsonValueKind::null_value &&
        !json_visit_indexed_array(doc, f[13].value, 64, badge_item, &context).ok())
        return false;
    if (present(f[5].value) && f[5].value.kind != JsonValueKind::null_value &&
        !json_visit_indexed_array(doc, f[5].value, 64, connection_item, &context).ok())
        return false;
    PeerContext guilds{&doc, p->mutual_guilds, &p->mutual_guild_count, false},
        friends{&doc, p->mutual_friends, &p->mutual_friends_count, true};
    if (present(f[6].value) && f[6].value.kind != JsonValueKind::null_value &&
        !json_visit_indexed_array(doc, f[6].value, 64, peer_item, &guilds).ok())
        return false;
    if (present(f[7].value) && f[7].value.kind != JsonValueKind::null_value &&
        !json_visit_indexed_array(doc, f[7].value, 64, peer_item, &friends).ok())
        return false;
    u64 premium = 0, count = 0;
    if (!number(f[8].value, &count) || count > 0xFFFFFFFF || !number(f[9].value, &premium) ||
        premium > 255 || !string(f[10].value, &p->premium_since) ||
        !string(f[11].value, &p->premium_guild_since) || !boolean(f[12].value, &p->private_profile))
        return false;
    p->premium_type = static_cast<u8>(premium);
    p->mutual_friend_count = present(f[8].value) ? static_cast<u32>(count) : p->mutual_friends_count;
    return true;
}
bool profile_effect_decode(Utf8View json, Slice<u32> scratch, u64 sku,
                                   ProfileEffect* output) noexcept {
    if (!output || !sku)
        return false;
    ProfileEffect effect{};
    effect.sku = sku;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Field f[] = {{utf8_literal("items")}, {utf8_literal("sku_id")}};
    u64 received = 0;
    if (!object(doc, {json, 0, JsonValueKind::object}, f) || !number(f[1].value, &received) ||
        (received && received != sku))
        return false;
    EffectLayerContext context{&doc, &effect};
    if (!present(f[0].value) ||
        !json_visit_indexed_array(doc, f[0].value, 64, profile_effect_item, &context).ok())
        return false;
    for (u32 i = 1; i < effect.count; ++i) {
        auto layer = effect.layers[i];
        u32 at = i;
        while (at && effect.layers[at - 1].z_index > layer.z_index) {
            effect.layers[at] = effect.layers[at - 1];
            --at;
        }
        effect.layers[at] = layer;
    }
    *output = effect;
    return true;
}
} // namespace maicord
