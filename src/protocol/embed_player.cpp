#include "maicord/protocol/embed_player.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/media/encoded_video_limits.hpp"
#include "maicord/protocol/cdn_http.hpp"
#include "maicord/protocol/embed_provider.hpp"

namespace maicord {
namespace {
bool video_id(Utf8View id) noexcept {
    if (!id.data || id.bytes != 11) return false;
    for (u32 i = 0; i < id.bytes; ++i)
        if (!ascii_is_alnum(id.data[i]) && id.data[i] != '_' && id.data[i] != '-') return false;
    return true;
}
struct Field { Utf8View name; JsonValue value{}; bool seen{}; };
struct Fields { Field* fields; u32 count; };
bool field(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& f = *static_cast<Fields*>(opaque);
    for (u32 i = 0; i < f.count; ++i)
        if (json_name_is(name, f.fields[i].name)) {
            if (f.fields[i].seen) return false;
            f.fields[i].seen = true;
            f.fields[i].value = value;
            break;
        }
    return true;
}
template <usize N>
bool fields(const JsonIndexedDocument& doc, const JsonValue& value, Field (&out)[N]) noexcept {
    Fields list{out, static_cast<u32>(N)};
    return json_visit_indexed_object(doc, value, 24, field, &list).ok();
}
bool text(const Field& f, JsonString* out) noexcept { return f.seen && json_value_as_string(f.value, out); }
bool equal(const Field& f, const Utf8View& value) noexcept {
    JsonString string{};
    return text(f, &string) && json_string_equals_ascii(string, value);
}
bool number(const Field& f, u32* out) noexcept {
    u64 n = 0;
    JsonString s{};
    if (!f.seen || !(f.value.kind == JsonValueKind::string ?
            json_value_as_string(f.value, &s) && json_string_to_u64(s, &n) :
            json_value_to_u64(f.value, &n)) || n > ~u32{0}) return false;
    *out = static_cast<u32>(n);
    return true;
}
EmbedPlayerError embedded_json(Utf8View html, Utf8View marker, Utf8View* output, bool array = false) noexcept {
    using E = EmbedPlayerError;
    // Extract one bounded JSON object from the public embed configuration;
    // strings and escapes cannot terminate it and no script is evaluated.
    reset_value(output);
    auto& json = *output;
    for (u32 i = 0; i + marker.bytes < html.bytes; ++i) {
        if (!bytes_equal(html.data + i, marker.data, marker.bytes) ||
            (i && (ascii_is_alnum(html.data[i - 1]) || html.data[i - 1] == '_'))) continue;
        u32 p = i + marker.bytes;
        while (p < html.bytes && (html.data[p] == ' ' || html.data[p] == '\t' || html.data[p] == '\r' || html.data[p] == '\n')) ++p;
        if (p == html.bytes || html.data[p++] != '=') continue;
        while (p < html.bytes && (html.data[p] == ' ' || html.data[p] == '\t' || html.data[p] == '\r' || html.data[p] == '\n')) ++p;
        if (p == html.bytes || html.data[p] != (array ? '[' : '{')) continue;
        if (json.bytes) return E::malformed;
        const u32 start = p;
        u32 depth = 0; bool quoted = false, escaped = false;
        for (; p < html.bytes; ++p) {
            const char c = html.data[p];
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') { if (++depth > 24) return E::capacity; }
            else if ((c == '}' || c == ']') && !--depth) { ++p; break; }
        }
        if (depth || quoted) return E::malformed;
        json = {html.data + start, p - start}; i = p - 1;
    }
    return json.bytes ? E::none : E::unsupported;
}
struct Candidate {
    JsonString url{};
    u32 content{}, bitrate{}, width{}, height{};
    bool combined{}, drc{};
};
struct Selection {
    JsonIndexedDocument doc;
    Candidate video, audio, combined;
    u32 maximum_height, count;
};
bool better_rendition(u32 height, u32 bitrate, u32 old_height, u32 old_bitrate, u32 cap) noexcept {
    if (!old_height) return true;
    if ((height <= cap) != (old_height <= cap)) return height <= cap;
    if (height != old_height) return height <= cap ? height > old_height : height < old_height;
    return bitrate < old_bitrate;
}
bool better_video(const Candidate& a, const Candidate& b, u32 cap) noexcept {
    if (!b.url.encoded.bytes) return true;
    // Save the second source only when both choices have the same resolution.
    if (a.height == b.height && a.combined != b.combined) return a.combined;
    return better_rendition(a.height, a.bitrate, b.height, b.bitrate, cap);
}
bool format(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<Selection*>(opaque);
    if (++s.count > 256) return false;
    Field f[]{{utf8_literal("url")}, {utf8_literal("mimeType")}, {utf8_literal("contentLength")},
        {utf8_literal("bitrate")}, {utf8_literal("width")}, {utf8_literal("height")},
        {utf8_literal("audioQuality")}, {utf8_literal("audioChannels")}, {utf8_literal("audioTrack")},
        {utf8_literal("drmFamilies")}, {utf8_literal("signatureCipher")}, {utf8_literal("cipher")}, {utf8_literal("isDrc")}};
    if (!fields(s.doc, value, f)) return false;
    if (f[9].seen || f[10].seen || f[11].seen || !f[0].seen) return true;
    Candidate c{};
    JsonString mime{};
    char mime_bytes[128];
    if (!text(f[0], &c.url) || !text(f[1], &mime)) return false;
    const auto decoded = json_string_decode_utf8(mime, {mime_bytes, sizeof(mime_bytes)});
    if (!decoded.ok()) return true;
    const Utf8View type{mime_bytes, decoded.bytes};
    const bool video = utf8_starts_with(type, utf8_literal("video/mp4; codecs=\"avc1."));
    const bool audio = utf8_equal(type, utf8_literal("audio/mp4; codecs=\"mp4a.40.2\""));
    if (!video && !audio) return true;
    if (f[2].seen && (!number(f[2], &c.content) || !c.content)) return false;
    if (c.content > kMediaMaximumProgressiveVideoBytes) return true;
    if (!number(f[3], &c.bitrate) || !c.bitrate) return false;
    if (video) {
        if (!number(f[4], &c.width) || !number(f[5], &c.height) || !c.width || !c.height) return false;
        if (c.width > 2048 || c.height > 2048) return true;
        c.combined = f[6].seen;
        if (c.combined && !ascii_contains_ignore_case(type, utf8_literal("mp4a.40.2"))) return true;
        if (c.combined && better_video(c, s.combined, s.maximum_height)) s.combined = c;
        if (better_video(c, s.video, s.maximum_height)) s.video = c;
    } else {
        if(f[12].seen && !json_value_to_bool(f[12].value,&c.drc)) return false;
        u32 channels = 0;
        if (!number(f[7], &channels) || !channels || channels > 2) return true;
        if (f[8].seen) {
            Field track[]{{utf8_literal("audioIsDefault")}};
            bool is_default = false;
            if (!fields(s.doc, f[8].value, track) || !track[0].seen ||
                !json_value_to_bool(track[0].value, &is_default)) return false;
            if (!is_default) return true;
        }
        // Admit higher-quality stereo AAC, including variable-rate encodes.
        // Prefer the original dynamic range. Tiny bitrate differences between
        // otherwise equivalent encodes must not select processed audio.
        if (c.bitrate <= 512000 && (!s.audio.url.encoded.bytes ||
            (c.drc != s.audio.drc ? !c.drc : c.bitrate > s.audio.bitrate))) s.audio = c;
    }
    return true;
}
bool store(const Candidate& candidate, EmbedStream* out) noexcept {
    const auto decoded = json_string_decode_utf8(candidate.url, {out->url, sizeof(out->url)});
    if (!decoded.ok() || !youtube_stream_url({out->url, decoded.bytes})) return false;
    out->url_bytes = decoded.bytes;
    out->content_bytes = candidate.content;
    out->bitrate = candidate.bitrate;
    out->width = static_cast<u16>(candidate.width);
    out->height = static_cast<u16>(candidate.height);
    return true;
}
struct TikTokSelection { JsonIndexedDocument doc; Utf8View id; u64 now; u32 height, items, profiles; EmbedStreams best{}; };
u64 stream_expiry(Utf8View url, Utf8View key = utf8_literal("expire="), bool* present = nullptr) noexcept {
    u64 result = 0;
    bool seen = false;
    for (u32 i = 0; i < url.bytes; ++i) {
        if (url.data[i] != '?' && url.data[i] != '&') continue;
        const u32 begin = i + 1;
        if (!utf8_starts_with({url.data + begin, url.bytes - begin}, key)) continue;
        if (present) *present = true;
        if (seen) return 0;
        seen = true;
        u32 n = begin + key.bytes;
        const u32 digits = n;
        for (; n < url.bytes && url.data[n] != '&'; ++n) {
            const u32 d = static_cast<u8>(url.data[n]) - '0';
            if (d > 9 || result > (~u64{0} - d) / 10) return 0;
            result = result * 10 + d;
        }
        if (n == digits) return 0;
    }
    return result;
}
struct TikTokUrls { TikTokSelection* selection; EmbedStream* stream; u32 expires, count; };
bool tiktok_url(void* opaque, u32, JsonValue value) noexcept {
    auto& u = *static_cast<TikTokUrls*>(opaque);
    if (++u.count > 8) return false;
    if (u.expires) return true;
    JsonString text{};
    if (!json_value_as_string(value, &text)) return false;
    const auto decoded = json_string_decode_utf8(text, {u.stream->url, sizeof(u.stream->url)});
    if (!decoded.ok()) return true;
    const Utf8View url{u.stream->url, decoded.bytes};
    if (!embed_stream_location(CdnHost::tiktok_video, url)) return true;
    const auto expires = stream_expiry(url);
    if (expires <= u.selection->now) return true;
    u.expires = static_cast<u32>(minimum<u64>(expires - u.selection->now, 86400));
    u.stream->url_bytes = decoded.bytes;
    return true;
}
bool tiktok_profile(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<TikTokSelection*>(opaque);
    if (++s.profiles > 32) return false;
    Field f[]{{utf8_literal("codec_type")}, {utf8_literal("bitrate")}, {utf8_literal("play_addr")}};
    if (!fields(s.doc, value, f)) return false;
    if (!equal(f[0], utf8_literal("h264"))) return true;
    Field address[]{{utf8_literal("width")}, {utf8_literal("height")}, {utf8_literal("data_size")},
                    {utf8_literal("url_list")}};
    if (!f[2].seen || !fields(s.doc, f[2].value, address)) return false;
    EmbedStreams candidate{};
    u32 width = 0, height = 0;
    if (!number(address[0], &width) || !number(address[1], &height) || !width || !height ||
        !number(address[2], &candidate.video.content_bytes) || !candidate.video.content_bytes ||
        !number(f[1], &candidate.video.bitrate) || !candidate.video.bitrate || !address[3].seen) return false;
    if (width > 2048 || height > 2048 || candidate.video.content_bytes > kMediaMaximumProgressiveVideoBytes) return true;
    candidate.video.width = static_cast<u16>(width); candidate.video.height = static_cast<u16>(height);
    TikTokUrls urls{&s, &candidate.video};
    if (!json_visit_indexed_array(s.doc, address[3].value, 24, tiktok_url, &urls).ok()) return false;
    if (!urls.expires) return true;
    if (better_rendition(height, candidate.video.bitrate, s.best.video.height, s.best.video.bitrate, s.height)) {
        candidate.expires_seconds = urls.expires;
        candidate.combined = true;
        s.best = candidate;
    }
    secure_clear_bytes(&candidate, sizeof(candidate));
    return true;
}
bool tiktok_item(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<TikTokSelection*>(opaque);
    if (++s.items > 1) return false;
    Field f[]{{utf8_literal("id_str")}, {utf8_literal("video_info")}};
    if (!fields(s.doc, value, f) || !equal(f[0], s.id)) return false;
    if (!f[1].seen) return true; // Photos and other non-video posts.
    Field video[]{{utf8_literal("profiles")}};
    if (!fields(s.doc, f[1].value, video)) return false;
    return !video[0].seen || json_visit_indexed_array(s.doc, video[0].value, 24, tiktok_profile, &s).ok();
}
struct ClipSelection { JsonIndexedDocument doc; EmbedStream best{}; u32 height, assets{}, profiles{}; };
bool clip_quality(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<ClipSelection*>(opaque);
    if (++s.profiles > 32) return false;
    Field f[]{{utf8_literal("videoCodec")}, {utf8_literal("height")}, {utf8_literal("width")},
              {utf8_literal("bitrate")}, {utf8_literal("sourceURL")}};
    if (!fields(s.doc, value, f)) return false;
    if (!equal(f[0], utf8_literal("AVC"))) return true;
    u32 height = 0, width = 0, bitrate = 0;
    JsonString source{};
    if (!number(f[1], &height) || !number(f[2], &width) || !number(f[3], &bitrate) ||
        !height || !width || !text(f[4], &source)) return false;
    if (height > 2048 || width > 2048) return true;
    if (!better_rendition(height, bitrate, s.best.height, s.best.bitrate, s.height)) return true;
    EmbedStream candidate{};
    const auto decoded = json_string_decode_utf8(source, {candidate.url, sizeof(candidate.url)});
    if (!decoded.ok() || !embed_stream_location(CdnHost::twitch_video, {candidate.url, decoded.bytes})) return true;
    for (u32 i = 0; i < decoded.bytes; ++i) if (candidate.url[i] == '?') return true;
    candidate.url_bytes = decoded.bytes;
    candidate.width = static_cast<u16>(width); candidate.height = static_cast<u16>(height);
    candidate.bitrate = bitrate;
    s.best = candidate;
    return true;
}
bool clip_asset(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<ClipSelection*>(opaque);
    if (++s.assets > 8) return false;
    Field f[]{{utf8_literal("videoQualities")}};
    return fields(s.doc, value, f) && f[0].seen &&
        json_visit_indexed_array(s.doc, f[0].value, 24, clip_quality, &s).ok();
}
// The dimensions in the public MP4 path describe the rendition, unlike the
// post's original_info dimensions. Keep them tied to the exact media host.
bool twitter_location(Utf8View url, EmbedStreamLocation* out, u16* width=nullptr, u16* height=nullptr) noexcept {
    constexpr auto origin=utf8_literal("https://video.twimg.com");
    if(!url.data || url.bytes>kEmbedStreamUrlBytes || !utf8_starts_with(url,origin)) return false;
    const Utf8View target{url.data+origin.bytes,url.bytes-origin.bytes};
    const bool amplify=utf8_starts_with(target,utf8_literal("/amplify_video/"));
    const auto prefix=amplify?utf8_literal("/amplify_video/"):utf8_literal("/ext_tw_video/");
    if(!utf8_starts_with(target,prefix)) return false;
    u32 at=prefix.bytes, start=at;
    while(at<target.bytes && ascii_is_digit(target.data[at])) ++at;
    if(!twitter_post_id({target.data+start,at-start})) return false;
    const auto path=amplify?utf8_literal("/vid/"):utf8_literal("/pu/vid/");
    if(!utf8_starts_with({target.data+at,target.bytes-at},path)) return false;
    at+=path.bytes;
    if(utf8_starts_with({target.data+at,target.bytes-at},utf8_literal("avc1/"))) at+=5;
    u32 dimensions[2]{};
    for(u32 i=0;i<2;++i) {
        start=at;
        while(at<target.bytes && ascii_is_digit(target.data[at])) {
            dimensions[i]=dimensions[i]*10+static_cast<u32>(target.data[at++]-'0');
            if(dimensions[i]>2048 || at-start>4) return false;
        }
        if(!dimensions[i] || at==target.bytes || target.data[at++]!=(i?'/' : 'x')) return false;
    }
    start=at;
    while(at<target.bytes && (ascii_is_alnum(target.data[at]) || target.data[at]=='_' || target.data[at]=='-')) ++at;
    if(at==start || at-start>128 || !utf8_starts_with({target.data+at,target.bytes-at},utf8_literal(".mp4"))) return false;
    at+=4;
    if(at!=target.bytes) {
        if(!utf8_starts_with({target.data+at,target.bytes-at},utf8_literal("?tag="))) return false;
        at+=5;start=at;
        while(at<target.bytes && ascii_is_digit(target.data[at])) ++at;
        if(at==start || at-start>10 || at!=target.bytes) return false;
    }
    if(out) *out={{url.data+8,origin.bytes-8},target};
    if(width) *width=static_cast<u16>(dimensions[0]);
    if(height) *height=static_cast<u16>(dimensions[1]);
    return true;
}
struct TwitterMedia { JsonIndexedDocument doc; JsonValue selected{}; u8 index; u32 count{}; bool found{}; };
bool twitter_media(void* context,u32 index,JsonValue value) noexcept {
    auto& s=*static_cast<TwitterMedia*>(context);
    if(++s.count>4) return false;
    Field f[]{{utf8_literal("type")}};
    if(!fields(s.doc,value,f)) return false;
    if((!s.index && !s.found) || s.index==index+1) {
        if(equal(f[0],utf8_literal("video")) || equal(f[0],utf8_literal("animated_gif"))) {
            s.selected=value;s.found=true;
        }
    }
    return true;
}
struct TwitterVariants { JsonIndexedDocument doc; EmbedStream selected{}; u16 cap; u32 count{}; };
bool twitter_variant(void* context,u32,JsonValue value) noexcept {
    auto& s=*static_cast<TwitterVariants*>(context);
    if(++s.count>16) return false;
    Field f[]{{utf8_literal("content_type")},{utf8_literal("url")},{utf8_literal("bitrate")}};
    if(!fields(s.doc,value,f)) return false;
    if(!equal(f[0],utf8_literal("video/mp4"))) return true;
    JsonString url{}; EmbedStream candidate{};
    if(!text(f[1],&url) || (f[2].seen && !number(f[2],&candidate.bitrate))) return false;
    const auto decoded=json_string_decode_utf8(url,{candidate.url,sizeof(candidate.url)});
    bool valid=decoded.ok();
    if(valid) {
        candidate.url_bytes=decoded.bytes;
        valid=twitter_location({candidate.url,candidate.url_bytes},nullptr,&candidate.width,&candidate.height);
        if(valid && better_rendition(candidate.height,candidate.bitrate,s.selected.height,s.selected.bitrate,s.cap))
            s.selected=candidate;
    }
    secure_clear_bytes(&candidate,sizeof(candidate));
    // Unsupported media paths/codecs may coexist with a usable AVC rendition.
    return decoded.ok();
}
} // namespace
bool twitch_clip_id(Utf8View id) noexcept {
    if (!id.data || !id.bytes || id.bytes > 128) return false;
    for (u32 i = 0; i < id.bytes; ++i)
        if (!ascii_is_alnum(id.data[i]) && id.data[i] != '_' && id.data[i] != '-') return false;
    return true;
}
u32 twitch_clip_body(Utf8View id, Slice<u8> out) noexcept {
    if (!twitch_clip_id(id)) return 0;
    u8 staged[1024];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    constexpr auto query = utf8_literal("query($id:ID!){clip(slug:$id){slug playbackAccessToken(params:{platform:\"web\",playerType:\"clips-viewing\"}){signature value} assets{videoQualities(params:{supportedCodecs:[AVC]}){videoCodec height width bitrate sourceURL}}}}");
    if (!protocol_writer_append_literal(&w, "{\"query\":") || !protocol_writer_append_json_string(&w, query) ||
        !protocol_writer_append_literal(&w, ",\"variables\":{\"id\":") || !protocol_writer_append_json_string(&w, id) ||
        !protocol_writer_append_literal(&w, "}}") || !out.data || out.count < w.used) return 0;
    copy_bytes(out.data, staged, w.used);
    return w.used;
}
EmbedPlayerError twitch_clip_streams(Utf8View json, Utf8View id, u16 height, u64 now,
    Slice<u32> scratch, EmbedStreams* out) noexcept {
    using E = EmbedPlayerError;
    if (!out || !twitch_clip_id(id) || !height || height > 2048 || !now ||
        !json.data || !json.bytes || json.bytes > 2u * 1024u * 1024u) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    ClipSelection s{{json, {scratch.data, scan.count}}, {}, height};
    Field root[]{{utf8_literal("data")}, {utf8_literal("errors")}};
    if (!fields(s.doc, {json, 0, JsonValueKind::object}, root)) return E::malformed;
    if (root[1].seen) return E::unavailable;
    Field data[]{{utf8_literal("clip")}};
    if (!root[0].seen || !fields(s.doc, root[0].value, data) || !data[0].seen) return E::malformed;
    if (data[0].value.kind == JsonValueKind::null_value) return E::unavailable;
    Field clip[]{{utf8_literal("slug")}, {utf8_literal("playbackAccessToken")}, {utf8_literal("assets")}};
    if (!fields(s.doc, data[0].value, clip) || !equal(clip[0], id) || !clip[1].seen || !clip[2].seen) return E::malformed;
    if (!json_visit_indexed_array(s.doc, clip[2].value, 24, clip_asset, &s).ok()) return E::malformed;
    if (!s.best.url_bytes) return E::unsupported;
    Field access[]{{utf8_literal("signature")}, {utf8_literal("value")}};
    JsonString signature{}, token{};
    if (!fields(s.doc, clip[1].value, access) || !text(access[0], &signature) || !text(access[1], &token)) return E::malformed;
    char signature_text[128], token_text[1024];
    const auto sig = json_string_decode_utf8(signature, {signature_text, sizeof(signature_text)});
    const auto tok = json_string_decode_utf8(token, {token_text, sizeof(token_text)});
    if (!sig.ok() || !sig.bytes || !tok.ok()) return E::malformed;
    for (u32 i = 0; i < sig.bytes; ++i) if (!ascii_is_hex_digit(signature_text[i])) return E::malformed;
    const Utf8View token_json{token_text, tok.bytes};
    // Reuse the caller's structural index after all outer fields are consumed.
    const auto token_scan = json_scan_structurals_avx1(token_json, scratch);
    if (!token_scan.ok()) return token_scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{token_json, {scratch.data, token_scan.count}};
    Field claims[]{{utf8_literal("clip_slug")}, {utf8_literal("expires")}, {utf8_literal("authorization")}};
    Field authorization[]{{utf8_literal("forbidden")}};
    u64 expires = 0;
    bool forbidden = true;
    if (!fields(doc, {token_json, 0, JsonValueKind::object}, claims) || !equal(claims[0], id) ||
        !claims[1].seen || !json_value_to_u64(claims[1].value, &expires) || !claims[2].seen ||
        !fields(doc, claims[2].value, authorization) || !authorization[0].seen ||
        !json_value_to_bool(authorization[0].value, &forbidden)) return E::malformed;
    if (forbidden || expires <= now) return E::unavailable;
    ProtocolWriter w{{reinterpret_cast<u8*>(s.best.url), sizeof(s.best.url)}, s.best.url_bytes};
    const bool written = protocol_writer_append_literal(&w, "?sig=") &&
        protocol_writer_append_percent_encoded(&w, {signature_text, sig.bytes}) &&
        protocol_writer_append_literal(&w, "&token=") && protocol_writer_append_percent_encoded(&w, token_json);
    secure_clear_bytes(token_text, sizeof(token_text));
    secure_clear_bytes(signature_text, sizeof(signature_text));
    if (!written) { secure_clear_bytes(&s.best, sizeof(s.best)); return E::capacity; }
    s.best.url_bytes = w.used;
    *out = {s.best, {}, static_cast<u32>(minimum<u64>(expires - now, 86400)), true};
    secure_clear_bytes(&s.best, sizeof(s.best));
    return E::none;
}
u32 tiktok_player_target(Utf8View id, Slice<u8> output) noexcept {
    if (!id.data || !id.bytes || id.bytes > 20 || id.data[0] == '0') return 0;
    u64 value = 0;
    for (u32 i = 0; i < id.bytes; ++i) {
        const u32 d = static_cast<u8>(id.data[i]) - '0';
        if (d > 9 || value > (~u64{0} - d) / 10) return 0;
        value = value * 10 + d;
    }
    ProtocolWriter w{output, 0};
    return protocol_writer_append_literal(&w, "/player/api/v1/items?item_ids=") &&
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(id.data), id.bytes) &&
        protocol_writer_append_literal(&w, "&language=en&aid=1459&data_source=web_core") ? w.used : 0;
}
EmbedPlayerError tiktok_player_streams(Utf8View json, Utf8View id, u16 height, u64 now,
    Slice<u32> scratch, EmbedStreams* out) noexcept {
    using E = EmbedPlayerError;
    u8 target[128];
    if (!out || !height || height > 2048 || !now || !tiktok_player_target(id, {target, sizeof(target)}) ||
        !json.data || !json.bytes || json.bytes > 2u * 1024u * 1024u) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    TikTokSelection s{{json, {scratch.data, scan.count}}, id, now, height};
    Field root[]{{utf8_literal("status_code")}, {utf8_literal("items")}};
    u32 status = 0;
    if (!fields(s.doc, {json, 0, JsonValueKind::object}, root) || !number(root[0], &status)) return E::malformed;
    if (status) return E::unavailable;
    if (!root[1].seen || !json_visit_indexed_array(s.doc, root[1].value, 24, tiktok_item, &s).ok()) return E::malformed;
    if (!s.best.video.url_bytes) return E::unsupported;
    *out = s.best;
    secure_clear_bytes(&s.best, sizeof(s.best));
    return E::none;
}
bool embed_stream_location(CdnHost cdn, const Utf8View& url_view, EmbedStreamLocation* out) noexcept {
    const Utf8View url = url_view; // *out is written inside; keep the caller's view out of the aliasing set
    if (cdn == CdnHost::soundcloud_audio) return soundcloud_location(url, false, out);
    if (cdn == CdnHost::vimeo_video) return vimeo_location(url, false, out);
    if (cdn == CdnHost::twitch_live || cdn == CdnHost::twitch_playlist)
        return twitch_live_location(url, cdn == CdnHost::twitch_playlist, out) ||
            (cdn == CdnHost::twitch_live && twitch_live_location(url, false, out, true));
    if (cdn == CdnHost::steam_video) return steam_media_location(url, out);
    if (cdn == CdnHost::twitter_video) return twitter_location(url, out);
    if (cdn == CdnHost::twitch_vod) return twitch_vod_location(url, false, out);
    constexpr auto scheme = utf8_literal("https://");
    constexpr auto suffix = utf8_literal(".googlevideo.com");
    if (!url.data || url.bytes > kEmbedStreamUrlBytes || !utf8_starts_with(url, scheme)) return false;
    u32 end = scheme.bytes;
    for (; end < url.bytes && url.data[end] != '/'; ++end)
        if (!ascii_is_alnum(url.data[end]) && url.data[end] != '-' && url.data[end] != '.') return false;
    if (end - scheme.bytes > 253 || end == scheme.bytes) return false;
    const Utf8View host{url.data + scheme.bytes, end - scheme.bytes};
    const Utf8View target{url.data + end, url.bytes - end};
    if (cdn == CdnHost::pornhub_video) {
        constexpr auto tail = utf8_literal(".phncdn.com");
        if (host.bytes <= tail.bytes || !ascii_equal_ignore_case({host.data + host.bytes - tail.bytes, tail.bytes}, tail) ||
            !utf8_starts_with(target, utf8_literal("/videos/"))) return false;
        u32 path_end = 0;
        for (; path_end < target.bytes && target.data[path_end] != '?'; ++path_end) {
            const char c = target.data[path_end];
            if (!ascii_is_alnum(c) && c != '/' && c != '_' && c != '-' && c != '.') return false;
            if (c == '.' && path_end && target.data[path_end - 1] == '.') return false;
        }
        if (path_end < 4 || !utf8_equal({target.data + path_end - 4, 4}, utf8_literal(".mp4")) ||
            !stream_expiry(url, utf8_literal("validto="))) return false;
    } else if (cdn == CdnHost::youtube_playlist) {
        if (!ascii_equal_ignore_case(host,utf8_literal("manifest.googlevideo.com")) ||
            (!utf8_starts_with(target,utf8_literal("/api/manifest/hls_variant/")) &&
             !utf8_starts_with(target,utf8_literal("/api/manifest/hls_playlist/")))) return false;
    } else if (cdn == CdnHost::youtube_live) {
        if (host.bytes<=suffix.bytes || !ascii_equal_ignore_case({host.data+host.bytes-suffix.bytes,suffix.bytes},suffix) ||
            !utf8_starts_with(target,utf8_literal("/videoplayback/"))) return false;
    } else if (cdn == CdnHost::google_video) {
        if (host.bytes <= suffix.bytes || !ascii_equal_ignore_case({host.data + host.bytes - suffix.bytes, suffix.bytes}, suffix) ||
            !utf8_starts_with(target, utf8_literal("/videoplayback?"))) return false;
    } else if (cdn == CdnHost::tiktok_video) {
        constexpr auto tail = utf8_literal("-webapp-prime.tiktok.com");
        if (host.bytes < tail.bytes + 2 || host.bytes > tail.bytes + 5 || ascii_lower(host.data[0]) != 'v' ||
            !ascii_equal_ignore_case({host.data + host.bytes - tail.bytes, tail.bytes}, tail) ||
            !utf8_starts_with(target, utf8_literal("/video/tos/"))) return false;
        for (u32 i = 1; i < host.bytes - tail.bytes; ++i)
            if (!ascii_is_digit(host.data[i]) || (i == 1 && host.data[i] == '0')) return false;
    } else if (cdn == CdnHost::twitch_video) {
        // A specific Twitch distribution; accepting arbitrary CloudFront
        // tenants would let metadata redirect media fetches to unrelated sites.
        if (!ascii_equal_ignore_case(host, utf8_literal("d1ndex63qxojbr.cloudfront.net")) ||
            !utf8_starts_with(target, utf8_literal("/nauth/"))) return false;
        u32 path_end = 0;
        while (path_end < target.bytes && target.data[path_end] != '?') ++path_end;
        if (path_end < 4 || !utf8_equal({target.data + path_end - 4, 4}, utf8_literal(".mp4"))) return false;
    } else if (cdn == CdnHost::imgur_video) {
        if(!ascii_equal_ignore_case(host,utf8_literal("i.imgur.com")) || target.bytes<6 || target.data[0]!='/' ||
            !utf8_equal({target.data+target.bytes-4,4},utf8_literal(".mp4")) ||
            !imgur_video_id({target.data+1,target.bytes-5})) return false;
    } else if (cdn == CdnHost::streamable_video) {
        static constexpr Utf8View hosts[]{utf8_literal("cdn-cf-east.streamable.com"), utf8_literal("cdn-cf-west.streamable.com"),
            utf8_literal("cdn-b-east.streamable.com"), utf8_literal("cdn-b-west.streamable.com")};
        bool allowed = false;
        for (auto name : hosts) allowed = allowed || ascii_equal_ignore_case(host, name);
        const u32 prefix_bytes = utf8_starts_with(target, utf8_literal("/video/mp4/")) ? 11
            : utf8_starts_with(target, utf8_literal("/video/mp4-mobile/")) ? 18 : 0;
        u32 path_end = 0;
        while (path_end < target.bytes && target.data[path_end] != '?') ++path_end;
        if (!allowed || !prefix_bytes || path_end <= prefix_bytes + 4 ||
            !utf8_equal({target.data + path_end - 4, 4}, utf8_literal(".mp4")) ||
            !streamable_video_id({target.data + prefix_bytes, path_end - prefix_bytes - 4})) return false;
    } else if (cdn == CdnHost::apple_music_preview || cdn == CdnHost::apple_music_video_preview) {
        const bool video = cdn == CdnHost::apple_music_video_preview;
        const auto prefix = video ? utf8_literal("/itunes-assets/Video") : utf8_literal("/itunes-assets/AudioPreview");
        if (!ascii_equal_ignore_case(host, video ? utf8_literal("video-ssl.itunes.apple.com") : utf8_literal("audio-ssl.itunes.apple.com")) ||
            !utf8_starts_with(target, prefix) || target.bytes < prefix.bytes + 8 ||
            !utf8_equal({target.data + target.bytes - 4, 4}, video ? utf8_literal(".m4v") : utf8_literal(".m4a"))) return false;
        u32 part = prefix.bytes;
        while (part < target.bytes && ascii_is_digit(target.data[part])) ++part;
        if (part == prefix.bytes || part >= target.bytes || target.data[part] != '/') return false;
        for (u32 i = part; i < target.bytes; ++i) {
            const char c = target.data[i];
            if (!ascii_is_alnum(c) && c != '/' && c != '.' && c != '-' && c != '_') return false;
            if (c == '.' && i + 1 < target.bytes && target.data[i + 1] == '.') return false;
        }
    } else if (cdn == CdnHost::amazon_music_preview) {
        if((!ascii_equal_ignore_case(host,utf8_literal("d28julafmv4ekl.cloudfront.net")) &&
            !ascii_equal_ignore_case(host,utf8_literal("d2q1srilgjznst.cloudfront.net"))) || !target.bytes || target.data[0]!='/') return false;
        u32 at=1;
        for(u32 part=0;part<3;++part) {
            const u32 start=at;
            while(at<target.bytes && ascii_is_digit(target.data[at])) ++at;
            if(at==start || at-start>(part==2?16u:3u)) return false;
            if(part<2) {
                if(at+3>target.bytes || !ascii_equal_ignore_case({target.data+at,3},utf8_literal("%2F"))) return false;
                at+=3;
            }
        }
        if(!utf8_starts_with({target.data+at,target.bytes-at},utf8_literal("_S64.mp3?"))) return false;
        if(!stream_expiry(url,utf8_literal("Expires="))) return false;
    } else if (cdn == CdnHost::spotify_preview && ascii_equal_ignore_case(host, utf8_literal("podz-content.spotifycdn.com"))) {
        constexpr auto path = utf8_literal("/audio/clips/");
        if (!utf8_starts_with(target,path) || target.bytes < path.bytes+22+1 ||
            !spotify_audio_id({target.data+path.bytes,22}) || target.data[path.bytes+22]!='/') return false;
        u32 at=path.bytes+23;
        if (!utf8_starts_with({target.data+at,target.bytes-at},utf8_literal("clip_"))) return false;
        at+=5;
        for(u32 part=0;part<2;++part) {
            const u32 first=at;
            while(at<target.bytes && ascii_is_digit(target.data[at])) ++at;
            if(at==first || at-first>10) return false;
            if(!part && (at==target.bytes || target.data[at++]!='_')) return false;
        }
        if(!utf8_equal({target.data+at,target.bytes-at},utf8_literal(".mp3"))) return false;
    } else if (cdn == CdnHost::spotify_preview) {
        constexpr auto path = utf8_literal("/mp3-preview/");
        if (!ascii_equal_ignore_case(host, utf8_literal("p.scdn.co")) || !utf8_starts_with(target, path)) return false;
        u32 path_end = path.bytes;
        while (path_end < target.bytes && target.data[path_end] != '?') {
            if (!ascii_is_hex_digit(target.data[path_end])) return false;
            ++path_end;
        }
        if (path_end - path.bytes != 40) return false;
    } else return false;
    for (u32 i = end; i < url.bytes; ++i) {
        const u8 c = static_cast<u8>(url.data[i]);
        if (c <= 32 || c >= 127 || c == '\\' || c == '#' || c == '"' || c == '<' || c == '>') return false;
        if (c == '%') {
            if (i + 2 >= url.bytes || !ascii_is_hex_digit(url.data[i + 1]) || !ascii_is_hex_digit(url.data[i + 2]))
                return false;
            i += 2;
        }
    }
    if (out) *out = {host, target};
    return true;
}
bool youtube_stream_url(Utf8View url) noexcept { return embed_stream_location(CdnHost::google_video, url); }
u32 youtube_stream_get(Utf8View url, u64 first, u32 bytes, Slice<u8> output) noexcept {
    return embed_stream_get(CdnHost::google_video, url, first, bytes, output);
}
u32 embed_stream_get(CdnHost cdn, Utf8View url, u64 first, u32 bytes, Slice<u8> output) noexcept {
    EmbedStreamLocation location{};
    const bool whole_preview = (embed_audio_preview_limit(cdn) || embed_playlist_host(cdn)) && !bytes && !first;
    if (embed_playlist_host(cdn) && !whole_preview) return 0;
    if (!embed_stream_location(cdn, url, &location) || (!bytes && !whole_preview) || bytes > kMediaMaximumProgressiveVideoBytes ||
        first > kMediaMaximumProgressiveVideoBytes - u64{bytes} || !output.data) return 0;
    u8 staged[kCdnMaximumRequestHeadBytes];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    const auto add = [&](Utf8View text) noexcept {
        return protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(text.data), text.bytes);
    };
    const bool ok = add(utf8_literal("GET ")) && add(location.target) &&
        add(utf8_literal(" HTTP/1.1\r\nHost: ")) && add(location.host) &&
        add(utf8_literal("\r\nUser-Agent: ")) && add(cdn == CdnHost::google_video ? kYoutubeNativeUserAgent : utf8_literal("Mozilla/5.0")) &&
        (cdn != CdnHost::tiktok_video || add(utf8_literal("\r\nReferer: https://www.tiktok.com/"))) &&
        (cdn != CdnHost::pornhub_video || add(utf8_literal("\r\nReferer: https://www.pornhub.com/"))) &&
        add(utf8_literal("\r\nAccept: */*\r\nAccept-Encoding: identity")) &&
        (whole_preview || (add(utf8_literal("\r\nRange: bytes=")) &&
        protocol_writer_append_u64(&w, first) && add(utf8_literal("-")) &&
          protocol_writer_append_u64(&w, first + bytes - 1))) &&
        add(utf8_literal("\r\nConnection: keep-alive\r\n\r\n")) && w.used <= output.count;
    if (ok) copy_bytes(output.data, staged, w.used);
    secure_clear_bytes(staged, sizeof(staged));
    return ok ? w.used : 0;
}
EmbedPlayerError youtube_player_streams(Utf8View json, Utf8View id, u16 maximum_height,
    Slice<u32> scratch, EmbedStreams* output) noexcept {
    using E = EmbedPlayerError;
    if (!output || !video_id(id) || !maximum_height || maximum_height > 2048 ||
        !json.data || !json.bytes || json.bytes > 2u * 1024u * 1024u) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    Selection s{{json, {scratch.data, scan.count}}, {}, {}, {}, maximum_height, 0};
    Field root[]{{utf8_literal("playabilityStatus")}, {utf8_literal("videoDetails")},
                 {utf8_literal("streamingData")}};
    if (!fields(s.doc, {json, 0, JsonValueKind::object}, root) || !root[0].seen) return E::malformed;
    Field status[]{{utf8_literal("status")}, {utf8_literal("playableInEmbed")}};
    if (!fields(s.doc, root[0].value, status)) return E::malformed;
    if (equal(status[0], utf8_literal("LOGIN_REQUIRED"))) return E::login_required;
    if (!equal(status[0], utf8_literal("OK"))) return status[0].seen ? E::unavailable : E::malformed;
    bool embeddable = false;
    if (!status[1].seen || !json_value_to_bool(status[1].value, &embeddable)) return E::malformed;
    if (!embeddable) return E::not_embeddable;
    Field details[]{{utf8_literal("videoId")},{utf8_literal("isLive")}};
    if (!root[1].seen || !fields(s.doc, root[1].value, details) || !equal(details[0], id)) return E::malformed;
    if (!root[2].seen) return E::unsupported;
    Field data[]{{utf8_literal("formats")}, {utf8_literal("adaptiveFormats")}, {utf8_literal("expiresInSeconds")},
        {utf8_literal("hlsManifestUrl")}};
    if (!fields(s.doc, root[2].value, data)) return E::malformed;
    u32 expires = 0;
    if (!number(data[2], &expires) || !expires || expires > 86400) return E::malformed;
    bool live=false;
    if(details[1].seen && !json_value_to_bool(details[1].value,&live)) return E::malformed;
    if(live) {
        JsonString manifest{};EmbedStreams staged{};EmbedStreamLocation location{};
        if(!text(data[3],&manifest)) return E::unsupported;
        const auto decoded=json_string_decode_utf8(manifest,{staged.video.url,sizeof(staged.video.url)});
        if(!decoded.ok() || !embed_stream_location(CdnHost::youtube_playlist,{staged.video.url,decoded.bytes},&location) ||
            !utf8_starts_with(location.target,utf8_literal("/api/manifest/hls_variant/"))) return E::malformed;
        staged.video.url_bytes=decoded.bytes;staged.expires_seconds=expires;staged.live=true;*output=staged;return E::none;
    }
    for (u32 i = 0; i < 2; ++i)
        if (data[i].seen && !json_visit_indexed_array(s.doc, data[i].value, 24, format, &s).ok())
            return E::malformed;
    if (!s.video.combined && !s.audio.url.encoded.bytes) s.video = s.combined;
    if (!s.video.url.encoded.bytes) return E::unsupported;
    EmbedStreams staged{};
    if (!store(s.video, &staged.video) || (!s.video.combined && !store(s.audio, &staged.audio))) return E::malformed;
    staged.expires_seconds = expires;
    staged.combined = s.video.combined;
    *output = staged;
    return E::none;
}
bool amazon_music_track_id(Utf8View id) noexcept {
    if(!id.data || id.bytes!=10 || id.data[0]!='B') return false;
    for(u32 i=1;i<id.bytes;++i) if(!ascii_is_digit(id.data[i]) && (id.data[i]<'A' || id.data[i]>'Z')) return false;
    return true;
}
bool amazon_music_redirect(Utf8View url,Utf8View id,EmbedStreamLocation* out) noexcept {
    constexpr auto origin=utf8_literal("https://d2usgkmxlnua9y.cloudfront.net");
    if(!url.data || !amazon_music_track_id(id) || url.bytes!=origin.bytes+14 || !utf8_starts_with(url,origin)) return false;
    const auto target=Utf8View{url.data+origin.bytes,url.bytes-origin.bytes};
    if(target.data[0]!='/' || target.data[3]!='/' || target.data[1]<'A' || target.data[1]>'Z' ||
        target.data[2]<'A' || target.data[2]>'Z' || !utf8_equal({target.data+4,10},id)) return false;
    if(out) *out={{origin.data+8,origin.bytes-8},target};
    return true;
}
EmbedPlayerError amazon_music_preview_stream(Utf8View url,u64 now,EmbedStreams* out) noexcept {
    using E=EmbedPlayerError;
    if(!out || !now) return E::invalid_input;
    if(!embed_stream_location(CdnHost::amazon_music_preview,url)) return E::malformed;
    const auto expires=stream_expiry(url,utf8_literal("Expires="));
    if(expires<=now) return E::unavailable;
    EmbedStreams staged{};
    copy_bytes(staged.audio.url,url.data,url.bytes);staged.audio.url_bytes=url.bytes;
    staged.expires_seconds=static_cast<u32>(minimum<u64>(expires-now,3600));
    *out=staged;secure_clear_bytes(&staged,sizeof(staged));return E::none;
}
u64 twitter_post_id(Utf8View id) noexcept {
    if(!id.data || !id.bytes || id.bytes>20 || id.data[0]=='0') return 0;
    u64 value=0;
    for(u32 i=0;i<id.bytes;++i) {
        const u32 digit=static_cast<u8>(id.data[i])-'0';
        if(digit>9 || value>(~u64{0}-digit)/10) return 0;
        value=value*10+digit;
    }
    return value;
}
#pragma float_control(precise, on, push)
u32 twitter_video_target(Utf8View id,Slice<u8> output) noexcept {
    const u64 post=twitter_post_id(id);
    if(!post || !output.data) return 0;
    // Public embed request proof: base36(Number(id)/1e15*pi), without dots or
    // zero digits. Binary64 rounding is part of that wire format. Only positive
    // u64 IDs are needed: no general formatter, runtime or allocations.
    const double value=(static_cast<double>(post)/1e15)*3.141592653589793;
    u32 whole=static_cast<u32>(value);
    double rest=value-whole;
    double tolerance=__builtin_bit_cast(double,(__builtin_bit_cast(u64,value)+1))-value;
    tolerance*=0.5;
    u8 digits[32]{};u32 count=0;
    while(rest>=tolerance) {
        if(count==sizeof(digits)) return 0;
        rest*=36;tolerance*=36;
        const auto digit=static_cast<u8>(rest);
        digits[count++]=digit;rest-=digit;
        if((rest>0.5 || (rest==0.5 && (digit&1))) && rest+tolerance>1) {
            while(count && digits[count-1]==35) --count;
            if(count) ++digits[count-1]; else ++whole;
            break;
        }
    }
    u8 target[128];ProtocolWriter w{{target,sizeof(target)},0};
    protocol_writer_append_literal(&w,"/tweet-result?id=");
    protocol_writer_append_bytes(&w,reinterpret_cast<const u8*>(id.data),id.bytes);
    protocol_writer_append_literal(&w,"&lang=en&token=");
    const auto append=[&](u8 digit) noexcept {
        const u8 byte=static_cast<u8>(digit<10?'0'+digit:'a'+digit-10);
        if(digit) protocol_writer_append_bytes(&w,&byte,1);
    };
    u8 integer[4];u32 n=0;
    while(whole) {integer[n++]=static_cast<u8>(whole%36);whole/=36;}
    while(n) append(integer[--n]);
    for(u32 i=0;i<count;++i) append(digits[i]);
    if(w.used>output.count) return 0;
    copy_bytes(output.data,target,w.used);
    return w.used;
}
#pragma float_control(pop)
EmbedPlayerError twitter_video_streams(Utf8View json,Utf8View id,u16 height,u8 media_index,
    Slice<u32> scratch,EmbedStreams* output) noexcept {
    using E=EmbedPlayerError;
    if(!output || !twitter_post_id(id) || !height || height>2048 || media_index>4 ||
        !json.data || !json.bytes || json.bytes>kTwitterMetadataMaximumBytes) return E::invalid_input;
    const auto scan=json_scan_structurals_avx1(json,scratch);
    if(!scan.ok()) return scan.error==JsonScanError::output_overflow?E::capacity:E::malformed;
    const JsonIndexedDocument doc{json,{scratch.data,scan.count}};
    Field root[]{{utf8_literal("__typename")},{utf8_literal("id_str")},{utf8_literal("mediaDetails")}};
    if(!fields(doc,{json,0,JsonValueKind::object},root)) return E::malformed;
    if(equal(root[0],utf8_literal("TweetTombstone"))) return E::unavailable;
    if(!equal(root[0],utf8_literal("Tweet")) || !equal(root[1],id)) return E::malformed;
    if(!root[2].seen) return E::unsupported;
    TwitterMedia media{doc,{},media_index};
    if(!json_visit_indexed_array(doc,root[2].value,24,twitter_media,&media).ok()) return E::malformed;
    if(!media.found) return E::unsupported;
    Field selected[]{{utf8_literal("video_info")},{utf8_literal("ext_media_availability")},
        {utf8_literal("additional_media_info")}};
    if(!fields(doc,media.selected,selected)) return E::malformed;
    if(selected[1].seen) {
        Field availability[]{{utf8_literal("status")}};
        if(!fields(doc,selected[1].value,availability)) return E::malformed;
        if(!equal(availability[0],utf8_literal("Available"))) return E::unavailable;
    }
    if(selected[2].seen) {
        Field info[]{{utf8_literal("embeddable")}};bool allowed=true;
        if(!fields(doc,selected[2].value,info) ||
            (info[0].seen && !json_value_to_bool(info[0].value,&allowed))) return E::malformed;
        if(!allowed) return E::not_embeddable;
    }
    Field video[]{{utf8_literal("variants")}};
    if(!selected[0].seen || !fields(doc,selected[0].value,video) || !video[0].seen) return E::malformed;
    TwitterVariants variants{doc,{},height};
    E error=E::unsupported;
    if(!json_visit_indexed_array(doc,video[0].value,24,twitter_variant,&variants).ok()) error=E::malformed;
    else if(variants.selected.url_bytes) {
        reset_value(output);output->video=variants.selected;output->combined=true;output->expires_seconds=3600;
        error=E::none;
    }
    secure_clear_bytes(&variants.selected,sizeof(variants.selected));
    return error;
}
bool pornhub_video_id(Utf8View id) noexcept {
    if (!id.data) return false;
    if (utf8_starts_with(id, utf8_literal("ph"))) id = {id.data + 2, id.bytes - 2};
    if (id.bytes < 13 || id.bytes > 32) return false;
    for (u32 i = 0; i < id.bytes; ++i) if (!ascii_is_hex_digit(id.data[i])) return false;
    return true;
}
u32 pornhub_embed_target(Utf8View id, Slice<u8> output) noexcept {
    constexpr auto prefix = utf8_literal("/embed/");
    if (!pornhub_video_id(id) || !output.data || output.count < prefix.bytes + id.bytes) return 0;
    copy_bytes(output.data, prefix.data, prefix.bytes);
    copy_bytes(output.data + prefix.bytes, id.data, id.bytes);
    return prefix.bytes + id.bytes;
}
bool pornhub_metadata_location(Utf8View url, EmbedStreamLocation* output) noexcept {
    constexpr auto origin = utf8_literal("https://www.pornhub.com");
    constexpr auto prefix = utf8_literal("https://www.pornhub.com/video/get_media?");
    if (!url.data || url.bytes <= prefix.bytes || url.bytes > kEmbedStreamUrlBytes || !utf8_starts_with(url, prefix)) return false;
    for (u32 i = prefix.bytes; i < url.bytes; ++i) {
        const char c = url.data[i];
        if (!ascii_is_alnum(c) && c != '&' && c != '=' && c != '_' && c != '-') return false;
    }
    if (output) *output = {utf8_literal("www.pornhub.com"), {url.data + origin.bytes, url.bytes - origin.bytes}};
    return true;
}
bool vimeo_location(Utf8View url, bool playlist, EmbedStreamLocation* out) noexcept {
    constexpr auto scheme = utf8_literal("https://");
    if (!url.data || url.bytes > kEmbedStreamUrlBytes || !utf8_starts_with(url, scheme)) return false;
    u32 end = scheme.bytes;
    while (end < url.bytes && url.data[end] != '/') ++end;
    const Utf8View host{url.data + scheme.bytes, end - scheme.bytes};
    if (end == url.bytes || (!utf8_equal(host, utf8_literal("vod-adaptive-ak.vimeocdn.com")) &&
        !utf8_equal(host, utf8_literal("vod-adaptive.vimeocdn.com")))) return false;
    const u32 path = end;
    while (end < url.bytes && url.data[end] != '?') ++end;
    u32 token_end = path + 1;
    while (token_end < end && url.data[token_end] != '/') ++token_end;
    const bool signed_prefix = utf8_starts_with({url.data + path, end - path}, utf8_literal("/exp="));
    bool kind = false, dots_only = true;
    u32 dots = 0;
    const auto expected = playlist ? utf8_literal("/v2/playlist/") : utf8_literal("/v2/range/");
    for (u32 i = path; i < url.bytes; ++i) {
        u8 c = static_cast<u8>(url.data[i]);
        if (c <= 32 || c >= 127 || c == '\\' || c == '#' || c == '"' || c == '<' || c == '>') return false;
        if (i >= end) continue;
        if (i + expected.bytes <= end && bytes_equal(url.data + i, expected.data, expected.bytes)) kind = true;
        if (c == '%') {
            if (!signed_prefix || i + 2 >= token_end || url.data[i + 1] < '2' || url.data[i + 1] > '3') return false;
            const char last = ascii_lower(url.data[i + 2]);
            if (url.data[i + 1] == '2' && (last == 'a' || last == 'f')) c = last == 'a' ? '*' : '/';
            else if (url.data[i + 1] == '3' && last == 'd') c = '=';
            else return false;
            i += 2;
        }
        if (c == '/') {
            if (dots_only && (dots == 1 || dots == 2)) return false;
            dots = 0; dots_only = true;
        } else if (c == '.') ++dots;
        else dots_only = false;
    }
    if (dots_only && (dots == 1 || dots == 2)) return false;
    if (!kind || (playlist && (end < 5 || !bytes_equal(url.data + end - 5, ".m3u8", 5)))) return false;
    if (out) *out = {host, {url.data + path, url.bytes - path}};
    return true;
}
EmbedPlayerError vimeo_video_manifest(Utf8View html, Utf8View id, Slice<u32> scratch, EmbedStream* output, Utf8View privacy_hash) noexcept {
    using E = EmbedPlayerError;
    if (!output || !steam_id(id) || !html.data || !html.bytes || html.bytes > 128 * 1024 ||
        (privacy_hash.bytes && !vimeo_privacy_hash(privacy_hash))) return E::invalid_input;
    Utf8View json{};
    const auto extracted = embedded_json(html, utf8_literal("window.playerConfig"), &json);
    if (extracted != E::none) return extracted;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Field root[]{{utf8_literal("video")}, {utf8_literal("request")}};
    Field video[]{{utf8_literal("id")}, {utf8_literal("privacy")}, {utf8_literal("embed_permission")}, {utf8_literal("unlisted_hash")}};
    u32 actual{};
    if (!fields(doc, {json, 0, JsonValueKind::object}, root) || !root[0].seen || !root[1].seen ||
        !fields(doc, root[0].value, video) || !number(video[0], &actual) || actual != steam_id(id)) return E::malformed;
    const bool unlisted = privacy_hash.bytes && equal(video[1], utf8_literal("unlisted")) && equal(video[3], privacy_hash);
    if ((!equal(video[1], utf8_literal("anybody")) && !unlisted) || !equal(video[2], utf8_literal("public"))) return E::not_embeddable;
    Field request[]{{utf8_literal("files")}}, files[]{{utf8_literal("hls")}},
          hls[]{{utf8_literal("default_cdn")}, {utf8_literal("cdns")}};
    if (!fields(doc, root[1].value, request) || !request[0].seen ||
        !fields(doc, request[0].value, files) || !files[0].seen ||
        !fields(doc, files[0].value, hls) || !hls[0].seen || !hls[1].seen) return E::unsupported;
    char name[64]; JsonString selected{};
    if (!text(hls[0], &selected)) return E::malformed;
    const auto decoded = json_string_decode_utf8(selected, {name, sizeof(name)});
    if (!decoded.ok() || !decoded.bytes) return E::malformed;
    Field cdn[]{{{name, decoded.bytes}}}, record[]{{utf8_literal("url")}};
    if (!fields(doc, hls[1].value, cdn) || !cdn[0].seen || !fields(doc, cdn[0].value, record) ||
        !text(record[0], &selected)) return E::malformed;
    EmbedStream staged{};
    const auto url = json_string_decode_utf8(selected, {staged.url, sizeof(staged.url)});
    if (!url.ok() || !vimeo_location({staged.url, url.bytes}, true)) return E::unsupported;
    staged.url_bytes = url.bytes;
    *output = staged;
    return E::none;
}
EmbedPlayerError pornhub_embed_metadata(Utf8View html, Utf8View id, Slice<u32> scratch, EmbedStream* output) noexcept {
    using E = EmbedPlayerError;
    if (!output || !pornhub_video_id(id) || !html.data || !html.bytes || html.bytes > kPornhubMetadataMaximumBytes) return E::invalid_input;
    Utf8View json{};
    const auto extracted = embedded_json(html, utf8_literal("flashvars"), &json);
    if (extracted != E::none) return extracted;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Field root[]{{utf8_literal("link_url")}, {utf8_literal("video_unavailable")},
        {utf8_literal("video_unavailable_country")}, {utf8_literal("mediaDefinitions")}};
    if (!fields(doc, {json, 0, JsonValueKind::object}, root) || !root[1].seen || !root[3].seen) return E::malformed;
    for (u32 i = 1; i <= 2; ++i) {
        if (!root[i].seen) continue;
        bool blocked = true;
        if (equal(root[i], utf8_literal("false"))) blocked = false;
        else if (!json_value_to_bool(root[i].value, &blocked)) return E::malformed;
        if (blocked) return E::unavailable;
    }
    JsonString link{}; char identity[512];
    if (!text(root[0], &link)) return E::malformed;
    const auto decoded = json_string_decode_utf8(link, {identity, sizeof(identity)});
    const auto locator = decoded.ok() ? embed_video_locator({identity, decoded.bytes}) : EmbedVideoLocator{};
    if (locator.provider != EmbedProvider::pornhub || !utf8_equal(locator.id, id)) return E::malformed;
    struct Metadata { JsonIndexedDocument doc; EmbedStream target{}; u32 count{}; } selected{doc};
    const auto visited = json_visit_indexed_array(doc, root[3].value, 24,
        +[](void* context, u32 index, JsonValue value) noexcept {
            auto& s = *static_cast<Metadata*>(context);
            if (index >= 32) return false;
            Field f[]{{utf8_literal("format")}, {utf8_literal("remote")}, {utf8_literal("videoUrl")}};
            if (!fields(s.doc, value, f)) return false;
            if (!equal(f[0], utf8_literal("mp4"))) return true;
            bool remote = false; JsonString url{};
            if (!f[1].seen || !json_value_to_bool(f[1].value, &remote) || !remote || !text(f[2], &url) || ++s.count != 1) return false;
            const auto result = json_string_decode_utf8(url, {s.target.url, sizeof(s.target.url)});
            if (!result.ok() || !pornhub_metadata_location({s.target.url, result.bytes})) return false;
            s.target.url_bytes = result.bytes; return true;
        }, &selected);
    const auto error = !visited.ok() ? E::malformed : !selected.target.url_bytes ? E::unsupported : E::none;
    if (error == E::none) *output = selected.target;
    secure_clear_bytes(&selected, sizeof(selected));
    return error;
}
EmbedPlayerError pornhub_video_streams(Utf8View json, u16 height, u64 now, Slice<u32> scratch, EmbedStreams* output) noexcept {
    using E = EmbedPlayerError;
    if (!output || !json.data || !json.bytes || json.bytes > kPornhubMetadataMaximumBytes || !height || height > 2048 || !now) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    struct Selection { JsonIndexedDocument doc; EmbedStreams streams{}; u64 now; u16 height; } selected{doc, {}, now, height};
    const auto visited = json_visit_indexed_array(doc, {json, 0, JsonValueKind::array}, 24,
        +[](void* context, u32 index, JsonValue value) noexcept {
            auto& s = *static_cast<Selection*>(context);
            if (index >= 32) return false;
            Field f[]{{utf8_literal("format")}, {utf8_literal("width")}, {utf8_literal("height")}, {utf8_literal("videoUrl")}};
            if (!fields(s.doc, value, f)) return false;
            if (!equal(f[0], utf8_literal("mp4"))) return true;
            u32 width = 0, height = 0; JsonString url{};
            if (!number(f[1], &width) || !number(f[2], &height) || !width || !height || !text(f[3], &url)) return false;
            if (width > 2048 || height > 2048) return true;
            EmbedStream candidate{};
            const auto decoded = json_string_decode_utf8(url, {candidate.url, sizeof(candidate.url)});
            const Utf8View source{candidate.url, decoded.bytes};
            const bool valid = decoded.ok() && embed_stream_location(CdnHost::pornhub_video, source);
            const u64 expiry = valid ? stream_expiry(source, utf8_literal("validto=")) : 0;
            if (valid && expiry > s.now && better_rendition(height, 0, s.streams.video.height, 0, s.height)) {
                candidate.url_bytes = decoded.bytes; candidate.width = static_cast<u16>(width); candidate.height = static_cast<u16>(height);
                s.streams.video = candidate; s.streams.combined = true;
                s.streams.expires_seconds = static_cast<u32>(minimum<u64>(expiry - s.now, 86400));
            }
            secure_clear_bytes(&candidate, sizeof(candidate));
            return valid;
        }, &selected);
    const auto error = !visited.ok() ? E::malformed : !selected.streams.video.url_bytes ? E::unavailable : E::none;
    if (error == E::none) *output = selected.streams;
    secure_clear_bytes(&selected, sizeof(selected));
    return error;
}
bool imgur_video_id(Utf8View id) noexcept {
    if(!id.data || (id.bytes!=5 && id.bytes!=7)) return false;
    for(u32 i=0;i<id.bytes;++i) if(!ascii_is_alnum(id.data[i])) return false;
    return true;
}
EmbedPlayerError imgur_video_streams(Utf8View id,EmbedStreams* output) noexcept {
    if(!output || !imgur_video_id(id)) return EmbedPlayerError::invalid_input;
    constexpr auto prefix=utf8_literal("https://i.imgur.com/");
    EmbedStreams value{};
    copy_bytes(value.video.url,prefix.data,prefix.bytes);
    copy_bytes(value.video.url+prefix.bytes,id.data,id.bytes);
    copy_bytes(value.video.url+prefix.bytes+id.bytes,".mp4",4);
    value.video.url_bytes=prefix.bytes+id.bytes+4;
    value.combined=true;value.expires_seconds=3600;
    *output=value;return EmbedPlayerError::none;
}
bool streamable_video_id(Utf8View id) noexcept {
    if (!id.data || id.bytes < 3 || id.bytes > 12) return false;
    for (u32 i = 0; i < id.bytes; ++i) if (!ascii_is_alnum(id.data[i])) return false;
    return true;
}
u32 streamable_video_target(Utf8View id, Slice<u8> output) noexcept {
    constexpr auto prefix = utf8_literal("/videos/");
    if (!streamable_video_id(id) || !output.data || output.count < prefix.bytes + id.bytes) return 0;
    copy_bytes(output.data, prefix.data, prefix.bytes);
    copy_bytes(output.data + prefix.bytes, id.data, id.bytes);
    return prefix.bytes + id.bytes;
}
EmbedPlayerError streamable_video_streams(Utf8View json, Utf8View id, u16 height, u64 now,
    Slice<u32> scratch, EmbedStreams* output) noexcept {
    using E = EmbedPlayerError;
    if (!output || !streamable_video_id(id) || !height || height > 2048 || !now ||
        !json.data || !json.bytes || json.bytes > kStreamableMetadataMaximumBytes) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Field root[]{{utf8_literal("status")}, {utf8_literal("url")}, {utf8_literal("files")}};
    u32 status = 0;
    if (!fields(doc, {json, 0, JsonValueKind::object}, root) || !number(root[0], &status)) return E::malformed;
    if (status != 2) return E::unavailable;
    char canonical[40]; constexpr auto host = utf8_literal("https://streamable.com/");
    copy_bytes(canonical, host.data, host.bytes); copy_bytes(canonical + host.bytes, id.data, id.bytes);
    if ((!equal(root[1], {canonical, host.bytes + id.bytes}) &&
         !equal(root[1], {canonical + 8, host.bytes + id.bytes - 8})) || !root[2].seen) return E::malformed;
    Field files[]{{utf8_literal("mp4-mobile")}, {utf8_literal("mp4")}};
    if (!fields(doc, root[2].value, files)) return E::malformed;
    EmbedStreams staged{};
    E error = E::unsupported;
    for (auto file : files) {
        if (!file.seen) continue;
        Field f[]{{utf8_literal("status")}, {utf8_literal("url")}, {utf8_literal("height")},
            {utf8_literal("width")}, {utf8_literal("size")}, {utf8_literal("bitrate")}};
        u32 width = 0, pixels = 0, bytes = 0, bitrate = 0;
        JsonString source{};
        if (!fields(doc, file.value, f) || !number(f[0], &status)) { error = E::malformed; break; }
        if (status != 2) continue;
        if (!text(f[1], &source) || !number(f[2], &pixels) || !number(f[3], &width) ||
            !number(f[4], &bytes) || !number(f[5], &bitrate) || !pixels || !width || !bytes || !bitrate) {
            error = E::malformed; break;
        }
        if (pixels > 2048 || width > 2048 || bytes > kMediaMaximumProgressiveVideoBytes) continue;
        EmbedStream candidate{};
        const auto decoded = json_string_decode_utf8(source, {candidate.url + 6, sizeof(candidate.url) - 6});
        if (!decoded.ok()) { secure_clear_bytes(&candidate, sizeof(candidate)); error = E::malformed; break; }
        if (utf8_starts_with({candidate.url + 6, decoded.bytes}, utf8_literal("//"))) {
            copy_bytes(candidate.url, "https:", 6); candidate.url_bytes = decoded.bytes + 6;
        } else {
            for (u32 i = 0; i < decoded.bytes; ++i) candidate.url[i] = candidate.url[i + 6];
            candidate.url_bytes = decoded.bytes;
        }
        const Utf8View url{candidate.url, candidate.url_bytes};
        EmbedStreamLocation location{};
        const bool located = embed_stream_location(CdnHost::streamable_video, url, &location);
        const u32 prefix_bytes = located && utf8_starts_with(location.target, utf8_literal("/video/mp4/")) ? 11 : 18;
        const bool same_id = located && location.target.bytes > prefix_bytes + id.bytes &&
            bytes_equal(location.target.data + prefix_bytes, id.data, id.bytes) && location.target.data[prefix_bytes + id.bytes] == '.';
        bool modern_seen = false, legacy_seen = false;
        const u64 expires = stream_expiry(url, utf8_literal("Expires="), &modern_seen);
        const u64 legacy = stream_expiry(url, utf8_literal("expires="), &legacy_seen);
        if (!same_id || modern_seen == legacy_seen || !maximum(expires, legacy)) error = E::malformed;
        else if (maximum(expires, legacy) > now) {
            if (better_rendition(pixels, bitrate, staged.video.height, staged.video.bitrate, height)) {
                candidate.width = static_cast<u16>(width); candidate.height = static_cast<u16>(pixels);
                candidate.content_bytes = bytes; candidate.bitrate = bitrate;
                staged.video = candidate;
                staged.expires_seconds = static_cast<u32>(minimum<u64>(maximum(expires, legacy) - now, 86400));
                staged.combined = true;
            }
        }
        secure_clear_bytes(&candidate, sizeof(candidate));
        if (error == E::malformed) break;
    }
    if (error != E::malformed && staged.video.url_bytes) { *output = staged; error = E::none; }
    secure_clear_bytes(&staged, sizeof(staged));
    return error;
}
bool apple_music_track_id(const Utf8View& id) noexcept {
    if (!id.data || !id.bytes || id.bytes > 20 || id.data[0] == '0') return false;
    u64 value = 0;
    for (u32 i = 0; i < id.bytes; ++i) {
        const u32 digit = static_cast<u8>(id.data[i]) - '0';
        if (digit > 9 || value > (~u64{0} - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return value != 0;
}
u32 apple_music_track_target(const Utf8View& id_view, const Utf8View& country_view, const Slice<u8>& output_view) noexcept {
    const Utf8View id = id_view, country = country_view; // the output stores below must not force reloads
    const Slice<u8> output = output_view;
    constexpr auto prefix = utf8_literal("/lookup?id="), suffix = utf8_literal("&country=");
    const u32 bytes = prefix.bytes + id.bytes + suffix.bytes + 2;
    if (!apple_music_track_id(id) || !country.data || country.bytes != 2 || !output.data || output.count < bytes) return 0;
    for (u32 i = 0; i < 2; ++i) if (ascii_lower(country.data[i]) < 'a' || ascii_lower(country.data[i]) > 'z') return 0;
    copy_bytes(output.data, prefix.data, prefix.bytes); copy_bytes(output.data + prefix.bytes, id.data, id.bytes);
    copy_bytes(output.data + prefix.bytes + id.bytes, suffix.data, suffix.bytes);
    output[bytes - 2] = ascii_lower(country.data[0]); output[bytes - 1] = ascii_lower(country.data[1]);
    return bytes;
}
EmbedPlayerError apple_music_track_preview(Utf8View json, Utf8View id, Utf8View country,
    Slice<u32> scratch, EmbedStream* output, bool video) noexcept {
    using E = EmbedPlayerError;
    u8 target[64];
    if (!output || !apple_music_track_target(id, country, {target, sizeof(target)}) ||
        !json.data || !json.bytes || json.bytes > kAppleMusicMetadataMaximumBytes) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Field root[]{{utf8_literal("resultCount")}, {utf8_literal("results")}};
    u32 count = 0;
    if (!fields(doc, {json, 0, JsonValueKind::object}, root) || !number(root[0], &count)) return E::malformed;
    if (!count) return E::unavailable;
    if (count != 1 || !root[1].seen) return E::malformed;
    JsonValue track{};
    if (!json_visit_indexed_array(doc, root[1].value, 24,
        +[](void* context, u32 index, JsonValue value) noexcept {
            if (index || value.kind != JsonValueKind::object) return false;
            *static_cast<JsonValue*>(context) = value; return true;
        }, &track).ok()) return E::malformed;
    Field f[]{{utf8_literal("wrapperType")}, {utf8_literal("kind")}, {utf8_literal("trackId")},
        {utf8_literal("previewUrl")}, {utf8_literal("trackViewUrl")}};
    u64 identity = 0, expected = 0;
    for (u32 i = 0; i < id.bytes; ++i) expected = expected * 10 + id.data[i] - '0';
    if (!fields(doc, track, f) || !equal(f[0], utf8_literal("track")) ||
        !equal(f[1], video ? utf8_literal("music-video") : utf8_literal("song")) ||
        !f[2].seen || !json_value_to_u64(f[2].value, &identity) || identity != expected) return E::malformed;
    if (!f[3].seen || f[3].value.kind == JsonValueKind::null_value) return E::unsupported;
    JsonString view{}, source{};
    char link[512];
    if (!text(f[4], &view) || !text(f[3], &source)) return E::malformed;
    const auto decoded_view = json_string_decode_utf8(view, {link, sizeof(link)});
    const auto matched = decoded_view.ok() ? apple_music_track_locator({link, decoded_view.bytes}) : AppleMusicTrackLocator{};
    if (!utf8_equal(matched.id, id) || !ascii_equal_ignore_case(matched.country, country) || matched.video != video) return E::malformed;
    EmbedStream staged{};
    const auto decoded = json_string_decode_utf8(source, {staged.url, sizeof(staged.url)});
    const bool valid = decoded.ok() && embed_stream_location(video ? CdnHost::apple_music_video_preview : CdnHost::apple_music_preview,
                                                           {staged.url, decoded.bytes});
    if (valid) { staged.url_bytes = decoded.bytes; *output = staged; }
    secure_clear_bytes(&staged, sizeof(staged));
    return valid ? E::none : E::malformed;
}
EmbedPlayerError apple_music_album(Utf8View json, Utf8View id, Utf8View country,
    Slice<u32> scratch, u16* output, AppleMusicCollectionSink sink) noexcept {
    using E=EmbedPlayerError;
    u8 target[64];
    if (!output || !sink.write || !apple_music_track_target(id,country,{target,sizeof(target)}) || !json.bytes || json.bytes>512*1024) return E::invalid_input;
    const auto scan=json_scan_structurals_avx1(json,scratch);
    if (!scan.ok()) return scan.error==JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json,{scratch.data,scan.count}};
    Field root[]{{utf8_literal("resultCount")},{utf8_literal("results")}};
    u32 count=0; u64 expected=0;
    for (u32 i=0;i<id.bytes;++i) expected=expected*10+id.data[i]-'0';
    if (!fields(doc,{json,0,JsonValueKind::object},root) || !number(root[0],&count) || !root[1].seen) return E::malformed;
    if (!count) return E::unavailable;
    if (count>201) return E::capacity;
    struct Item { JsonValue value; u16 disc,track; };
    struct Album { const JsonIndexedDocument* doc; u64 id; Utf8View country; Item items[200]; u32 count{},expected{},seen{}; } album{&doc,expected,country};
    const auto visit=+[](void* opaque,u32 index,JsonValue value) noexcept {
        auto& a=*static_cast<Album*>(opaque); ++a.seen;
        Field f[]{{utf8_literal("wrapperType")},{utf8_literal("collectionId")},{utf8_literal("collectionType")},
            {utf8_literal("trackCount")},{utf8_literal("collectionViewUrl")},{utf8_literal("kind")},{utf8_literal("discNumber")},{utf8_literal("trackNumber")}};
        u64 id{};
        if (!fields(*a.doc,value,f) || !f[1].seen || !json_value_to_u64(f[1].value,&id) || id!=a.id) return false;
        if (!index) {
            JsonString view{}; char url[512];
            if (!equal(f[0],utf8_literal("collection")) || (!equal(f[2],utf8_literal("Album")) && !equal(f[2],utf8_literal("Compilation"))) || !number(f[3],&a.expected) || !text(f[4],&view)) return false;
            const auto decoded=json_string_decode_utf8(view,{url,sizeof(url)});
            const auto located=decoded.ok()?apple_music_album_locator({url,decoded.bytes}):AppleMusicTrackLocator{};
            u64 located_id{}; for (u32 i=0;i<located.id.bytes;++i) located_id=located_id*10+located.id.data[i]-'0';
            return located_id==a.id && ascii_equal_ignore_case(located.country,a.country);
        }
        u32 disc{},track{};
        if (a.count==200 || !equal(f[0],utf8_literal("track")) || !equal(f[5],utf8_literal("song")) ||
            !number(f[6],&disc) || !disc || disc>65535 || !number(f[7],&track) || !track || track>65535) return false;
        a.items[a.count++]={value,static_cast<u16>(disc),static_cast<u16>(track)}; return true;
    };
    if (!json_visit_indexed_array(doc,root[1].value,24,visit,&album).ok() || album.seen!=count) return E::malformed;
    if (!album.count) return E::unavailable;
    if (album.count!=album.expected) return E::unsupported; // Never present a truncated album as complete.
    for (u32 i=1;i<album.count;++i) {
        const auto item=album.items[i]; u32 at=i;
        while (at && (album.items[at-1].disc>item.disc || (album.items[at-1].disc==item.disc && album.items[at-1].track>item.track))) {
            album.items[at]=album.items[at-1]; --at;
        }
        album.items[at]=item;
    }
    for (u32 i=0;i<album.count;++i) {
        const auto& item=album.items[i];
        if (i && item.disc==album.items[i-1].disc && item.track==album.items[i-1].track) return E::malformed;
        Field f[]{{utf8_literal("trackId")},{utf8_literal("trackViewUrl")},{utf8_literal("trackName")},{utf8_literal("previewUrl")}};
        u64 track{}; JsonString view{},name{},preview{}; char link[512],source[kEmbedStreamUrlBytes];
        if (!fields(doc,item.value,f) || !f[0].seen || !json_value_to_u64(f[0].value,&track) || !track || !text(f[1],&view) || !text(f[2],&name)) return E::malformed;
        const auto decoded=json_string_decode_utf8(view,{link,sizeof(link)});
        const auto located=decoded.ok()?apple_music_track_locator({link,decoded.bytes}):AppleMusicTrackLocator{};
        u64 located_id{}; for (u32 j=0;j<located.id.bytes;++j) located_id=located_id*10+located.id.data[j]-'0';
        if (located_id!=track || located.video || !ascii_equal_ignore_case(located.country,country)) return E::malformed;
        MediaTrackTitle title{}; const auto named=json_string_decode_utf8(name,{title.text,sizeof(title.text)});
        if (!named.ok() || !named.bytes) return E::capacity;
        title.bytes=static_cast<u16>(named.bytes);
        for (u32 j=0;j<title.bytes;++j) if (static_cast<u8>(title.text[j])<32) title.text[j]=' ';
        Utf8View url{};
        if (f[3].seen && f[3].value.kind!=JsonValueKind::null_value) {
            if (!text(f[3],&preview)) return E::malformed;
            const auto decoded_preview=json_string_decode_utf8(preview,{source,sizeof(source)});
            if (!decoded_preview.ok() || !embed_stream_location(CdnHost::apple_music_preview,{source,decoded_preview.bytes})) return E::malformed;
            url={source,decoded_preview.bytes};
        }
        if (!sink.write(sink.context,static_cast<u16>(i+1),track,title,url)) return E::capacity;
    }
    *output=static_cast<u16>(album.count); return E::none;
}
bool spotify_audio_id(Utf8View id) noexcept {
    if (!id.data || id.bytes != 22) return false;
    for (u32 i = 0; i < id.bytes; ++i) if (!ascii_is_alnum(id.data[i])) return false;
    return true;
}
u32 spotify_audio_target(Utf8View id, Slice<u8> output, bool episode) noexcept {
    const auto prefix = episode ? utf8_literal("/embed/episode/") : utf8_literal("/embed/track/");
    if (!spotify_audio_id(id) || !output.data || output.count < prefix.bytes + id.bytes) return 0;
    copy_bytes(output.data, prefix.data, prefix.bytes);
    copy_bytes(output.data + prefix.bytes, id.data, id.bytes);
    return prefix.bytes + id.bytes;
}
EmbedPlayerError spotify_audio_preview(Utf8View html, Utf8View id, Slice<u32> scratch, EmbedStream* output, bool episode) noexcept {
    using E = EmbedPlayerError;
    if (!output || !spotify_audio_id(id) || !html.data || !html.bytes || html.bytes > kSpotifyEmbedMaximumBytes)
        return E::invalid_input;
    constexpr auto begin = utf8_literal("<script id=\"__NEXT_DATA__\" type=\"application/json\">");
    constexpr auto end = utf8_literal("</script>");
    Utf8View json{};
    for (u32 i = 0; i + begin.bytes <= html.bytes; ++i) {
        if (!bytes_equal(html.data + i, begin.data, begin.bytes)) continue;
        if (json.data) return E::malformed;
        const u32 first = i + begin.bytes;
        u32 last = first;
        while (last + end.bytes <= html.bytes && !bytes_equal(html.data + last, end.data, end.bytes)) ++last;
        if (last + end.bytes > html.bytes || first == last) return E::malformed;
        json = {html.data + first, last - first};
        i = last + end.bytes - 1;
    }
    if (!json.bytes) return E::unsupported;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    JsonValue value{json, 0, JsonValueKind::object};
    static constexpr Utf8View path[]{utf8_literal("props"), utf8_literal("pageProps"), utf8_literal("state"),
        utf8_literal("data"), utf8_literal("entity")};
    for (auto name : path) {
        Field entry[]{{name}};
        if (!fields(doc, value, entry) || !entry[0].seen) return E::malformed;
        value = entry[0].value;
    }
    Field track[]{{utf8_literal("type")}, {utf8_literal("id")}, {utf8_literal("isPlayable")},
        {utf8_literal("audioPreview")}, {utf8_literal("uri")}, {utf8_literal("isNineteenPlus")}};
    if (!fields(doc, value, track) || !equal(track[0], episode ? utf8_literal("episode") : utf8_literal("track")) || !equal(track[1], id)) return E::malformed;
    bool playable = false, restricted = false;
    if (!track[2].seen || !json_value_to_bool(track[2].value, &playable) ||
        (track[5].seen && !json_value_to_bool(track[5].value, &restricted))) return E::malformed;
    if (!playable || restricted) return E::unavailable;
    if (track[4].seen) {
        char uri[38]; const auto prefix = episode ? utf8_literal("spotify:episode:") : utf8_literal("spotify:track:");
        copy_bytes(uri, prefix.data, prefix.bytes); copy_bytes(uri + prefix.bytes, id.data, id.bytes);
        if (!equal(track[4], {uri, prefix.bytes+id.bytes})) return E::malformed;
    }
    if (!track[3].seen || track[3].value.kind == JsonValueKind::null_value) return E::unsupported;
    Field preview[]{{utf8_literal("url")}}; JsonString url{};
    if (!fields(doc, track[3].value, preview) || !text(preview[0], &url)) return E::malformed;
    EmbedStream staged{};
    const auto decoded = json_string_decode_utf8(url, {staged.url, sizeof(staged.url)});
    EmbedStreamLocation location{};
    const bool valid = decoded.ok() && embed_stream_location(CdnHost::spotify_preview, {staged.url, decoded.bytes},&location) &&
        ascii_equal_ignore_case(location.host,episode ? utf8_literal("podz-content.spotifycdn.com") : utf8_literal("p.scdn.co"));
    if (valid) { staged.url_bytes = decoded.bytes; *output = staged; }
    secure_clear_bytes(&staged, sizeof(staged));
    return valid ? E::none : E::malformed;
}
bool soundcloud_location(Utf8View url, bool playlist, EmbedStreamLocation* out) noexcept {
    constexpr auto origin = utf8_literal("https://playback.media-streaming.soundcloud.cloud");
    if (!url.data || url.bytes > kEmbedStreamUrlBytes || !utf8_starts_with(url, origin) ||
        url.bytes <= origin.bytes || url.data[origin.bytes] != '/') return false;
    u32 end = origin.bytes;
    while (end < url.bytes && url.data[end] != '?') ++end;
    Utf8View parts[4]{}; u32 count = 0, begin = origin.bytes + 1;
    for (u32 i = begin; i <= end; ++i) if (i == end || url.data[i] == '/') {
        if (count == 4 || i == begin) return false;
        parts[count++] = {url.data + begin, i - begin}; begin = i + 1;
    }
    if (count != 4 || (!utf8_equal(parts[1], utf8_literal("aac_160k")) &&
        !utf8_equal(parts[1], utf8_literal("aac_96k")))) return false;
    for (u32 part = 0; part <= 2; part += 2) {
        if (parts[part].bytes > 128) return false;
        for (u32 i = 0; i < parts[part].bytes; ++i)
            if (!ascii_is_alnum(parts[part].data[i]) && parts[part].data[i] != '-' && parts[part].data[i] != '_') return false;
    }
    const auto leaf = parts[3];
    if (playlist) { if (!utf8_equal(leaf, utf8_literal("playlist.m3u8"))) return false; }
    else if (!utf8_equal(leaf, utf8_literal("init.mp4"))) {
        if (leaf.bytes < 9 || leaf.bytes > 16 || !utf8_starts_with(leaf, utf8_literal("data")) ||
            !utf8_equal({leaf.data + leaf.bytes - 4, 4}, utf8_literal(".m4s"))) return false;
        for (u32 i = 4; i < leaf.bytes - 4; ++i) if (!ascii_is_digit(leaf.data[i])) return false;
    }
    for (u32 i = end; i < url.bytes; ++i) {
        const u8 c = static_cast<u8>(url.data[i]);
        if (c <= 32 || c >= 127 || c == '\\' || c == '#' || c == '"' || c == '<' || c == '>') return false;
    }
    if (!stream_expiry(url, utf8_literal("expires="))) return false;
    if (out) *out = {{origin.data + 8, origin.bytes - 8}, {url.data + origin.bytes, url.bytes - origin.bytes}};
    return true;
}
static EmbedPlayerError track_title(Utf8View json, u64 track, Utf8View video, Slice<u32> scratch, MediaTrackTitle* out) noexcept {
    using E = EmbedPlayerError;
    if (!out || (!track && !video_id(video)) || !json.data || !json.bytes || json.bytes > 32768) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Field f[]{{utf8_literal("type")}, {utf8_literal("provider_name")}, {utf8_literal("provider_url")},
        {utf8_literal("title")}, {utf8_literal("html")}};
    JsonString title{}, markup{};
    if (!fields(doc, {json, 0, JsonValueKind::object}, f) || !equal(f[0], video.bytes ? utf8_literal("video") : utf8_literal("rich")) ||
        !equal(f[1], video.bytes ? utf8_literal("YouTube") : utf8_literal("SoundCloud")) || !equal(f[2], video.bytes ? utf8_literal("https://www.youtube.com/") : utf8_literal("https://soundcloud.com")) ||
        !text(f[3], &title) || !text(f[4], &markup)) return E::malformed;
    char html[4096], decoded[1024];
    const auto rendered = json_string_decode_utf8(markup, {html, sizeof(html)});
    if (!rendered.ok()) return E::malformed;
    constexpr auto marker = utf8_literal("src=\""); Utf8View source{};
    for (u32 at = 0; at + marker.bytes < rendered.bytes; ++at) {
        if (!bytes_equal(html + at, marker.data, marker.bytes)) continue;
        if (source.data) return E::malformed;
        const u32 first = at + marker.bytes; u32 last = first;
        while (last < rendered.bytes && html[last] != '"') ++last;
        if (last == rendered.bytes) return E::malformed;
        source = {html + first, last - first}; at = last;
    }
    if (video.bytes) {
        const auto locator = embed_video_locator(source);
        if (locator.provider != EmbedProvider::youtube || !utf8_equal(locator.id, video)) return E::malformed;
    } else {
        const auto locator = soundcloud_track_locator(source, {decoded, sizeof(decoded)});
        if (locator.id != track || locator.path.bytes) return E::malformed;
    }
    MediaTrackTitle staged{};
    const auto name = json_string_decode_utf8(title, {staged.text, sizeof(staged.text)});
    if (!name.ok()) return name.error == JsonStringDecodeError::output_too_small ? E::capacity : E::malformed;
    if (!name.bytes) return E::unavailable;
    staged.bytes = static_cast<u16>(name.bytes);
    for (u32 i = 0; i < name.bytes; ++i) if (static_cast<u8>(staged.text[i]) < 32) staged.text[i] = ' ';
    copy_value(out, staged);
    return E::none;
}

namespace {
JsonValue queue_child(const JsonIndexedDocument& doc, const JsonValue& parent, const Utf8View& name) noexcept {
    if (!parent.source.bytes || parent.kind != JsonValueKind::object) return {{}, 0, JsonValueKind::null_value};
    Field f[]{{name}};
    Fields selected{f, 1};
    return json_visit_indexed_object(doc, parent, 48, field, &selected).ok() && f[0].seen ? f[0].value : JsonValue{{}, 0, JsonValueKind::null_value};
}
bool queue_panel(const JsonIndexedDocument& doc, JsonValue root, JsonValue* panel) noexcept {
    auto next = queue_child(doc, root, utf8_literal("continuationContents"));
    next = queue_child(doc, next, utf8_literal("playlistPanelContinuation"));
    if (next.kind == JsonValueKind::object) { *panel = next; return true; }
    next = queue_child(doc, root, utf8_literal("contents"));
    next = queue_child(doc, next, utf8_literal("singleColumnMusicWatchNextResultsRenderer"));
    next = queue_child(doc, next, utf8_literal("tabbedRenderer"));
    next = queue_child(doc, next, utf8_literal("watchNextTabbedResultsRenderer"));
    next = queue_child(doc, next, utf8_literal("tabs"));
    struct Tabs { const JsonIndexedDocument* doc; JsonValue panel; u32 count; } tabs{&doc};
    const auto visit = [](void* opaque, u32, JsonValue tab) noexcept {
        auto& t = *static_cast<Tabs*>(opaque);
        auto v = queue_child(*t.doc, tab, utf8_literal("tabRenderer"));
        v = queue_child(*t.doc, v, utf8_literal("content"));
        v = queue_child(*t.doc, v, utf8_literal("musicQueueRenderer"));
        v = queue_child(*t.doc, v, utf8_literal("content"));
        v = queue_child(*t.doc, v, utf8_literal("playlistPanelRenderer"));
        if (v.kind == JsonValueKind::object) { t.panel = v; ++t.count; }
        return t.count <= 1;
    };
    if (!json_visit_indexed_array(doc, next, 48, visit, &tabs).ok() || tabs.count != 1) return false;
    *panel = tabs.panel; return true;
}
}
EmbedPlayerError youtube_collection_page(Utf8View json, Utf8View list, Slice<u32> scratch, YoutubeCollectionPage* out, MediaTrackTitleSink sink) noexcept {
    using E = EmbedPlayerError;
    if (!out || !list.bytes || !json.bytes || json.bytes > 2 * 1024 * 1024) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    JsonValue panel{};
    if (!queue_panel(doc, {json, 0, JsonValueKind::object}, &panel)) return E::unavailable;
    Field f[]{{utf8_literal("playlistId")}, {utf8_literal("contents")}, {utf8_literal("continuations")}};
    if (!fields(doc, panel, f) || !equal(f[0], list) || !f[1].seen) return E::malformed;
    YoutubeCollectionPage staged{};
    struct Items { const JsonIndexedDocument* doc; YoutubeCollectionPage* page; MediaTrackTitleSink sink; bool overflow; } items{&doc, &staged, sink};
    const auto visit = [](void* opaque, u32, JsonValue value) noexcept {
        auto& c = *static_cast<Items*>(opaque); auto& page = *c.page;
        auto v = queue_child(*c.doc, value, utf8_literal("playlistPanelVideoRenderer"));
        if (v.kind != JsonValueKind::object) {
            v = queue_child(*c.doc, value, utf8_literal("playlistPanelVideoWrapperRenderer"));
            v = queue_child(*c.doc, v, utf8_literal("primaryRenderer"));
            v = queue_child(*c.doc, v, utf8_literal("playlistPanelVideoRenderer"));
        }
        if (v.kind != JsonValueKind::object) return true; // Automix controls are not songs.
        Field fields_out[]{{utf8_literal("videoId")}, {utf8_literal("selected")}, {utf8_literal("title")}};
        JsonString id{};
        if (!fields(*c.doc, v, fields_out) || !text(fields_out[0], &id)) return false;
        MediaTrackId track{}; track.youtube_track = true;
        const auto decoded = json_string_decode_utf8(id, {track.youtube, 11});
        if (!decoded.ok() || !video_id({track.youtube, decoded.bytes})) return false;
        if (page.count == kYoutubeCollectionPageTracks) { c.overflow = true; return false; }
        bool selected = false;
        if (fields_out[1].seen && !json_value_to_bool(fields_out[1].value, &selected)) return false;
        if (selected) { if (page.selected) return false; page.selected = static_cast<u16>(page.count + 1); }
        page.tracks[page.count++] = track;
        if (fields_out[2].seen) {
            MediaTrackTitle cached{};
            const auto runs = queue_child(*c.doc, fields_out[2].value, utf8_literal("runs"));
            struct TitleRuns { const JsonIndexedDocument* doc; MediaTrackTitle* title; } title_runs{c.doc, &cached};
            const auto run = [](void* opaque_title, u32, JsonValue item) noexcept {
                auto& t = *static_cast<TitleRuns*>(opaque_title);
                const auto value = queue_child(*t.doc, item, utf8_literal("text")); JsonString text{};
                if (!json_value_as_string(value, &text)) return false;
                auto& title = *t.title;
                const auto decoded = json_string_decode_utf8(text, {title.text + title.bytes, static_cast<u32>(sizeof(title.text) - title.bytes)});
                if (!decoded.ok()) return false;
                title.bytes += static_cast<u16>(decoded.bytes); return true;
            };
            if (!json_visit_indexed_array(*c.doc, runs, 24, run, &title_runs).ok()) reset_value(&cached);
            for (u32 i = 0; i < cached.bytes; ++i) if (static_cast<u8>(cached.text[i]) < 32) cached.text[i] = ' ';
            if (cached.bytes) {
                if (c.sink.write && !c.sink.write(c.sink.context, page.count, cached)) { c.overflow = true; return false; }
            }
        }
        return true;
    };
    if (!json_visit_indexed_array(doc, f[1].value, 24, visit, &items).ok()) return items.overflow ? E::capacity : E::malformed;
    if (!staged.count) return E::unavailable;
    if (f[2].seen) {
        const auto continuation = [](void* opaque, u32, JsonValue value) noexcept {
            auto& c = *static_cast<Items*>(opaque);
            auto v = queue_child(*c.doc, value, utf8_literal("nextRadioContinuationData"));
            if (v.kind != JsonValueKind::object) v = queue_child(*c.doc, value, utf8_literal("nextContinuationData"));
            v = queue_child(*c.doc, v, utf8_literal("continuation"));
            if (!v.source.bytes) return true;
            JsonString text{};
            if (c.page->continuation_bytes || !json_value_as_string(v, &text)) return false;
            const auto decoded = json_string_decode_utf8(text, {c.page->continuation, sizeof(c.page->continuation)});
            if (!decoded.ok()) return false;
            c.page->continuation_bytes = static_cast<u16>(decoded.bytes); return true;
        };
        if (!json_visit_indexed_array(doc, f[2].value, 24, continuation, &items).ok()) return E::malformed;
    }
    *out = staged; return E::none;
}
namespace {
bool html_space(char c) noexcept { return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f'; }
EmbedPlayerError playlist_json(Utf8View html, Utf8View* out) noexcept {
    reset_value(out);
    constexpr auto start=utf8_literal("<script"),end=utf8_literal("</script>");
    for (u32 i=0;i+start.bytes<html.bytes;++i) {
        if (!bytes_equal(html.data+i,start.data,start.bytes) || (html.data[i+start.bytes]!='>' && !html_space(html.data[i+start.bytes]))) continue;
        u32 at=i+start.bytes; Utf8View id{},type{}; bool seen_id=false,seen_type=false;
        while (at<html.bytes && html.data[at]!='>') {
            while (at<html.bytes && html_space(html.data[at])) ++at;
            if (at==html.bytes || html.data[at]=='>') break;
            const u32 first=at;
            while (at<html.bytes && (ascii_is_alnum(html.data[at]) || html.data[at]=='-' || html.data[at]=='_' || html.data[at]==':')) ++at;
            if (at==first) return EmbedPlayerError::malformed;
            const Utf8View name{html.data+first,at-first};
            while (at<html.bytes && html_space(html.data[at])) ++at;
            Utf8View value{};
            if (at<html.bytes && html.data[at]=='=') {
                ++at; while (at<html.bytes && html_space(html.data[at])) ++at;
                if (at==html.bytes) return EmbedPlayerError::malformed;
                const char quote=html.data[at]=='\'' || html.data[at]=='"' ? html.data[at++] : 0;
                const u32 begin=at;
                while (at<html.bytes && (quote ? html.data[at]!=quote : !html_space(html.data[at]) && html.data[at]!='>')) ++at;
                value={html.data+begin,at-begin};
                if (quote) { if (at==html.bytes) return EmbedPlayerError::malformed; ++at; }
            }
            if (ascii_equal_ignore_case(name,utf8_literal("id"))) { if (seen_id) return EmbedPlayerError::malformed; seen_id=true; id=value; }
            if (ascii_equal_ignore_case(name,utf8_literal("type"))) { if (seen_type) return EmbedPlayerError::malformed; seen_type=true; type=value; }
        }
        if (at==html.bytes) return EmbedPlayerError::malformed;
        const u32 begin=++at;
        while (at+end.bytes<=html.bytes && !bytes_equal(html.data+at,end.data,end.bytes)) ++at;
        if (at+end.bytes>html.bytes) return EmbedPlayerError::malformed;
        if (utf8_equal(id,utf8_literal("serialized-server-data"))) {
            if (out->bytes || !utf8_equal(type,utf8_literal("application/json"))) return EmbedPlayerError::malformed;
            *out={html.data+begin,at-begin};
        }
        i=at+end.bytes-1;
    }
    return out->bytes ? EmbedPlayerError::none : EmbedPlayerError::unsupported;
}
JsonValue single_item(const JsonIndexedDocument& doc,const JsonValue& array) noexcept {
    JsonValue item{{},0,JsonValueKind::null_value};
    if (!json_visit_indexed_array(doc,array,48,+[](void* opaque,u32 index,JsonValue value) noexcept {
        if (index) return false; *static_cast<JsonValue*>(opaque)=value; return true;
    },&item).ok()) return {{},0,JsonValueKind::null_value};
    return item;
}
bool playlist_identity(const JsonIndexedDocument& doc,const JsonValue& descriptor,const Utf8View& id,const Utf8View& country) noexcept {
    Field f[]{{utf8_literal("kind")},{utf8_literal("identifiers")},{utf8_literal("url")}};
    JsonString identifier{},url{}; char buffer[1024];
    if (!fields(doc,descriptor,f) || !equal(f[0],utf8_literal("playlist")) || !text(f[2],&url) ||
        !json_value_as_string(queue_child(doc,f[1].value,utf8_literal("storeAdamID")),&identifier) || !json_string_equals_ascii(identifier,id)) return false;
    const auto decoded=json_string_decode_utf8(url,{buffer,sizeof(buffer)});
    const auto match=decoded.ok()?apple_music_collection_locator({buffer,decoded.bytes}):AppleMusicCollectionLocator{};
    return match.playlist && utf8_equal(match.id,id) && ascii_equal_ignore_case(match.country,country);
}
}
EmbedPlayerError apple_music_playlist(Utf8View html,Utf8View id,Utf8View country,Slice<u32> scratch,
    AppleMusicPlaylist* output,AppleMusicCollectionSink sink) noexcept {
    using E=EmbedPlayerError;
    if (!output || !sink.write || !html.bytes || html.bytes>2*1024*1024) return E::invalid_input;
    Utf8View json{}; const auto extracted=playlist_json(html,&json); if (extracted!=E::none) return extracted;
    const auto scan=json_scan_structurals_avx1(json,scratch);
    if (!scan.ok()) return scan.error==JsonScanError::output_overflow?E::capacity:E::malformed;
    const JsonIndexedDocument doc{json,{scratch.data,scan.count}};
    auto page=single_item(doc,queue_child(doc,{json,0,JsonValueKind::object},utf8_literal("data")));
    page=queue_child(doc,page,utf8_literal("data"));
    char canonical[1024]; JsonString url{};
    if (!json_value_as_string(queue_child(doc,page,utf8_literal("canonicalURL")),&url)) return E::malformed;
    const auto decoded=json_string_decode_utf8(url,{canonical,sizeof(canonical)});
    const auto located=decoded.ok()?apple_music_collection_locator({canonical,decoded.bytes}):AppleMusicCollectionLocator{};
    if (!located.playlist || !utf8_equal(located.id,id) || !ascii_equal_ignore_case(located.country,country)) return E::malformed;
    struct Sections { const JsonIndexedDocument* doc; Utf8View id,country; JsonValue tracks{}; u32 count{}; bool header{},list{}; } sections{&doc,id,country};
    const auto visit=+[](void* opaque,u32,JsonValue value) noexcept {
        auto& s=*static_cast<Sections*>(opaque); Field f[]{{utf8_literal("itemKind")},{utf8_literal("items")},{utf8_literal("containerContentDescriptor")}};
        if (!fields(*s.doc,value,f)) return false;
        if (equal(f[0],utf8_literal("containerDetailHeaderLockup"))) {
            if (s.header) return false; s.header=true; const auto item=single_item(*s.doc,f[1].value);
            Field header[]{{utf8_literal("contentDescriptor")},{utf8_literal("trackCount")}};
            return fields(*s.doc,item,header) && playlist_identity(*s.doc,header[0].value,s.id,s.country) && number(header[1],&s.count);
        }
        if (equal(f[0],utf8_literal("trackLockup"))) {
            if (s.list || !playlist_identity(*s.doc,f[2].value,s.id,s.country)) return false;
            s.list=true; s.tracks=f[1].value;
        }
        return true;
    };
    if (!json_visit_indexed_array(doc,queue_child(doc,page,utf8_literal("sections")),48,visit,&sections).ok() || !sections.header || !sections.list) return E::malformed;
    if (!sections.count) return E::unavailable;
    if (sections.count>200) return E::capacity;
    AppleMusicPlaylist staged{};
    struct Tracks { const JsonIndexedDocument* doc; Utf8View country; AppleMusicPlaylist* out; AppleMusicCollectionSink sink; } tracks{&doc,country,&staged,sink};
    const auto song=+[](void* opaque,u32 index,JsonValue value) noexcept {
        auto& t=*static_cast<Tracks*>(opaque); if (index>=200) return false;
        Field f[]{{utf8_literal("title")},{utf8_literal("contentDescriptor")},{utf8_literal("isDisabled")}};
        if (!fields(*t.doc,value,f)) return false;
        Field d[]{{utf8_literal("kind")},{utf8_literal("identifiers")},{utf8_literal("url")}};
        JsonString identifier{},name{},view{}; bool disabled=false; u64 id{};
        if (!fields(*t.doc,f[1].value,d) || !equal(d[0],utf8_literal("song")) || !text(f[0],&name) || !text(d[2],&view) ||
            !json_value_as_string(queue_child(*t.doc,d[1].value,utf8_literal("storeAdamID")),&identifier) || !json_string_to_u64(identifier,&id) || !id ||
            (f[2].seen && !json_value_to_bool(f[2].value,&disabled))) return false;
        char url[1024]; const auto decoded=json_string_decode_utf8(view,{url,sizeof(url)});
        const auto match=decoded.ok()?apple_music_track_locator({url,decoded.bytes}):AppleMusicTrackLocator{};
        u64 match_id{}; for (u32 i=0;i<match.id.bytes;++i) match_id=match_id*10+match.id.data[i]-'0';
        if (match_id!=id || match.video || !ascii_equal_ignore_case(match.country,t.country)) return false;
        MediaTrackTitle title{}; const auto named=json_string_decode_utf8(name,{title.text,sizeof(title.text)});
        if (!named.ok() || !named.bytes) return false;
        title.bytes=static_cast<u16>(named.bytes); for (u32 i=0;i<title.bytes;++i) if (static_cast<u8>(title.text[i])<32) title.text[i]=' ';
        t.out->tracks[t.out->count++]=disabled?0:id;
        return t.sink.write(t.sink.context,static_cast<u16>(index+1),id,title,{});
    };
    if (!json_visit_indexed_array(doc,sections.tracks,48,song,&tracks).ok()) return E::malformed;
    if (staged.count!=sections.count) return E::unsupported;
    *output=staged; return E::none;
}
EmbedPlayerError apple_music_preview_batch(Utf8View json,ConstSlice<u64> ids,u16 offset,Utf8View country,Slice<u32> scratch,AppleMusicCollectionSink sink) noexcept {
    using E=EmbedPlayerError;
    if (!ids.count || ids.count>50 || !sink.write || !json.bytes || json.bytes>512*1024 || offset+ids.count>200) return E::invalid_input;
    const auto scan=json_scan_structurals_avx1(json,scratch);
    if (!scan.ok()) return scan.error==JsonScanError::output_overflow?E::capacity:E::malformed;
    const JsonIndexedDocument doc{json,{scratch.data,scan.count}}; Field root[]{{utf8_literal("resultCount")},{utf8_literal("results")}}; u32 count{};
    if (!fields(doc,{json,0,JsonValueKind::object},root) || !number(root[0],&count) || count>ids.count) return E::malformed;
    struct Batch { const JsonIndexedDocument* doc; ConstSlice<u64> ids; Utf8View country; AppleMusicCollectionSink sink; u64 seen{}; u32 count{}; u16 offset; } batch{&doc,ids,country,sink,0,0,offset};
    const auto visit=+[](void* opaque,u32,JsonValue value) noexcept {
        auto& b=*static_cast<Batch*>(opaque); ++b.count;
        Field f[]{{utf8_literal("wrapperType")},{utf8_literal("kind")},{utf8_literal("trackId")},{utf8_literal("trackName")},{utf8_literal("trackViewUrl")},{utf8_literal("previewUrl")}};
        u64 id{}; JsonString name{},view{},preview{};
        if (!fields(*b.doc,value,f) || !equal(f[0],utf8_literal("track")) || !equal(f[1],utf8_literal("song")) || !f[2].seen || !json_value_to_u64(f[2].value,&id) || !id || !text(f[3],&name) || !text(f[4],&view)) return false;
        u32 found=0; while (found<b.ids.count && b.ids[found]!=id) ++found;
        if (found==b.ids.count || (b.seen&(u64{1}<<found))) return false;
        b.seen|=u64{1}<<found;
        char url[1024]; const auto decoded=json_string_decode_utf8(view,{url,sizeof(url)});
        const auto match=decoded.ok()?apple_music_track_locator({url,decoded.bytes}):AppleMusicTrackLocator{};
        u64 match_id{}; for (u32 i=0;i<match.id.bytes;++i) match_id=match_id*10+match.id.data[i]-'0';
        if (match_id!=id || match.video || !ascii_equal_ignore_case(match.country,b.country)) return false;
        if (!f[5].seen || f[5].value.kind==JsonValueKind::null_value) return true;
        MediaTrackTitle title{}; const auto named=json_string_decode_utf8(name,{title.text,sizeof(title.text)});
        if (!named.ok() || !named.bytes || !text(f[5],&preview)) return false;
        title.bytes=static_cast<u16>(named.bytes);
        for (u32 i=0;i<title.bytes;++i) if (static_cast<u8>(title.text[i])<32) title.text[i]=' ';
        char source[kEmbedStreamUrlBytes]; const auto source_bytes=json_string_decode_utf8(preview,{source,sizeof(source)});
        if (!source_bytes.ok() || !embed_stream_location(CdnHost::apple_music_preview,{source,source_bytes.bytes})) return false;
        for (u32 i=0;i<b.ids.count;++i) if (b.ids[i]==id && !b.sink.write(b.sink.context,static_cast<u16>(b.offset+i+1),id,title,{source,source_bytes.bytes})) return false;
        return true;
    };
    return json_visit_indexed_array(doc,root[1].value,24,visit,&batch).ok() && batch.count==count ? E::none : E::malformed;
}
EmbedPlayerError soundcloud_track_title(Utf8View json, u64 track, Slice<u32> scratch, MediaTrackTitle* out) noexcept {
    return track_title(json, track, {}, scratch, out);
}
EmbedPlayerError youtube_track_title(Utf8View json, Utf8View video, Slice<u32> scratch, MediaTrackTitle* out) noexcept {
    return track_title(json, 0, video, scratch, out);
}
EmbedPlayerError soundcloud_manifest(Utf8View json, Slice<u32> scratch, EmbedStream* out) noexcept {
    using E = EmbedPlayerError;
    if (!out || !json.data || !json.bytes || json.bytes > 8192) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Field f[]{{utf8_literal("url")}}; JsonString value{}; EmbedStream staged{};
    if (!fields(doc, {json, 0, JsonValueKind::object}, f) || !text(f[0], &value)) return E::malformed;
    const auto decoded = json_string_decode_utf8(value, {staged.url, sizeof(staged.url)});
    const bool valid = decoded.ok() && soundcloud_location({staged.url, decoded.bytes}, true);
    if (valid) { staged.url_bytes = decoded.bytes; *out = staged; }
    secure_clear_bytes(&staged, sizeof(staged));
    return valid ? E::none : E::malformed;
}
EmbedPlayerError soundcloud_widget_page(Utf8View html, EmbedStream* out, bool collection) noexcept {
    using E = EmbedPlayerError;
    if (!out || !html.data || !html.bytes || html.bytes > 65536) return E::invalid_input;
    constexpr auto marker = utf8_literal("<link rel=\"canonical\" href=\"");
    Utf8View canonical{};
    for (u32 i = 0; i + marker.bytes < html.bytes; ++i) {
        if (!bytes_equal(html.data + i, marker.data, marker.bytes)) continue;
        if (canonical.data) return E::malformed;
        const u32 begin = i + marker.bytes;
        u32 end = begin;
        while (end < html.bytes && html.data[end] != '"') ++end;
        if (end == html.bytes || !(collection ? soundcloud_collection_path({html.data + begin, end - begin}) :
            soundcloud_track_path({html.data + begin, end - begin})).bytes) return E::malformed;
        canonical = {html.data + begin, end - begin}; i = end;
    }
    if (!canonical.bytes) return E::unsupported;
    EmbedStream staged{};
    if (canonical.bytes > sizeof(staged.url)) return E::capacity;
    copy_bytes(staged.url, canonical.data, canonical.bytes); staged.url_bytes = canonical.bytes;
    *out = staged;
    return E::none;
}
namespace {
struct SoundcloudHydration { JsonIndexedDocument doc; JsonValue record{}, client{}; Utf8View kind; u32 count{}; };
EmbedPlayerError soundcloud_hydration(Utf8View html, Utf8View kind, Slice<u32> scratch, SoundcloudHydration* out) noexcept {
    using E = EmbedPlayerError;
    Utf8View json{};
    const auto extracted = embedded_json(html, utf8_literal("window.__sc_hydration"), &json, true);
    if (extracted != E::none) return extracted;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    SoundcloudHydration h{{json, {scratch.data, scan.count}}, {}, {}, kind};
    const auto visit = +[](void* opaque, u32, JsonValue value) noexcept {
        auto& h = *static_cast<SoundcloudHydration*>(opaque);
        if (++h.count > 64) return false;
        Field f[]{{utf8_literal("hydratable")}, {utf8_literal("data")}};
        if (!fields(h.doc, value, f) || !f[1].seen) return false;
        auto* selected = equal(f[0], h.kind) ? &h.record : equal(f[0], utf8_literal("apiClient")) ? &h.client : nullptr;
        if (selected) { if (selected->source.bytes) return false; *selected = f[1].value; }
        return true;
    };
    if (!json_visit_indexed_array(h.doc, {json, 0, JsonValueKind::array}, 24, visit, &h).ok() ||
        !h.record.source.bytes) return E::malformed;
    *out = h;
    return E::none;
}
} // namespace
EmbedPlayerError soundcloud_track_request(Utf8View html, Utf8View path, Slice<u32> scratch, EmbedStream* out, u64 expected_id) noexcept {
    using E = EmbedPlayerError;
    if (!out || !html.data || !html.bytes || html.bytes > 512 * 1024 || !path.data || path.bytes > 512) return E::invalid_input;
    char permalink[544]; ProtocolWriter canonical{{reinterpret_cast<u8*>(permalink), sizeof(permalink)}, 0};
    if (!protocol_writer_append_literal(&canonical, "https://soundcloud.com") ||
        !protocol_writer_append_bytes(&canonical, reinterpret_cast<const u8*>(path.data), path.bytes) ||
        !soundcloud_track_path({permalink, canonical.used}).bytes) return E::invalid_input;
    SoundcloudHydration h{};
    const auto parsed = soundcloud_hydration(html, utf8_literal("sound"), scratch, &h);
    if (parsed != E::none) return parsed;
    if (!h.client.source.bytes) return E::malformed;
    Field sound[]{{utf8_literal("id")}, {utf8_literal("kind")}, {utf8_literal("permalink_url")},
        {utf8_literal("streamable")}, {utf8_literal("policy")}, {utf8_literal("embeddable_by")},
        {utf8_literal("media")}, {utf8_literal("track_authorization")}, {utf8_literal("public")}};
    u64 id{}; bool streamable{}, public_track{};
    if (!fields(h.doc, h.record, sound) || !sound[0].seen || !json_value_to_u64(sound[0].value, &id) || !id ||
        !equal(sound[1], utf8_literal("track")) || !equal(sound[2], {permalink, canonical.used}) ||
        !sound[3].seen || !json_value_to_bool(sound[3].value, &streamable) ||
        !sound[8].seen || !json_value_to_bool(sound[8].value, &public_track) || (expected_id && id != expected_id)) return E::malformed;
    if (!streamable || !public_track || !equal(sound[4], utf8_literal("ALLOW"))) return E::unavailable;
    if (!equal(sound[5], utf8_literal("all"))) return E::not_embeddable;
    Field media[]{{utf8_literal("transcodings")}}, client[]{{utf8_literal("id")}};
    if (!sound[6].seen || !fields(h.doc, sound[6].value, media) || !media[0].seen || !fields(h.doc, h.client, client)) return E::malformed;
    struct Renditions { JsonIndexedDocument doc; JsonString url{}; u32 count{}, bitrate{}; } r{h.doc};
    const auto rendition = +[](void* opaque, u32, JsonValue value) noexcept {
        auto& r = *static_cast<Renditions*>(opaque);
        if (++r.count > 32) return false;
        Field f[]{{utf8_literal("url")}, {utf8_literal("preset")}, {utf8_literal("snipped")}, {utf8_literal("format")}};
        if (!fields(r.doc, value, f)) return false;
        const u32 bitrate = equal(f[1], utf8_literal("aac_160k")) ? 160 : equal(f[1], utf8_literal("aac_96k")) ? 96 : 0;
        if (!bitrate) return true;
        bool snipped{}; Field format[]{{utf8_literal("protocol")}, {utf8_literal("mime_type")}}; JsonString url{};
        if (!f[2].seen || !json_value_to_bool(f[2].value, &snipped) || !f[3].seen || !fields(r.doc, f[3].value, format) || !text(f[0], &url)) return false;
        if (!snipped && equal(format[0], utf8_literal("hls")) &&
            equal(format[1], utf8_literal("audio/mp4; codecs=\"mp4a.40.2\"")) && bitrate > r.bitrate) { r.url = url; r.bitrate = bitrate; }
        return true;
    };
    if (!json_visit_indexed_array(h.doc, media[0].value, 24, rendition, &r).ok()) return E::malformed;
    if (!r.bitrate) return E::unsupported;
    EmbedStream staged{};
    const auto decoded = json_string_decode_utf8(r.url, {staged.url, sizeof(staged.url)});
    char prefix[128]; ProtocolWriter head{{reinterpret_cast<u8*>(prefix), sizeof(prefix)}, 0};
    protocol_writer_append_literal(&head, "https://api-v2.soundcloud.com/media/soundcloud:tracks:");
    protocol_writer_append_u64(&head, id); protocol_writer_append_literal(&head, "/");
    bool valid = decoded.ok() && utf8_starts_with({staged.url, decoded.bytes}, {prefix, head.used});
    u32 at = head.used;
    while (valid && at < decoded.bytes && (ascii_is_alnum(staged.url[at]) || staged.url[at] == '-' || staged.url[at] == '_')) ++at;
    valid = valid && at > head.used && utf8_equal({staged.url + at, decoded.bytes - at}, utf8_literal("/stream/hls"));
    char key[64]{}, grant[512]{}; JsonString key_json{}, grant_json{};
    const auto key_decoded = text(client[0], &key_json) ? json_string_decode_utf8(key_json, {key, sizeof(key)}) : JsonStringDecodeResult{};
    const auto grant_decoded = text(sound[7], &grant_json) ? json_string_decode_utf8(grant_json, {grant, sizeof(grant)}) : JsonStringDecodeResult{};
    valid = valid && key_decoded.ok() && key_decoded.bytes == 32 && grant_decoded.ok() && grant_decoded.bytes;
    for (u32 i = 0; valid && i < key_decoded.bytes; ++i) valid = ascii_is_alnum(key[i]);
    ProtocolWriter request{{reinterpret_cast<u8*>(staged.url), sizeof(staged.url)}, decoded.ok() ? decoded.bytes : 0};
    valid = valid && protocol_writer_append_literal(&request, "?client_id=") &&
        protocol_writer_append_percent_encoded(&request, {key, key_decoded.bytes}) &&
        protocol_writer_append_literal(&request, "&track_authorization=") &&
        protocol_writer_append_percent_encoded(&request, {grant, grant_decoded.bytes});
    if (valid) { staged.url_bytes = request.used; staged.bitrate = r.bitrate * 1000; *out = staged; }
    secure_clear_bytes(key, sizeof(key)); secure_clear_bytes(grant, sizeof(grant)); secure_clear_bytes(&staged, sizeof(staged));
    return valid ? E::none : E::malformed;
}
EmbedPlayerError soundcloud_collection(Utf8View html, Utf8View path, u64 expected_id,
    u32 selected, Slice<u32> scratch, Slice<u64> tracks, SoundcloudCollection* out, MediaTrackTitleSink sink) noexcept {
    using E = EmbedPlayerError;
    if (!out || !tracks.data || !tracks.count || !html.data || !html.bytes || html.bytes > 512 * 1024 ||
        !path.data || path.bytes > 512) return E::invalid_input;
    char permalink[544]; ProtocolWriter canonical{{reinterpret_cast<u8*>(permalink), sizeof(permalink)}, 0};
    if (!protocol_writer_append_literal(&canonical, "https://soundcloud.com") ||
        !protocol_writer_append_bytes(&canonical, reinterpret_cast<const u8*>(path.data), path.bytes) ||
        !soundcloud_collection_path({permalink, canonical.used}).bytes) return E::invalid_input;
    SoundcloudHydration h{};
    const auto station = soundcloud_station_locator(path);
    const auto parsed = soundcloud_hydration(html, station.seed ? utf8_literal("systemPlaylist") : utf8_literal("playlist"), scratch, &h);
    if (parsed != E::none) return parsed;
    Field f[]{{utf8_literal("id")}, {utf8_literal("kind")}, {utf8_literal("permalink_url")},
        {utf8_literal("public")}, {utf8_literal("sharing")}, {utf8_literal("embeddable_by")},
        {utf8_literal("track_count")}, {utf8_literal("tracks")}, {utf8_literal("is_public")},
        {utf8_literal("urn")}, {utf8_literal("playlist_type")}, {utf8_literal("seed")}};
    SoundcloudCollection staged{}; bool public_set{}; u64 count{};
    Field client[]{{utf8_literal("id")}}; JsonString client_id{};
    if (h.client.source.bytes && fields(h.doc, h.client, client) && text(client[0], &client_id)) {
        const auto decoded = json_string_decode_utf8(client_id, {staged.client, sizeof(staged.client)});
        if (!decoded.ok() || decoded.bytes != 32) secure_clear_bytes(staged.client, sizeof(staged.client));
    }
    if (!fields(h.doc, h.record, f) || !equal(f[2], {permalink, canonical.used}) || !f[7].seen) return E::malformed;
    if (station.seed) {
        // Stations publish one finite public queue, without track_count. Bind
        // its string identity, seed kind and seed ID to the requested station before reading it.
        char urn[96], seed[64];
        ProtocolWriter identity{{reinterpret_cast<u8*>(urn), sizeof(urn)}, 0},
            origin{{reinterpret_cast<u8*>(seed), sizeof(seed)}, 0};
        protocol_writer_append_literal(&identity, "soundcloud:system-playlists:");
        const auto type = station.track ? utf8_literal("track-stations:") : utf8_literal("artist-stations:");
        protocol_writer_append_bytes(&identity,reinterpret_cast<const u8*>(type.data),type.bytes);
        protocol_writer_append_u64(&identity, station.seed);
        const auto kind = station.track ? utf8_literal("soundcloud:tracks:") : utf8_literal("soundcloud:users:");
        protocol_writer_append_bytes(&origin,reinterpret_cast<const u8*>(kind.data),kind.bytes); protocol_writer_append_u64(&origin, station.seed);
        Field seed_fields[]{{utf8_literal("urn")}};
        if (expected_id || !equal(f[0], {urn, identity.used}) || !equal(f[9], {urn, identity.used}) ||
            !equal(f[1], utf8_literal("system-playlist")) || !equal(f[10], station.track ? utf8_literal("TRACK_STATION") : utf8_literal("ARTIST_STATION")) ||
            !f[8].seen || !json_value_to_bool(f[8].value, &public_set) || !f[11].seen ||
            !fields(h.doc, f[11].value, seed_fields) || !equal(seed_fields[0], {seed, origin.used})) return E::malformed;
        if (!public_set) return E::unavailable;
        staged.id = station.seed;
    } else {
        if (!f[0].seen || !json_value_to_u64(f[0].value, &staged.id) || !staged.id ||
            (expected_id && expected_id != staged.id) || !equal(f[1], utf8_literal("playlist")) ||
            !equal(f[2], {permalink, canonical.used}) || !f[3].seen || !json_value_to_bool(f[3].value, &public_set) ||
            !f[6].seen || !json_value_to_u64(f[6].value, &count) || !f[7].seen) return E::malformed;
        if (!public_set || !equal(f[4], utf8_literal("public"))) return E::unavailable;
        if (!equal(f[5], utf8_literal("all"))) return E::not_embeddable;
        if (!count) return E::unavailable;
        if (count > kMediaCollectionTracks || count > tracks.count) return E::capacity;
        if (selected >= count) return E::invalid_input;
    }
    struct Tracks { JsonIndexedDocument doc; u32 limit{}, count{}; u64* output{}; MediaTrackTitleSink sink; } list{h.doc, station.seed ? kMediaCollectionTracks : static_cast<u32>(count), 0, nullptr, sink};
    const auto visit = +[](void* opaque, u32, JsonValue value) noexcept {
        auto& list = *static_cast<Tracks*>(opaque);
        if (list.count == list.limit) return false;
        Field f[]{{utf8_literal("id")}, {utf8_literal("kind")}, {utf8_literal("title")}}; u64 id{};
        if (!fields(list.doc, value, f) || !f[0].seen || !json_value_to_u64(f[0].value, &id) || !id ||
            (f[1].seen && !equal(f[1], utf8_literal("track")))) return false;
        if (list.output) {
            list.output[list.count] = id;
            JsonString name{}; MediaTrackTitle title{};
            if (list.sink.write && text(f[2], &name)) {
                const auto decoded = json_string_decode_utf8(name, {title.text, sizeof(title.text)});
                if (decoded.ok() && decoded.bytes) {
                    title.bytes = static_cast<u16>(decoded.bytes);
                    for (u32 i = 0; i < title.bytes; ++i) if (static_cast<u8>(title.text[i]) < 32) title.text[i] = ' ';
                    if (!list.sink.write(list.sink.context, static_cast<u16>(list.count + 1), title)) return false;
                }
            }
        }
        ++list.count;
        return true;
    };
    // Validate first so malformed trailing entries cannot corrupt a live queue.
    if (!json_visit_indexed_array(h.doc, f[7].value, 24, visit, &list).ok() || (!station.seed && list.count != count)) return E::malformed;
    if (!list.count) return E::unavailable;
    if (list.count > tracks.count) return E::capacity;
    if (selected >= list.count) return E::invalid_input;
    list.count = 0; list.output = tracks.data;
    if (!json_visit_indexed_array(h.doc, f[7].value, 24, visit, &list).ok()) return E::capacity;
    staged.count = list.count; staged.selected = selected; *out = staged;
    return E::none;
}

EmbedPlayerError soundcloud_batch_titles(Utf8View json, const SoundcloudTitleBatch& batch, u16 preferred,
    Slice<u32> scratch, MediaTrackTitleSink sink) noexcept {
    using E = EmbedPlayerError;
    if (!batch.count || batch.count > 50 || !sink.write || !json.bytes || json.bytes > 512 * 1024) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    struct Names { JsonIndexedDocument doc; const SoundcloudTitleBatch* batch; MediaTrackTitleSink sink; u64 seen; u16 preferred; bool first; } names{{json,{scratch.data,scan.count}},&batch,sink,0,preferred,true};
    const auto visit = [](void* opaque, u32, JsonValue value) noexcept {
        auto& n = *static_cast<Names*>(opaque);
        Field f[]{{utf8_literal("id")}, {utf8_literal("kind")}, {utf8_literal("title")}, {utf8_literal("public")}};
        u64 id{}; JsonString name{}; bool public_track = true;
        if (!fields(n.doc,value,f) || !f[0].seen || !json_value_to_u64(f[0].value,&id) || !equal(f[1],utf8_literal("track")) ||
            !text(f[2],&name) || (f[3].seen && !json_value_to_bool(f[3].value,&public_track))) return false;
        u32 found = 0;
        while (found < n.batch->count && n.batch->tracks[found].id != id) ++found;
        if (found == n.batch->count || (n.seen & (u64{1} << found))) return false;
        n.seen |= u64{1} << found;
        if (!public_track) return true;
        MediaTrackTitle title{}; const auto decoded = json_string_decode_utf8(name,{title.text,sizeof(title.text)});
        if (!decoded.ok() || !decoded.bytes) return true;
        title.bytes = static_cast<u16>(decoded.bytes);
        for (u32 i=0;i<title.bytes;++i) if (static_cast<u8>(title.text[i]) < 32) title.text[i]=' ';
        for (u32 i=0;i<n.batch->count;++i) if (n.batch->tracks[i].id == id && ((n.batch->tracks[i].index == n.preferred) == n.first))
            if (!n.sink.write(n.sink.context,n.batch->tracks[i].index,title)) return false;
        return true;
    };
    // The demanded title is first, so bounded mailbox capacity cannot starve it.
    for (u32 pass=0;pass<2;++pass) {
        names.first = pass == 0; names.seen=0;
        if (!json_visit_indexed_array(names.doc,{json,0,JsonValueKind::array},24,visit,&names).ok()) return E::malformed;
    }
    return E::none;
}
u32 youtube_player_body(Utf8View id, Utf8View visitor, Slice<u8> output) noexcept {
    if (!video_id(id) || !ascii_graphic_string(visitor, 1024, true)) return 0;
    u8 staged[2048];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    const bool ok = protocol_writer_append_literal(&w, "{\"context\":{\"client\":{\"clientName\":\"VISIONOS\",\"clientVersion\":\"1.02\",\"hl\":\"en\",\"deviceMake\":\"Apple\",\"deviceModel\":\"RealityDevice17,1\",\"osName\":\"visionOS\",\"osVersion\":\"26.5.23O471\",\"visitorData\":") &&
        protocol_writer_append_json_string(&w, visitor) && protocol_writer_append_literal(&w, ",\"userAgent\":") &&
        protocol_writer_append_json_string(&w, kYoutubeNativeUserAgent) && protocol_writer_append_literal(&w, "}},\"videoId\":") &&
        protocol_writer_append_json_string(&w, id) && protocol_writer_append_literal(&w, "}");
    if (!ok || !output.data || output.count < w.used) return 0;
    copy_bytes(output.data, staged, w.used);
    return w.used;
}
} // namespace maicord
