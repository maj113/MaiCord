#include "embed_player_internal.hpp"

namespace maicord {
using namespace embed_player_detail;
bool apple_music_track_id(const Utf8View& id) noexcept { return numeric_id(id) != 0; }
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
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"resultCount", "results"> root;
    u32 count = 0;
    if (!fields(doc, root) || !number(root[0], &count)) return E::malformed;
    if (!count) return E::unavailable;
    if (count != 1 || !root[1].seen) return E::malformed;
    JsonValue track{};
    if (!json_visit_indexed_array(doc, root[1].value, 24,
        +[](void* context, u32 index, JsonValue value) noexcept {
            if (index || value.kind != JsonValueKind::object) return false;
            *static_cast<JsonValue*>(context) = value; return true;
        }, &track).ok()) return E::malformed;
    FieldSet<"wrapperType", "kind", "trackId", "previewUrl", "trackViewUrl"> f;
    u64 identity = 0;
    const u64 expected = decimal(id);
    if (!fields(doc, track, f) || !equal(f[0], "track") ||
        !equal(f[1], video ? utf8_literal("music-video") : utf8_literal("song")) ||
        !u64_of(f[2], &identity) || identity != expected) return E::malformed;
    if (!f[3].seen || f[3].value.kind == JsonValueKind::null_value) return E::unsupported;
    JsonString view{}, source{};
    char link[512];
    if (!text(f[4], &view) || !text(f[3], &source)) return E::malformed;
    const auto decoded_view = decode(view, link, sizeof(link));
    const auto matched = decoded_view.ok() ? apple_music_track_locator({link, decoded_view.bytes}) : AppleMusicTrackLocator{};
    if (!utf8_equal(matched.id, id) || !ascii_equal_ignore_case(matched.country, country) || matched.video != video) return E::malformed;
    EmbedStream staged{};
    const auto decoded = decode(source, staged.url, sizeof(staged.url));
    return publish_stream(&staged, decoded.bytes, decoded.ok() &&
        embed_stream_location(video ? CdnHost::apple_music_video_preview : CdnHost::apple_music_preview, {staged.url, decoded.bytes}),
        output);
}
EmbedPlayerError apple_music_album(Utf8View json, Utf8View id, Utf8View country,
    Slice<u32> scratch, u16* output, AppleMusicCollectionSink sink) noexcept {
    using E=EmbedPlayerError;
    u8 target[64];
    if (!output || !sink.write || !apple_music_track_target(id,country,{target,sizeof(target)}) || !json.bytes || json.bytes>512*1024) return E::invalid_input;
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"resultCount", "results"> root;
    u32 count=0; const u64 expected=decimal(id);
    if (!fields(doc,root) || !number(root[0],&count) || !root[1].seen) return E::malformed;
    if (!count) return E::unavailable;
    if (count>201) return E::capacity;
    struct Item { JsonValue value; u16 disc,track; };
    struct Album { const JsonIndexedDocument* doc; u64 id; Utf8View country; Item items[200]; u32 count{},expected{},seen{}; } album{&doc,expected,country};
    const auto visit=+[](void* opaque,u32 index,JsonValue value) noexcept {
        auto& a=*static_cast<Album*>(opaque); ++a.seen;
        FieldSet<"wrapperType", "collectionId", "collectionType", "trackCount", "collectionViewUrl", "kind", "discNumber", "trackNumber"> f;
        u64 id{};
        if (!fields(*a.doc,value,f) || !u64_of(f[1],&id) || id!=a.id) return false;
        if (!index) {
            JsonString view{}; char url[512];
            if (!equal(f[0], "collection") || (!equal(f[2], "Album") && !equal(f[2], "Compilation")) || !number(f[3],&a.expected) || !text(f[4],&view)) return false;
            const auto decoded=decode(view, url, sizeof(url));
            const auto located=decoded.ok()?apple_music_album_locator({url,decoded.bytes}):AppleMusicTrackLocator{};
            return decimal(located.id)==a.id && ascii_equal_ignore_case(located.country,a.country);
        }
        u32 disc{},track{};
        if (a.count==200 || !equal(f[0], "track") || !equal(f[5], "song") ||
            !number(f[6],&disc) || !disc || disc>65535 || !number(f[7],&track) || !track || track>65535) return false;
        a.items[a.count++]={value,static_cast<u16>(disc),static_cast<u16>(track)}; return true;
    };
    if (!visit_array(doc, root[1].value, visit, &album) || album.seen!=count) return E::malformed;
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
        FieldSet<"trackId", "trackViewUrl", "trackName", "previewUrl"> f;
        u64 track{}; JsonString view{},name{},preview{}; char source[kEmbedStreamUrlBytes];
        if (!fields(doc,item.value,f) || !u64_of(f[0],&track) || !track || !text(f[1],&view) || !text(f[2],&name)) return E::malformed;
        if (!track_page_is(view,track,country,512)) return E::malformed;
        MediaTrackTitle title{}; const auto named=decode_title(name,&title);
        if (!named.ok() || !named.bytes) return E::capacity;
        Utf8View url{};
        if (f[3].seen && f[3].value.kind!=JsonValueKind::null_value) {
            if (!text(f[3],&preview)) return E::malformed;
            const auto decoded_preview=decode(preview, source, sizeof(source));
            if (!decoded_preview.ok() || !embed_stream_location(CdnHost::apple_music_preview,{source,decoded_preview.bytes})) return E::malformed;
            url={source,decoded_preview.bytes};
        }
        if (!sink.write(sink.context,static_cast<u16>(i+1),track,title,url)) return E::capacity;
    }
    *output=static_cast<u16>(album.count); return E::none;
}
bool spotify_audio_id(const Utf8View& id) noexcept { return id.bytes == 22 && id_chars(id); }
u32 spotify_audio_target(Utf8View id, Slice<u8> output, bool episode) noexcept {
    return spotify_audio_id(id) ? id_target(episode ? utf8_literal("/embed/episode/") : utf8_literal("/embed/track/"), id, output) : 0;
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
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    JsonValue value{json, 0, JsonValueKind::object};
    const u8* name = MemberNames<"props", "pageProps", "state", "data", "entity">::data.bytes + 1;
    for (u32 i = 0; i < 5; ++i, name += 1 + name[0]) {
        Field entry{};
        if (!fields_n(doc, value, 24, &entry, 1, name) || !entry.seen) return E::malformed;
        value = entry.value;
    }
    FieldSet<"type", "id", "isPlayable", "audioPreview", "uri", "isNineteenPlus"> track;
    if (!fields(doc, value, track) || !equal(track[0], episode ? utf8_literal("episode") : utf8_literal("track")) || !equal(track[1], id)) return E::malformed;
    bool playable = false, restricted = false;
    if (!bool_of(track[2], &playable) ||
        (track[5].seen && !json_value_to_bool(track[5].value, &restricted))) return E::malformed;
    if (!playable || restricted) return E::unavailable;
    if (track[4].seen) {
        char uri[38]; const auto prefix = episode ? utf8_literal("spotify:episode:") : utf8_literal("spotify:track:");
        copy_bytes(uri, prefix.data, prefix.bytes); copy_bytes(uri + prefix.bytes, id.data, id.bytes);
        if (!equal(track[4], {uri, prefix.bytes+id.bytes})) return E::malformed;
    }
    if (!track[3].seen || track[3].value.kind == JsonValueKind::null_value) return E::unsupported;
    FieldSet<"url"> preview; JsonString url{};
    if (!fields(doc, track[3].value, preview) || !text(preview[0], &url)) return E::malformed;
    EmbedStream staged{};
    const auto decoded = decode(url, staged.url, sizeof(staged.url));
    EmbedStreamLocation location{};
    return publish_stream(&staged, decoded.bytes, decoded.ok() &&
        embed_stream_location(CdnHost::spotify_preview, {staged.url, decoded.bytes}, &location) &&
        ascii_equal_ignore_case(location.host, episode ? utf8_literal("podz-content.spotifycdn.com") : utf8_literal("p.scdn.co")),
        output);
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
    if (count != 4 || (!is_text(parts[1], "aac_160k") &&
        !is_text(parts[1], "aac_96k"))) return false;
    for (u32 part = 0; part <= 2; part += 2) {
        if (parts[part].bytes > 128) return false;
        for (u32 i = 0; i < parts[part].bytes; ++i)
            if (!ascii_is_alnum(parts[part].data[i]) && parts[part].data[i] != '-' && parts[part].data[i] != '_') return false;
    }
    const auto leaf = parts[3];
    if (playlist) { if (!is_text(leaf, "playlist.m3u8")) return false; }
    else if (!is_text(leaf, "init.mp4")) {
        if (leaf.bytes < 9 || leaf.bytes > 16 || !has_prefix(leaf, "data") ||
            !is_text({leaf.data + leaf.bytes - 4, 4}, ".m4s")) return false;
        for (u32 i = 4; i < leaf.bytes - 4; ++i) if (!ascii_is_digit(leaf.data[i])) return false;
    }
    for (u32 i = end; i < url.bytes; ++i) {
        const u8 c = static_cast<u8>(url.data[i]);
        if (c <= 32 || c >= 127 || c == '\\' || c == '#' || c == '"' || c == '<' || c == '>') return false;
    }
    if (!stream_expiry(url, "expires=")) return false;
    if (out) *out = {{origin.data + 8, origin.bytes - 8}, {url.data + origin.bytes, url.bytes - origin.bytes}};
    return true;
}
static EmbedPlayerError track_title(Utf8View json, u64 track, Utf8View video, Slice<u32> scratch, MediaTrackTitle* out) noexcept {
    using E = EmbedPlayerError;
    if (!out || (!track && !video_id(video)) || !json.data || !json.bytes || json.bytes > 32768) return E::invalid_input;
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"type", "provider_name", "provider_url", "title", "html"> f;
    JsonString title{}, markup{};
    if (!fields(doc, f) || !equal(f[0], video.bytes ? utf8_literal("video") : utf8_literal("rich")) ||
        !equal(f[1], video.bytes ? utf8_literal("YouTube") : utf8_literal("SoundCloud")) || !equal(f[2], video.bytes ? utf8_literal("https://www.youtube.com/") : utf8_literal("https://soundcloud.com")) ||
        !text(f[3], &title) || !text(f[4], &markup)) return E::malformed;
    char html[4096], decoded[1024];
    const auto rendered = decode(markup, html, sizeof(html));
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
    const auto name = decode_title(title, &staged);
    if (!name.ok()) return name.error == JsonStringDecodeError::output_too_small ? E::capacity : E::malformed;
    if (!name.bytes) return E::unavailable;
    copy_value(out, staged);
    return E::none;
}

