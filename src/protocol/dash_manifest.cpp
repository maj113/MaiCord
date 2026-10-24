#include "maicord/protocol/dash_manifest.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using E = DashManifestError;
constexpr u32 maximum_bytes = 64 * 1024, maximum_segments = 8192;
bool equal(Utf8View a, Utf8View b) noexcept { return utf8_equal(a, b); }
bool space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool name_char(char c) noexcept { return ascii_is_alnum(c) || c == '_' || c == '-' || c == '.' || c == ':'; }
bool name_start(char c) noexcept { return (ascii_is_alnum(c) && !ascii_is_digit(c)) || c == '_'; }
bool number(Utf8View s, u64* out) noexcept {
    if (!s.bytes) return false;
    u64 value = 0;
    for (u32 i = 0; i < s.bytes; ++i) {
        const u32 digit = static_cast<u8>(s.data[i]) - '0';
        if (digit > 9 || value > (~u64{0} - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value; return true;
}
bool duration(Utf8View s, u64* out, bool allow_zero = false) noexcept {
    if (s.bytes < 4 || s.data[0] != 'P' || s.data[1] != 'T') return false;
    u64 total = 0; u32 at = 2, previous = 3601;
    while (at < s.bytes) {
        const u32 start = at;
        while (at < s.bytes && ascii_is_digit(s.data[at])) ++at;
        u64 whole{}, fraction = 0; u32 fraction_digits = 0;
        if (!number({s.data + start, at - start}, &whole) || whole > 86400) return false;
        if (at < s.bytes && s.data[at] == '.') {
            ++at;
            while (at < s.bytes && ascii_is_digit(s.data[at])) {
                if (++fraction_digits > 6) return false;
                fraction = fraction * 10 + s.data[at++] - '0';
            }
            if (!fraction_digits) return false;
        }
        if (at == s.bytes) return false;
        const char unit = s.data[at++];
        const u32 scale = unit == 'H' ? 3600 : unit == 'M' ? 60 : unit == 'S' ? 1 : 0;
        if (!scale || scale >= previous || (fraction_digits && scale != 1)) return false;
        previous = scale;
        for (u32 i = fraction_digits; i < 6; ++i) fraction *= 10;
        total += whole * scale * 1000000 + fraction;
        if (total > 86400000000ull) return false;
    }
    if (!total && !allow_zero) return false;
    *out = total; return true;
}
struct Attribute { Utf8View name{}, value{}; };
struct Element { Utf8View name{}, text{}; Attribute attributes[20]{}; u32 count{}; bool closing{}, empty{}; };
struct Reader { Utf8View input; u32 at{}, tokens{}; };
E next(Reader& r, Element& e) noexcept {
    e = {};
    if (++r.tokens > 4096) return E::capacity;
    const auto s = r.input;
    if (s.data[r.at] != '<') {
        const u32 start = r.at;
        while (r.at < s.bytes && s.data[r.at] != '<') ++r.at;
        e.text = {s.data + start, r.at - start}; return E::none;
    }
    const u32 opening = r.at++;
    if (r.at + 3 <= s.bytes && bytes_equal(s.data + r.at, "!--", 3)) {
        r.at += 3;
        while (r.at + 2 < s.bytes) {
            if (s.data[r.at] == '-' && s.data[r.at + 1] == '-') {
                if (s.data[r.at + 2] != '>') return E::malformed;
                r.at += 3; return E::none;
            }
            ++r.at;
        }
        return E::malformed;
    }
    if (!opening && r.at + 4 <= s.bytes && bytes_equal(s.data + r.at, "?xml", 4)) {
        r.at += 4;
        if (r.at == s.bytes || !space(s.data[r.at])) return E::malformed;
        while (r.at + 1 < s.bytes) {
            if (s.data[r.at] == '?' && s.data[r.at + 1] == '>') { r.at += 2; return E::none; }
            if (s.data[r.at++] == '<') return E::malformed;
        }
        return E::malformed;
    }
    if (r.at == s.bytes || s.data[r.at] == '!' || s.data[r.at] == '?') return E::unsupported;
    e.closing = s.data[r.at] == '/'; if (e.closing) ++r.at;
    const u32 start = r.at;
    while (r.at < s.bytes && name_char(s.data[r.at])) ++r.at;
    if (r.at == start || !name_start(s.data[start])) return E::malformed;
    e.name = {s.data + start, r.at - start};
    for (u32 i = 0; i < e.name.bytes; ++i) if (e.name.data[i] == ':') return E::unsupported;
    for (;;) {
        const u32 before_space = r.at;
        while (r.at < s.bytes && space(s.data[r.at])) ++r.at;
        if (r.at == s.bytes) return E::malformed;
        if (s.data[r.at] == '>') { ++r.at; return E::none; }
        if (!e.closing && r.at + 1 < s.bytes && s.data[r.at] == '/' && s.data[r.at + 1] == '>') {
            r.at += 2; e.empty = true; return E::none;
        }
        if (e.closing || before_space == r.at || e.count == 20) return E::malformed;
        const u32 key = r.at;
        while (r.at < s.bytes && name_char(s.data[r.at])) ++r.at;
        if (r.at == key || !name_start(s.data[key])) return E::malformed;
        Attribute a{{s.data + key, r.at - key}, {}};
        while (r.at < s.bytes && space(s.data[r.at])) ++r.at;
        if (r.at == s.bytes || s.data[r.at++] != '=') return E::malformed;
        while (r.at < s.bytes && space(s.data[r.at])) ++r.at;
        if (r.at == s.bytes || (s.data[r.at] != '"' && s.data[r.at] != '\'')) return E::malformed;
        const char quote = s.data[r.at++]; const u32 value = r.at;
        while (r.at < s.bytes && s.data[r.at] != quote) {
            if (s.data[r.at] == '<' || static_cast<u8>(s.data[r.at]) < 32 || r.at - value >= 2048) return E::malformed;
            ++r.at;
        }
        if (r.at == s.bytes) return E::malformed;
        a.value = {s.data + value, r.at - value}; ++r.at;
        for (u32 i = 0; i < e.count; ++i) if (equal(a.name, e.attributes[i].name)) return E::malformed;
        e.attributes[e.count++] = a;
    }
}
Utf8View attribute(const Element& e, Utf8View name) noexcept {
    for (u32 i = 0; i < e.count; ++i) if (equal(e.attributes[i].name, name)) return e.attributes[i].value;
    return {};
}
bool identifier(Utf8View s) noexcept {
    if (!s.data || !s.bytes || s.bytes > 32) return false;
    for (u32 i = 0; i < s.bytes; ++i) if (!ascii_is_alnum(s.data[i]) && s.data[i] != '_' && s.data[i] != '-') return false;
    return true;
}
u32 path(const DashTrack& t, u32 index, bool init, u8* output) noexcept {
    const auto text = init ? t.initialization : t.media;
    if (!identifier(t.id) || !text.data || !text.bytes || text.bytes > 256 || text.data[0] == '/' ||
        !t.segment_count || index >= t.segment_count || t.start_number > ~u32{0} - index) return 0;
    char digits[10]; u32 digit_count = 0, n = t.start_number + index;
    do { digits[digit_count++] = static_cast<char>('0' + n % 10); n /= 10; } while (n);
    u32 size = 0, numbers = 0;
    const auto put = [&](char c) noexcept { if (output) output[size] = static_cast<u8>(c); ++size; };
    for (u32 at = 0; at < text.bytes;) {
        const char c = text.data[at++];
        if (c != '$') {
            if ((!ascii_is_alnum(c) && c != '/' && c != '.' && c != '-' && c != '_') ||
                (c == '.' && at < text.bytes && text.data[at] == '.') ||
                (c == '/' && (at == text.bytes || text.data[at] == '/'))) return 0;
            put(c); continue;
        }
        const u32 start = at;
        while (at < text.bytes && text.data[at] != '$') ++at;
        if (at == text.bytes) return 0;
        const Utf8View token{text.data + start, at++ - start};
        if (equal(token, utf8_literal("RepresentationID"))) {
            for (u32 i = 0; i < t.id.bytes; ++i) put(t.id.data[i]);
        } else {
            if (init || ++numbers != 1 || token.bytes < 6 || !bytes_equal(token.data, "Number", 6)) return 0;
            u64 width = 0;
            if (token.bytes != 6 && (token.bytes < 10 || token.data[6] != '%' || token.data[7] != '0' ||
                token.data[token.bytes - 1] != 'd' || !number({token.data + 8, token.bytes - 9}, &width) ||
                !width || width > 10)) return 0;
            for (u32 i = digit_count; i < width; ++i) put('0');
            for (u32 i = digit_count; i-- > 0;) put(digits[i]);
        }
    }
    return !init && numbers != 1 ? 0 : size;
}
enum class Tag : u8 { other, mpd, period, adaptation, representation, segment_template };
Tag tag(Utf8View name) noexcept {
    return equal(name, utf8_literal("MPD")) ? Tag::mpd : equal(name, utf8_literal("Period")) ? Tag::period :
        equal(name, utf8_literal("AdaptationSet")) ? Tag::adaptation :
        equal(name, utf8_literal("Representation")) ? Tag::representation :
        equal(name, utf8_literal("SegmentTemplate")) ? Tag::segment_template : Tag::other;
}
struct Candidate { DashTrack track{}; bool video{}, audio{}, has_template{}; };
bool unsigned_attribute(const Element& e, Utf8View name, u32* value) noexcept {
    const auto s = attribute(e, name); if (!s.data) return true;
    u64 n{}; if (!number(s, &n) || n > ~u32{0}) return false;
    *value = static_cast<u32>(n); return true;
}
bool better(const DashTrack& a, const DashTrack& b, u16 cap) noexcept {
    if (!b.id.bytes) return true;
    if ((a.height <= cap) != (b.height <= cap)) return a.height <= cap;
    if (a.height != b.height) return a.height <= cap ? a.height > b.height : a.height < b.height;
    return a.bandwidth < b.bandwidth;
}
} // namespace
u32 dash_segment_path(const DashTrack& t, u32 segment, bool init, Slice<u8> output) noexcept {
    const u32 size = path(t, segment, init, nullptr);
    if (!size || size > 512 || !output.data || output.count < size) return 0;
    if (!byte_range_valid(output.data, size) || byte_ranges_overlap(output.data, size, &t, sizeof(t)) ||
        byte_ranges_overlap(output.data, size, t.id.data, t.id.bytes) ||
        byte_ranges_overlap(output.data, size, t.initialization.data, t.initialization.bytes) ||
        byte_ranges_overlap(output.data, size, t.media.data, t.media.bytes)) return 0;
    return path(t, segment, init, output.data);
}
DashManifestError dash_manifest_parse(Utf8View input, u16 cap, DashManifest* output) noexcept {
    if (!input.data || !input.bytes || input.bytes > maximum_bytes || !cap || cap > 2048 || !output) return E::invalid_input;
    if (!byte_range_valid(input.data, input.bytes) || !byte_range_valid(output, sizeof(*output)) ||
        byte_ranges_overlap(input.data, input.bytes, output, sizeof(*output))) return E::invalid_input;
    for (u32 i = 0; i < input.bytes; ++i)
        if (static_cast<u8>(input.data[i]) < 32 && !space(input.data[i])) return E::malformed;
    Reader reader{input}; Element element{}; Utf8View stack[8]{}, ids[64]{};
    u32 depth = 0, periods = 0, representations = 0; bool root = false, closed = false, audio_present = false;
    Utf8View adaptation_mime{}, adaptation_codec{}; bool adaptation_image = false;
    DashManifest result{}; Candidate candidate{};
    const auto finish = [&]() noexcept {
        auto& t = candidate.track;
        if (candidate.has_template && t.segment_count && t.bandwidth) {
            if (candidate.video && t.width && t.height && t.width <= 2048 && t.height <= 2048 && better(t, result.video, cap)) result.video = t;
            if (candidate.audio && t.bandwidth <= 192000 && (!result.audio.id.bytes || t.bandwidth > result.audio.bandwidth)) result.audio = t;
        }
        candidate = {};
    };
    while (reader.at < input.bytes) {
        const auto error = next(reader, element); if (error != E::none) return error;
        if (element.text.bytes) {
            for (u32 i = 0; i < element.text.bytes; ++i)
                if (!space(element.text.data[i]) && (!depth || tag(stack[depth - 1]) != Tag::other)) return E::malformed;
        }
        if (!element.name.bytes) continue;
        const auto kind = tag(element.name);
        if (element.closing) {
            if (!depth || !equal(stack[depth - 1], element.name)) return E::malformed;
            --depth; if (kind == Tag::representation) finish(); if (!depth) closed = true;
            continue;
        }
        if (closed || depth == 8) return E::malformed;
        const auto parent = depth ? tag(stack[depth - 1]) : Tag::other;
        if (equal(element.name, utf8_literal("ContentProtection")) || equal(element.name, utf8_literal("SegmentTimeline")) ||
            equal(element.name, utf8_literal("SegmentBase")) || equal(element.name, utf8_literal("SegmentList"))) return E::unsupported;
        const auto ns = attribute(element, utf8_literal("xmlns"));
        if (ns.data && !equal(ns, utf8_literal("urn:mpeg:dash:schema:mpd:2011"))) return E::unsupported;
        if (kind == Tag::mpd) {
            if (root || depth || !ns.bytes) return E::malformed;
            root = true;
            if (!equal(attribute(element, utf8_literal("type")), utf8_literal("static"))) return E::unsupported;
            if (!duration(attribute(element, utf8_literal("mediaPresentationDuration")), &result.duration_us)) return E::malformed;
        } else if (!root || !depth) return E::malformed;
        else if (kind == Tag::period) {
            if (parent != Tag::mpd || ++periods != 1) return E::unsupported;
            const auto start = attribute(element, utf8_literal("start"));
            const auto period_duration = attribute(element, utf8_literal("duration")); u64 value{};
            if ((start.data && (!duration(start, &value, true) || value)) ||
                (period_duration.data && (!duration(period_duration, &value) || value != result.duration_us))) return E::unsupported;
        } else if (kind == Tag::adaptation) {
            if (parent != Tag::period) return E::malformed;
            adaptation_mime = attribute(element, utf8_literal("mimeType"));
            adaptation_codec = attribute(element, utf8_literal("codecs"));
            adaptation_image = equal(attribute(element, utf8_literal("contentType")), utf8_literal("image")) ||
                (adaptation_mime.bytes >= 6 && bytes_equal(adaptation_mime.data, "image/", 6));
            audio_present |= equal(attribute(element, utf8_literal("contentType")), utf8_literal("audio")) ||
                (adaptation_mime.bytes >= 6 && bytes_equal(adaptation_mime.data, "audio/", 6));
        } else if (kind == Tag::representation) {
            if (parent != Tag::adaptation || representations == 64) return E::malformed;
            candidate = {}; auto& t = candidate.track; t.id = attribute(element, utf8_literal("id"));
            if (!identifier(t.id)) return E::malformed;
            for (u32 i = 0; i < representations; ++i) if (equal(ids[i], t.id)) return E::malformed;
            ids[representations++] = t.id;
            auto mime = attribute(element, utf8_literal("mimeType")), codec = attribute(element, utf8_literal("codecs"));
            if (!mime.data) mime = adaptation_mime;
            if (!codec.data) codec = adaptation_codec;
            audio_present |= mime.bytes >= 6 && bytes_equal(mime.data, "audio/", 6);
            candidate.video = equal(mime, utf8_literal("video/mp4")) && codec.bytes == 11 && bytes_equal(codec.data, "avc1.", 5);
            for (u32 i = 5; candidate.video && i < codec.bytes; ++i) candidate.video = ascii_is_hex_digit(codec.data[i]);
            candidate.audio = equal(mime, utf8_literal("audio/mp4")) && equal(codec, utf8_literal("mp4a.40.2"));
            u32 width = 0, height = 0;
            if (!unsigned_attribute(element, utf8_literal("bandwidth"), &t.bandwidth) ||
                !unsigned_attribute(element, utf8_literal("width"), &width) || !unsigned_attribute(element, utf8_literal("height"), &height)) return E::malformed;
            if (width <= 2048 && height <= 2048) { t.width = static_cast<u16>(width); t.height = static_cast<u16>(height); }
        } else if (kind == Tag::segment_template) {
            if (parent == Tag::adaptation && adaptation_image) {
                if (!element.empty) stack[depth++] = element.name;
                continue;
            }
            if (parent != Tag::representation || candidate.has_template) return E::unsupported;
            auto& t = candidate.track; candidate.has_template = true;
            if (!candidate.video && !candidate.audio) {
                if (!element.empty) stack[depth++] = element.name;
                continue;
            }
            t.initialization = attribute(element, utf8_literal("initialization")); t.media = attribute(element, utf8_literal("media"));
            if (!unsigned_attribute(element, utf8_literal("timescale"), &t.timescale) || !t.timescale || t.timescale > 1000000000 ||
                !unsigned_attribute(element, utf8_literal("startNumber"), &t.start_number) ||
                !number(attribute(element, utf8_literal("duration")), &t.segment_duration) || !t.segment_duration ||
                t.segment_duration > u64{86400} * t.timescale) return E::malformed;
            const auto offset = attribute(element, utf8_literal("presentationTimeOffset"));
            u64 value{}; if (offset.data && (!number(offset, &value) || value)) return E::unsupported;
            const u64 ticks = result.duration_us / 1000000 * t.timescale +
                (result.duration_us % 1000000 * t.timescale + 999999) / 1000000;
            const u64 count = (ticks + t.segment_duration - 1) / t.segment_duration;
            if (!count || count > maximum_segments || t.start_number > ~u32{0} - (count - 1)) return E::capacity;
            t.segment_count = static_cast<u32>(count);
            if (!path(t, 0, true, nullptr) || !path(t, t.segment_count - 1, false, nullptr)) return E::unsupported;
        } else if (equal(element.name, utf8_literal("BaseURL")) && (parent != Tag::representation || candidate.video || candidate.audio)) return E::unsupported;
        if (element.empty) { if (kind == Tag::representation) finish(); if (!depth) closed = true; }
        else stack[depth++] = element.name;
    }
    if (!root || !closed || depth || periods != 1) return E::malformed;
    if (!result.video.id.bytes || (audio_present && !result.audio.id.bytes)) return E::unsupported;
    *output = result; return E::none;
}
} // namespace maicord
