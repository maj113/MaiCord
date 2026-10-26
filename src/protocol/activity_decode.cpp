#include "maicord/model/user_activity.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "json_field_lists.hpp"

namespace maicord {
namespace {
u32 field_offset(const UserActivity& a, UserActivityField field) noexcept {
    u32 offset = 0;
    for (u32 i = 0; i < static_cast<u32>(field); ++i)
        offset += a.lengths[i];
    return offset;
}
struct Writer {
    char* data;
    u32 capacity, used;
    bool ok = true;
    void put(Utf8View text) noexcept {
        if (!ok || text.bytes > capacity - used) {
            ok = false;
            return;
        }
        copy_bytes(data + used, text.data, text.bytes);
        used += text.bytes;
    }
    void number(u64 value) noexcept {
        char digits[20];
        put({digits, decimal_write(digits, value)});
    }
    u32 finish() const noexcept {
        return ok ? used : 0;
    }
};
bool digits_only(Utf8View text) noexcept {
    if (!text.bytes || text.bytes > 20)
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (text.data[i] < '0' || text.data[i] > '9')
            return false;
    return true;
}
// A media proxy path must stay a plain URL path: no scheme switch, query
// injection or whitespace. The media proxy's paths are "external/<token>/..."
// and "attachments/...".
bool proxy_path(Utf8View path) noexcept {
    if (path.bytes < 2 || path.data[0] == '/' || path.bytes > 400)
        return false;
    for (u32 i = 0; i < path.bytes; ++i) {
        const u8 c = static_cast<u8>(path.data[i]);
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == '#' || c == '<' || c == '>')
            return false;
        if (c == '.' && i + 1 < path.bytes && path.data[i + 1] == '.')
            return false;
    }
    return true;
}
} // namespace

