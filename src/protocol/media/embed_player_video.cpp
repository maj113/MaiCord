#include "embed_player_internal.hpp"
#include "maicord/protocol/media/cdn_http.hpp"

namespace maicord {
using namespace embed_player_detail;
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
            !has_prefix(target, "/videos/")) return false;
        u32 path_end = 0;
        for (; path_end < target.bytes && target.data[path_end] != '?'; ++path_end) {
            const char c = target.data[path_end];
            if (!ascii_is_alnum(c) && c != '/' && c != '_' && c != '-' && c != '.') return false;
            if (c == '.' && path_end && target.data[path_end - 1] == '.') return false;
        }
        if (path_end < 4 || !is_text({target.data + path_end - 4, 4}, ".mp4") ||
            !stream_expiry(url, "validto=")) return false;
    } else if (cdn == CdnHost::youtube_playlist) {
        if (!is_text_nocase(host, "manifest.googlevideo.com") ||
            (!has_prefix(target, "/api/manifest/hls_variant/") &&
             !has_prefix(target, "/api/manifest/hls_playlist/"))) return false;
    } else if (cdn == CdnHost::youtube_live) {
        if (host.bytes<=suffix.bytes || !ascii_equal_ignore_case({host.data+host.bytes-suffix.bytes,suffix.bytes},suffix) ||
            !has_prefix(target, "/videoplayback/")) return false;
    } else if (cdn == CdnHost::google_video) {
        if (host.bytes <= suffix.bytes || !ascii_equal_ignore_case({host.data + host.bytes - suffix.bytes, suffix.bytes}, suffix) ||
            !has_prefix(target, "/videoplayback?")) return false;
    } else if (cdn == CdnHost::tiktok_video) {
        constexpr auto tail = utf8_literal("-webapp-prime.tiktok.com");
        if (host.bytes < tail.bytes + 2 || host.bytes > tail.bytes + 5 || ascii_lower(host.data[0]) != 'v' ||
            !ascii_equal_ignore_case({host.data + host.bytes - tail.bytes, tail.bytes}, tail) ||
            !has_prefix(target, "/video/tos/")) return false;
        for (u32 i = 1; i < host.bytes - tail.bytes; ++i)
            if (!ascii_is_digit(host.data[i]) || (i == 1 && host.data[i] == '0')) return false;
    } else if (cdn == CdnHost::twitch_video) {
        // A specific Twitch distribution; accepting arbitrary CloudFront
        // tenants would let metadata redirect media fetches to unrelated sites.
        if (!is_text_nocase(host, "d1ndex63qxojbr.cloudfront.net") ||
            !has_prefix(target, "/nauth/")) return false;
        u32 path_end = 0;
        while (path_end < target.bytes && target.data[path_end] != '?') ++path_end;
        if (path_end < 4 || !is_text({target.data + path_end - 4, 4}, ".mp4")) return false;
    } else if (cdn == CdnHost::imgur_video) {
        if(!is_text_nocase(host, "i.imgur.com") || target.bytes<6 || target.data[0]!='/' ||
            !is_text({target.data+target.bytes-4,4}, ".mp4") ||
            !imgur_video_id({target.data+1,target.bytes-5})) return false;
    } else if (cdn == CdnHost::streamable_video) {
        static constexpr Utf8View hosts[]{utf8_literal("cdn-cf-east.streamable.com"), utf8_literal("cdn-cf-west.streamable.com"),
            utf8_literal("cdn-b-east.streamable.com"), utf8_literal("cdn-b-west.streamable.com")};
        bool allowed = false;
        for (auto name : hosts) allowed = allowed || ascii_equal_ignore_case(host, name);
        const u32 prefix_bytes = has_prefix(target, "/video/mp4/") ? 11
            : has_prefix(target, "/video/mp4-mobile/") ? 18 : 0;
        u32 path_end = 0;
        while (path_end < target.bytes && target.data[path_end] != '?') ++path_end;
        if (!allowed || !prefix_bytes || path_end <= prefix_bytes + 4 ||
            !is_text({target.data + path_end - 4, 4}, ".mp4") ||
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
        if((!is_text_nocase(host, "d28julafmv4ekl.cloudfront.net") &&
            !is_text_nocase(host, "d2q1srilgjznst.cloudfront.net")) || !target.bytes || target.data[0]!='/') return false;
        u32 at=1;
        for(u32 part=0;part<3;++part) {
            const u32 start=at;
            while(at<target.bytes && ascii_is_digit(target.data[at])) ++at;
            if(at==start || at-start>(part==2?16u:3u)) return false;
            if(part<2) {
                if(at+3>target.bytes || !is_text_nocase({target.data+at,3}, "%2F")) return false;
                at+=3;
            }
        }
        if(!has_prefix({target.data+at,target.bytes-at}, "_S64.mp3?")) return false;
        if(!stream_expiry(url, "Expires=")) return false;
    } else if (cdn == CdnHost::spotify_preview && is_text_nocase(host, "podz-content.spotifycdn.com")) {
        constexpr auto path = utf8_literal("/audio/clips/");
        if (!utf8_starts_with(target,path) || target.bytes < path.bytes+22+1 ||
            !spotify_audio_id({target.data+path.bytes,22}) || target.data[path.bytes+22]!='/') return false;
        u32 at=path.bytes+23;
        if (!has_prefix({target.data+at,target.bytes-at}, "clip_")) return false;
        at+=5;
        for(u32 part=0;part<2;++part) {
            const u32 first=at;
            while(at<target.bytes && ascii_is_digit(target.data[at])) ++at;
            if(at==first || at-first>10) return false;
            if(!part && (at==target.bytes || target.data[at++]!='_')) return false;
        }
        if(!is_text({target.data+at,target.bytes-at}, ".mp3")) return false;
    } else if (cdn == CdnHost::spotify_preview) {
        constexpr auto path = utf8_literal("/mp3-preview/");
        if (!is_text_nocase(host, "p.scdn.co") || !utf8_starts_with(target, path)) return false;
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
u32 embed_stream_get(CdnHost cdn, Utf8View url, u64 first, u32 bytes, Slice<u8> output) noexcept {
    EmbedStreamLocation location{};
    const bool whole_preview = (embed_audio_preview_limit(cdn) || embed_playlist_host(cdn)) && !bytes && !first;
    if (embed_playlist_host(cdn) && !whole_preview) return 0;
    if (!embed_stream_location(cdn, url, &location) || (!bytes && !whole_preview) || bytes > kMediaMaximumProgressiveVideoBytes ||
        first > kMediaMaximumProgressiveVideoBytes - u64{bytes} || !output.data) return 0;
    u8 staged[kCdnMaximumRequestHeadBytes];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    const bool ok = protocol_write(&w, "GET ", location.target, " HTTP/1.1\r\nHost: ", location.host, "\r\nUser-Agent: ",
            cdn == CdnHost::google_video ? kYoutubeNativeUserAgent : utf8_literal("Mozilla/5.0")) &&
        (cdn != CdnHost::tiktok_video || protocol_write(&w, "\r\nReferer: https://www.tiktok.com/")) &&
        (cdn != CdnHost::pornhub_video || protocol_write(&w, "\r\nReferer: https://www.pornhub.com/")) &&
        protocol_write(&w, "\r\nAccept: */*\r\nAccept-Encoding: identity") &&
        (whole_preview || protocol_write(&w, "\r\nRange: bytes=", first, '-', first + bytes - 1)) &&
        protocol_write(&w, "\r\nConnection: keep-alive\r\n\r\n") && w.used <= output.count;
    if (ok) copy_bytes(output.data, staged, w.used);
    secure_clear_bytes(staged, sizeof(staged));
    return ok ? w.used : 0;
}
EmbedPlayerError youtube_player_streams(Utf8View json, Utf8View id, u16 maximum_height,
    Slice<u32> scratch, EmbedStreams* output) noexcept {
    using E = EmbedPlayerError;
    if (!output || !video_id(id) || !maximum_height || maximum_height > 2048 ||
        !json.data || !json.bytes || json.bytes > 2u * 1024u * 1024u) return E::invalid_input;
    Selection s{{}, {}, {}, {}, maximum_height, 0};
    if (const auto indexed = index_json(json, scratch, &s.doc); indexed != E::none) return indexed;
    FieldSet<"playabilityStatus", "videoDetails", "streamingData"> root;
    if (!fields(s.doc, root) || !root[0].seen) return E::malformed;
    FieldSet<"status", "playableInEmbed"> status;
    if (!fields(s.doc, root[0].value, status)) return E::malformed;
    if (equal(status[0], "LOGIN_REQUIRED")) return E::login_required;
    if (!equal(status[0], "OK")) return status[0].seen ? E::unavailable : E::malformed;
    bool embeddable = false;
    if (!bool_of(status[1], &embeddable)) return E::malformed;
    if (!embeddable) return E::not_embeddable;
    FieldSet<"videoId", "isLive"> details;
    if (!root[1].seen || !fields(s.doc, root[1].value, details) || !equal(details[0], id)) return E::malformed;
    if (!root[2].seen) return E::unsupported;
    FieldSet<"formats", "adaptiveFormats", "expiresInSeconds", "hlsManifestUrl"> data;
    if (!fields(s.doc, root[2].value, data)) return E::malformed;
    u32 expires = 0;
    if (!number(data[2], &expires) || !expires || expires > 86400) return E::malformed;
    bool live=false;
    if(details[1].seen && !json_value_to_bool(details[1].value,&live)) return E::malformed;
    if(live) {
        JsonString manifest{};EmbedStreams staged{};EmbedStreamLocation location{};
        if(!text(data[3],&manifest)) return E::unsupported;
        const auto decoded=decode(manifest, staged.video.url, sizeof(staged.video.url));
        if(!decoded.ok() || !embed_stream_location(CdnHost::youtube_playlist,{staged.video.url,decoded.bytes},&location) ||
            !has_prefix(location.target, "/api/manifest/hls_variant/")) return E::malformed;
        staged.video.url_bytes=decoded.bytes;staged.expires_seconds=expires;staged.live=true;*output=staged;return E::none;
    }
    for (u32 i = 0; i < 2; ++i)
        if (data[i].seen && !visit_array(s.doc, data[i].value, format, &s))
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
    const auto expires=stream_expiry(url, "Expires=");
    if(expires<=now) return E::unavailable;
    EmbedStreams staged{};
    copy_bytes(staged.audio.url,url.data,url.bytes);staged.audio.url_bytes=url.bytes;
    staged.expires_seconds=static_cast<u32>(minimum<u64>(expires-now,3600));
    *out=staged;secure_clear_bytes(&staged,sizeof(staged));return E::none;
}
u64 twitter_post_id(Utf8View id) noexcept { return numeric_id(id); }
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
    protocol_write(&w,"/tweet-result?id=",id,"&lang=en&token=");
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
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"__typename", "id_str", "mediaDetails"> root;
    if(!fields(doc,root)) return E::malformed;
    if(equal(root[0], "TweetTombstone")) return E::unavailable;
    if(!equal(root[0], "Tweet") || !equal(root[1],id)) return E::malformed;
    if(!root[2].seen) return E::unsupported;
    TwitterMedia media{doc,{},media_index};
    if(!visit_array(doc, root[2].value, twitter_media, &media)) return E::malformed;
    if(!media.found) return E::unsupported;
    FieldSet<"video_info", "ext_media_availability", "additional_media_info"> selected;
    if(!fields(doc,media.selected,selected)) return E::malformed;
    if(selected[1].seen) {
        FieldSet<"status"> availability;
        if(!fields(doc,selected[1].value,availability)) return E::malformed;
        if(!equal(availability[0], "Available")) return E::unavailable;
    }
    if(selected[2].seen) {
        FieldSet<"embeddable"> info;bool allowed=true;
        if(!fields(doc,selected[2].value,info) ||
            (info[0].seen && !json_value_to_bool(info[0].value,&allowed))) return E::malformed;
        if(!allowed) return E::not_embeddable;
    }
    FieldSet<"variants"> video;
    if(!selected[0].seen || !fields(doc,selected[0].value,video) || !video[0].seen) return E::malformed;
    TwitterVariants variants{doc,{},height};
    E error=E::unsupported;
    if(!visit_array(doc, video[0].value, twitter_variant, &variants)) error=E::malformed;
    else if(variants.selected.url_bytes) {
        reset_value(output);output->video=variants.selected;output->combined=true;output->expires_seconds=3600;
        error=E::none;
    }
    secure_clear_bytes(&variants.selected,sizeof(variants.selected));
    return error;
}
bool pornhub_video_id(Utf8View id) noexcept {
    if (!id.data) return false;
    if (has_prefix(id, "ph")) id = {id.data + 2, id.bytes - 2};
    if (id.bytes < 13 || id.bytes > 32) return false;
    for (u32 i = 0; i < id.bytes; ++i) if (!ascii_is_hex_digit(id.data[i])) return false;
    return true;
}
u32 pornhub_embed_target(Utf8View id, Slice<u8> output) noexcept {
    return pornhub_video_id(id) ? id_target(utf8_literal("/embed/"), id, output) : 0;
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
    if (end == url.bytes || (!is_text(host, "vod-adaptive-ak.vimeocdn.com") &&
        !is_text(host, "vod-adaptive.vimeocdn.com"))) return false;
    const u32 path = end;
    while (end < url.bytes && url.data[end] != '?') ++end;
    u32 token_end = path + 1;
    while (token_end < end && url.data[token_end] != '/') ++token_end;
    const bool signed_prefix = has_prefix({url.data + path, end - path}, "/exp=");
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
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"video", "request"> root;
    FieldSet<"id", "privacy", "embed_permission", "unlisted_hash"> video;
    u32 actual{};
    if (!fields(doc, root) || !root[0].seen || !root[1].seen ||
        !fields(doc, root[0].value, video) || !number(video[0], &actual) || actual != steam_id(id)) return E::malformed;
    const bool unlisted = privacy_hash.bytes && equal(video[1], "unlisted") && equal(video[3], privacy_hash);
    if ((!equal(video[1], "anybody") && !unlisted) || !equal(video[2], "public")) return E::not_embeddable;
    FieldSet<"files"> request;
    FieldSet<"hls"> files;
    FieldSet<"default_cdn", "cdns"> hls;
    if (!fields(doc, root[1].value, request) || !request[0].seen ||
        !fields(doc, request[0].value, files) || !files[0].seen ||
        !fields(doc, files[0].value, hls) || !hls[0].seen || !hls[1].seen) return E::unsupported;
    char name[65]; JsonString selected{};
    if (!text(hls[0], &selected)) return E::malformed;
    const auto decoded = decode(selected, name + 1, 64);
    if (!decoded.ok() || !decoded.bytes) return E::malformed;
    name[0] = static_cast<char>(decoded.bytes); // length-prefixed member name
    Field cdn{};
    FieldSet<"url"> record;
    if (!fields_n(doc, hls[1].value, 24, &cdn, 1, reinterpret_cast<const u8*>(name)) || !cdn.seen || !fields(doc, cdn.value, record) ||
        !text(record[0], &selected)) return E::malformed;
    EmbedStream staged{};
    const auto url = decode(selected, staged.url, sizeof(staged.url));
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
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"link_url", "video_unavailable", "video_unavailable_country", "mediaDefinitions"> root;
    if (!fields(doc, root) || !root[1].seen || !root[3].seen) return E::malformed;
    for (u32 i = 1; i <= 2; ++i) {
        if (!root[i].seen) continue;
        bool blocked = true;
        if (equal(root[i], utf8_literal("false"))) blocked = false;
        else if (!json_value_to_bool(root[i].value, &blocked)) return E::malformed;
        if (blocked) return E::unavailable;
    }
    JsonString link{}; char identity[512];
    if (!text(root[0], &link)) return E::malformed;
    const auto decoded = decode(link, identity, sizeof(identity));
    const auto locator = decoded.ok() ? embed_video_locator({identity, decoded.bytes}) : EmbedVideoLocator{};
    if (locator.provider != EmbedProvider::pornhub || !utf8_equal(locator.id, id)) return E::malformed;
    struct Metadata { JsonIndexedDocument doc; EmbedStream target{}; u32 count{}; } selected{doc};
    const auto visited = json_visit_indexed_array(doc, root[3].value, 24,
        +[](void* context, u32 index, JsonValue value) noexcept {
            auto& s = *static_cast<Metadata*>(context);
            if (index >= 32) return false;
            FieldSet<"format", "remote", "videoUrl"> f;
            if (!fields(s.doc, value, f)) return false;
            if (!equal(f[0], "mp4")) return true;
            bool remote = false; JsonString url{};
            if (!bool_of(f[1], &remote) || !remote || !text(f[2], &url) || ++s.count != 1) return false;
            const auto result = decode(url, s.target.url, sizeof(s.target.url));
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
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    struct Selection { JsonIndexedDocument doc; EmbedStreams streams{}; u64 now; u16 height; } selected{doc, {}, now, height};
    const auto visited = json_visit_indexed_array(doc, {json, 0, JsonValueKind::array}, 24,
        +[](void* context, u32 index, JsonValue value) noexcept {
            auto& s = *static_cast<Selection*>(context);
            if (index >= 32) return false;
            FieldSet<"format", "width", "height", "videoUrl"> f;
            if (!fields(s.doc, value, f)) return false;
            if (!equal(f[0], "mp4")) return true;
            u32 width = 0, height = 0; JsonString url{};
            if (!number(f[1], &width) || !number(f[2], &height) || !width || !height || !text(f[3], &url)) return false;
            if (width > 2048 || height > 2048) return true;
            EmbedStream candidate{};
            const auto decoded = decode(url, candidate.url, sizeof(candidate.url));
            const Utf8View source{candidate.url, decoded.bytes};
            const bool valid = decoded.ok() && embed_stream_location(CdnHost::pornhub_video, source);
            const u64 expiry = valid ? stream_expiry(source, "validto=") : 0;
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
bool imgur_video_id(Utf8View id) noexcept { return (id.bytes == 5 || id.bytes == 7) && id_chars(id); }
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
bool streamable_video_id(Utf8View id) noexcept { return id.bytes >= 3 && id.bytes <= 12 && id_chars(id); }
u32 streamable_video_target(Utf8View id, Slice<u8> output) noexcept {
    return streamable_video_id(id) ? id_target(utf8_literal("/videos/"), id, output) : 0;
}
EmbedPlayerError streamable_video_streams(Utf8View json, Utf8View id, u16 height, u64 now,
    Slice<u32> scratch, EmbedStreams* output) noexcept {
    using E = EmbedPlayerError;
    if (!output || !streamable_video_id(id) || !height || height > 2048 || !now ||
        !json.data || !json.bytes || json.bytes > kStreamableMetadataMaximumBytes) return E::invalid_input;
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"status", "url", "files"> root;
    u32 status = 0;
    if (!fields(doc, root) || !number(root[0], &status)) return E::malformed;
    if (status != 2) return E::unavailable;
    char canonical[40]; constexpr auto host = utf8_literal("https://streamable.com/");
    copy_bytes(canonical, host.data, host.bytes); copy_bytes(canonical + host.bytes, id.data, id.bytes);
    if ((!equal(root[1], {canonical, host.bytes + id.bytes}) &&
         !equal(root[1], {canonical + 8, host.bytes + id.bytes - 8})) || !root[2].seen) return E::malformed;
    FieldSet<"mp4-mobile", "mp4"> files;
    if (!fields(doc, root[2].value, files)) return E::malformed;
    EmbedStreams staged{};
    E error = E::unsupported;
    for (auto file : files) {
        if (!file.seen) continue;
        FieldSet<"status", "url", "height", "width", "size", "bitrate"> f;
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
        const auto decoded = decode(source, candidate.url + 6, sizeof(candidate.url) - 6);
        if (!decoded.ok()) { secure_clear_bytes(&candidate, sizeof(candidate)); error = E::malformed; break; }
        if (has_prefix({candidate.url + 6, decoded.bytes}, "//")) {
            copy_bytes(candidate.url, "https:", 6); candidate.url_bytes = decoded.bytes + 6;
        } else {
            for (u32 i = 0; i < decoded.bytes; ++i) candidate.url[i] = candidate.url[i + 6];
            candidate.url_bytes = decoded.bytes;
        }
        const Utf8View url{candidate.url, candidate.url_bytes};
        EmbedStreamLocation location{};
        const bool located = embed_stream_location(CdnHost::streamable_video, url, &location);
        const u32 prefix_bytes = located && has_prefix(location.target, "/video/mp4/") ? 11 : 18;
        const bool same_id = located && location.target.bytes > prefix_bytes + id.bytes &&
            bytes_equal(location.target.data + prefix_bytes, id.data, id.bytes) && location.target.data[prefix_bytes + id.bytes] == '.';
        bool modern_seen = false, legacy_seen = false;
        const u64 expires = stream_expiry(url, "Expires=", &modern_seen);
        const u64 legacy = stream_expiry(url, "expires=", &legacy_seen);
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
} // namespace maicord
