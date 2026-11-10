#include "maicord/model/user/profile.hpp"
#include "maicord/model/user/profile_connection.hpp"
#include "maicord/model/user/activity.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/name_list.hpp"
#include "../encoding/json_fields.hpp"

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
    u32 capacity, count, nulls;
};
bool color_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Colors*>(opaque);
    u64 n = 0;
    if (!number(value, &n) || n > 0xFFFFFF || c.count == c.capacity)
        return false;
    c.nulls += !filled(value);
    c.values[c.count++] = static_cast<u32>(n);
    return true;
}
// A null entry reads as 0 and counts in `nulls`.
bool colors(const JsonIndexedDocument& doc, const JsonValue& v, u32* out, u32 capacity, u32* count,
            u32* nulls = nullptr) noexcept {
    *count = 0;
    if (nulls)
        *nulls = 0;
    if (!filled(v))
        return true;
    Colors c{out, capacity, 0, 0};
    if (!json_visit_indexed_array(doc, v, 64, color_item, &c).ok())
        return false;
    *count = c.count;
    if (nulls)
        *nulls = c.nulls;
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
    if (!c.friends && !string(f[1].value, &b.nick))
        reset_value(&b.nick);
    ++*c.count;
    return true;
}
// A short JSON string as text in `buffer`; empty when it is not a string or does not fit.
Utf8View short_text(const JsonValue& v, char* buffer, u32 capacity) noexcept {
    JsonString input{};
    if (v.kind != JsonValueKind::string || !json_value_as_string(v, &input))
        return {};
    const auto result = json_string_decode_utf8(input, {buffer, capacity});
    return result.ok() ? Utf8View{buffer, result.bytes} : Utf8View{};
}
struct MetadataContext {
    ProfileConnection* out;
    ProfileConnectionKind kind;
};
// A metadata value as a number, `scale` times its value (a percentage keeps its first decimal): a JSON number or a
// numeric string ("20223", "99.8"). False for anything else, and the statistic is left out.
bool metadata_number(const JsonValue& v, u32 scale, u32* out) noexcept {
    char buffer[24];
    const Utf8View text = v.kind == JsonValueKind::number ? v.source : short_text(v, buffer, sizeof(buffer));
    u64 value = 0;
    u32 at = 0;
    for (; at < text.bytes && text.data[at] >= '0' && text.data[at] <= '9'; ++at) {
        value = value * 10 + static_cast<u32>(text.data[at] - '0');
        if (value > 0xFFFFFFFFull)
            return false;
    }
    if (!at)
        return false;
    value *= scale;
    if (at < text.bytes) {
        if (text.data[at] != '.' || at + 1 == text.bytes)
            return false;
        for (u32 i = at + 1; i < text.bytes; ++i)
            if (text.data[i] < '0' || text.data[i] > '9')
                return false;
        if (scale == 10)
            value += static_cast<u32>(text.data[at + 1] - '0');
    }
    *out = static_cast<u32>(value > 0xFFFFFFFFull ? 0xFFFFFFFFull : value);
    return true;
}
bool metadata_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& c = *static_cast<MetadataContext*>(opaque);
    auto& b = *c.out;
    char buffer[40];
    const auto decoded = json_string_decode_utf8(key, {buffer, sizeof(buffer)});
    if (!decoded.ok())
        return true;
    const Utf8View name{buffer, decoded.bytes};
    if (utf8_equal(name, utf8_literal("created_at"))) {
        if (!string(value, &b.created_at))
            reset_value(&b.created_at);
        // Some services leave the zone out (Reddit); the date is then read as UTC.
        else if (b.created_at.bytes == 19)
            b.created_at.data[b.created_at.bytes++] = 'Z';
        return true;
    }
    const auto field = profile_connection_field(c.kind, name);
    u32 number = 0;
    if (field.slot < 3 && metadata_number(value, field.scale, &number)) {
        b.count[field.slot] = number;
        b.count_mask = static_cast<u8>(b.count_mask | (1u << field.slot));
    } else if (field.mark && metadata_number(value, 1, &number) && number == 1)
        b.marks = static_cast<u8>(b.marks | field.mark);
    return true;
}
// A connection whose type the profile does not list, or whose fields do not fit, is left out, not the profile.
bool connection_item(void* opaque, u32 i, JsonValue v) noexcept {
    auto& c = *static_cast<ProfileContext*>(opaque);
    if (i >= 16)
        return true;
    FieldSet<"type", "id", "name", "verified", "metadata"> f;
    auto& b = c.profile->connections[c.profile->connection_count];
    if (!object(*c.doc, v, f))
        return false;
    if (!string(f[0].value, &b.type) || !string(f[2].value, &b.name) || !boolean(f[3].value, &b.verified) ||
        profile_connection_kind(b.type.view()) == ProfileConnectionKind::unknown) {
        reset_value(&b);
        return true;
    }
    // An ID that does not fit only costs the link.
    if (!string(f[1].value, &b.id))
        reset_value(&b.id);
    // Metadata is loosely typed: a field that does not read is left out.
    MetadataContext metadata{&b, profile_connection_kind(b.type.view())};
    if (f[4].value.kind == JsonValueKind::object)
        static_cast<void>(json_visit_indexed_object(*c.doc, f[4].value, 64, metadata_member, &metadata));
    ++c.profile->connection_count;
    return true;
}
// The index of `text` among the names of `list` (a NameList string), or the number of names when it is none of them.
u32 name_index(Utf8View text, const char* list) noexcept {
    u32 i = 0;
    for (NameList names{list}; names && !utf8_equal(text, names.name()); names.next())
        ++i;
    return i;
}
constexpr char kWidgetKinds[] = "favorite_games\0played_games\0current_games\0want_to_play_games\0application\0";
constexpr char kWidgetTags[] =
    "noob\0learning_the_ropes\0casual\0getting_good\0intermediate\0expert\0better_than_you\0obsessed\0love_it\0"
    "kind_of_love_it\0kind_of_hate_it\0rage_quitting\0like_it\0frustrated\0too_easy\0looking_for_group\0"
    "open_to_play\0looking_for_tips\0open_to_teach\0looking_to_discuss\0";
