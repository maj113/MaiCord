#include "maicord/model/user_activity.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/json_ids.hpp"

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
    if (a.type != UserActivityType::listening)
        return false;
    const auto name = user_activity_text(a, UserActivityField::name);
    const auto image = user_activity_text(a, UserActivityField::large_image);
    return (name.bytes == 7 && bytes_equal(name.data, "Spotify", 7)) ||
           utf8_starts_with(image, utf8_literal("spotify:"));
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
    if (!subject.bytes || !output.data || output.count <= verb.bytes)
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
u32 user_activity_heading(const UserActivity& a, Slice<char> output) noexcept {
    if (!output.data)
        return 0;
    Writer w{output.data, static_cast<u32>(output.count), 0};
    switch (a.type) {
    case UserActivityType::playing:
        w.put(utf8_literal("Playing"));
        break;
    case UserActivityType::streaming:
        w.put(utf8_literal("Live on "));
        {
            const auto name = user_activity_text(a, UserActivityField::name);
            w.put({name.data, utf8_truncated_length(name, w.capacity - w.used)});
        }
        break;
    case UserActivityType::listening: {
        w.put(utf8_literal("Listening to "));
        const auto name = user_activity_text(a, UserActivityField::name);
        w.put({name.data, utf8_truncated_length(name, w.capacity - w.used)});
        break;
    }
    case UserActivityType::watching:
        w.put(utf8_literal("Watching"));
        break;
    case UserActivityType::competing:
        w.put(utf8_literal("Competing"));
        break;
    default:
        return 0;
    }
    return w.finish();
}
u32 user_activity_image_url(const UserActivity& a, bool large, Slice<char> output, bool* webp) noexcept {
    if (!output.data || !webp)
        return 0;
    const auto key = user_activity_text(a, large ? UserActivityField::large_image : UserActivityField::small_image);
    Writer w{output.data, static_cast<u32>(output.count), 0};
    if (utf8_starts_with(key, utf8_literal("mp:"))) {
        const Utf8View path{key.data + 3, key.bytes - 3};
        if (!proxy_path(path))
            return 0;
        bool query = false;
        for (u32 i = 0; i < path.bytes; ++i)
            query |= path.data[i] == '?';
        w.put(utf8_literal("https://media.discordapp.net/"));
        w.put(path);
        w.put(query ? utf8_literal("&format=webp&width=128&height=128")
                    : utf8_literal("?format=webp&width=128&height=128"));
        *webp = true;
        return w.finish();
    }
    if (!a.application_id || !digits_only(key))
        return 0;
    w.put(utf8_literal("https://cdn.discordapp.com/app-assets/"));
    w.number(a.application_id);
    w.put(utf8_literal("/"));
    w.put(key);
    w.put(utf8_literal(".png?size=128"));
    *webp = false;
    return w.finish();
}
float user_activity_progress(const UserActivity& a, u64 now) noexcept {
    if (!a.start_ms || !a.end_ms || a.end_ms <= a.start_ms)
        return -1;
    if (now <= a.start_ms)
        return 0;
    if (now >= a.end_ms)
        return 1;
    return static_cast<float>(static_cast<double>(now - a.start_ms) / static_cast<double>(a.end_ms - a.start_ms));
}

