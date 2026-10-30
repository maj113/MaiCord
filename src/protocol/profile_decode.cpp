#include "maicord/model/user_profile.hpp"
#include "maicord/model/user_activity.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "json_fields.hpp"

namespace maicord {
namespace {
using namespace protocol_detail;
bool present(const JsonValue& v) noexcept {
    return v.source.data != nullptr;
}
bool boolean(const JsonValue& v, bool* b) noexcept {
    if (!filled(v)) {
        *b = false;
        return true;
    }
    return json_value_to_bool(v, b);
}
// One body for every ProfileString<N>: object is the whole string (data first),
// cleared like `*s = {}` before decoding into its first capacity bytes.
__declspec(noinline) bool string_into(const JsonValue& v, char* object, u32 object_bytes, u32 capacity,
                                      u16* bytes) noexcept {
    clear_bytes(object, object_bytes);
    if (!filled(v))
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
template <u32 N> bool string(const JsonValue& v, ProfileString<N>* s) noexcept {
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
bool colors(const JsonIndexedDocument& doc, const JsonValue& v, u32* out, u32 capacity, u32* count) noexcept {
    *count = 0;
    if (!filled(v))
        return true;
    Colors c{out, capacity, 0};
    if (!json_visit_indexed_array(doc, v, 64, color_item, &c).ok())
        return false;
    *count = c.count;
    return true;
}
bool style(const JsonIndexedDocument& doc, const JsonValue& v, NameStyle* out) noexcept {
    FieldSet<"font_id", "effect_id", "colors"> f;
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
    FieldSet<"type", "name", "state", "emoji", "details"> f;
    if (!object(*c.doc, v, f))
        return false;
    u64 type = 0;
    if (!number(f[0].value, &type))
        return false;
    if (c.custom || (type != 4 && c.out->activity.bytes) || type > 5)
        return true;
    if (type != 4) {
        // One-line status such as "Playing X" or "Listening to X".
        ProfileString<256> name, details;
        if (!string(f[1].value, &name) || !name.bytes)
            return true; // Oversized or missing names do not invalidate the presence.
        string(f[4].value, &details);
        const u32 bytes = user_activity_summary(static_cast<UserActivityType>(type), name.view(), details.view(),
                                                {c.out->activity.data, sizeof(c.out->activity.data)});
        c.out->activity.bytes = static_cast<u16>(bytes);
        return true;
    }
    ProfileString<256> text;
    if (!string(f[2].value, &text))
        return false;
    copy_value(&c.out->activity, text);
    c.out->fields |= appearance_custom_status;
    c.custom = true;
    FieldSet<"id", "name", "animated"> emoji;
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
    FieldSet<"id", "icon", "description", "link"> f;
    auto& b = c.profile->badges[c.profile->badge_count];
    if (!object(*c.doc, v, f))
        return false;
    // A badge whose id or description does not fit is left out, not the
    // profile; an oversized icon draws nothing, and only https links are kept.
    if (!string(f[0].value, &b.id) || !string(f[2].value, &b.description) || !b.id.bytes)
        return true;
    if (!string(f[1].value, &b.icon))
        reset_value(&b.icon);
    constexpr auto https = utf8_literal("https://");
    if (!string(f[3].value, &b.link) || b.link.bytes <= https.bytes ||
        !utf8_equal({b.link.data, https.bytes}, https))
        reset_value(&b.link);
    ++c.profile->badge_count;
    return true;
}
bool connection_item(void* opaque, u32 i, JsonValue v) noexcept {
    auto& c = *static_cast<ProfileContext*>(opaque);
    if (i >= 16)
        return true;
    FieldSet<"type", "name", "verified", "metadata"> f;
    auto& b = c.profile->connections[c.profile->connection_count];
    if (!object(*c.doc, v, f))
        return false;
    // A connection whose fields do not fit is left out, not the profile.
    if (!string(f[0].value, &b.type) || !string(f[1].value, &b.name) || !boolean(f[2].value, &b.verified)) {
        reset_value(&b);
        return true;
    }
    // Platform metadata is loosely typed; only a string created_at is read.
    FieldSet<"created_at"> m;
    if (f[3].value.kind == JsonValueKind::object && object(*c.doc, f[3].value, m) &&
        m[0].value.kind == JsonValueKind::string && !string(m[0].value, &b.created_at))
        reset_value(&b.created_at);
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
    FieldSet<"id", "nick", "name", "username", "global_name", "avatar", "icon"> f;
    auto& b = c.peers[*c.count];
    if (!object(*c.doc, v, f) || !number(f[0].value, &b.id) || !b.id)
        return false;
    JsonValue name = c.friends ? f[4].value : f[2].value;
    if (!filled(name))
        name = c.friends ? f[3].value : JsonValue{};
    // An oversized name or icon drops that field, not the profile.
    if (!string(name, &b.name))
        reset_value(&b.name);
    if (!string(c.friends ? f[5].value : f[6].value, &b.icon))
        reset_value(&b.icon);
    ++*c.count;
    return true;
}
bool role_item(void* opaque, u32 i, JsonValue v) noexcept {
    auto& p = *static_cast<UserProfile*>(opaque);
    if (i >= sizeof(p.roles) / sizeof(p.roles[0]))
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
bool signed_number(const JsonValue& value, i32* output) noexcept {
    if (!filled(value))
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
    FieldSet<"src", "id", "start", "duration", "loopDelay", "zIndex", "loop", "position"> f;
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
    FieldSet<"x", "y"> position;
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
    FieldSet<"type", "effects", "reducedMotionSrc"> f;
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
    FieldSet<"type", "sku_id"> f;
    u64 type = 0, sku = 0;
    if (!object(*e.doc, v, f) || !number(f[0].value, &type) || !number(f[1].value, &sku))
        return false;
    if (type == 1)
        *e.sku = sku;
    return true;
}
bool profile_details(const JsonIndexedDocument& doc, const JsonValue& value, UserProfile* p,
                     bool guild) noexcept {
    FieldSet<"banner", "bio", "pronouns", "theme_colors", "accent_color", "collectibles"> f;
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
    return !filled(f[5].value) || json_visit_indexed_array(doc, f[5].value, 64, effect_item, &effect).ok();
}
struct PopularCommands {
    UserProfile* profile;
};
bool popular_command_item(void* opaque, u32, JsonValue v) noexcept {
    auto& p = *static_cast<PopularCommands*>(opaque)->profile;
    u64 id = 0;
    if (!number(v, &id))
        return false;
    if (id && p.popular_command_count < 5)
        p.popular_commands[p.popular_command_count++] = id;
    return true;
}
// A bot's application: an unusable object only drops its commands.
void profile_application(const JsonIndexedDocument& doc, const JsonValue& value, UserProfile* p) noexcept {
    FieldSet<"id", "popular_application_command_ids"> f;
    PopularCommands commands{p};
    if (!filled(value) || !object(doc, value, f) || !number(f[0].value, &p->application_id) ||
        (filled(f[1].value) &&
         !json_visit_indexed_array(doc, f[1].value, 64, popular_command_item, &commands).ok())) {
        p->application_id = 0;
        p->popular_command_count = 0;
    }
}
// "0" for unique usernames; bots keep four digits.
u16 discriminator(const JsonValue& value) noexcept {
    ProfileString<8> text;
    if (!string(value, &text) || !text.bytes || text.bytes > 4)
        return 0;
    u32 n = 0;
    for (u32 i = 0; i < text.bytes; ++i) {
        if (text.data[i] < '0' || text.data[i] > '9')
            return 0;
        n = n * 10 + static_cast<u32>(text.data[i] - '0');
    }
    return static_cast<u16>(n);
}
} // namespace
bool user_appearance_decode(const JsonIndexedDocument& doc, JsonValue user, u64 id, u64 guild,
                               UserAppearance* output) noexcept {
    if (!id)
        return false;
    UserAppearance a{};
    a.user_id = id;
    a.guild_id = guild;
    FieldSet<"bot", "public_flags", "display_name_styles", "avatar_decoration_data", "collectibles", "primary_guild",
             "avatar"> f;
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
        FieldSet<"asset"> d;
        if (!object(doc, f[3].value, d) || !string(d[0].value, &a.decoration))
            return false;
    }
    if (present(f[4].value)) {
        FieldSet<"nameplate"> c;
        if (!object(doc, f[4].value, c))
            return false;
        if (present(c[0].value) || f[4].value.kind == JsonValueKind::null_value) {
            a.fields |= appearance_nameplate;
            FieldSet<"asset", "sku_id"> n;
            if (!object(doc, c[0].value, n) || !string(n[0].value, &a.nameplate) ||
                !number(n[1].value, &a.nameplate_sku))
                return false;
        }
    }
    if (present(f[5].value)) {
        a.fields |= appearance_guild_tag;
        FieldSet<"identity_guild_id", "identity_enabled", "tag", "badge"> g;
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
    copy_value(output, a);
    return true;
}
bool activity_decode(const JsonIndexedDocument& doc, JsonValue presence,
                             UserAppearance* output) noexcept {
    FieldSet<"activities"> f;
    if (!object(doc, presence, f))
        return false;
    if (!present(f[0].value))
        return true;
    output->fields = (output->fields | appearance_activity) & ~u32{appearance_custom_status};
    reset_value(&output->activity);
    reset_value(&output->status_emoji);
    output->status_emoji_id = 0;
    output->emoji_animated = false;
    Activities context{&doc, output, false};
    return f[0].value.kind == JsonValueKind::null_value ||
           json_visit_indexed_array(doc, f[0].value, 64, activity_item, &context).ok();
}
bool user_profile_decode(Utf8View json, Slice<u32> scratch, u64 expected, u64 guild,
                            UserProfile* p) noexcept {
    if (!expected)
        return false;
    reset_value(p);
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    FieldSet<"user", "user_profile", "guild_member", "guild_member_profile", "badges", "connected_accounts",
             "mutual_guilds", "mutual_friends", "mutual_friends_count", "premium_type", "private", "guild_badges",
             "application"> f;
    if (!object(doc, {json, 0, JsonValueKind::object}, f))
        return false;
    FieldSet<"id", "username", "global_name", "avatar", "banner", "discriminator"> u;
    if (!present(f[0].value) || !object(doc, f[0].value, u) || !number(u[0].value, &p->user_id) ||
        p->user_id != expected || !string(u[1].value, &p->username) || !p->username.bytes ||
        !string(u[2].value, &p->display_name) || !string(u[3].value, &p->avatar))
        return false;
    if (!p->display_name.bytes)
        p->display_name = p->username;
    p->guild_id = guild;
    p->discriminator = discriminator(u[5].value);
    profile_application(doc, f[12].value, p);
    if (!user_appearance_decode(doc, f[0].value, expected, 0, &p->user_appearance) ||
        !profile_details(doc, f[1].value, p, false) || !profile_details(doc, f[3].value, p, true))
        return false;
    // Some profile responses carry the global banner on the user object.
    if (!p->banner.bytes && !string(u[4].value, &p->banner))
        return false;
    if (filled(f[2].value)) {
        FieldSet<"nick", "roles", "joined_at"> m;
        if (!object(doc, f[2].value, m) || !string(m[0].value, &p->nickname) ||
            !string(m[2].value, &p->joined_at) ||
            !user_appearance_decode(doc, f[2].value, expected, guild, &p->guild_appearance))
            return false;
        if (present(m[1].value) && !json_visit_indexed_array(doc, m[1].value, 64, role_item, p).ok())
            return false;
    }
    ProfileContext context{&doc, p};
    if (filled(f[4].value) && !json_visit_indexed_array(doc, f[4].value, 64, badge_item, &context).ok())
        return false;
    if (filled(f[11].value) && !json_visit_indexed_array(doc, f[11].value, 64, badge_item, &context).ok())
        return false;
    if (filled(f[5].value) && !json_visit_indexed_array(doc, f[5].value, 64, connection_item, &context).ok())
        return false;
    PeerContext guilds{&doc, p->mutual_guilds, &p->mutual_guild_count, false},
        friends{&doc, p->mutual_friends, &p->mutual_friends_count, true};
    if (filled(f[6].value) && !json_visit_indexed_array(doc, f[6].value, 64, peer_item, &guilds).ok())
        return false;
    if (filled(f[7].value) && !json_visit_indexed_array(doc, f[7].value, 64, peer_item, &friends).ok())
        return false;
    u64 premium = 0, count = 0;
    if (!number(f[8].value, &count) || count > 0xFFFFFFFF || !number(f[9].value, &premium) ||
        premium > 255 || !boolean(f[10].value, &p->private_profile))
        return false;
    p->premium_type = static_cast<u8>(premium);
    p->mutual_friend_count = present(f[8].value) ? static_cast<u32>(count) : p->mutual_friends_count;
    return true;
}
bool profile_effect_decode(Utf8View json, Slice<u32> scratch, u64 sku,
                                   ProfileEffect* output) noexcept {
    if (!sku)
        return false;
    ProfileEffect effect{};
    effect.sku = sku;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    FieldSet<"items", "sku_id"> f;
    u64 received = 0;
    if (!object(doc, {json, 0, JsonValueKind::object}, f) || !number(f[1].value, &received) ||
        (received && received != sku))
        return false;
    EffectLayerContext context{&doc, &effect};
    if (!present(f[0].value) ||
        !json_visit_indexed_array(doc, f[0].value, 64, profile_effect_item, &context).ok())
        return false;
    for (u32 i = 1; i < effect.count; ++i) {
        u32 at = i;
        while (at && effect.layers[at - 1].z_index > effect.layers[i].z_index)
            --at;
        if (at == i)
            continue;
        ProfileEffectLayer layer;
        copy_value(&layer, effect.layers[i]);
        copy_bytes(&effect.layers[at + 1], &effect.layers[at], (i - at) * static_cast<u32>(sizeof(layer)));
        copy_value(&effect.layers[at], layer);
    }
    copy_value(output, effect);
    return true;
}
} // namespace maicord
