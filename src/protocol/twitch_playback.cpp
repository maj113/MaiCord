#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "json_field_lists.hpp"

namespace maicord {
bool twitch_vod_id(Utf8View id) noexcept {
    if (!id.data || !id.bytes || id.bytes > 20 || id.data[0] == '0') return false;
    u64 value = 0;
    for (u32 i = 0; i < id.bytes; ++i) {
        const u32 digit = static_cast<u8>(id.data[i]) - '0';
        if (digit > 9 || value > (~u64{0} - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return value != 0;
}
bool twitch_channel_id(Utf8View id) noexcept {
    if (!id.data || !id.bytes || id.bytes > 25) return false;
    for (u32 i = 0; i < id.bytes; ++i) if (!ascii_is_alnum(id.data[i]) && id.data[i] != '_') return false;
    return true;
}
u32 twitch_playback_body(Utf8View id, Slice<u8> output, bool live) noexcept {
    if (!(live ? twitch_channel_id(id) : twitch_vod_id(id))) return 0;
    char canonical[25];
    if (live) { for (u32 i = 0; i < id.bytes; ++i) canonical[i] = ascii_lower(id.data[i]); id.data = canonical; }
    u8 staged[512]; ProtocolWriter w{{staged, sizeof(staged)}, 0};
    const auto prefix = live ? utf8_literal("{\"query\":\"query { streamPlaybackAccessToken(channelName:\\\"")
                             : utf8_literal("{\"query\":\"query { videoPlaybackAccessToken(id:\\\"");
    const bool valid = protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(prefix.data), prefix.bytes) &&
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(id.data), id.bytes) &&
        protocol_writer_append_literal(&w, "\\\", params:{platform:\\\"web\\\",playerBackend:\\\"mediaplayer\\\",playerType:\\\"embed\\\"}) { value signature } }\"}");
    if (!valid || !output.data || output.count < w.used) return 0;
    copy_bytes(output.data, staged, w.used); return w.used;
}
namespace {
using protocol_detail::FieldSet;
using protocol_detail::FieldValue;
using protocol_detail::MemberNames;
template <protocol_detail::MemberName... Names>
__forceinline bool fields(const JsonIndexedDocument& doc, const JsonValue& value, FieldSet<Names...>& set) noexcept {
    return protocol_detail::fields_at<16>(doc, value, set);
}
struct Scratch {
    char signature[128]{}, token[2048]{};
    EmbedStream result{};
    ~Scratch() noexcept { secure_clear_bytes(this, sizeof(*this)); }
};
}
EmbedPlayerError twitch_playback_manifest(Utf8View json, Utf8View id, u64 now, Slice<u32> scratch, EmbedStream* output, bool live) noexcept {
    using E = EmbedPlayerError;
    if (!(live ? twitch_channel_id(id) : twitch_vod_id(id)) || !now || !output || !json.data || !json.bytes || json.bytes > 64 * 1024 ||
        !byte_range_valid(json.data, json.bytes) || !byte_range_valid(output, sizeof(*output)) ||
        byte_ranges_overlap(json.data, json.bytes, output, sizeof(*output))) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    FieldSet<"data", "errors"> root;
    if (!fields(doc, {json, 0, JsonValueKind::object}, root)) return E::malformed;
    if (root[1].seen) return E::unavailable;
    FieldValue data;
    if (!root[0].seen || !protocol_detail::fields_n(doc, root[0].value, 16, &data, 1,
            (live ? MemberNames<"streamPlaybackAccessToken">::data.bytes : MemberNames<"videoPlaybackAccessToken">::data.bytes) + 1) ||
        !data.seen) return E::malformed;
    if (data.value.kind == JsonValueKind::null_value) return E::unavailable;
    FieldSet<"signature", "value"> access; JsonString signature{}, token{};
    if (!fields(doc, data.value, access) || !access[0].seen || !access[1].seen ||
        !json_value_as_string(access[0].value, &signature) || !json_value_as_string(access[1].value, &token)) return E::malformed;
    Scratch s;
    const auto sig = json_string_decode_utf8(signature, {s.signature, sizeof(s.signature)});
    const auto tok = json_string_decode_utf8(token, {s.token, sizeof(s.token)});
    if (!sig.ok() || sig.bytes != 40 || !tok.ok() || !tok.bytes) return E::malformed;
    for (u32 i = 0; i < sig.bytes; ++i) if (!ascii_is_hex_digit(s.signature[i])) return E::malformed;
    const Utf8View token_json{s.token, tok.bytes};
    const auto token_scan = json_scan_structurals_avx1(token_json, scratch);
    if (!token_scan.ok()) return token_scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument claims_doc{token_json, {scratch.data, token_scan.count}};
    FieldValue claims[3];
    FieldSet<"forbidden"> authorization;
    u64 expires{}, identity{}, requested{}; bool forbidden = true;
    if (!protocol_detail::fields_n(claims_doc, {token_json, 0, JsonValueKind::object}, 16, claims, 3,
            (live ? MemberNames<"channel", "expires", "authorization">::data.bytes : MemberNames<"vod_id", "expires", "authorization">::data.bytes) + 1) ||
        !claims[0].seen ||
        !claims[1].seen || !json_value_to_u64(claims[1].value, &expires) || !claims[2].seen ||
        !fields(claims_doc, claims[2].value, authorization) || !authorization[0].seen ||
        !json_value_to_bool(authorization[0].value, &forbidden)) return E::malformed;
    if (live) {
        JsonString channel{}; char name[25];
        if (!json_value_as_string(claims[0].value, &channel)) return E::malformed;
        const auto decoded = json_string_decode_utf8(channel, {name, sizeof(name)});
        if (!decoded.ok() || !ascii_equal_ignore_case({name, decoded.bytes}, id)) return E::malformed;
    } else {
        for (u32 i = 0; i < id.bytes; ++i) requested = requested * 10 + static_cast<u32>(id.data[i] - '0');
        if (!json_value_to_u64(claims[0].value, &identity) || identity != requested) return E::malformed;
    }
    if (forbidden || expires <= now) return E::unavailable;
    char canonical[25];
    if (live) { for (u32 i = 0; i < id.bytes; ++i) canonical[i] = ascii_lower(id.data[i]); id.data = canonical; }
    ProtocolWriter w{{reinterpret_cast<u8*>(s.result.url), sizeof(s.result.url)}, 0};
    const auto origin = live ? utf8_literal("https://usher.ttvnw.net/api/channel/hls/") : utf8_literal("https://usher.ttvnw.net/vod/");
    const auto parameters = live ? utf8_literal(".m3u8?allow_source=true&allow_audio_only=true&sig=")
                                 : utf8_literal(".m3u8?allow_source=true&allow_audio_only=true&nauthsig=");
    const auto token_parameter = live ? utf8_literal("&token=") : utf8_literal("&nauth=");
    const bool written = protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(origin.data), origin.bytes) &&
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(id.data), id.bytes) &&
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(parameters.data), parameters.bytes) &&
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(s.signature), sig.bytes) &&
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(token_parameter.data), token_parameter.bytes) &&
        protocol_writer_append_percent_encoded(&w, token_json);
    if (!written) return E::capacity;
    s.result.url_bytes = w.used; *output = s.result; return E::none;
}
bool twitch_vod_location(const Utf8View& url_view, bool playlist, EmbedStreamLocation* output) noexcept {
    const Utf8View url = url_view;
    constexpr auto origin = utf8_literal("https://d2nvs31859zcd8.cloudfront.net");
    if (!url.data || url.bytes > kEmbedStreamUrlBytes || !utf8_starts_with(url, origin) ||
        url.bytes <= origin.bytes || url.data[origin.bytes] != '/') return false;
    u32 start = origin.bytes + 1, parts = 0;
    for (u32 i = start; i <= url.bytes; ++i) {
        if (i == url.bytes || url.data[i] == '/') {
            const Utf8View part{url.data + start, i - start};
            if (!part.bytes || utf8_equal(part, utf8_literal(".")) || utf8_equal(part, utf8_literal(".."))) return false;
            ++parts; start = i + 1;
        } else if (!ascii_is_alnum(url.data[i]) && url.data[i] != '_' && url.data[i] != '-' && url.data[i] != '.') return false;
    }
    const auto suffix = playlist ? utf8_literal("/index-dvr.m3u8") : utf8_literal(".mp4");
    if (parts < 3 || url.bytes < suffix.bytes || !utf8_equal({url.data + url.bytes - suffix.bytes, suffix.bytes}, suffix)) return false;
    if (output) *output = {{origin.data + 8, origin.bytes - 8}, {url.data + origin.bytes, url.bytes - origin.bytes}};
    return true;
}
bool twitch_live_location(const Utf8View& url_view, bool playlist, EmbedStreamLocation* output, bool transport) noexcept {
    const Utf8View url = url_view;
    constexpr auto scheme = utf8_literal("https://");
    if (!url.data || url.bytes > kEmbedStreamUrlBytes || !utf8_starts_with(url, scheme)) return false;
    u32 end = scheme.bytes;
    while (end < url.bytes && url.data[end] != '/') ++end;
    const Utf8View host{url.data + scheme.bytes, end - scheme.bytes};
    const auto suffix = playlist ? utf8_literal(".playlist.ttvnw.net") : utf8_literal(".hls.ttvnw.net");
    if (host.bytes > 253 || host.bytes <= suffix.bytes ||
        !ascii_equal_ignore_case({host.data + host.bytes - suffix.bytes, suffix.bytes}, suffix)) return false;
    u32 label = 0;
    for (u32 i = 0; i <= host.bytes; ++i) {
        if (i == host.bytes || host.data[i] == '.') {
            if (i == label || i - label > 63 || host.data[label] == '-' || host.data[i - 1] == '-') return false;
            label = i + 1;
        } else if (!ascii_is_alnum(host.data[i]) && host.data[i] != '-') return false;
    }
    const Utf8View target{url.data + end, url.bytes - end};
    const auto prefix = playlist ? utf8_literal("/v1/playlist/") : utf8_literal("/v1/segment/");
    const auto extension = playlist ? utf8_literal(".m3u8") : transport ? utf8_literal(".ts") : utf8_literal(".mp4");
    // Live media can carry a delivery shard before the versioned route.
    // Retain it in the request target: it is part of the advertised URL.
    u32 route = 0;
    if (!playlist && utf8_starts_with(target, utf8_literal("/s"))) {
        route = 2;
        while (route < target.bytes && ascii_is_digit(target.data[route])) ++route;
        if (route == 2) return false;
    }
    if (!utf8_starts_with({target.data + route, target.bytes - route}, prefix)) return false;
    const u32 key_begin = route + prefix.bytes;
    u32 path_end = key_begin;
    while (path_end < target.bytes && target.data[path_end] != '?') ++path_end;
    if (path_end <= key_begin + extension.bytes ||
        !utf8_equal({target.data + path_end - extension.bytes, extension.bytes}, extension)) return false;
    u32 padding = 0;
    for (u32 i = key_begin; i < path_end - extension.bytes; ++i) {
        const char c = target.data[i];
        if (c == '=') { if (++padding > 2) return false; }
        // Twitch uses both URL-safe and standard base64 in opaque media keys.
        // A slash inside that key does not change the fixed host/path prefix.
        else if (padding || (!ascii_is_alnum(c) && c != '_' && c != '-' && c != '+' && c != '/')) return false;
    }
    if (path_end < target.bytes) {
        if (playlist || !utf8_starts_with({target.data + path_end, target.bytes - path_end}, utf8_literal("?dna=")) ||
            path_end + 5 == target.bytes) return false;
        for (u32 i = path_end + 5; i < target.bytes; ++i) {
            const char c = target.data[i];
            if (c == '%') {
                if (i + 2 >= target.bytes || !ascii_is_hex_digit(target.data[i + 1]) || !ascii_is_hex_digit(target.data[i + 2])) return false;
                const auto digit = [](char v) noexcept -> u32 { return ascii_is_digit(v) ? v - '0' : ascii_lower(v) - 'a' + 10; };
                const u32 decoded = digit(target.data[i + 1]) * 16 + digit(target.data[i + 2]);
                if (decoded <= 32 || decoded >= 127 || decoded == '#' || decoded == '&') return false;
                i += 2;
            } else if (!ascii_is_alnum(c) && c != '_' && c != '-' && c != '.' && c != '~' && c != '=' && c != '+' && c != '/') return false;
        }
    }
    if (output) *output = {host, target};
    return true;
}
} // namespace maicord