namespace {
JsonValue child_path(const JsonIndexedDocument& doc, JsonValue value, const u8* list) noexcept {
    const u8* names = list + 1;
    for (u32 count = list[0]; count; --count, names += 1 + names[0]) {
        Field f{};
        if (!value.source.bytes || value.kind != JsonValueKind::object || !fields_n(doc, value, 48, &f, 1, names) || !f.seen)
            return {{}, 0, JsonValueKind::null_value};
        value = f.value;
    }
    return value;
}
template <MemberName... Names>
__forceinline JsonValue child(const JsonIndexedDocument& doc, const JsonValue& parent) noexcept {
    return child_path(doc, parent, MemberNames<Names...>::data.bytes);
}
bool queue_panel(const JsonIndexedDocument& doc, JsonValue root, JsonValue* panel) noexcept {
    auto next = child<"continuationContents", "playlistPanelContinuation">(doc, root);
    if (next.kind == JsonValueKind::object) { *panel = next; return true; }
    next = child<"contents", "singleColumnMusicWatchNextResultsRenderer", "tabbedRenderer",
        "watchNextTabbedResultsRenderer", "tabs">(doc, root);
    struct Tabs { const JsonIndexedDocument* doc; JsonValue panel; u32 count; } tabs{&doc};
    const auto visit = [](void* opaque, u32, JsonValue tab) noexcept {
        auto& t = *static_cast<Tabs*>(opaque);
        const auto v = child<"tabRenderer", "content", "musicQueueRenderer", "content", "playlistPanelRenderer">(*t.doc, tab);
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
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    JsonValue panel{};
    if (!queue_panel(doc, {json, 0, JsonValueKind::object}, &panel)) return E::unavailable;
    FieldSet<"playlistId", "contents", "continuations"> f;
    if (!fields(doc, panel, f) || !equal(f[0], list) || !f[1].seen) return E::malformed;
    YoutubeCollectionPage staged{};
    struct Items { const JsonIndexedDocument* doc; YoutubeCollectionPage* page; MediaTrackTitleSink sink; bool overflow; } items{&doc, &staged, sink};
    const auto visit = [](void* opaque, u32, JsonValue value) noexcept {
        auto& c = *static_cast<Items*>(opaque); auto& page = *c.page;
        auto v = child<"playlistPanelVideoRenderer">(*c.doc, value);
        if (v.kind != JsonValueKind::object)
            v = child<"playlistPanelVideoWrapperRenderer", "primaryRenderer", "playlistPanelVideoRenderer">(*c.doc, value);
        if (v.kind != JsonValueKind::object) return true; // Automix controls are not songs.
        FieldSet<"videoId", "selected", "title"> fields_out;
        JsonString id{};
        if (!fields(*c.doc, v, fields_out) || !text(fields_out[0], &id)) return false;
        MediaTrackId track{}; track.youtube_track = true;
        const auto decoded = decode(id, track.youtube, 11);
        if (!decoded.ok() || !video_id({track.youtube, decoded.bytes})) return false;
        if (page.count == kYoutubeCollectionPageTracks) { c.overflow = true; return false; }
        bool selected = false;
        if (fields_out[1].seen && !json_value_to_bool(fields_out[1].value, &selected)) return false;
        if (selected) { if (page.selected) return false; page.selected = static_cast<u16>(page.count + 1); }
        page.tracks[page.count++] = track;
        if (fields_out[2].seen) {
            MediaTrackTitle cached{};
            const auto runs = child<"runs">(*c.doc, fields_out[2].value);
            struct TitleRuns { const JsonIndexedDocument* doc; MediaTrackTitle* title; } title_runs{c.doc, &cached};
            const auto run = [](void* opaque_title, u32, JsonValue item) noexcept {
                auto& t = *static_cast<TitleRuns*>(opaque_title);
                const auto value = child<"text">(*t.doc, item); JsonString text{};
                if (!json_value_as_string(value, &text)) return false;
                auto& title = *t.title;
                const auto decoded = decode(text, title.text + title.bytes, static_cast<u32>(sizeof(title.text) - title.bytes));
                if (!decoded.ok()) return false;
                title.bytes += static_cast<u16>(decoded.bytes); return true;
            };
            if (!visit_array(*c.doc, runs, run, &title_runs)) reset_value(&cached);
            sanitize_title(&cached);
            if (cached.bytes) {
                if (c.sink.write && !c.sink.write(c.sink.context, page.count, cached)) { c.overflow = true; return false; }
            }
        }
        return true;
    };
    if (!visit_array(doc, f[1].value, visit, &items)) return items.overflow ? E::capacity : E::malformed;
    if (!staged.count) return E::unavailable;
    if (f[2].seen) {
        const auto continuation = [](void* opaque, u32, JsonValue value) noexcept {
            auto& c = *static_cast<Items*>(opaque);
            auto v = child<"nextRadioContinuationData">(*c.doc, value);
            if (v.kind != JsonValueKind::object) v = child<"nextContinuationData">(*c.doc, value);
            v = child<"continuation">(*c.doc, v);
            if (!v.source.bytes) return true;
            JsonString text{};
            if (c.page->continuation_bytes || !json_value_as_string(v, &text)) return false;
            const auto decoded = decode(text, c.page->continuation, sizeof(c.page->continuation));
            if (!decoded.ok()) return false;
            c.page->continuation_bytes = static_cast<u16>(decoded.bytes); return true;
        };
        if (!visit_array(doc, f[2].value, continuation, &items)) return E::malformed;
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
            if (is_text_nocase(name, "id")) { if (seen_id) return EmbedPlayerError::malformed; seen_id=true; id=value; }
            if (is_text_nocase(name, "type")) { if (seen_type) return EmbedPlayerError::malformed; seen_type=true; type=value; }
        }
        if (at==html.bytes) return EmbedPlayerError::malformed;
        const u32 begin=++at;
        while (at+end.bytes<=html.bytes && !bytes_equal(html.data+at,end.data,end.bytes)) ++at;
        if (at+end.bytes>html.bytes) return EmbedPlayerError::malformed;
        if (is_text(id, "serialized-server-data")) {
            if (out->bytes || !is_text(type, "application/json")) return EmbedPlayerError::malformed;
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
    FieldSet<"kind", "identifiers", "url"> f;
    JsonString identifier{},url{}; char buffer[1024];
    if (!fields(doc,descriptor,f) || !equal(f[0], "playlist") || !text(f[2],&url) ||
        !json_value_as_string(child<"storeAdamID">(doc,f[1].value),&identifier) || !json_string_equals_ascii(identifier,id)) return false;
    const auto decoded=decode(url, buffer, sizeof(buffer));
    const auto match=decoded.ok()?apple_music_collection_locator({buffer,decoded.bytes}):AppleMusicCollectionLocator{};
    return match.playlist && utf8_equal(match.id,id) && ascii_equal_ignore_case(match.country,country);
}
}
EmbedPlayerError apple_music_playlist(Utf8View html,Utf8View id,Utf8View country,Slice<u32> scratch,
    AppleMusicPlaylist* output,AppleMusicCollectionSink sink) noexcept {
    using E=EmbedPlayerError;
    if (!output || !sink.write || !html.bytes || html.bytes>2*1024*1024) return E::invalid_input;
    Utf8View json{}; const auto extracted=playlist_json(html,&json); if (extracted!=E::none) return extracted;
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    auto page=single_item(doc,child<"data">(doc,{json,0,JsonValueKind::object}));
    page=child<"data">(doc,page);
    char canonical[1024]; JsonString url{};
    if (!json_value_as_string(child<"canonicalURL">(doc,page),&url)) return E::malformed;
    const auto decoded=decode(url, canonical, sizeof(canonical));
    const auto located=decoded.ok()?apple_music_collection_locator({canonical,decoded.bytes}):AppleMusicCollectionLocator{};
    if (!located.playlist || !utf8_equal(located.id,id) || !ascii_equal_ignore_case(located.country,country)) return E::malformed;
    struct Sections { const JsonIndexedDocument* doc; Utf8View id,country; JsonValue tracks{}; u32 count{}; bool header{},list{}; } sections{&doc,id,country};
    const auto visit=+[](void* opaque,u32,JsonValue value) noexcept {
        auto& s=*static_cast<Sections*>(opaque); FieldSet<"itemKind","items","containerContentDescriptor"> f;
        if (!fields(*s.doc,value,f)) return false;
        if (equal(f[0], "containerDetailHeaderLockup")) {
            if (s.header) return false; s.header=true; const auto item=single_item(*s.doc,f[1].value);
            FieldSet<"contentDescriptor","trackCount"> header;
            return fields(*s.doc,item,header) && playlist_identity(*s.doc,header[0].value,s.id,s.country) && number(header[1],&s.count);
        }
        if (equal(f[0], "trackLockup")) {
            if (s.list || !playlist_identity(*s.doc,f[2].value,s.id,s.country)) return false;
            s.list=true; s.tracks=f[1].value;
        }
        return true;
    };
    if (!json_visit_indexed_array(doc,child<"sections">(doc,page),48,visit,&sections).ok() || !sections.header || !sections.list) return E::malformed;
    if (!sections.count) return E::unavailable;
    if (sections.count>200) return E::capacity;
    AppleMusicPlaylist staged{};
    struct Tracks { const JsonIndexedDocument* doc; Utf8View country; AppleMusicPlaylist* out; AppleMusicCollectionSink sink; } tracks{&doc,country,&staged,sink};
    const auto song=+[](void* opaque,u32 index,JsonValue value) noexcept {
        auto& t=*static_cast<Tracks*>(opaque); if (index>=200) return false;
        FieldSet<"title","contentDescriptor","isDisabled"> f;
        if (!fields(*t.doc,value,f)) return false;
        FieldSet<"kind","identifiers","url"> d;
        JsonString identifier{},name{},view{}; bool disabled=false; u64 id{};
        if (!fields(*t.doc,f[1].value,d) || !equal(d[0], "song") || !text(f[0],&name) || !text(d[2],&view) ||
            !json_value_as_string(child<"storeAdamID">(*t.doc,d[1].value),&identifier) || !json_string_to_u64(identifier,&id) || !id ||
            (f[2].seen && !json_value_to_bool(f[2].value,&disabled))) return false;
        if (!track_page_is(view,id,t.country,1024)) return false;
        MediaTrackTitle title{}; const auto named=decode_title(name,&title);
        if (!named.ok() || !named.bytes) return false;
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
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"resultCount","results"> root; u32 count{};
    if (!fields(doc,root) || !number(root[0],&count) || count>ids.count) return E::malformed;
    struct Batch { const JsonIndexedDocument* doc; ConstSlice<u64> ids; Utf8View country; AppleMusicCollectionSink sink; u64 seen{}; u32 count{}; u16 offset; } batch{&doc,ids,country,sink,0,0,offset};
    const auto visit=+[](void* opaque,u32,JsonValue value) noexcept {
        auto& b=*static_cast<Batch*>(opaque); ++b.count;
        FieldSet<"wrapperType","kind","trackId","trackName","trackViewUrl","previewUrl"> f;
        u64 id{}; JsonString name{},view{},preview{};
        if (!fields(*b.doc,value,f) || !equal(f[0], "track") || !equal(f[1], "song") || !u64_of(f[2],&id) || !id || !text(f[3],&name) || !text(f[4],&view)) return false;
        u32 found=0; while (found<b.ids.count && b.ids[found]!=id) ++found;
        if (found==b.ids.count || (b.seen&(u64{1}<<found))) return false;
        b.seen|=u64{1}<<found;
        if (!track_page_is(view,id,b.country,1024)) return false;
        if (!f[5].seen || f[5].value.kind==JsonValueKind::null_value) return true;
        MediaTrackTitle title{}; const auto named=decode_title(name,&title);
        if (!named.ok() || !named.bytes || !text(f[5],&preview)) return false;
        char source[kEmbedStreamUrlBytes]; const auto source_bytes=decode(preview, source, sizeof(source));
        if (!source_bytes.ok() || !embed_stream_location(CdnHost::apple_music_preview,{source,source_bytes.bytes})) return false;
        for (u32 i=0;i<b.ids.count;++i) if (b.ids[i]==id && !b.sink.write(b.sink.context,static_cast<u16>(b.offset+i+1),id,title,{source,source_bytes.bytes})) return false;
        return true;
    };
    return visit_array(doc, root[1].value, visit, &batch) && batch.count==count ? E::none : E::malformed;
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
    JsonIndexedDocument doc;
    if (const auto indexed = index_json(json, scratch, &doc); indexed != E::none) return indexed;
    FieldSet<"url"> f; JsonString value{}; EmbedStream staged{};
    if (!fields(doc, f) || !text(f[0], &value)) return E::malformed;
    const auto decoded = decode(value, staged.url, sizeof(staged.url));
    return publish_stream(&staged, decoded.bytes, decoded.ok() && soundcloud_location({staged.url, decoded.bytes}, true), out);
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
    SoundcloudHydration h{{}, {}, {}, kind};
    if (const auto indexed = index_json(json, scratch, &h.doc); indexed != E::none) return indexed;
    const auto visit = +[](void* opaque, u32, JsonValue value) noexcept {
        auto& h = *static_cast<SoundcloudHydration*>(opaque);
        if (++h.count > 64) return false;
        FieldSet<"hydratable", "data"> f;
        if (!fields(h.doc, value, f) || !f[1].seen) return false;
        auto* selected = equal(f[0], h.kind) ? &h.record : equal(f[0], "apiClient") ? &h.client : nullptr;
        if (selected) { if (selected->source.bytes) return false; *selected = f[1].value; }
        return true;
    };
    if (!visit_array(h.doc, {json, 0, JsonValueKind::array}, visit, &h) ||
        !h.record.source.bytes) return E::malformed;
    *out = h;
    return E::none;
}
// Canonical page URL of a SoundCloud track or collection path; 0 bytes when the path is not one.
u32 soundcloud_permalink(const Utf8View& path, bool collection, char (&out)[544]) noexcept {
    ProtocolWriter canonical{{reinterpret_cast<u8*>(out), sizeof(out)}, 0};
    if (!protocol_write(&canonical, "https://soundcloud.com", path)) return 0;
    const Utf8View page{out, canonical.used};
    return (collection ? soundcloud_collection_path(page) : soundcloud_track_path(page)).bytes ? canonical.used : 0;
}
} // namespace
EmbedPlayerError soundcloud_track_request(Utf8View html, Utf8View path, Slice<u32> scratch, EmbedStream* out, u64 expected_id) noexcept {
    using E = EmbedPlayerError;
    if (!out || !html.data || !html.bytes || html.bytes > 512 * 1024 || !path.data || path.bytes > 512) return E::invalid_input;
    char permalink[544]; const u32 permalink_bytes = soundcloud_permalink(path, false, permalink);
    if (!permalink_bytes) return E::invalid_input;
    SoundcloudHydration h{};
    const auto parsed = soundcloud_hydration(html, utf8_literal("sound"), scratch, &h);
    if (parsed != E::none) return parsed;
    if (!h.client.source.bytes) return E::malformed;
    FieldSet<"id", "kind", "permalink_url", "streamable", "policy", "embeddable_by", "media", "track_authorization", "public"> sound;
    u64 id{}; bool streamable{}, public_track{};
    if (!fields(h.doc, h.record, sound) || !u64_of(sound[0], &id) || !id ||
        !equal(sound[1], "track") || !equal(sound[2], {permalink, permalink_bytes}) ||
        !bool_of(sound[3], &streamable) ||
        !bool_of(sound[8], &public_track) || (expected_id && id != expected_id)) return E::malformed;
    if (!streamable || !public_track || !equal(sound[4], "ALLOW")) return E::unavailable;
    if (!equal(sound[5], "all")) return E::not_embeddable;
    FieldSet<"transcodings"> media;
    FieldSet<"id"> client;
    if (!sound[6].seen || !fields(h.doc, sound[6].value, media) || !media[0].seen || !fields(h.doc, h.client, client)) return E::malformed;
    struct Renditions { JsonIndexedDocument doc; JsonString url{}; u32 count{}, bitrate{}; } r{h.doc};
    const auto rendition = +[](void* opaque, u32, JsonValue value) noexcept {
        auto& r = *static_cast<Renditions*>(opaque);
        if (++r.count > 32) return false;
        FieldSet<"url", "preset", "snipped", "format"> f;
        if (!fields(r.doc, value, f)) return false;
        const u32 bitrate = equal(f[1], "aac_160k") ? 160 : equal(f[1], "aac_96k") ? 96 : 0;
        if (!bitrate) return true;
        bool snipped{}; FieldSet<"protocol", "mime_type"> format; JsonString url{};
        if (!bool_of(f[2], &snipped) || !f[3].seen || !fields(r.doc, f[3].value, format) || !text(f[0], &url)) return false;
        if (!snipped && equal(format[0], "hls") &&
            equal(format[1], "audio/mp4; codecs=\"mp4a.40.2\"") && bitrate > r.bitrate) { r.url = url; r.bitrate = bitrate; }
        return true;
    };
    if (!visit_array(h.doc, media[0].value, rendition, &r)) return E::malformed;
    if (!r.bitrate) return E::unsupported;
    EmbedStream staged{};
    const auto decoded = decode(r.url, staged.url, sizeof(staged.url));
    char prefix[128]; ProtocolWriter head{{reinterpret_cast<u8*>(prefix), sizeof(prefix)}, 0};
    protocol_write(&head, "https://api-v2.soundcloud.com/media/soundcloud:tracks:", id, '/');
    bool valid = decoded.ok() && utf8_starts_with({staged.url, decoded.bytes}, {prefix, head.used});
    u32 at = head.used;
    while (valid && at < decoded.bytes && (ascii_is_alnum(staged.url[at]) || staged.url[at] == '-' || staged.url[at] == '_')) ++at;
    valid = valid && at > head.used && is_text({staged.url + at, decoded.bytes - at}, "/stream/hls");
    char key[64], grant[512]; JsonString key_json{}, grant_json{}; // read only up to their decoded bytes
    const auto key_decoded = text(client[0], &key_json) ? decode(key_json, key, sizeof(key)) : JsonStringDecodeResult{};
    const auto grant_decoded = text(sound[7], &grant_json) ? decode(grant_json, grant, sizeof(grant)) : JsonStringDecodeResult{};
    valid = valid && key_decoded.ok() && key_decoded.bytes == 32 && grant_decoded.ok() && grant_decoded.bytes;
    for (u32 i = 0; valid && i < key_decoded.bytes; ++i) valid = ascii_is_alnum(key[i]);
    ProtocolWriter request{{reinterpret_cast<u8*>(staged.url), sizeof(staged.url)}, decoded.ok() ? decoded.bytes : 0};
    valid = valid && protocol_write(&request, "?client_id=", percent_text({key, key_decoded.bytes}),
        "&track_authorization=", percent_text({grant, grant_decoded.bytes}));
    if (valid) { staged.url_bytes = request.used; staged.bitrate = r.bitrate * 1000; *out = staged; }
    secure_clear_bytes(key, sizeof(key)); secure_clear_bytes(grant, sizeof(grant)); secure_clear_bytes(&staged, sizeof(staged));
    return valid ? E::none : E::malformed;
}
EmbedPlayerError soundcloud_collection(Utf8View html, Utf8View path, u64 expected_id,
    u32 selected, Slice<u32> scratch, Slice<u64> tracks, SoundcloudCollection* out, MediaTrackTitleSink sink) noexcept {
    using E = EmbedPlayerError;
    if (!out || !tracks.data || !tracks.count || !html.data || !html.bytes || html.bytes > 512 * 1024 ||
        !path.data || path.bytes > 512) return E::invalid_input;
    char permalink[544]; const u32 permalink_bytes = soundcloud_permalink(path, true, permalink);
    if (!permalink_bytes) return E::invalid_input;
    SoundcloudHydration h{};
    const auto station = soundcloud_station_locator(path);
    const auto parsed = soundcloud_hydration(html, station.seed ? utf8_literal("systemPlaylist") : utf8_literal("playlist"), scratch, &h);
    if (parsed != E::none) return parsed;
    FieldSet<"id", "kind", "permalink_url", "public", "sharing", "embeddable_by", "track_count", "tracks", "is_public",
        "urn", "playlist_type", "seed"> f;
    SoundcloudCollection staged{}; bool public_set{}; u64 count{};
    FieldSet<"id"> client; JsonString client_id{};
    if (h.client.source.bytes && fields(h.doc, h.client, client) && text(client[0], &client_id)) {
        const auto decoded = decode(client_id, staged.client, sizeof(staged.client));
        if (!decoded.ok() || decoded.bytes != 32) secure_clear_bytes(staged.client, sizeof(staged.client));
    }
    if (!fields(h.doc, h.record, f) || !equal(f[2], {permalink, permalink_bytes}) || !f[7].seen) return E::malformed;
    if (station.seed) {
        // Stations publish one finite public queue, without track_count. Bind
        // its string identity, seed kind and seed ID to the requested station before reading it.
        char urn[96], seed[64];
        ProtocolWriter identity{{reinterpret_cast<u8*>(urn), sizeof(urn)}, 0},
            origin{{reinterpret_cast<u8*>(seed), sizeof(seed)}, 0};
        const auto type = station.track ? utf8_literal("track-stations:") : utf8_literal("artist-stations:");
        protocol_write(&identity, "soundcloud:system-playlists:", type, station.seed);
        const auto kind = station.track ? utf8_literal("soundcloud:tracks:") : utf8_literal("soundcloud:users:");
        protocol_write(&origin, kind, station.seed);
        FieldSet<"urn"> seed_fields;
        if (expected_id || !equal(f[0], {urn, identity.used}) || !equal(f[9], {urn, identity.used}) ||
            !equal(f[1], "system-playlist") || !equal(f[10], station.track ? utf8_literal("TRACK_STATION") : utf8_literal("ARTIST_STATION")) ||
            !bool_of(f[8], &public_set) || !f[11].seen ||
            !fields(h.doc, f[11].value, seed_fields) || !equal(seed_fields[0], {seed, origin.used})) return E::malformed;
        if (!public_set) return E::unavailable;
        staged.id = station.seed;
    } else {
        if (!u64_of(f[0], &staged.id) || !staged.id ||
            (expected_id && expected_id != staged.id) || !equal(f[1], "playlist") ||
            !equal(f[2], {permalink, permalink_bytes}) || !bool_of(f[3], &public_set) ||
            !u64_of(f[6], &count) || !f[7].seen) return E::malformed;
        if (!public_set || !equal(f[4], "public")) return E::unavailable;
        if (!equal(f[5], "all")) return E::not_embeddable;
        if (!count) return E::unavailable;
        if (count > kMediaCollectionTracks || count > tracks.count) return E::capacity;
        if (selected >= count) return E::invalid_input;
    }
    struct Tracks { JsonIndexedDocument doc; u32 limit{}, count{}; u64* output{}; MediaTrackTitleSink sink; } list{h.doc, station.seed ? kMediaCollectionTracks : static_cast<u32>(count), 0, nullptr, sink};
    const auto visit = +[](void* opaque, u32, JsonValue value) noexcept {
        auto& list = *static_cast<Tracks*>(opaque);
        if (list.count == list.limit) return false;
        FieldSet<"id", "kind", "title"> f; u64 id{};
        if (!fields(list.doc, value, f) || !u64_of(f[0], &id) || !id ||
            (f[1].seen && !equal(f[1], "track"))) return false;
        if (list.output) {
            list.output[list.count] = id;
            JsonString name{}; MediaTrackTitle title{};
            if (list.sink.write && text(f[2], &name)) {
                const auto decoded = decode_title(name, &title);
                if (decoded.ok() && decoded.bytes &&
                    !list.sink.write(list.sink.context, static_cast<u16>(list.count + 1), title)) return false;
            }
        }
        ++list.count;
        return true;
    };
    // Validate first so malformed trailing entries cannot corrupt a live queue.
    if (!visit_array(h.doc, f[7].value, visit, &list) || (!station.seed && list.count != count)) return E::malformed;
    if (!list.count) return E::unavailable;
    if (list.count > tracks.count) return E::capacity;
    if (selected >= list.count) return E::invalid_input;
    list.count = 0; list.output = tracks.data;
    if (!visit_array(h.doc, f[7].value, visit, &list)) return E::capacity;
    staged.count = list.count; staged.selected = selected; *out = staged;
    return E::none;
}

EmbedPlayerError soundcloud_batch_titles(Utf8View json, const SoundcloudTitleBatch& batch, u16 preferred,
    Slice<u32> scratch, MediaTrackTitleSink sink) noexcept {
    using E = EmbedPlayerError;
    if (!batch.count || batch.count > 50 || !sink.write || !json.bytes || json.bytes > 512 * 1024) return E::invalid_input;
    struct Names { JsonIndexedDocument doc; const SoundcloudTitleBatch* batch; MediaTrackTitleSink sink; u64 seen; u16 preferred; bool first; } names{{},&batch,sink,0,preferred,true};
    if (const auto indexed = index_json(json, scratch, &names.doc); indexed != E::none) return indexed;
    const auto visit = [](void* opaque, u32, JsonValue value) noexcept {
        auto& n = *static_cast<Names*>(opaque);
        FieldSet<"id", "kind", "title", "public"> f;
        u64 id{}; JsonString name{}; bool public_track = true;
        if (!fields(n.doc,value,f) || !u64_of(f[0],&id) || !equal(f[1], "track") ||
            !text(f[2],&name) || (f[3].seen && !json_value_to_bool(f[3].value,&public_track))) return false;
        u32 found = 0;
        while (found < n.batch->count && n.batch->tracks[found].id != id) ++found;
        if (found == n.batch->count || (n.seen & (u64{1} << found))) return false;
        n.seen |= u64{1} << found;
        if (!public_track) return true;
        MediaTrackTitle title{}; const auto decoded = decode_title(name,&title);
        if (!decoded.ok() || !decoded.bytes) return true;
        for (u32 i=0;i<n.batch->count;++i) if (n.batch->tracks[i].id == id && ((n.batch->tracks[i].index == n.preferred) == n.first))
            if (!n.sink.write(n.sink.context,n.batch->tracks[i].index,title)) return false;
        return true;
    };
    // The demanded title is first, so bounded mailbox capacity cannot starve it.
    for (u32 pass=0;pass<2;++pass) {
        names.first = pass == 0; names.seen=0;
        if (!visit_array(names.doc, {json,0,JsonValueKind::array}, visit, &names)) return E::malformed;
    }
    return E::none;
}
u32 youtube_player_body(Utf8View id, Utf8View visitor, Slice<u8> output) noexcept {
    if (!video_id(id) || !ascii_graphic_string(visitor, 1024, true)) return 0;
    u8 staged[2048];
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    const bool ok = protocol_write(&w, "{\"context\":{\"client\":{\"clientName\":\"VISIONOS\",\"clientVersion\":\"1.02\",\"hl\":\"en\",\"deviceMake\":\"Apple\",\"deviceModel\":\"RealityDevice17,1\",\"osName\":\"visionOS\",\"osVersion\":\"26.5.23O471\",\"visitorData\":",
        json_text(visitor), ",\"userAgent\":", json_text(kYoutubeNativeUserAgent), "}},\"videoId\":", json_text(id), '}');
    if (!ok || !output.data || output.count < w.used) return 0;
    copy_bytes(output.data, staged, w.used);
    return w.used;
}
} // namespace maicord