Utf8View user_activity_text(const UserActivity& a, UserActivityField field) noexcept {
    if (field >= UserActivityField::count)
        return {};
    const u32 offset = field_offset(a, field);
    const u32 bytes = a.lengths[static_cast<u32>(field)];
    if (offset > kUserActivityTextBytes || bytes > kUserActivityTextBytes - offset)
        return {};
    return {a.text + offset, bytes};
}
bool user_activity_is_spotify(const UserActivity& a) noexcept {
    return a.type == UserActivityType::listening && (a.detail & user_activity_spotify_party) &&
           utf8_equal(user_activity_text(a, UserActivityField::name), utf8_literal("Spotify"));
}
u32 user_activity_summary(UserActivityType type, Utf8View name, Utf8View details, Slice<char> output) noexcept {
    Utf8View verb{};
    Utf8View subject = name;
    switch (type) {
    case UserActivityType::playing:
        verb = utf8_literal("Playing ");
        break;
    case UserActivityType::streaming:
        verb = utf8_literal("Streaming ");
        if (details.bytes)
            subject = details;
        break;
    case UserActivityType::listening:
        verb = utf8_literal("Listening to ");
        break;
    case UserActivityType::watching:
        verb = utf8_literal("Watching ");
        break;
    case UserActivityType::competing:
        verb = utf8_literal("Competing in ");
        break;
    default:
        return 0;
    }
    if (!subject.bytes || output.count <= verb.bytes)
        return 0;
    Writer w{output.data, static_cast<u32>(output.count), 0};
    w.put(verb);
    w.put({subject.data, utf8_truncated_length(subject, w.capacity - w.used)});
    return w.finish();
}
u32 user_activity_summary(const UserActivity& a, Slice<char> output) noexcept {
    return user_activity_summary(a.type, user_activity_text(a, UserActivityField::name),
                                 user_activity_text(a, UserActivityField::details), output);
}
u32 user_activity_status_text(const UserActivity& a, Slice<char> output) noexcept {
    using F = UserActivityField;
    const auto name = user_activity_text(a, F::name), details = user_activity_text(a, F::details),
               state = user_activity_text(a, F::state);
    Writer w{output.data, static_cast<u32>(output.count), 0};
    if (user_activity_is_spotify(a) && state.bytes) {
        // Artists arrive as "A; B".
        for (u32 i = 0; i < state.bytes && w.ok; ++i) {
            if (state.data[i] == ';' && i + 1 < state.bytes && state.data[i + 1] == ' ')
                w.put(utf8_literal(","));
            else
                w.put({state.data + i, 1});
        }
        return w.finish();
    }
    Utf8View value = a.type == UserActivityType::streaming && details.bytes ? details : name;
    const u32 display = (a.detail & user_activity_display_mask) >> user_activity_display_shift;
    if (display == 2 && state.bytes)
        value = state;
    else if (display == 3 && details.bytes)
        value = details;
    else if (display == 1)
        value = name;
    Utf8View verb{};
    switch (a.type) {
    case UserActivityType::playing:
        break;
    case UserActivityType::streaming:
        verb = utf8_literal("Streaming ");
        break;
    case UserActivityType::listening:
        verb = utf8_literal("Listening to ");
        break;
    case UserActivityType::watching:
        verb = utf8_literal("Watching ");
        break;
    case UserActivityType::competing:
        verb = utf8_literal("Competing in ");
        break;
    default:
        return 0;
    }
    if (!value.bytes || output.count <= verb.bytes)
        return 0;
    w.put(verb);
    w.put({value.data, utf8_truncated_length(value, w.capacity - w.used)});
    return w.finish();
}
namespace {
// The Xbox application (its presences may carry no platform).
constexpr u64 kXboxApplicationId = 438122941302046720ull;
// A headless session ("h:<platform>,...") names the service the activity
// runs on.
Utf8View session_platform(const UserActivity& a) noexcept {
    const auto session = user_activity_text(a, UserActivityField::session);
    if (!utf8_starts_with(session, utf8_literal("h:")))
        return {};
    u32 end = 2;
    while (end < session.bytes && session.data[end] != ',')
        ++end;
    const Utf8View type{session.data + 2, end - 2};
    struct Name {
        Utf8View type, name;
    };
    static constexpr Name names[] = {
        {utf8_literal("battlenet"), utf8_literal("Battle.net")}, {utf8_literal("epicgames"), utf8_literal("Epic Games")},
        {utf8_literal("leagueoflegends"), utf8_literal("League of Legends")},
        {utf8_literal("playstation"), utf8_literal("PlayStation")}, {utf8_literal("riotgames"), utf8_literal("Riot Games")},
        {utf8_literal("roblox"), utf8_literal("Roblox")},         {utf8_literal("steam"), utf8_literal("Steam")},
        {utf8_literal("xbox"), utf8_literal("Xbox")}};
    for (const auto& n : names)
        if (utf8_equal(n.type, type))
            return n.name;
    return {};
}
} // namespace
u32 user_activity_heading(const UserActivity& a, Slice<char> output) noexcept {
    Writer w{output.data, static_cast<u32>(output.count), 0};
    const auto name = user_activity_text(a, UserActivityField::name);
    const bool details = user_activity_text(a, UserActivityField::details).bytes != 0;
    const bool xbox = a.application_id == kXboxApplicationId || a.platform == UserActivityPlatform::xbox;
    const bool playstation = a.platform == UserActivityPlatform::ps4 || a.platform == UserActivityPlatform::ps5;
    if (a.type == UserActivityType::playing && (xbox || playstation)) {
        w.put(utf8_literal("Playing on "));
        w.put(xbox ? utf8_literal("Xbox") : utf8_literal("PlayStation"));
        return w.finish();
    }
    if (a.type == UserActivityType::streaming &&
        (utf8_equal(name, utf8_literal("Twitch")) || utf8_equal(name, utf8_literal("YouTube")))) {
        w.put(utf8_literal("Streaming "));
        w.put(name);
        return w.finish();
    }
    const auto platform = session_platform(a);
    if (platform.bytes && a.type != UserActivityType::custom) {
        static constexpr Utf8View verbs[] = {utf8_literal("Playing on "),   utf8_literal("Live on "),
                                      utf8_literal("Listening on "), utf8_literal("Watching on "),
                                      {},                            utf8_literal("Competing on ")};
        w.put(verbs[static_cast<u32>(a.type)]);
        w.put(platform);
        return w.finish();
    }
    Utf8View verb{};
    switch (a.type) {
    case UserActivityType::playing:
        w.put(utf8_literal("Playing"));
        return w.finish();
    case UserActivityType::streaming:
        w.put(utf8_literal("Streaming"));
        return w.finish();
    case UserActivityType::listening:
        verb = utf8_literal("Listening to");
        break;
    case UserActivityType::watching:
        verb = utf8_literal("Watching");
        break;
    case UserActivityType::competing:
        verb = utf8_literal("Competing in");
        break;
    default:
        return 0;
    }
    // "Listening to Spotify" names the application only beside details.
    w.put(verb);
    if (details && name.bytes) {
        w.put(utf8_literal(" "));
        w.put({name.data, utf8_truncated_length(name, w.ok ? w.capacity - w.used : 0)});
    }
    return w.finish();
}
namespace {
// Assets are requested at the official card's 160 px.
constexpr auto kAssetSize = utf8_literal("160");
bool alphanumeric(Utf8View text, u32 limit) noexcept {
    if (!text.bytes || text.bytes > limit)
        return false;
    for (u32 i = 0; i < text.bytes; ++i) {
        const char c = text.data[i];
        if (!ascii_is_alnum(c))
            return false;
    }
    return true;
}
} // namespace
u32 user_activity_image_url(const UserActivity& a, bool large, Slice<char> output,
                            MediaAttachmentFormat* format) noexcept {
    const auto key = user_activity_text(a, large ? UserActivityField::large_image : UserActivityField::small_image);
    Writer w{output.data, static_cast<u32>(output.count), 0};
    u32 colon = 0;
    while (colon < key.bytes && key.data[colon] != ':')
        ++colon;
    if (colon < key.bytes) {
        // "<prefix>:<value>"; as in the official client the value ends at a
        // second colon.
        u32 end = colon + 1;
        while (end < key.bytes && key.data[end] != ':')
            ++end;
        const Utf8View prefix{key.data, colon}, value{key.data + colon + 1, end - colon - 1};
        if (utf8_equal(prefix, utf8_literal("mp"))) {
            if (!proxy_path(value))
                return 0;
            bool query = false;
            for (u32 i = 0; i < value.bytes; ++i)
                query |= value.data[i] == '?';
            w.put(utf8_literal("https://media.discordapp.net/"));
            w.put(value);
            w.put(query ? utf8_literal("&format=webp&width=") : utf8_literal("?format=webp&width="));
            w.put(kAssetSize);
            w.put(utf8_literal("&height="));
            w.put(kAssetSize);
            *format = MediaAttachmentFormat::webp;
            return w.finish();
        }
        if (utf8_equal(prefix, utf8_literal("spotify")) && alphanumeric(value, 64)) {
            w.put(utf8_literal("https://i.scdn.co/image/"));
            w.put(value);
            *format = MediaAttachmentFormat::jpeg;
            return w.finish();
        }
        return 0;
    }
    if (!a.application_id || !digits_only(key))
        return 0;
    w.put(utf8_literal("https://cdn.discordapp.com/app-assets/"));
    w.number(a.application_id);
    w.put(utf8_literal("/"));
    w.put(key);
    w.put(utf8_literal(".png?size="));
    w.put(kAssetSize);
    *format = MediaAttachmentFormat::png;
    return w.finish();
}
u32 user_activity_application_icon_url(u64 application_id, Utf8View icon, Slice<char> output) noexcept {
    if (!application_id || !alphanumeric(icon, 64))
        return 0;
    Writer w{output.data, static_cast<u32>(output.count), 0};
    w.put(utf8_literal("https://cdn.discordapp.com/app-icons/"));
    w.number(application_id);
    w.put(utf8_literal("/"));
    w.put(icon);
    w.put(utf8_literal(".png?size="));
    w.put(kAssetSize);
    return w.finish();
}
UserActivityArtwork user_activity_artwork(const UserActivity& a, bool application_icon) noexcept {
    char url[512];
    MediaAttachmentFormat format{};
    const bool large = user_activity_image_url(a, true, {url, sizeof(url)}, &format) != 0;
    const bool badge = user_activity_image_url(a, false, {url, sizeof(url)}, &format) != 0;
    if (large)
        return {UserActivityArt::large, badge};
    if (application_icon)
        return {UserActivityArt::application, badge};
    return {badge ? UserActivityArt::small_image : UserActivityArt::none, false};
}
u32 user_activity_applications_target(ConstSlice<u64> ids, Slice<u8> output) noexcept {
    if (!ids.count || ids.count > kUserActivityMaximum)
        return 0;
    Writer w{reinterpret_cast<char*>(output.data), static_cast<u32>(output.count), 0};
    w.put(utf8_literal("/api/v10/applications/public"));
    for (u32 i = 0; i < ids.count; ++i) {
        if (!ids[i])
            return 0;
        w.put(i ? utf8_literal("&application_ids=") : utf8_literal("?application_ids="));
        w.number(ids[i]);
    }
    return w.finish();
}
u32 user_activity_metadata_target(u64 user_id, Utf8View session, u64 application_id, Slice<u8> output) noexcept {
    constexpr u32 session_limit = kUserActivityFieldBudget[static_cast<u32>(UserActivityField::session)];
    if (!user_id || !session.bytes || session.bytes > session_limit)
        return 0;
    // Session IDs are hex, or "h:<platform>,..." for headless sessions.
    for (u32 i = 0; i < session.bytes; ++i) {
        const char c = session.data[i];
        if (!(ascii_is_alnum(c) || c == ':' || c == ',' ||
              c == '_' || c == '-' || c == '.'))
            return 0;
    }
    Writer w{reinterpret_cast<char*>(output.data), static_cast<u32>(output.count), 0};
    w.put(utf8_literal("/api/v10/users/"));
    w.number(user_id);
    w.put(utf8_literal("/sessions/"));
    w.put(session);
    w.put(utf8_literal("/activities/"));
    w.number(application_id);
    w.put(utf8_literal("/metadata"));
    return w.finish();
}
namespace {
enum ActivityMember : u32 {
    activity_type, activity_name, activity_details, activity_state, activity_url, activity_application_id,
    activity_timestamps, activity_created_at, activity_party, activity_assets, activity_buttons, activity_flags,
    activity_session_id, activity_platform, activity_display, activity_member_count
};
struct ActivityFields {
    JsonValue v[activity_member_count];
};
bool activity_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& f = *static_cast<ActivityFields*>(opaque);
    const u32 index = protocol_detail::wide_member_index<"type", "name", "details", "state", "url", "application_id",
                                                         "timestamps", "created_at", "party", "assets", "buttons",
                                                         "flags", "session_id", "platform", "status_display_type">(key);
    // First occurrence wins.
    if (index < activity_member_count && !f.v[index].source.data)
        f.v[index] = value;
    return true;
}
bool present(const JsonValue& v) noexcept {
    return v.source.data && v.kind != JsonValueKind::null_value;
}
// Numbers may arrive as JSON numbers or decimal strings (snowflakes).
u64 number(const JsonValue& v) noexcept {
    u64 n = 0;
    if (!present(v))
        return 0;
    if (v.kind == JsonValueKind::string)
        return json_value_to_snowflake(v, &n) ? n : 0;
    return json_value_to_u64(v, &n) ? n : 0;
}
// Decoded, validated and bounded; oversized or malformed strings are empty.
u32 decode(const JsonValue& v, char* output, u32 budget, bool whole) noexcept {
    JsonString s{};
    if (!present(v) || v.kind != JsonValueKind::string || !json_value_as_string(v, &s))
        return 0;
    char scratch[1024];
    const auto result = json_string_decode_utf8(s, {scratch, sizeof(scratch)});
    if (!result.ok())
        return 0;
    const Utf8View text{scratch, result.bytes};
    if (!utf8_validate(text).valid)
        return 0;
    if (whole && text.bytes > budget)
        return 0;
    u32 begin = 0, end = text.bytes;
    while (begin < end && (scratch[begin] == ' ' || scratch[begin] == '\n' || scratch[begin] == '\t'))
        ++begin;
    while (end > begin && (scratch[end - 1] == ' ' || scratch[end - 1] == '\n' || scratch[end - 1] == '\t'))
        --end;
    const u32 bytes = utf8_truncated_length({scratch + begin, end - begin}, budget);
    copy_bytes(output, scratch + begin, bytes);
    return bytes;
}
struct Pending {
    char text[kUserActivityFieldCount][320];
    u16 bytes[kUserActivityFieldCount];
};
UserActivityPlatform platform(const JsonValue& v) noexcept {
    char text[16];
    const u32 bytes = decode(v, text, sizeof(text), true);
    static constexpr Utf8View names[] = {utf8_literal("desktop"),  utf8_literal("xbox"),     utf8_literal("ps4"),
                                  utf8_literal("ps5"),      utf8_literal("ios"),      utf8_literal("android"),
                                  utf8_literal("embedded"), utf8_literal("samsung"),  utf8_literal("meta_quest")};
    for (u32 i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (utf8_equal(names[i], {text, bytes}))
            return static_cast<UserActivityPlatform>(i);
    return UserActivityPlatform::desktop;
}
struct Members {
    JsonValue a, b, c, d;
    Utf8View ka, kb, kc, kd;
};
bool members(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& m = *static_cast<Members*>(opaque);
    if (m.ka.bytes && json_name_is(key, m.ka) && !m.a.source.data)
        m.a = value;
    else if (m.kb.bytes && json_name_is(key, m.kb) && !m.b.source.data)
        m.b = value;
    else if (m.kc.bytes && json_name_is(key, m.kc) && !m.c.source.data)
        m.c = value;
    else if (m.kd.bytes && json_name_is(key, m.kd) && !m.d.source.data)
        m.d = value;
    return true;
}
bool object(const JsonIndexedDocument& doc, JsonValue v, Members* m) noexcept {
    if (!present(v))
        return true;
    return v.kind == JsonValueKind::object && json_visit_indexed_object(doc, v, 64, members, m).ok();
}
struct Array {
    JsonValue items[2];
    u32 count;
};
bool array_item(void* opaque, u32, JsonValue value) noexcept {
    auto& a = *static_cast<Array*>(opaque);
    if (a.count < 2)
        a.items[a.count] = value;
    ++a.count;
    return true;
}
struct Decode {
    const JsonIndexedDocument* doc;
    UserActivitySet* out;
};
bool activity_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Decode*>(opaque);
    if (value.kind != JsonValueKind::object)
        return true; // Ignore malformed rows; the presence itself stays valid.
    ActivityFields f{};
    if (!json_visit_indexed_object(*c.doc, value, 64, activity_member, &f).ok())
        return false;
    const u64 type = number(f.v[activity_type]);
    if (type == 4 || type > 5 || c.out->count == kUserActivityMaximum)
        return true;
    Pending pending{};
    using F = UserActivityField;
    const auto take = [&](F field, JsonValue v, bool whole) noexcept {
        const u32 i = static_cast<u32>(field);
        pending.bytes[i] = static_cast<u16>(decode(v, pending.text[i], kUserActivityFieldBudget[i], whole));
    };
    take(F::name, f.v[activity_name], false);
    if (!pending.bytes[0])
        return true;
    take(F::details, f.v[activity_details], false);
    take(F::state, f.v[activity_state], false);
    if (type == 1)
        take(F::url, f.v[activity_url], true);
    take(F::session, f.v[activity_session_id], true);
    Members assets{{}, {}, {}, {}, utf8_literal("large_image"), utf8_literal("large_text"),
                   utf8_literal("small_image"), utf8_literal("small_text")};
    if (!object(*c.doc, f.v[activity_assets], &assets))
        return false;
    take(F::large_image, assets.a, true);
    take(F::large_text, assets.b, false);
    take(F::small_image, assets.c, true);
    take(F::small_text, assets.d, false);
    u8 buttons = 0;
    if (present(f.v[activity_buttons]) && f.v[activity_buttons].kind == JsonValueKind::array) {
        Array list{};
        if (!json_visit_indexed_array(*c.doc, f.v[activity_buttons], 64, array_item, &list).ok())
            return false;
        for (u32 i = 0; i < list.count && i < 2; ++i) {
            // Other users' buttons are labels; our own may be {label,url}.
            JsonValue label = list.items[i];
            if (label.kind == JsonValueKind::object) {
                Members m{{}, {}, {}, {}, utf8_literal("label")};
                if (!object(*c.doc, label, &m))
                    return false;
                label = m.a;
            }
            const auto field = i ? F::button_1 : F::button_0;
            take(field, label, false);
            if (pending.bytes[static_cast<u32>(field)])
                ++buttons;
        }
        if (buttons == 1 && !pending.bytes[static_cast<u32>(F::button_0)]) {
            copy_bytes(pending.text[8], pending.text[9], pending.bytes[9]);
            pending.bytes[8] = pending.bytes[9];
            pending.bytes[9] = 0;
        }
    }
    auto& a = c.out->items[c.out->count];
    reset_value(&a);
    a.type = static_cast<UserActivityType>(type);
    a.application_id = number(f.v[activity_application_id]);
    a.created_ms = number(f.v[activity_created_at]);
    a.flags = static_cast<u32>(number(f.v[activity_flags]));
    Members times{{}, {}, {}, {}, utf8_literal("start"), utf8_literal("end")};
    if (!object(*c.doc, f.v[activity_timestamps], &times))
        return false;
    a.start_ms = number(times.a);
    a.end_ms = number(times.b);
    // Some integrations send seconds instead of milliseconds.
    if (a.start_ms && a.start_ms < 100'000'000'000ull)
        a.start_ms *= 1000;
    if (a.end_ms && a.end_ms < 100'000'000'000ull)
        a.end_ms *= 1000;
    Members party{{}, {}, {}, {}, utf8_literal("size"), utf8_literal("id")};
    if (!object(*c.doc, f.v[activity_party], &party))
        return false;
    if (present(f.v[activity_party]) && f.v[activity_party].kind == JsonValueKind::object) {
        a.detail |= user_activity_has_party;
        char id[16];
        if (decode(party.b, id, sizeof(id), false) >= 8 && bytes_equal(id, "spotify:", 8))
            a.detail |= user_activity_spotify_party;
    }
    a.platform = platform(f.v[activity_platform]);
    if (present(f.v[activity_display]) && f.v[activity_display].kind == JsonValueKind::number) {
        const u64 display = number(f.v[activity_display]);
        if (display <= 2)
            a.detail |= static_cast<u8>((display + 1) << user_activity_display_shift);
    }
    if (present(party.a) && party.a.kind == JsonValueKind::array) {
        Array size{};
        if (!json_visit_indexed_array(*c.doc, party.a, 64, array_item, &size).ok())
            return false;
        if (size.count == 2) {
            const u64 now = number(size.items[0]), max = number(size.items[1]);
            if (now <= 0xffff && max <= 0xffff && now <= max) {
                a.party_size = static_cast<u32>(now);
                a.party_max = static_cast<u32>(max);
            }
        }
    }
    // Pack in field order; text first, image keys only when they fit whole.
    u32 total = 0;
    for (u32 i = 0; i < kUserActivityFieldCount; ++i)
        total += pending.bytes[i];
    const F drop_order[] = {F::session, F::small_image, F::small_text, F::url, F::large_text, F::button_1,
                            F::button_0, F::large_image, F::state, F::details};
    for (u32 k = 0; total > kUserActivityTextBytes && k < sizeof(drop_order) / sizeof(drop_order[0]); ++k) {
        const u32 i = static_cast<u32>(drop_order[k]);
        total -= pending.bytes[i];
        pending.bytes[i] = 0;
    }
    u32 at = 0;
    for (u32 i = 0; i < kUserActivityFieldCount; ++i) {
        copy_bytes(a.text + at, pending.text[i], pending.bytes[i]);
        a.lengths[i] = pending.bytes[i];
        at += pending.bytes[i];
    }
    a.button_count = static_cast<u8>((a.lengths[8] != 0) + (a.lengths[9] != 0));
    ++c.out->count;
    return true;
}
bool root_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& activities = *static_cast<JsonValue*>(opaque);
    if (json_name_is(key, utf8_literal("activities")) && !activities.source.data)
        activities = value;
    return true;
}
} // namespace

