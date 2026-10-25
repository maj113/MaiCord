#include "maicord/ui/profile_activity.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using F = UserActivityField;
Utf8View text(const UserActivity& a, F field) noexcept {
    return user_activity_text(a, field);
}
u32 rank(UserActivityType type) noexcept {
    return type == UserActivityType::competing ? 3 : type == UserActivityType::streaming ? 2
           : type == UserActivityType::playing ? 1 : 0;
}
// Rich presence: anything beyond a name (secrets are not kept).
bool rich(const UserActivity& a) noexcept {
    return text(a, F::details).bytes || text(a, F::large_image).bytes || text(a, F::small_text).bytes ||
           (a.detail & user_activity_has_party) || text(a, F::state).bytes;
}
bool before(const UserActivity& a, const UserActivity& b) noexcept {
    if (rank(a.type) != rank(b.type))
        return rank(a.type) > rank(b.type);
    if (rich(a) != rich(b))
        return rich(a);
    return a.created_ms > b.created_ms;
}
bool same_card(const UserActivity& a, const UserActivity& b) noexcept {
    const auto name = text(a, F::name), other = text(b, F::name);
    return (a.application_id && a.application_id == b.application_id) ||
           (name.bytes && other.bytes && utf8_equal(name, other));
}
template <u32 N> void set(ProfileString<N>* out, Utf8View value) noexcept {
    out->bytes = static_cast<u16>(utf8_truncated_length(value, N));
    copy_bytes(out->data, value.data, out->bytes);
}
template <u32 N> void put(ProfileString<N>* out, Utf8View value) noexcept {
    const u32 bytes = utf8_truncated_length(value, N - out->bytes);
    copy_bytes(out->data + out->bytes, value.data, bytes);
    out->bytes = static_cast<u16>(out->bytes + bytes);
}
struct Writer {
    Slice<char> output;
    u32 used = 0;
    bool ok = true;
    void text(Utf8View value) noexcept {
        if (!ok || value.bytes > output.count - used) {
            ok = false;
            return;
        }
        copy_bytes(output.data + used, value.data, value.bytes);
        used += value.bytes;
    }
    void number(u64 value, u32 width = 0) noexcept {
        char digits[20];
        const u32 bytes = decimal_write(digits, value);
        for (u32 i = bytes; i < width; ++i)
            text(utf8_literal("0"));
        text({digits, bytes});
    }
    u32 finish() const noexcept {
        return ok ? used : 0;
    }
};
bool word(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
// Matches \w+ (\d+) at s[at..]: returns the digits' end, or 0.
u32 word_number(Utf8View s, u32 at, u32* digits_begin) noexcept {
    u32 i = at;
    while (i < s.bytes && word(s.data[i]))
        ++i;
    if (i == at || i + 1 >= s.bytes || s.data[i] != ' ')
        return 0;
    *digits_begin = ++i;
    while (i < s.bytes && digit(s.data[i]))
        ++i;
    return i > *digits_begin ? i : 0;
}
} // namespace

