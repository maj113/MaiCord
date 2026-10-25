#include "maicord/protocol/hls_playlist.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
using E = HlsPlaylistError;
bool equal(Utf8View a, Utf8View b) noexcept { return utf8_equal(a, b); }
bool prefix(Utf8View a, Utf8View b) noexcept { return utf8_starts_with(a, b); }
bool number(Utf8View s, u64* output) noexcept {
    if (!s.bytes) return false;
    u64 value = 0;
    for (u32 i = 0; i < s.bytes; ++i) {
        const u32 digit = static_cast<u8>(s.data[i]) - '0';
        if (digit > 9 || value > (~u64{0} - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *output = value; return true;
}
bool duration(Utf8View s, u32* output) noexcept {
    u32 at = 0; while (at < s.bytes && ascii_is_digit(s.data[at])) ++at;
    u64 seconds{};
    if (!number({s.data, at}, &seconds) || seconds > 600) return false;
    u32 fraction = 0, digits = 0;
    if (at < s.bytes && s.data[at] == '.') {
        ++at;
        while (at < s.bytes && ascii_is_digit(s.data[at])) {
            if (++digits > 6) return false;
            fraction = fraction * 10 + s.data[at++] - '0';
        }
        if (!digits) return false;
    }
    if (at == s.bytes || s.data[at] != ',') return false;
    for (; digits < 6; ++digits) fraction *= 10;
    const u64 value = seconds * 1000000 + fraction;
    if (!value || value > 600000000) return false;
    *output = static_cast<u32>(value); return true;
}
bool local_path(Utf8View s) noexcept {
    if (!s.bytes || s.bytes > 2048 || s.data[0] == '/') return false;
    u32 start = 0;
    for (u32 i = 0; i <= s.bytes; ++i) {
        if (i == s.bytes || s.data[i] == '/') {
            const Utf8View part{s.data + start, i - start};
            if (!part.bytes || equal(part, utf8_literal(".")) || equal(part, utf8_literal(".."))) return false;
            start = i + 1;
        } else if (!ascii_is_alnum(s.data[i]) && s.data[i] != '-' && s.data[i] != '_' && s.data[i] != '.') return false;
    }
    return true;
}
struct Reader {
    Utf8View input; u32 at{};
    bool line(Utf8View* output) noexcept {
        if (at >= input.bytes) return false;
        const u32 start = at;
        while (at < input.bytes && input.data[at] != '\n') ++at;
        u32 end = at;
        if (at < input.bytes) ++at;
        if (end > start && input.data[end - 1] == '\r') --end;
        *output = {input.data + start, end - start}; return true;
    }
};
struct Attribute { Utf8View name{}, value{}; bool quoted{}; };
bool attributes(Utf8View text, Attribute* output, u32* count) noexcept {
    u32 at = 0, used = 0;
    while (at < text.bytes) {
        if (used == 16) return false;
        const u32 start = at;
        while (at < text.bytes && ((text.data[at] >= 'A' && text.data[at] <= 'Z') || ascii_is_digit(text.data[at]) || text.data[at] == '-')) ++at;
        if (at == start || at == text.bytes || text.data[at] != '=') return false;
        Attribute value{{text.data + start, at - start}}; ++at;
        for (u32 i = 0; i < used; ++i) if (equal(output[i].name, value.name)) return false;
        value.quoted = at < text.bytes && text.data[at] == '"';
        if (value.quoted) ++at;
        const u32 begin = at;
        while (at < text.bytes && text.data[at] != (value.quoted ? '"' : ',')) ++at;
        value.value = {text.data + begin, at - begin};
        if (!value.value.bytes || (value.quoted && at == text.bytes)) return false;
        if (value.quoted) ++at;
        if (at < text.bytes && (text.data[at++] != ',' || at == text.bytes)) return false;
        output[used++] = value;
    }
    *count = used; return used != 0;
}
u32 codecs(Utf8View s) noexcept {
    u32 flags = 0, at = 0;
    while (at < s.bytes) {
        const u32 start = at; while (at < s.bytes && s.data[at] != ',') ++at;
        const Utf8View part{s.data + start, at - start};
        u32 bit = 0;
        if (part.bytes == 11 && prefix(part, utf8_literal("avc1."))) {
            bit = 1;
            for (u32 i = 5; i < part.bytes; ++i) if (!ascii_is_hex_digit(part.data[i])) return 0;
        } else if (equal(part, utf8_literal("mp4a.40.2"))) bit = 2;
        if (!bit || (flags & bit)) return 0;
        flags |= bit;
        if (at < s.bytes && ++at == s.bytes) return 0;
    }
    return flags;
}
bool reference_uri(Utf8View s, u32* origin = nullptr, u32* path_end = nullptr) noexcept {
    if (!s.data || !s.bytes || s.bytes > 2048 || !byte_range_valid(s.data, s.bytes)) return false;
    u32 start = 0;
    const bool absolute = prefix(s, utf8_literal("https://"));
    if (absolute) {
        start = 8;
        while (start < s.bytes && s.data[start] != '/' && s.data[start] != '?') {
            const char c = s.data[start++];
            if (!ascii_is_alnum(c) && c != '-' && c != '.') return false;
        }
        if (start == 8 || start - 8 > 253 || start == s.bytes || s.data[start] != '/') return false;
    } else if (s.bytes > 1 && s.data[0] == '/' && s.data[1] == '/') return false;
    u32 end = s.bytes;
    bool query = false;
    for (u32 i = start; i < s.bytes; ++i) {
        const char c = s.data[i];
        if (c == '?' && !query) { end = i; query = true; }
        if (static_cast<u8>(c) <= 32 || static_cast<u8>(c) >= 127 || c == '#' || c == '\\' ||
            c == '"' || c == '<' || c == '>' || (!query && c == ':')) return false;
        if (!query && c == '/' && i > start && s.data[i - 1] == '/') return false;
        if (c == '%') {
            if (i + 2 >= s.bytes || !ascii_is_hex_digit(s.data[i + 1]) || !ascii_is_hex_digit(s.data[i + 2])) return false;
            const auto hex = [](char digit) noexcept -> u32 {
                return ascii_is_digit(digit) ? digit - '0' : ascii_lower(digit) - 'a' + 10;
            };
            const u32 decoded = hex(s.data[i + 1]) * 16 + hex(s.data[i + 2]);
            if (decoded <= 32 || decoded >= 127 || decoded == '\\' ||
                (!absolute && !query && (decoded == '.' || decoded == '/' || decoded == ':'))) return false;
            i += 2;
        }
    }
    if (origin) *origin = start;
    if (path_end) *path_end = end;
    return true;
}
bool master_uri(Utf8View s, bool relative = false) noexcept {
    if (relative) return reference_uri(s);
    if (!s.bytes || s.bytes > 2048) return false;
    if (!prefix(s, utf8_literal("https://"))) return local_path(s);
    if (s.bytes <= 8) return false;
    for (u32 i = 8; i < s.bytes; ++i)
        if (static_cast<u8>(s.data[i]) <= 32 || static_cast<u8>(s.data[i]) >= 127 || s.data[i] == '#' ||
            s.data[i] == '\\' || s.data[i] == '"' || s.data[i] == '<' || s.data[i] == '>') return false;
    return true;
}
bool valid_text(Utf8View input) noexcept {
    for (u32 at = 0; at < input.bytes;) {
        const auto c = utf8_decode_one(input.data + at, input.bytes - at);
        if (!c.valid || (c.codepoint < 32 && c.codepoint != '\n' && c.codepoint != '\r') ||
            (c.codepoint >= 127 && c.codepoint <= 159) || c.codepoint == 0xfeff) return false;
        if (c.codepoint == '\r' && (at + 1 == input.bytes || input.data[at + 1] != '\n')) return false;
        at += c.width;
    }
    return true;
}
bool output_valid(const HlsMediaPlaylist& p, const void* output, u32 bytes) noexcept {
    return output && byte_range_valid(output, bytes) && p.input.data && p.input.bytes &&
        (p.mode == HlsPlaylistMode::complete || p.mode == HlsPlaylistMode::sliding || p.mode == HlsPlaylistMode::complete_uri) &&
        p.input.bytes <= (p.mode == HlsPlaylistMode::sliding ? 64u : 256u) * 1024 && byte_range_valid(p.input.data, p.input.bytes) &&
        p.segments && p.segments <= (p.mode == HlsPlaylistMode::sliding ? 128u : 8192u) && p.duration_us && p.duration_us <= 86400000000ull &&
        !byte_ranges_overlap(p.input.data, p.input.bytes, output, bytes) &&
        !byte_ranges_overlap(&p, sizeof(p), output, bytes);
}
bool segment(const HlsMediaPlaylist& p, u32 offset, u32 index, u64 time, HlsMediaSegment* output) noexcept {
    Reader r{p.input, offset}; Utf8View line{}; u32 duration_us = 0;
    while (r.line(&line)) {
        if (prefix(line, utf8_literal("#EXTINF:"))) {
            if (duration_us || !duration({line.data + 8, line.bytes - 8}, &duration_us)) return false;
        } else if (line.bytes && line.data[0] != '#') {
            if (!duration_us || !(p.mode == HlsPlaylistMode::complete_uri ? reference_uri(line) : p.mode == HlsPlaylistMode::sliding ? master_uri(line) : local_path(line)) ||
                time > p.duration_us || duration_us > p.duration_us - time) return false;
            *output = {line, time, duration_us, index, r.at}; return true;
        }
    }
    return false;
}
}
u32 hls_resolve_uri(Utf8View base, Utf8View reference, Slice<char> output) noexcept {
    u32 base_origin{}, base_end{}, origin{}, end{};
    if (!output.data || !byte_range_valid(output.data, output.count) ||
        !reference_uri(base, &base_origin, &base_end) || !base_origin ||
        !reference_uri(reference, &origin, &end)) return 0;
    char staged[2048];
    u32 used{};
    const auto append = [&](Utf8View part) noexcept {
        if (part.bytes > sizeof(staged) - used) return false;
        copy_bytes(staged + used, part.data, part.bytes); used += part.bytes; return true;
    };
    const auto input = origin ? reference : base;
    const u32 root = origin ? origin : base_origin;
    if (!append({input.data, root}) || !append(utf8_literal("/"))) return 0;
    const auto path = [&](Utf8View value) noexcept {
        for (u32 at = 0; at < value.bytes;) {
            if (value.data[at] == '/') { ++at; continue; }
            const u32 begin = at;
            while (at < value.bytes && value.data[at] != '/') ++at;
            const Utf8View part{value.data + begin, at - begin};
            if (equal(part, utf8_literal(".."))) {
                if (used <= root + 1) return false;
                if (staged[used - 1] == '/') --used;
                while (used > root + 1 && staged[used - 1] != '/') --used;
            } else if (!equal(part, utf8_literal("."))) {
                if (staged[used - 1] != '/' && !append(utf8_literal("/"))) return false;
                if (!append(part)) return false;
                if (at < value.bytes && !append(utf8_literal("/"))) return false;
            }
        }
        return true;
    };
    if (!origin && reference.data[0] != '/') {
        u32 leaf = base_end;
        if (end) while (leaf > base_origin && base.data[leaf - 1] != '/') --leaf;
        if (!path({base.data + base_origin, leaf - base_origin})) return 0;
    }
    if (!path({reference.data + origin, end - origin}) ||
        !append({reference.data + end, reference.bytes - end}) || used > output.count) return 0;
    copy_bytes(output.data, staged, used);
    return used;
}
HlsPlaylistError hls_master_playlist(Utf8View input, u16 maximum_height, HlsMasterPlaylist* output, u32 maximum_bandwidth, bool relative_uris) noexcept {
    if (!input.data || !input.bytes || !output || !maximum_height || maximum_height > 2048 || !maximum_bandwidth ||
        !byte_range_valid(input.data, input.bytes) || !byte_range_valid(output, sizeof(*output)) ||
        byte_ranges_overlap(input.data, input.bytes, output, sizeof(*output))) return E::invalid_input;
    if (input.bytes > 64 * 1024) return E::capacity;
    if (!valid_text(input)) return E::malformed;
    Reader reader{input}; Utf8View line{};
    if (!reader.line(&line) || !equal(line, utf8_literal("#EXTM3U"))) return E::malformed;
    HlsMasterPlaylist value{}; HlsVariant candidate{};
    Utf8View audio_group{}, candidate_group{};
    u32 count = 0, codec = 0; bool pending = false, version = false;
    while (reader.line(&line)) {
        if (!line.bytes) continue;
        if (line.bytes > 4096) return E::capacity;
        if (prefix(line, utf8_literal("#EXT-X-STREAM-INF:"))) {
            if (pending) return E::malformed;
            if (++count > 64) return E::capacity;
            Attribute fields[16]{}; u32 fields_count{}; reset_value(&candidate); reset_value(&candidate_group); codec = 0;
            if (!attributes({line.data + 18, line.bytes - 18}, fields, &fields_count)) return E::malformed;
            bool known_codec = false;
            for (u32 i = 0; i < fields_count; ++i) {
                const auto& f = fields[i]; u64 n{};
                if (equal(f.name, utf8_literal("BANDWIDTH"))) {
                    if (f.quoted || !number(f.value, &n) || !n || n > ~u32{0}) return E::malformed;
                    candidate.bandwidth = static_cast<u32>(n);
                } else if (equal(f.name, utf8_literal("CODECS"))) {
                    if (!f.quoted) return E::malformed;
                    known_codec = true; codec = codecs(f.value);
                } else if (equal(f.name, utf8_literal("RESOLUTION"))) {
                    u32 at = 0; while (at < f.value.bytes && f.value.data[at] != 'x') ++at;
                    u64 width{}, height{};
                    if (f.quoted || at == f.value.bytes || !number({f.value.data, at}, &width) ||
                        !number({f.value.data + at + 1, f.value.bytes - at - 1}, &height) || !width || !height ||
                        width > 65535 || height > 65535) return E::malformed;
                    candidate.width = static_cast<u16>(width); candidate.height = static_cast<u16>(height);
                } else if (equal(f.name, utf8_literal("AUDIO"))) {
                    if (!f.quoted) return E::malformed;
                    candidate_group = f.value;
                }
            }
            if (!candidate.bandwidth || !known_codec) return E::malformed;
            pending = true;
        } else if (line.data[0] != '#') {
            if (!pending || !master_uri(line, relative_uris)) return E::malformed;
            pending = false; candidate.uri = line;
            if ((codec & 1) && candidate.width && candidate.width <= 2048 && candidate.height && candidate.height <= 2048 &&
                candidate.bandwidth <= maximum_bandwidth) {
                const auto& old = value.video;
                const bool within = candidate.height <= maximum_height, old_within = old.height <= maximum_height;
                if (!old.uri.bytes || (within != old_within ? within : candidate.height != old.height
                    ? (within ? candidate.height > old.height : candidate.height < old.height)
                    : candidate.bandwidth < old.bandwidth)) {
                    value.video = candidate; audio_group = candidate_group;
                }
            } else if (codec == 2 && !candidate.width && !candidate.height && candidate.bandwidth <= 512000 &&
                       (!value.audio.uri.bytes || candidate.bandwidth > value.audio.bandwidth)) value.audio = candidate;
        } else if (prefix(line, utf8_literal("#EXT-X-VERSION:"))) {
            u64 n{};
            if (version || !number({line.data + 15, line.bytes - 15}, &n) || !n || n > 10) return E::malformed;
            version = true;
        } else if (equal(line, utf8_literal("#EXTM3U")) || prefix(line, utf8_literal("#EXTINF:")) ||
            prefix(line, utf8_literal("#EXT-X-MAP:")) || equal(line, utf8_literal("#EXT-X-ENDLIST"))) return E::malformed;
        else if (prefix(line, utf8_literal("#EXT-X-KEY")) || prefix(line, utf8_literal("#EXT-X-SESSION-KEY:")) ||
            prefix(line, utf8_literal("#EXT-X-DEFINE:"))) return E::unsupported;
    }
    if (pending) return E::malformed;
    if (audio_group.bytes) {
        // Only the selected variant's group can supply its audio. A second
        // bounded pass avoids retaining a table of rendition strings.
        reset_value(&value.audio); reader.at = 0; u32 renditions = 0, priority = 0;
        while (reader.line(&line)) {
            if (!prefix(line, utf8_literal("#EXT-X-MEDIA:"))) continue;
            if (++renditions > 64) return E::capacity;
            Attribute fields[16]{}; u32 fields_count{};
            if (!attributes({line.data + 13, line.bytes - 13}, fields, &fields_count)) return E::malformed;
            Utf8View type{}, group{}, name{}, uri{}; bool preferred = false, automatic = false;
            for (u32 i = 0; i < fields_count; ++i) {
                const auto& f = fields[i];
                if (equal(f.name, utf8_literal("TYPE"))) { if (f.quoted) return E::malformed; type = f.value; }
                else if (equal(f.name, utf8_literal("GROUP-ID"))) { if (!f.quoted) return E::malformed; group = f.value; }
                else if (equal(f.name, utf8_literal("NAME"))) { if (!f.quoted) return E::malformed; name = f.value; }
                else if (equal(f.name, utf8_literal("URI"))) { if (!f.quoted || !master_uri(f.value, relative_uris)) return E::malformed; uri = f.value; }
                else if (equal(f.name, utf8_literal("DEFAULT")) || equal(f.name, utf8_literal("AUTOSELECT"))) {
                    if (f.quoted || (!equal(f.value, utf8_literal("YES")) && !equal(f.value, utf8_literal("NO")))) return E::malformed;
                    const bool yes = equal(f.value, utf8_literal("YES"));
                    if (equal(f.name, utf8_literal("DEFAULT"))) preferred = yes; else automatic = yes;
                }
            }
            if (!type.bytes || !group.bytes || !name.bytes) return E::malformed;
            if (!equal(type, utf8_literal("AUDIO")) || !equal(group, audio_group)) continue;
            const u32 rank = preferred ? 3 : automatic ? 2 : 1;
            if (preferred && priority == 3) return E::malformed;
            if (rank > priority) { value.audio.uri = uri; priority = rank; }
        }
    }
    if (!value.video.uri.bytes || !value.audio.uri.bytes) return E::unsupported;
    *output = value; return E::none;
}
HlsPlaylistError hls_media_playlist(Utf8View input, HlsMediaPlaylist* output, HlsPlaylistMode mode) noexcept {
    if (!input.data || !input.bytes || !output || (mode != HlsPlaylistMode::complete && mode != HlsPlaylistMode::sliding && mode != HlsPlaylistMode::complete_uri) ||
        !byte_range_valid(input.data, input.bytes) ||
        !byte_range_valid(output, sizeof(*output)) || byte_ranges_overlap(input.data, input.bytes, output, sizeof(*output))) return E::invalid_input;
    const bool sliding = mode == HlsPlaylistMode::sliding;
    if (input.bytes > (sliding ? 64u : 256u) * 1024) return E::capacity;
    if (!valid_text(input)) return E::malformed;
    Reader reader{input}; Utf8View line{};
    if (!reader.line(&line) || !equal(line, utf8_literal("#EXTM3U"))) return E::malformed;
    HlsMediaPlaylist value{}; value.input = input; value.mode = mode;
    u32 pending = 0, largest = 0, seen = 0; u64 version = 1; bool ended = false, vod = false;
    while (reader.at < input.bytes) {
        const u32 offset = reader.at; reader.line(&line);
        if (line.bytes > 4096) return E::capacity;
        if (!line.bytes) continue;
        if (prefix(line, utf8_literal("#EXTINF:"))) {
            if (ended || pending || !duration({line.data + 8, line.bytes - 8}, &pending)) return E::malformed;
            if (!value.segments) value.first_offset = offset;
            if (pending > largest) largest = pending;
        } else if (line.data[0] != '#') {
            if (ended || !pending) return E::malformed;
            if (!(mode == HlsPlaylistMode::complete_uri ? reference_uri(line) : sliding ? master_uri(line) : local_path(line))) return E::unsupported;
            if (++value.segments > (sliding ? 128u : 8192u) || pending > 86400000000ull - value.duration_us) return E::capacity;
            value.duration_us += pending; pending = 0;
        } else if (prefix(line, utf8_literal("#EXT-X-MAP:"))) {
            constexpr auto start = utf8_literal("#EXT-X-MAP:URI=\"");
            if (value.initialization.bytes) return E::unsupported;
            if (!prefix(line, start) || line.bytes <= start.bytes || line.data[line.bytes - 1] != '"') return E::unsupported;
            value.initialization = {line.data + start.bytes, line.bytes - start.bytes - 1};
            if (!(mode == HlsPlaylistMode::complete_uri ? reference_uri(value.initialization) : sliding ? master_uri(value.initialization) : local_path(value.initialization)) || value.segments || pending) return E::unsupported;
        } else if (equal(line, utf8_literal("#EXT-X-ENDLIST"))) {
            if (ended || pending || !value.segments) return E::malformed;
            ended = true;
        } else if (equal(line, utf8_literal("#EXTM3U"))) return E::malformed;
        else if (prefix(line, utf8_literal("#EXT-X-KEY"))) {
            if (!equal(line, utf8_literal("#EXT-X-KEY:METHOD=NONE"))) return E::unsupported;
        } else {
            static constexpr Utf8View numeric[]{utf8_literal("#EXT-X-VERSION:"), utf8_literal("#EXT-X-TARGETDURATION:"),
                utf8_literal("#EXT-X-MEDIA-SEQUENCE:"), utf8_literal("#EXT-X-DISCONTINUITY-SEQUENCE:")};
            bool parsed = false;
            for (u32 i = 0; i < 4; ++i) if (prefix(line, numeric[i])) {
                u64 n{};
                if ((seen & (1u << i)) || !number({line.data + numeric[i].bytes, line.bytes - numeric[i].bytes}, &n)) return E::malformed;
                seen |= 1u << i; parsed = true;
                if (!i) { if (!n || n > 10) return E::unsupported; version = n; }
                else if (i == 1) { if (!n || n > 600) return E::unsupported; value.target_seconds = static_cast<u32>(n); }
                else {
                    if (value.segments || pending) return E::malformed;
                    if (i == 2) value.sequence = n;
                    else { if (!sliding) return E::unsupported; value.discontinuity_sequence = n; }
                }
                break;
            }
            if (parsed) continue;
            if (prefix(line, utf8_literal("#EXT-X-PLAYLIST-TYPE:"))) {
                if (seen & 16) return E::malformed;
                seen |= 16;
                if (!equal(line, utf8_literal("#EXT-X-PLAYLIST-TYPE:VOD")) &&
                    !equal(line, utf8_literal("#EXT-X-PLAYLIST-TYPE:EVENT"))) return E::unsupported;
                vod = equal(line, utf8_literal("#EXT-X-PLAYLIST-TYPE:VOD"));
            } else if (prefix(line, utf8_literal("#EXT-X-STREAM-INF:")) || prefix(line, utf8_literal("#EXT-X-MEDIA:"))) return E::malformed;
            else if (prefix(line, utf8_literal("#EXT-X-DISCONTINUITY")) || prefix(line, utf8_literal("#EXT-X-BYTERANGE:")) ||
                prefix(line, utf8_literal("#EXT-X-GAP")) || prefix(line, utf8_literal("#EXT-X-I-FRAMES-ONLY")) ||
                prefix(line, utf8_literal("#EXT-X-SESSION-KEY:")) || prefix(line, utf8_literal("#EXT-X-DEFINE:")) ||
                prefix(line, utf8_literal("#EXT-X-PART")) || prefix(line, utf8_literal("#EXT-X-SKIP:"))) return E::unsupported;
        }
    }
    if (pending || !value.segments || !value.target_seconds || (u64{largest} + 500000) / 1000000 > value.target_seconds ||
        value.sequence > ~u64{0} - (value.segments - 1)) return E::malformed;
    if ((!ended && (!sliding || vod)) || (!value.initialization.bytes && !sliding) ||
        (value.initialization.bytes && version < 6)) return E::unsupported;
    value.transport = !value.initialization.bytes;
    value.maximum_duration_us = largest; value.ended = ended;
    *output = value; return E::none;
}
bool hls_media_segment(const HlsMediaPlaylist& p, u32 index, HlsMediaSegment* output) noexcept {
    if (!output_valid(p, output, sizeof(*output)) || index >= p.segments) return false;
    HlsMediaSegment value{}; u32 offset = p.first_offset; u64 time = 0;
    for (u32 i = 0; i <= index; ++i) {
        if (!segment(p, offset, i, time, &value)) return false;
        offset = value.next_offset; time += value.duration_us;
    }
    *output = value; return true;
}
bool hls_media_follow(const HlsMediaPlaylist& p, const HlsMediaSegment& previous, HlsMediaSegment* output) noexcept {
    if (!output_valid(p, output, sizeof(*output)) || previous.index >= p.segments - 1 || previous.next_offset > p.input.bytes ||
        previous.time_us > p.duration_us || previous.duration_us > p.duration_us - previous.time_us) return false;
    return segment(p, previous.next_offset, previous.index + 1, previous.time_us + previous.duration_us, output);
}
bool hls_media_find(const HlsMediaPlaylist& p, u64 time, HlsMediaSegment* output) noexcept {
    if (!output_valid(p, output, sizeof(*output)) || time >= p.duration_us) return false;
    HlsMediaSegment value{}; u32 offset = p.first_offset; u64 at = 0;
    for (u32 i = 0; i < p.segments; ++i) {
        if (!segment(p, offset, i, at, &value)) return false;
        if (time - at < value.duration_us) { *output = value; return true; }
        at += value.duration_us; offset = value.next_offset;
    }
    return false;
}
HlsPlaylistError hls_media_update(const HlsMediaPlaylist& previous, const HlsMediaPlaylist& next, HlsPlaylistAdvance* output) noexcept {
    if (!output_valid(previous, output, sizeof(*output)) || !output_valid(next, output, sizeof(*output)) ||
        previous.mode != HlsPlaylistMode::sliding || next.mode != HlsPlaylistMode::sliding ||
        previous.sequence > ~u64{0} - (previous.segments - 1) || next.sequence > ~u64{0} - (next.segments - 1)) return E::invalid_input;
    if (next.sequence < previous.sequence || next.sequence + (next.segments - 1) < previous.sequence + (previous.segments - 1) ||
        (previous.ended && (!next.ended || next.sequence != previous.sequence || next.segments != previous.segments))) return E::stale;
    if (next.transport != previous.transport || next.target_seconds != previous.target_seconds || next.discontinuity_sequence != previous.discontinuity_sequence ||
        !equal(next.initialization, previous.initialization)) return E::unsupported;
    const u64 dropped = next.sequence - previous.sequence;
    if (dropped > previous.segments) return E::gap;
    HlsPlaylistAdvance value{}; HlsMediaSegment old{}, current{};
    if (!hls_media_segment(previous, 0, &old) || !hls_media_segment(next, 0, &current)) return E::malformed;
    const u32 common = previous.segments - static_cast<u32>(dropped);
    for (u32 i = 0; i < previous.segments; ++i) {
        if (i < dropped) value.discarded_us += old.duration_us;
        else {
            if (old.duration_us != current.duration_us || !equal(old.uri, current.uri)) return E::malformed;
            if (i + 1 < previous.segments && !hls_media_follow(next, current, &current)) return E::malformed;
        }
        if (i + 1 < previous.segments && !hls_media_follow(previous, old, &old)) return E::malformed;
    }
    value.new_segments = next.segments - common;
    value.changed = dropped || value.new_segments || previous.ended != next.ended;
    *output = value; return E::none;
}
} // namespace maicord