static_assert(name_list_count(kWidgetKinds) == 5 && name_list_count(kWidgetTags) == profile_widget_tag_count);
bool widget_tag(void* opaque, u32, JsonValue v) noexcept {
    char text[32];
    const u32 tag = name_index(short_text(v, text, sizeof(text)), kWidgetTags);
    if (tag < profile_widget_tag_count)
        *static_cast<u32*>(opaque) |= 1u << tag;
    return true;
}
// A widget's game; one that does not read is left out, not the profile.
bool widget_game(void* opaque, u32, JsonValue v) noexcept {
    auto& c = *static_cast<ProfileContext*>(opaque);
    auto& p = *c.profile;
    if (p.widget_game_count >= kProfileWidgetGameMaximum)
        return true;
    FieldSet<"game_id", "comment", "tags"> f;
    auto& g = p.widget_games[p.widget_game_count];
    reset_value(&g);
    if (!object(*c.doc, v, f) || !number(f[0].value, &g.game_id) || !g.game_id)
        return true;
    // A long comment is cut at a character boundary.
    char text[1024];
    const auto comment = short_text(f[1].value, text, sizeof(text));
    const u32 bytes = utf8_truncated_length(comment, sizeof(g.comment.data));
    copy_bytes(g.comment.data, comment.data, bytes);
    g.comment.bytes = static_cast<u16>(bytes);
    if (f[2].value.kind == JsonValueKind::array)
        static_cast<void>(json_visit_indexed_array(*c.doc, f[2].value, 64, widget_tag, &g.tags));
    ++p.widget_game_count;
    ++p.widgets[p.widget_count].game_count;
    return true;
}
// A game widget; one of an unknown type, or that does not read, is left out.
bool widget_item(void* opaque, u32, JsonValue v) noexcept {
    auto& c = *static_cast<ProfileContext*>(opaque);
    auto& p = *c.profile;
    if (p.widget_count >= kProfileWidgetMaximum)
        return true;
    FieldSet<"id", "data"> f;
    FieldSet<"type", "games", "application_id"> d;
    char text[32];
    if (!object(*c.doc, v, f) || !object(*c.doc, f[1].value, d))
        return true;
    const u32 kind = name_index(short_text(d[0].value, text, sizeof(text)), kWidgetKinds);
    auto& w = p.widgets[p.widget_count];
    reset_value(&w);
    if (kind == 5 || !number(f[0].value, &w.id) || !number(d[2].value, &w.application_id))
        return true;
    w.kind = static_cast<ProfileWidgetKind>(kind);
    w.first_game = p.widget_game_count;
    if (kind != 4 && d[1].value.kind == JsonValueKind::array)
        static_cast<void>(json_visit_indexed_array(*c.doc, d[1].value, 64, widget_game, &c));
    ++p.widget_count;
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
    u32 count = 0, nulls = 0;
    u64 accent = 0;
    if (!colors(doc, f[3].value, guild ? p->guild_theme_colors : p->theme_colors, 2, &count, &nulls) ||
        !number(f[4].value, &accent) || accent > 0xFFFFFF)
        return false;
    // Both colours make a theme. A member profile's list with a null ([null, null], a per-server reset) still
    // stands in for the user's theme: the card then has no theme colours of its own.
    const bool theme = count == 2 && !nulls;
    if (guild) {
        p->has_guild_theme = theme;
        p->guild_theme_reset = count && !theme;
    } else {
        p->has_theme = theme;
        p->has_accent = filled(f[4].value);
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
             "avatar", "system"> f;
    if (!object(doc, user, f))
        return false;
    if (present(f[0].value)) {
        a.fields |= appearance_bot;
        if (!boolean(f[0].value, &a.bot))
            return false;
    }
    if (present(f[7].value)) {
        a.fields |= appearance_system;
        if (!boolean(f[7].value, &a.system))
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
             "application", "widgets"> f;
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
    // Widgets that do not read are left out, not the profile.
    if (filled(f[13].value) && f[13].value.kind == JsonValueKind::array)
        static_cast<void>(json_visit_indexed_array(doc, f[13].value, 64, widget_item, &context));
    u64 premium = 0, count = 0;
    if (!number(f[8].value, &count) || count > 0xFFFFFFFF || !number(f[9].value, &premium) ||
        premium > 255 || !boolean(f[10].value, &p->private_profile))
        return false;
    p->premium_type = static_cast<u8>(premium);
    p->mutual_friend_count = present(f[8].value) ? static_cast<u32>(count) : p->mutual_friends_count;
    return true;
}
u32 profile_widget_game_ids(const UserProfile& p, u64* output, u32 capacity) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < p.widget_game_count && count < capacity; ++i) {
        const u64 game = p.widget_games[i].game_id;
        u32 seen = 0;
        while (seen < count && output[seen] != game)
            ++seen;
        if (seen == count)
            output[count++] = game;
    }
    return count;
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