u32 profile_activity_order(const UserActivitySet& set, Slice<u8> order) noexcept {
    if (!order.data)
        return 0;
    u8 sorted[kUserActivityMaximum];
    u32 count = 0;
    for (u32 i = 0; i < set.count && i < kUserActivityMaximum; ++i) {
        // Stable insertion keeps the presence order between equal activities.
        u32 at = count++;
        while (at && before(set.items[i], set.items[sorted[at - 1]])) {
            sorted[at] = sorted[at - 1];
            --at;
        }
        sorted[at] = static_cast<u8>(i);
    }
    u32 used = 0;
    bool playing = false;
    for (u32 i = 0; i < count && used < order.count; ++i) {
        const auto& a = set.items[sorted[i]];
        if (a.type == UserActivityType::playing) {
            if (playing)
                continue;
            playing = true;
        }
        bool duplicate = false;
        for (u32 k = 0; k < used && !duplicate; ++k)
            duplicate = same_card(a, set.items[order[k]]);
        if (!duplicate)
            order[used++] = sorted[i];
    }
    return used;
}
u32 profile_activity_clock_text(u64 seconds, Slice<char> output) noexcept {
    Writer w{output};
    const u64 hours = seconds / 3600, minutes = seconds / 60 % 60;
    if (hours) {
        w.number(hours);
        w.text(utf8_literal(":"));
        w.number(minutes, 2);
    } else
        w.number(minutes);
    w.text(utf8_literal(":"));
    w.number(seconds % 60, 2);
    return w.finish();
}
u32 profile_activity_track_text(u64 seconds, Slice<char> output) noexcept {
    Writer w{output};
    if (seconds >= 3600) {
        w.number(seconds / 3600, 2);
        w.text(utf8_literal(":"));
    }
    w.number(seconds / 60 % 60, 2);
    w.text(utf8_literal(":"));
    w.number(seconds % 60, 2);
    return w.finish();
}
ProfileActivityGlyph profile_activity_placeholder(UserActivityType type) noexcept {
    switch (type) {
    case UserActivityType::listening:
        return ProfileActivityGlyph::music;
    case UserActivityType::watching:
        return ProfileActivityGlyph::screen;
    case UserActivityType::streaming:
        return ProfileActivityGlyph::stream;
    case UserActivityType::competing:
        return ProfileActivityGlyph::competition;
    default:
        return ProfileActivityGlyph::game;
    }
}
ProfileActivityClock profile_activity_clock(const UserActivity& a, u64 now) noexcept {
    // The chip ticks on whole seconds.
    now -= now % 1000;
    ProfileActivityProgress progress{};
    const u64 start = a.start_ms ? a.start_ms : a.created_ms;
    if (!start || profile_activity_progress(a, now, &progress))
        return {};
    const bool countdown = a.end_ms && a.created_ms && a.end_ms > a.created_ms &&
                           a.type != UserActivityType::listening;
    if (countdown && a.end_ms > now)
        return {ProfileActivityTimer::countdown, (a.end_ms - now) / 1000};
    const u64 until = a.end_ms && !countdown ? minimum(a.end_ms, now) : now;
    return {ProfileActivityTimer::elapsed, until > start ? (until - start) / 1000 : 0};
}
bool profile_activity_progress(const UserActivity& a, u64 now, ProfileActivityProgress* output) noexcept {
    if ((a.type != UserActivityType::listening && a.type != UserActivityType::watching) || !a.start_ms ||
        !a.end_ms)
        return false;
    if (!output)
        return true;
    const u64 duration = a.end_ms > a.start_ms ? a.end_ms - a.start_ms : 0;
    const u64 elapsed = now > a.start_ms ? minimum(now - a.start_ms, duration) : 0;
    output->duration = duration / 1000;
    output->elapsed = elapsed / 1000;
    output->fraction = duration ? static_cast<float>(static_cast<double>(elapsed) / static_cast<double>(duration)) : 0.f;
    return true;
}
u32 profile_activity_party_text(Utf8View state, u32 size, u32 max, Slice<char> output) noexcept {
    char count[48];
    Writer n{{count, sizeof(count)}};
    if (size && max) {
        n.number(size);
        n.text(utf8_literal(" of "));
        n.number(max);
    } else if (size)
        n.number(size);
    Writer w{output};
    if (state.bytes) {
        w.text(state);
        if (n.used) {
            w.text(utf8_literal(" ("));
            w.text({count, n.used});
            w.text(utf8_literal(")"));
        }
    } else
        w.text({count, n.used});
    return w.finish();
}
u32 profile_activity_episode_text(Utf8View s, Slice<char> output) noexcept {
    // The first match of /\w+ (\d+), \w+ (\d+)/.
    for (u32 at = 0; at < s.bytes; ++at) {
        if (!word(s.data[at]) || (at && word(s.data[at - 1])))
            continue;
        u32 season = 0;
        const u32 season_end = word_number(s, at, &season);
        if (!season_end || season_end + 2 > s.bytes || s.data[season_end] != ',' || s.data[season_end + 1] != ' ')
            continue;
        u32 episode = 0;
        const u32 episode_end = word_number(s, season_end + 2, &episode);
        if (!episode_end)
            continue;
        Writer w{output};
        w.text(utf8_literal("S"));
        w.text({s.data + season, season_end - season});
        w.text(utf8_literal("E"));
        w.text({s.data + episode, episode_end - episode});
        return w.finish();
    }
    return 0;
}
bool profile_activity_content(const UserActivity& a, bool bot, bool self, ProfileActivityContent* out) noexcept {
    if (!out || a.type == UserActivityType::custom)
        return false;
    reset_value(out);
    auto& c = *out;
    c.heading.bytes = static_cast<u16>(user_activity_heading(a, {c.heading.data, sizeof(c.heading.data)}));
    c.spotify = user_activity_is_spotify(a);
    c.artwork = !bot;
    c.chips = !bot;
    c.progress = profile_activity_progress(a, 0, nullptr);
    const bool playing = a.type == UserActivityType::playing;
    const bool embedded = (a.flags & user_activity_flag_embedded) != 0;
    const bool party = (a.detail & user_activity_has_party) != 0;
    const auto name = text(a, F::name), details = text(a, F::details), state = text(a, F::state);
    const auto add_line = [&](Utf8View value) noexcept {
        if (value.bytes && c.line_count < 2)
            set(&c.lines[c.line_count++], value);
    };
    if (c.spotify) {
        // Track, then its artists ("A; B" reads "A, B").
        set(&c.title, details);
        auto& artists = c.lines[0];
        for (u32 i = 0; i < state.bytes;) {
            if (i + 1 < state.bytes && state.data[i] == ';' && state.data[i + 1] == ' ') {
                put(&artists, utf8_literal(", "));
                i += 2;
                continue;
            }
            u32 next = i + 1;
            while (next < state.bytes && (static_cast<u8>(state.data[next]) & 0xC0) == 0x80)
                ++next;
            put(&artists, {state.data + i, next - i});
            i = next;
        }
        c.line_count = artists.bytes ? 1 : 0;
    } else if (playing) {
        set(&c.title, name);
        add_line(details);
        if (embedded && (a.party_size || a.party_max)) {
            char line[192];
            Writer w{{line, sizeof(line)}};
            if (state.bytes) {
                w.text(state);
                w.text(utf8_literal(" "));
            }
            w.text(utf8_literal("("));
            w.number(a.party_size);
            if (a.party_max) {
                w.text(utf8_literal(" of "));
                w.number(a.party_max);
            }
            w.text(utf8_literal(")"));
            add_line({line, w.finish()});
        } else if (!party)
            add_line(state);
    } else {
        set(&c.title, details.bytes ? details : name);
        add_line(state);
        if (a.type != UserActivityType::watching)
            add_line(text(a, F::large_text));
    }
    if (c.chips && playing && party && !embedded)
        c.party.bytes = static_cast<u16>(profile_activity_party_text(state, a.party_size, a.party_max,
                                                                     {c.party.data, sizeof(c.party.data)}));
    if (c.chips && a.type == UserActivityType::watching)
        c.episode.bytes = static_cast<u16>(
            profile_activity_episode_text(text(a, F::large_text), {c.episode.data, sizeof(c.episode.data)}));
    c.chips = c.chips && a.type != UserActivityType::streaming;
    c.clock_glyph = a.type == UserActivityType::listening  ? ProfileActivityGlyph::music
                    : a.type == UserActivityType::watching ? ProfileActivityGlyph::screen
                                                           : ProfileActivityGlyph::game;
    if (self || c.spotify)
        return true;
    if (a.button_count)
        c.action = ProfileActivityAction::custom;
    else if (playing && (a.flags & user_activity_flag_join) && !embedded && a.application_id &&
             a.platform == UserActivityPlatform::desktop) {
        c.action = ProfileActivityAction::join;
        put(&c.hint, name);
        put(&c.hint, utf8_literal(" is not detected"));
    } else if (a.type == UserActivityType::streaming) {
        const auto url = text(a, F::url);
        const Utf8View prefixes[] = {utf8_literal("https://twitch.tv/"), utf8_literal("https://www.twitch.tv/"),
                                     utf8_literal("http://twitch.tv/"), utf8_literal("http://www.twitch.tv/"),
                                     utf8_literal("https://youtube.com/watch?v="),
                                     utf8_literal("https://www.youtube.com/watch?v="),
                                     utf8_literal("http://youtube.com/watch?v="),
                                     utf8_literal("http://www.youtube.com/watch?v=")};
        for (const auto& prefix : prefixes)
            if (utf8_starts_with(url, prefix) && (url.bytes > prefix.bytes || prefix.data[prefix.bytes - 1] == '='))
                c.action = ProfileActivityAction::watch;
    }
    return true;
}
} // namespace maicord
