#include "embed_player_internal.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/media/video/encoded_video_limits.hpp"

namespace maicord {
namespace embed_player_detail {
// Callers check the length; only the characters are validated here.
bool id_chars(const Utf8View& id, bool dashes) noexcept {
    if (!id.data) return false;
    for (u32 i = 0; i < id.bytes; ++i)
        if (!ascii_is_alnum(id.data[i]) && !(dashes && (id.data[i] == '_' || id.data[i] == '-'))) return false;
    return true;
}
bool video_id(const Utf8View& id) noexcept { return id.bytes == 11 && id_chars(id, true); }
// Decimal identifier of at most 20 digits without a leading zero; 0 when invalid or above 2^64.
u64 numeric_id(const Utf8View& id) noexcept {
    u64 value = 0;
    return id.data && snowflake_parse(id, &value) ? value : 0;
}
// Value of already validated digits; empty is 0.
u64 decimal(const Utf8View& digits) noexcept {
    u64 value = 0;
    for (u32 i = 0; i < digits.bytes; ++i) value = value * 10 + digits.data[i] - '0';
    return value;
}
u32 id_target(const Utf8View& prefix, const Utf8View& id, const Slice<u8>& output) noexcept {
    if (!output.data || output.count < prefix.bytes + id.bytes) return 0;
    copy_bytes(output.data, prefix.data, prefix.bytes);
    copy_bytes(output.data + prefix.bytes, id.data, id.bytes);
    return prefix.bytes + id.bytes;
}
bool u64_of(const Field& f, u64* out) noexcept { return f.seen && json_value_to_u64(f.value, out); }
bool bool_of(const Field& f, bool* out) noexcept { return f.seen && json_value_to_bool(f.value, out); }
bool text(const Field& f, JsonString* out) noexcept { return f.seen && json_value_as_string(f.value, out); }
bool equal_chars(const Field& f, const char* value, u32 bytes) noexcept {
    JsonString string{};
    return text(f, &string) && json_string_equals_ascii_bytes(string.encoded.data, string.encoded.bytes, value, bytes);
}
bool equal(const Field& f, const Utf8View& value) noexcept { return equal_chars(f, value.data, value.bytes); }
bool has_prefix_chars(const Utf8View& view, const char* prefix, u32 bytes) noexcept {
    return view.bytes >= bytes && bytes_equal(view.data, prefix, bytes);
}
bool is_text_chars(const Utf8View& view, const char* value, u32 bytes) noexcept {
    return view.bytes == bytes && bytes_equal(view.data, value, bytes);
}
bool is_text_nocase_chars(const Utf8View& view, const char* value, u32 bytes) noexcept {
    return ascii_equal_ignore_case(view, {value, bytes});
}
// Decodes into a caller buffer; the scalar arguments avoid building a slice at each call.
JsonStringDecodeResult decode(const JsonString& value, char* out, u32 capacity) noexcept {
    return json_string_decode_utf8(value, {out, capacity});
}
bool visit_array(const JsonIndexedDocument& doc, const JsonValue& value, JsonArrayVisitor visitor, void* context) noexcept {
    return json_visit_indexed_array(doc, value, 24, visitor, context).ok();
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
// Builds the structural index of one JSON document in the caller's scratch.
EmbedPlayerError index_json(const Utf8View& json, const Slice<u32>& scratch, JsonIndexedDocument* doc) noexcept {
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? EmbedPlayerError::capacity : EmbedPlayerError::malformed;
    *doc = {json, {scratch.data, scan.count}};
    return EmbedPlayerError::none;
}
// Control bytes are not displayable in a title.
void sanitize_title(MediaTrackTitle* title) noexcept {
    for (u32 i = 0; i < title->bytes; ++i) if (static_cast<u8>(title->text[i]) < 32) title->text[i] = ' ';
}
JsonStringDecodeResult decode_title(const JsonString& name, MediaTrackTitle* title) noexcept {
    const auto decoded = decode(name, title->text, sizeof(title->text));
    if (decoded.ok()) { title->bytes = static_cast<u16>(decoded.bytes); sanitize_title(title); }
    return decoded;
}
// True when a store track page names track `id` (not a music video) in `country`.
bool track_page_is(const JsonString& page, u64 id, const Utf8View& country, u32 capacity) noexcept {
    char url[1024];
    const auto decoded = decode(page, url, capacity);
    const auto match = decoded.ok() ? apple_music_track_locator({url, decoded.bytes}) : AppleMusicTrackLocator{};
    return decimal(match.id) == id && !match.video && ascii_equal_ignore_case(match.country, country);
}
// Publishes a decoded, validated stream URL and wipes the staging copy either way.
EmbedPlayerError publish_stream(EmbedStream* staged, u32 url_bytes, bool valid, EmbedStream* output) noexcept {
    if (valid) { staged->url_bytes = url_bytes; *output = *staged; }
    secure_clear_bytes(staged, sizeof(*staged));
    return valid ? EmbedPlayerError::none : EmbedPlayerError::malformed;
}
EmbedPlayerError embedded_json(Utf8View html, Utf8View marker, Utf8View* output, bool array) noexcept {
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
    FieldSet<"url", "mimeType", "contentLength", "bitrate", "width", "height", "audioQuality", "audioChannels",
        "audioTrack", "drmFamilies", "signatureCipher", "cipher", "isDrc"> f;
    if (!fields(s.doc, value, f)) return false;
    if (f[9].seen || f[10].seen || f[11].seen || !f[0].seen) return true;
    Candidate c{};
    JsonString mime{};
    char mime_bytes[128];
    if (!text(f[0], &c.url) || !text(f[1], &mime)) return false;
    const auto decoded = decode(mime, mime_bytes, sizeof(mime_bytes));
    if (!decoded.ok()) return true;
    const Utf8View type{mime_bytes, decoded.bytes};
    const bool video = has_prefix(type, "video/mp4; codecs=\"avc1.");
    const bool audio = is_text(type, "audio/mp4; codecs=\"mp4a.40.2\"");
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
            FieldSet<"audioIsDefault"> track;
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
    const auto decoded = decode(candidate.url, out->url, sizeof(out->url));
    if (!decoded.ok() || !youtube_stream_url({out->url, decoded.bytes})) return false;
    out->url_bytes = decoded.bytes;
    out->content_bytes = candidate.content;
    out->bitrate = candidate.bitrate;
    out->width = static_cast<u16>(candidate.width);
    out->height = static_cast<u16>(candidate.height);
    return true;
}
struct TikTokSelection { JsonIndexedDocument doc; Utf8View id; u64 now; u32 height, items, profiles; EmbedStreams best{}; };
u64 stream_expiry_chars(const Utf8View& url, const char* key_data, u32 key_bytes, bool* present) noexcept {
    const Utf8View key{key_data, key_bytes};
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
u64 stream_expiry(const Utf8View& url) noexcept { return stream_expiry(url, "expire="); }
struct TikTokUrls { TikTokSelection* selection; EmbedStream* stream; u32 expires, count; };
bool tiktok_url(void* opaque, u32, JsonValue value) noexcept {
    auto& u = *static_cast<TikTokUrls*>(opaque);
    if (++u.count > 8) return false;
    if (u.expires) return true;
    JsonString text{};
    if (!json_value_as_string(value, &text)) return false;
    const auto decoded = decode(text, u.stream->url, sizeof(u.stream->url));
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
    FieldSet<"codec_type", "bitrate", "play_addr"> f;
    if (!fields(s.doc, value, f)) return false;
    if (!equal(f[0], "h264")) return true;
    FieldSet<"width", "height", "data_size", "url_list"> address;
    if (!f[2].seen || !fields(s.doc, f[2].value, address)) return false;
    EmbedStreams candidate{};
    u32 width = 0, height = 0;
    if (!number(address[0], &width) || !number(address[1], &height) || !width || !height ||
        !number(address[2], &candidate.video.content_bytes) || !candidate.video.content_bytes ||
        !number(f[1], &candidate.video.bitrate) || !candidate.video.bitrate || !address[3].seen) return false;
    if (width > 2048 || height > 2048 || candidate.video.content_bytes > kMediaMaximumProgressiveVideoBytes) return true;
    candidate.video.width = static_cast<u16>(width); candidate.video.height = static_cast<u16>(height);
    TikTokUrls urls{&s, &candidate.video};
    if (!visit_array(s.doc, address[3].value, tiktok_url, &urls)) return false;
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
    FieldSet<"id_str", "video_info"> f;
    if (!fields(s.doc, value, f) || !equal(f[0], s.id)) return false;
    if (!f[1].seen) return true; // Photos and other non-video posts.
    FieldSet<"profiles"> video;
    if (!fields(s.doc, f[1].value, video)) return false;
    return !video[0].seen || visit_array(s.doc, video[0].value, tiktok_profile, &s);
}
struct ClipSelection { JsonIndexedDocument doc; EmbedStream best{}; u32 height, assets{}, profiles{}; };
bool clip_quality(void* opaque, u32, JsonValue value) noexcept {
    auto& s = *static_cast<ClipSelection*>(opaque);
    if (++s.profiles > 32) return false;
    FieldSet<"videoCodec", "height", "width", "bitrate", "sourceURL"> f;
    if (!fields(s.doc, value, f)) return false;
    if (!equal(f[0], "AVC")) return true;
    u32 height = 0, width = 0, bitrate = 0;
    JsonString source{};
    if (!number(f[1], &height) || !number(f[2], &width) || !number(f[3], &bitrate) ||
        !height || !width || !text(f[4], &source)) return false;
    if (height > 2048 || width > 2048) return true;
    if (!better_rendition(height, bitrate, s.best.height, s.best.bitrate, s.height)) return true;
    EmbedStream candidate{};
    const auto decoded = decode(source, candidate.url, sizeof(candidate.url));
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
    FieldSet<"videoQualities"> f;
    return fields(s.doc, value, f) && f[0].seen &&
        visit_array(s.doc, f[0].value, clip_quality, &s);
}
// The dimensions in the public MP4 path describe the rendition, unlike the
// post's original_info dimensions. Keep them tied to the exact media host.
bool twitter_location(Utf8View url, EmbedStreamLocation* out, u16* width, u16* height) noexcept {
    constexpr auto origin=utf8_literal("https://video.twimg.com");
    if(!url.data || url.bytes>kEmbedStreamUrlBytes || !utf8_starts_with(url,origin)) return false;
    const Utf8View target{url.data+origin.bytes,url.bytes-origin.bytes};
    const bool amplify=has_prefix(target, "/amplify_video/");
    const auto prefix=amplify?utf8_literal("/amplify_video/"):utf8_literal("/ext_tw_video/");
    if(!utf8_starts_with(target,prefix)) return false;
    u32 at=prefix.bytes, start=at;
    while(at<target.bytes && ascii_is_digit(target.data[at])) ++at;
    if(!twitter_post_id({target.data+start,at-start})) return false;
    const auto path=amplify?utf8_literal("/vid/"):utf8_literal("/pu/vid/");
    if(!utf8_starts_with({target.data+at,target.bytes-at},path)) return false;
    at+=path.bytes;
    if(has_prefix({target.data+at,target.bytes-at}, "avc1/")) at+=5;
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
    if(at==start || at-start>128 || !has_prefix({target.data+at,target.bytes-at}, ".mp4")) return false;
    at+=4;
    if(at!=target.bytes) {
        if(!has_prefix({target.data+at,target.bytes-at}, "?tag=")) return false;
        at+=5;start=at;
        while(at<target.bytes && ascii_is_digit(target.data[at])) ++at;
        if(at==start || at-start>10 || at!=target.bytes) return false;
    }
    if(out) *out={{url.data+8,origin.bytes-8},target};
    if(width) *width=static_cast<u16>(dimensions[0]);
    if(height) *height=static_cast<u16>(dimensions[1]);
    return true;
}
bool twitter_media(void* context,u32 index,JsonValue value) noexcept {
    auto& s=*static_cast<TwitterMedia*>(context);
    if(++s.count>4) return false;
    FieldSet<"type"> f;
    if(!fields(s.doc,value,f)) return false;
    if((!s.index && !s.found) || s.index==index+1) {
        if(equal(f[0], "video") || equal(f[0], "animated_gif")) {
            s.selected=value;s.found=true;
        }
    }
    return true;
}
bool twitter_variant(void* context,u32,JsonValue value) noexcept {
    auto& s=*static_cast<TwitterVariants*>(context);
    if(++s.count>16) return false;
    FieldSet<"content_type", "url", "bitrate"> f;
    if(!fields(s.doc,value,f)) return false;
    if(!equal(f[0], "video/mp4")) return true;
    JsonString url{}; EmbedStream candidate{};
    if(!text(f[1],&url) || (f[2].seen && !number(f[2],&candidate.bitrate))) return false;
    const auto decoded=decode(url, candidate.url, sizeof(candidate.url));
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
} // namespace embed_player_detail
using namespace embed_player_detail;
bool twitch_clip_id(const Utf8View& id) noexcept {
    return id.bytes && id.bytes <= 128 && id_chars(id, true);
}
u32 twitch_clip_body(Utf8View id, Slice<u8> out) noexcept {
    if (!twitch_clip_id(id)) return 0;
    u8 staged[1024];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    constexpr auto query = utf8_literal("query($id:ID!){clip(slug:$id){slug playbackAccessToken(params:{platform:\"web\",playerType:\"clips-viewing\"}){signature value} assets{videoQualities(params:{supportedCodecs:[AVC]}){videoCodec height width bitrate sourceURL}}}}");
    if (!protocol_write(&w, "{\"query\":", json_text(query), ",\"variables\":{\"id\":", json_text(id), "}}") ||
        !out.data || out.count < w.used) return 0;
    copy_bytes(out.data, staged, w.used);
    return w.used;
}
EmbedPlayerError twitch_clip_streams(Utf8View json, Utf8View id, u16 height, u64 now,
    Slice<u32> scratch, EmbedStreams* out) noexcept {
    using E = EmbedPlayerError;
    if (!out || !twitch_clip_id(id) || !height || height > 2048 || !now ||
        !json.data || !json.bytes || json.bytes > 2u * 1024u * 1024u) return E::invalid_input;
    ClipSelection s{{}, {}, height};
    if (const auto indexed = index_json(json, scratch, &s.doc); indexed != E::none) return indexed;
    FieldSet<"data", "errors"> root;
    if (!fields(s.doc, root)) return E::malformed;
    if (root[1].seen) return E::unavailable;
    FieldSet<"clip"> data;
    if (!root[0].seen || !fields(s.doc, root[0].value, data) || !data[0].seen) return E::malformed;
    if (data[0].value.kind == JsonValueKind::null_value) return E::unavailable;
    FieldSet<"slug", "playbackAccessToken", "assets"> clip;
    if (!fields(s.doc, data[0].value, clip) || !equal(clip[0], id) || !clip[1].seen || !clip[2].seen) return E::malformed;
    if (!visit_array(s.doc, clip[2].value, clip_asset, &s)) return E::malformed;
    if (!s.best.url_bytes) return E::unsupported;
    FieldSet<"signature", "value"> access;
    JsonString signature{}, token{};
    if (!fields(s.doc, clip[1].value, access) || !text(access[0], &signature) || !text(access[1], &token)) return E::malformed;
    char signature_text[128], token_text[1024];
    const auto sig = decode(signature, signature_text, sizeof(signature_text));
    const auto tok = decode(token, token_text, sizeof(token_text));
    if (!sig.ok() || !sig.bytes || !tok.ok()) return E::malformed;
    for (u32 i = 0; i < sig.bytes; ++i) if (!ascii_is_hex_digit(signature_text[i])) return E::malformed;
    const Utf8View token_json{token_text, tok.bytes};
    // Reuse the caller's structural index after all outer fields are consumed.
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(token_json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"clip_slug", "expires", "authorization"> claims;
    FieldSet<"forbidden"> authorization;
    u64 expires = 0;
    bool forbidden = true;
    if (!fields(doc, claims) || !equal(claims[0], id) ||
        !u64_of(claims[1], &expires) || !claims[2].seen ||
        !fields(doc, claims[2].value, authorization) ||
        !bool_of(authorization[0], &forbidden)) return E::malformed;
    if (forbidden || expires <= now) return E::unavailable;
    ProtocolWriter w{{reinterpret_cast<u8*>(s.best.url), sizeof(s.best.url)}, s.best.url_bytes};
    const bool written = protocol_write(&w, "?sig=", percent_text({signature_text, sig.bytes}), "&token=",
                                        percent_text(token_json));
    secure_clear_bytes(token_text, sizeof(token_text));
    secure_clear_bytes(signature_text, sizeof(signature_text));
    if (!written) { secure_clear_bytes(&s.best, sizeof(s.best)); return E::capacity; }
    s.best.url_bytes = w.used;
    *out = {s.best, {}, static_cast<u32>(minimum<u64>(expires - now, 86400)), true};
    secure_clear_bytes(&s.best, sizeof(s.best));
    return E::none;
}
u32 tiktok_player_target(Utf8View id, Slice<u8> output) noexcept {
    if (!numeric_id(id)) return 0;
    ProtocolWriter w{output, 0};
    return protocol_write(&w, "/player/api/v1/items?item_ids=", id, "&language=en&aid=1459&data_source=web_core") ? w.used : 0;
}
EmbedPlayerError tiktok_player_streams(Utf8View json, Utf8View id, u16 height, u64 now,
    Slice<u32> scratch, EmbedStreams* out) noexcept {
    using E = EmbedPlayerError;
    u8 target[128];
    if (!out || !height || height > 2048 || !now || !tiktok_player_target(id, {target, sizeof(target)}) ||
        !json.data || !json.bytes || json.bytes > 2u * 1024u * 1024u) return E::invalid_input;
    TikTokSelection s{{}, id, now, height};
    if (const auto indexed = index_json(json, scratch, &s.doc); indexed != E::none) return indexed;
    FieldSet<"status_code", "items"> root;
    u32 status = 0;
    if (!fields(s.doc, root) || !number(root[0], &status)) return E::malformed;
    if (status) return E::unavailable;
    if (!root[1].seen || !visit_array(s.doc, root[1].value, tiktok_item, &s)) return E::malformed;
    if (!s.best.video.url_bytes) return E::unsupported;
    *out = s.best;
    secure_clear_bytes(&s.best, sizeof(s.best));
    return E::none;
}
} // namespace maicord