namespace {
struct ActivityFields {
    JsonValue type, name, details, state, url, application_id, timestamps, created_at, party, assets, buttons,
        flags;
};
bool activity_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& f = *static_cast<ActivityFields*>(opaque);
    struct Entry {
        Utf8View key;
        JsonValue* slot;
    } const entries[] = {{utf8_literal("type"), &f.type},
                         {utf8_literal("name"), &f.name},
                         {utf8_literal("details"), &f.details},
                         {utf8_literal("state"), &f.state},
                         {utf8_literal("url"), &f.url},
                         {utf8_literal("application_id"), &f.application_id},
                         {utf8_literal("timestamps"), &f.timestamps},
                         {utf8_literal("created_at"), &f.created_at},
                         {utf8_literal("party"), &f.party},
                         {utf8_literal("assets"), &f.assets},
                         {utf8_literal("buttons"), &f.buttons},
                         {utf8_literal("flags"), &f.flags}};
    for (const auto& entry : entries)
        if (json_string_equals_ascii(key, entry.key)) {
            if (!entry.slot->source.data) // first occurrence wins
                *entry.slot = value;
            break;
        }
    return true;
}
bool present(JsonValue v) noexcept {
    return v.source.data && v.kind != JsonValueKind::null_value;
}
// Numbers may arrive as JSON numbers or decimal strings (snowflakes).
u64 number(JsonValue v) noexcept {
    u64 n = 0;
    if (!present(v))
        return 0;
    if (v.kind == JsonValueKind::string)
        return json_value_to_snowflake(v, &n) ? n : 0;
    return json_value_to_u64(v, &n) ? n : 0;
}
// Decoded, validated and bounded; oversized or malformed strings are empty.
u32 decode(JsonValue v, char* output, u32 budget, bool whole) noexcept {
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
    char text[kUserActivityFieldCount][256];
    u16 bytes[kUserActivityFieldCount];
};
struct Members {
    JsonValue a, b, c, d;
    Utf8View ka, kb, kc, kd;
};
bool members(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& m = *static_cast<Members*>(opaque);
    if (m.ka.bytes && json_string_equals_ascii(key, m.ka) && !m.a.source.data)
        m.a = value;
    else if (m.kb.bytes && json_string_equals_ascii(key, m.kb) && !m.b.source.data)
        m.b = value;
    else if (m.kc.bytes && json_string_equals_ascii(key, m.kc) && !m.c.source.data)
        m.c = value;
    else if (m.kd.bytes && json_string_equals_ascii(key, m.kd) && !m.d.source.data)
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
    const u64 type = number(f.type);
    if (type == 4 || type > 5 || c.out->count == kUserActivityMaximum)
        return true;
    Pending pending{};
    using F = UserActivityField;
    const auto take = [&](F field, JsonValue v, bool whole) noexcept {
        const u32 i = static_cast<u32>(field);
        pending.bytes[i] = static_cast<u16>(decode(v, pending.text[i], kUserActivityFieldBudget[i], whole));
    };
    take(F::name, f.name, false);
    if (!pending.bytes[0])
        return true;
    take(F::details, f.details, false);
    take(F::state, f.state, false);
    if (type == 1)
        take(F::url, f.url, true);
    Members assets{{}, {}, {}, {}, utf8_literal("large_image"), utf8_literal("large_text"),
                   utf8_literal("small_image"), utf8_literal("small_text")};
    if (!object(*c.doc, f.assets, &assets))
        return false;
    take(F::large_image, assets.a, true);
    take(F::large_text, assets.b, false);
    take(F::small_image, assets.c, true);
    take(F::small_text, assets.d, false);
    u8 buttons = 0;
    if (present(f.buttons) && f.buttons.kind == JsonValueKind::array) {
        Array list{};
        if (!json_visit_indexed_array(*c.doc, f.buttons, 64, array_item, &list).ok())
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
    a = {};
    a.type = static_cast<UserActivityType>(type);
    a.application_id = number(f.application_id);
    a.created_ms = number(f.created_at);
    a.flags = static_cast<u32>(number(f.flags));
    Members times{{}, {}, {}, {}, utf8_literal("start"), utf8_literal("end")};
    if (!object(*c.doc, f.timestamps, &times))
        return false;
    a.start_ms = number(times.a);
    a.end_ms = number(times.b);
    // Some integrations send seconds instead of milliseconds.
    if (a.start_ms && a.start_ms < 100'000'000'000ull)
        a.start_ms *= 1000;
    if (a.end_ms && a.end_ms < 100'000'000'000ull)
        a.end_ms *= 1000;
    Members party{{}, {}, {}, {}, utf8_literal("size")};
    if (!object(*c.doc, f.party, &party))
        return false;
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
    const F drop_order[] = {F::small_image, F::small_text, F::url, F::large_text, F::button_1, F::button_0,
                            F::large_image, F::state, F::details};
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
struct Root {
    JsonValue activities;
};
bool root_member(void* opaque, JsonString key, JsonValue value) noexcept {
    auto& r = *static_cast<Root*>(opaque);
    if (json_string_equals_ascii(key, utf8_literal("activities")) && !r.activities.source.data)
        r.activities = value;
    return true;
}
} // namespace

bool user_activities_decode(const JsonIndexedDocument& doc, JsonValue presence, u64 user_id,
                            UserActivitySet* output, bool* has) noexcept {
    if (!output || !has)
        return false;
    *output = {};
    output->user_id = user_id;
    *has = false;
    if (!presence.source.data || presence.kind != JsonValueKind::object)
        return presence.kind == JsonValueKind::null_value || !presence.source.data;
    Root root{};
    if (!json_visit_indexed_object(doc, presence, 64, root_member, &root).ok())
        return false;
    if (!root.activities.source.data)
        return true;
    *has = true;
    if (root.activities.kind == JsonValueKind::null_value)
        return true;
    if (root.activities.kind != JsonValueKind::array)
        return false;
    Decode context{&doc, output};
    return json_visit_indexed_array(doc, root.activities, 64, activity_item, &context).ok();
}
} // namespace maicord