bool user_activities_decode(const JsonIndexedDocument& doc, JsonValue presence, u64 user_id,
                            UserActivitySet* output, bool* has) noexcept {
    reset_value(output);
    output->user_id = user_id;
    *has = false;
    if (!presence.source.data || presence.kind != JsonValueKind::object)
        return presence.kind == JsonValueKind::null_value || !presence.source.data;
    JsonValue activities{};
    if (!json_visit_indexed_object(doc, presence, 64, root_member, &activities).ok())
        return false;
    if (!activities.source.data)
        return true;
    *has = true;
    if (activities.kind == JsonValueKind::null_value)
        return true;
    if (activities.kind != JsonValueKind::array)
        return false;
    Decode context{&doc, output};
    return json_visit_indexed_array(doc, activities, 64, activity_item, &context).ok();
}
namespace {
struct ApplicationsContext {
    const JsonIndexedDocument* doc;
    UserActivityApplicationVisit visit;
    void* context;
};
bool application_item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<ApplicationsContext*>(opaque);
    Members m{{}, {}, {}, {}, utf8_literal("id"), utf8_literal("icon")};
    if (value.kind != JsonValueKind::object || !object(*c.doc, value, &m))
        return false;
    const u64 id = number(m.a);
    char icon[72];
    const u32 bytes = decode(m.b, icon, 64, true);
    if (id)
        c.visit(c.context, id, {icon, bytes});
    return true;
}
} // namespace
bool user_activity_applications_decode(Utf8View body, Slice<u32> structurals, UserActivityApplicationVisit visit,
                                       void* context) noexcept {
    if (!body.bytes || body.data[0] != '[')
        return false;
    const auto scan = json_scan_structurals_avx1(body, structurals);
    if (!scan.ok())
        return false;
    const JsonIndexedDocument doc{body, {structurals.data, scan.count}};
    ApplicationsContext c{&doc, visit, context};
    return json_visit_indexed_array(doc, {body, 0, JsonValueKind::array}, 64, application_item, &c).ok();
}
u32 user_activity_button_url(Utf8View body, Slice<u32> structurals, u32 index, Slice<char> output) noexcept {
    if (index > 1 || !body.bytes || body.data[0] != '{')
        return 0;
    const auto scan = json_scan_structurals_avx1(body, structurals);
    if (!scan.ok())
        return 0;
    const JsonIndexedDocument doc{body, {structurals.data, scan.count}};
    Members m{{}, {}, {}, {}, utf8_literal("button_urls")};
    if (!object(doc, {body, 0, JsonValueKind::object}, &m) || !present(m.a) || m.a.kind != JsonValueKind::array)
        return 0;
    Array urls{};
    if (!json_visit_indexed_array(doc, m.a, 64, array_item, &urls).ok() || index >= urls.count)
        return 0;
    char text[1024];
    const u32 bytes = decode(urls.items[index], text, sizeof(text), true);
    const Utf8View url{text, bytes};
    // Opened like any untrusted link: http(s) only, no whitespace or controls.
    if (!utf8_starts_with(url, utf8_literal("https://")) && !utf8_starts_with(url, utf8_literal("http://")))
        return 0;
    for (u32 i = 0; i < bytes; ++i)
        if (static_cast<u8>(text[i]) <= 0x20 || text[i] == 0x7f)
            return 0;
    if (bytes > output.count)
        return 0;
    copy_bytes(output.data, text, bytes);
    return bytes;
}
} // namespace maicord
