#include "maicord/protocol/embed_provider.hpp"
#include "maicord/protocol/embed_player.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool equal(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && (!a.bytes || bytes_equal(a.data, b.data, a.bytes));
}
bool prefix(Utf8View a, Utf8View b) noexcept {
    return a.bytes >= b.bytes && bytes_equal(a.data, b.data, b.bytes);
}
struct Url { Utf8View host, path, query, fragment; };
bool parse(Utf8View text, Url* out) noexcept {
    if (!text.data || text.bytes > 2048 || text.bytes < 9 ||
        !ascii_equal_ignore_case({text.data, 8}, utf8_literal("https://"))) return false;
    for (u32 i = 8; i < text.bytes; ++i)
        if (static_cast<u8>(text.data[i]) <= 32 || static_cast<u8>(text.data[i]) >= 127 ||
            text.data[i] == '\\') return false;
    u32 end = 8;
    while (end < text.bytes && text.data[end] != '/' && text.data[end] != '?' && text.data[end] != '#') {
        const char c = text.data[end++];
        if (!ascii_is_alnum(c) && c != '-' && c != '.') return false;
    }
    if (end == 8) return false;
    out->host = {text.data + 8, end - 8};
    const u32 begin = end;
    while (end < text.bytes && text.data[end] != '?' && text.data[end] != '#') ++end;
    out->path = {text.data + begin, end - begin};
    if (end < text.bytes && text.data[end] == '?') {
        const u32 query = ++end;
        while (end < text.bytes && text.data[end] != '#') ++end;
        out->query = {text.data + query, end - query};
    }
    if (end < text.bytes && text.data[end] == '#') out->fragment = {text.data + end + 1, text.bytes - end - 1};
    return true;
}
bool host(const Url& u, Utf8View value) noexcept { return ascii_equal_ignore_case(u.host, value); }
EmbedProvider provider(const Url& u) noexcept {
    if (host(u, utf8_literal("soundcloud.com")) || host(u, utf8_literal("www.soundcloud.com")) ||
        host(u, utf8_literal("w.soundcloud.com"))) return EmbedProvider::soundcloud;
    if (host(u, utf8_literal("vimeo.com")) || host(u, utf8_literal("www.vimeo.com")) ||
        host(u, utf8_literal("player.vimeo.com"))) return EmbedProvider::vimeo;
    if (host(u, utf8_literal("www.pornhub.com")) || host(u, utf8_literal("pornhub.com")))
        return EmbedProvider::pornhub;
    if (host(u, utf8_literal("music.youtube.com"))) return EmbedProvider::youtube_music;
    if (host(u, utf8_literal("youtube.com")) || host(u, utf8_literal("www.youtube.com")) ||
        host(u, utf8_literal("m.youtube.com")) || host(u, utf8_literal("youtu.be")) ||
        host(u, utf8_literal("www.youtube-nocookie.com"))) return EmbedProvider::youtube;
    if (host(u, utf8_literal("twitch.tv")) || host(u, utf8_literal("www.twitch.tv")) ||
        host(u, utf8_literal("player.twitch.tv")) || host(u, utf8_literal("clips.twitch.tv")))
        return EmbedProvider::twitch;
    if (host(u, utf8_literal("www.tiktok.com")) || host(u, utf8_literal("tiktok.com")))
        return EmbedProvider::tiktok;
    if (host(u, utf8_literal("open.spotify.com"))) return EmbedProvider::spotify;
    if (host(u, utf8_literal("x.com")) || host(u, utf8_literal("www.x.com")) ||
        host(u, utf8_literal("twitter.com")) || host(u, utf8_literal("www.twitter.com")) ||
        host(u, utf8_literal("mobile.twitter.com")) || host(u, utf8_literal("platform.twitter.com")))
        return EmbedProvider::twitter;
    if (host(u, utf8_literal("imgur.com")) || host(u, utf8_literal("www.imgur.com")) ||
        host(u, utf8_literal("i.imgur.com"))) return EmbedProvider::imgur;
    if (host(u, utf8_literal("store.steampowered.com"))) return EmbedProvider::steam;
    if (host(u, utf8_literal("streamable.com")) || host(u, utf8_literal("www.streamable.com")))
        return EmbedProvider::streamable;
    if (host(u, utf8_literal("music.apple.com")) || host(u, utf8_literal("embed.music.apple.com")))
        return EmbedProvider::apple_music;
    constexpr Utf8View regions[]{utf8_literal("com"), utf8_literal("co.uk"), utf8_literal("de"),
        utf8_literal("co.jp"), utf8_literal("es"), utf8_literal("fr"), utf8_literal("it"),
        utf8_literal("com.au"), utf8_literal("in"), utf8_literal("ca"), utf8_literal("com.mx"),
        utf8_literal("com.br")};
    constexpr auto amazon = utf8_literal("music.amazon.");
    if (u.host.bytes > amazon.bytes && ascii_equal_ignore_case({u.host.data, amazon.bytes}, amazon))
        for (auto region : regions)
            if (ascii_equal_ignore_case({u.host.data + amazon.bytes, u.host.bytes - amazon.bytes}, region))
                return EmbedProvider::amazon_music;
    if (host(u, utf8_literal("session-share.playstation.com")) ||
        host(u, utf8_literal("sessionshare.sp-int.playstation.com"))) return EmbedProvider::playstation;
    return EmbedProvider::none;
}
bool id_valid(Utf8View id) noexcept {
    if (id.bytes != 11) return false;
    for (u32 i = 0; i < id.bytes; ++i) {
        const char c = id.data[i];
        if (!ascii_is_alnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}
// Duplicate playback parameters are rejected instead of choosing a different
// video from the page/player. Unrelated parameters remain opaque.
bool parameter(Utf8View query, Utf8View key, Utf8View* value) noexcept {
    bool found = false;
    for (u32 begin = 0; begin < query.bytes;) {
        u32 end = begin, equals = begin;
        while (end < query.bytes && query.data[end] != '&') ++end;
        while (equals < end && query.data[equals] != '=') ++equals;
        if (equal({query.data + begin, equals - begin}, key)) {
            if (found || equals == end) return false;
            *value = {query.data + equals + 1, end - equals - 1};
            found = true;
        }
        begin = end + 1;
    }
    return true;
}
bool seconds(Utf8View input, u32* out) noexcept {
    if (!input.bytes) return true;
    u32 sum = 0, number = 0, previous_unit = 3601;
    bool digit = false;
    for (u32 i = 0; i < input.bytes; ++i) {
        const char c = input.data[i];
        if (ascii_is_digit(c)) {
            if (number > (~u32{0} - static_cast<u32>(c - '0')) / 10) return false;
            number = number * 10 + static_cast<u32>(c - '0');
            digit = true;
        } else {
            const u32 unit = c == 'h' ? 3600 : c == 'm' ? 60 : c == 's' ? 1 : 0;
            if (!digit || !unit || unit >= previous_unit || number > (~u32{0} - sum) / unit) return false;
            sum += number * unit;
            number = 0;
            previous_unit = unit;
            digit = false;
        }
    }
    if (sum > ~u32{0} - number || (digit && previous_unit != 3601)) return false;
    *out = sum + number;
    return true;
}
} // namespace
EmbedProvider embed_provider(Utf8View url) noexcept {
    Url parsed{};
    return parse(url, &parsed) ? provider(parsed) : EmbedProvider::none;
}
SoundcloudStation soundcloud_station_locator(Utf8View path) noexcept {
    if (!path.data) return {};
    constexpr auto track = utf8_literal("/discover/sets/track-stations:"), artist = utf8_literal("/discover/sets/artist-stations:");
    const bool from_track = prefix(path, track);
    const auto station = from_track ? track : artist;
    if (!prefix(path, station) || path.bytes <= station.bytes ||
        path.bytes > station.bytes + 20 || path.data[station.bytes] == '0') return {};
    u64 id = 0;
    for (u32 i = station.bytes; i < path.bytes; ++i) {
        if (!ascii_is_digit(path.data[i]) || id > (~u64{0} - static_cast<u32>(path.data[i] - '0')) / 10) return {};
        id = id * 10 + static_cast<u32>(path.data[i] - '0');
    }
    return {id,from_track};
}
static Utf8View soundcloud_path(Utf8View url, bool collection) noexcept {
    Url u{};
    if (!parse(url, &u) || (!host(u, utf8_literal("soundcloud.com")) && !host(u, utf8_literal("www.soundcloud.com")))) return {};
    auto path = u.path;
    if (path.bytes && path.data[path.bytes - 1] == '/') --path.bytes;
    Utf8View secret{};
    if (!parameter(u.query, utf8_literal("secret_token"), &secret) || secret.data) return {};
    if (collection && soundcloud_station_locator(path).seed) return path;
    if (path.bytes < 4 || path.bytes > 512 || path.data[0] != '/') return {};
    u32 separators[2]{}, count = 0;
    for (u32 i = 1; i < path.bytes; ++i) {
        const char c = path.data[i];
        if (c == '/') {
            if (count == 2 || i == 1 || i + 1 == path.bytes || path.data[i - 1] == '/') return {};
            separators[count++] = i;
        } else if (!ascii_is_alnum(c) && c != '-' && c != '_') return {};
    }
    if (count != (collection ? 2u : 1u)) return {};
    const u32 first = separators[0] + 1;
    if (collection) {
        if (!equal({path.data + first, separators[1] - first}, utf8_literal("sets"))) return {};
    } else {
        const Utf8View leaf{path.data + first, path.bytes - first};
        constexpr Utf8View collections[]{utf8_literal("sets"), utf8_literal("tracks"), utf8_literal("albums"),
            utf8_literal("likes"), utf8_literal("reposts"), utf8_literal("popular-tracks")};
        for (auto name : collections) if (equal(leaf, name)) return {};
    }
    return path;
}
Utf8View soundcloud_track_path(Utf8View url) noexcept { return soundcloud_path(url, false); }
Utf8View soundcloud_collection_path(Utf8View url) noexcept { return soundcloud_path(url, true); }
static SoundcloudCollectionLocator soundcloud_locator(Utf8View url, Slice<char> decoded, bool collection) noexcept {
    if (const auto path = soundcloud_path(url, collection); path.bytes) return {path};
    Url widget{}; Utf8View nested{};
    if (!parse(url, &widget) || !host(widget, utf8_literal("w.soundcloud.com")) ||
        (!equal(widget.path, utf8_literal("/player/")) && !equal(widget.path, utf8_literal("/player"))) ||
        !parameter(widget.query, utf8_literal("url"), &nested) || !nested.bytes || !decoded.data) return {};
    u32 bytes = 0;
    for (u32 i = 0; i < nested.bytes; ++i) {
        char c = nested.data[i];
        if (c == '%') {
            if (i + 2 >= nested.bytes || !ascii_is_hex_digit(nested.data[i + 1]) || !ascii_is_hex_digit(nested.data[i + 2])) return {};
            const auto hex = [](char value) noexcept -> u8 { return static_cast<u8>(ascii_is_digit(value) ? value - '0' : ascii_lower(value) - 'a' + 10); };
            c = static_cast<char>((hex(nested.data[i + 1]) << 4) | hex(nested.data[i + 2])); i += 2;
        }
        if (bytes == decoded.count) return {};
        decoded[bytes++] = c;
    }
    u32 start = 0; Utf8View index{};
    if (collection) {
        if (!parameter(widget.query, utf8_literal("start_track"), &index)) return {};
        if (index.data && !index.bytes) return {};
        for (u32 i = 0; i < index.bytes; ++i) {
            if (!ascii_is_digit(index.data[i]) || start > (~u32{0} - static_cast<u32>(index.data[i] - '0')) / 10) return {};
            start = start * 10 + static_cast<u32>(index.data[i] - '0');
        }
    }
    const Utf8View target{decoded.data, bytes};
    if (const auto path = soundcloud_path(target, collection); path.bytes) return {path, 0, start};
    Url api{};
    const auto prefix = collection ? utf8_literal("/playlists/") : utf8_literal("/tracks/");
    if (!parse(target, &api) || !host(api, utf8_literal("api.soundcloud.com")) || api.query.bytes ||
        !utf8_starts_with(api.path, prefix)) return {};
    const Utf8View id{api.path.data + prefix.bytes, api.path.bytes - prefix.bytes};
    if (!id.bytes || id.bytes > 20 || id.data[0] == '0') return {};
    u64 value = 0;
    for (u32 i = 0; i < id.bytes; ++i) {
        if (!ascii_is_digit(id.data[i]) || value > (~u64{0} - static_cast<u32>(id.data[i] - '0')) / 10) return {};
        value = value * 10 + static_cast<u32>(id.data[i] - '0');
    }
    return {{}, value, start};
}
SoundcloudTrackLocator soundcloud_track_locator(Utf8View url, Slice<char> decoded) noexcept {
    const auto result = soundcloud_locator(url, decoded, false);
    return {result.path, result.id};
}
SoundcloudCollectionLocator soundcloud_collection_locator(Utf8View url, Slice<char> decoded) noexcept {
    return soundcloud_locator(url, decoded, true);
}
bool soundcloud_collection_url(Utf8View url) noexcept {
    char decoded[1024];
    const auto locator = soundcloud_collection_locator(url, {decoded, sizeof(decoded)});
    return locator.path.bytes || locator.id;
}
bool soundcloud_track_url(Utf8View url) noexcept {
    char decoded[1024];
    const auto locator = soundcloud_track_locator(url, {decoded, sizeof(decoded)});
    return locator.path.bytes || locator.id;
}
Utf8View spotify_audio_locator(Utf8View url, bool* episode) noexcept {
    Url u{};
    if (!parse(url, &u) || provider(u) != EmbedProvider::spotify) return {};
    auto path = u.path;
    if (prefix(path, utf8_literal("/intl-"))) {
        u32 end = 6;
        while (end < path.bytes && path.data[end] != '/') {
            const char c = ascii_lower(path.data[end]);
            if (end >= 16 || ((c < 'a' || c > 'z') && c != '-')) return {};
            ++end;
        }
        if (end == 6) return {};
        path = {path.data + end, path.bytes - end};
    }
    if (prefix(path, utf8_literal("/embed/"))) path = {path.data + 6, path.bytes - 6};
    const bool podcast = prefix(path, utf8_literal("/episode/"));
    const auto track = podcast ? utf8_literal("/episode/") : utf8_literal("/track/");
    if (!prefix(path, track)) return {};
    const Utf8View id{path.data + track.bytes, path.bytes - track.bytes};
    if (!spotify_audio_id(id)) return {};
    if (episode) *episode = podcast;
    return id;
}
AmazonMusicTrackLocator amazon_music_track_locator(Utf8View url) noexcept {
    Url outer{};
    if(!parse(url,&outer) || provider(outer)!=EmbedProvider::amazon_music) return {};
    const auto identify=[](const Url& u) noexcept -> Utf8View {
        auto path=u.path;
        if(path.bytes && path.data[path.bytes-1]=='/') --path.bytes;
        const bool album=prefix(path,utf8_literal("/albums/"));
        const u32 start=album?8:prefix(path,utf8_literal("/tracks/"))?8:prefix(path,utf8_literal("/embed/"))?7:0;
        if(!start || path.bytes<=start) return {};
        const Utf8View resource{path.data+start,path.bytes-start};
        Utf8View track{};
        if(!amazon_music_track_id(resource) || !parameter(u.query,utf8_literal("trackAsin"),&track)) return {};
        if(!album) {
            if(track.bytes && !equal(track,resource)) return {};
            track=resource;
        }
        return amazon_music_track_id(track)?track:Utf8View{};
    };
    const auto id=identify(outer);
    if(!id.bytes) return {};
    Utf8View frame{};
    if(!parameter(outer.query,utf8_literal("iframe_url"),&frame)) return {};
    if(frame.bytes) {
        char decoded[2048];u32 count=0;
        for(u32 i=0;i<frame.bytes;++i) {
            char c=frame.data[i];
            if(c=='%') {
                if(i+2>=frame.bytes || !ascii_is_hex_digit(frame.data[i+1]) || !ascii_is_hex_digit(frame.data[i+2])) return {};
                const auto hex=[](char v) noexcept -> u8 {return static_cast<u8>(ascii_is_digit(v)?v-'0':ascii_lower(v)-'a'+10);};
                c=static_cast<char>((hex(frame.data[i+1])<<4)|hex(frame.data[i+2]));i+=2;
            }
            if(count==sizeof(decoded)) return {};
            decoded[count++]=c;
        }
        Url nested{};Utf8View recursive{};
        if(!parse({decoded,count},&nested) || !ascii_equal_ignore_case(outer.host,nested.host) ||
            !equal(identify(nested),id) || !parameter(nested.query,utf8_literal("iframe_url"),&recursive) || recursive.bytes) return {};
    }
    return {id,outer.host};
}
static AppleMusicTrackLocator apple_music_locator(Utf8View url, bool collection) noexcept {
    Url u{};
    if (!parse(url, &u) || provider(u) != EmbedProvider::apple_music || u.path.bytes < 10 ||
        u.path.data[0] != '/' || u.path.data[3] != '/') return {};
    const Utf8View country{u.path.data + 1, 2};
    for (u32 i = 0; i < country.bytes; ++i)
        if (ascii_lower(country.data[i]) < 'a' || ascii_lower(country.data[i]) > 'z') return {};
    const Utf8View path{u.path.data + 3, u.path.bytes - 3};
    const bool song = prefix(path, utf8_literal("/song/")), album = prefix(path, utf8_literal("/album/"));
    const bool video = prefix(path, utf8_literal("/music-video/"));
    if (!song && !album && !video) return {};
    u32 last = path.bytes;
    while (last && path.data[last - 1] != '/') --last;
    const Utf8View resource{path.data + last, path.bytes - last};
    if (!apple_music_track_id(resource)) return {};
    Utf8View track{};
    if (!parameter(u.query, utf8_literal("i"), &track)) return {};
    if (collection) return album && !track.bytes ? AppleMusicTrackLocator{resource, country} : AppleMusicTrackLocator{};
    if (song || video) {
        if (track.bytes && !utf8_equal(track, resource)) return {};
        track = resource;
    }
    return apple_music_track_id(track) ? AppleMusicTrackLocator{track, country, video} : AppleMusicTrackLocator{};
}
AppleMusicTrackLocator apple_music_track_locator(Utf8View url) noexcept { return apple_music_locator(url,false); }
AppleMusicTrackLocator apple_music_album_locator(Utf8View url) noexcept { return apple_music_locator(url,true); }
AppleMusicCollectionLocator apple_music_collection_locator(Utf8View url) noexcept {
    if (const auto album=apple_music_album_locator(url); album.id.bytes) return {album.id,album.country,false};
    Url u{}; Utf8View song{};
    if (!parse(url,&u) || provider(u)!=EmbedProvider::apple_music || u.path.bytes<14 || u.path.data[0]!='/' || u.path.data[3]!='/' ||
        !parameter(u.query,utf8_literal("i"),&song) || song.bytes) return {};
    const Utf8View country{u.path.data+1,2},path{u.path.data+3,u.path.bytes-3};
    for (u32 i=0;i<country.bytes;++i) if (ascii_lower(country.data[i])<'a' || ascii_lower(country.data[i])>'z') return {};
    if (!prefix(path,utf8_literal("/playlist/"))) return {};
    u32 last=path.bytes; while (last && path.data[last-1]!='/') --last;
    const Utf8View id{path.data+last,path.bytes-last};
    if (id.bytes<4 || id.bytes>128 || !prefix(id,utf8_literal("pl."))) return {};
    for (u32 n=3;n<id.bytes;++n) if (!ascii_is_alnum(id.data[n]) && id.data[n]!='-' && id.data[n]!='_') return {};
    return {id,country,true,u.path};
}
bool embed_collection_url(Utf8View url) noexcept { return soundcloud_collection_url(url) || youtube_collection_locator(url).list.bytes || apple_music_collection_locator(url).id.bytes; }
bool embed_provider_video_frame(EmbedProvider value) noexcept {
    return value == EmbedProvider::youtube || value == EmbedProvider::youtube_music ||
           value == EmbedProvider::twitch || value == EmbedProvider::twitch_vod || value == EmbedProvider::twitch_live || value == EmbedProvider::tiktok;
}
SteamVideoLocator steam_video_locator(Utf8View url) noexcept {
    Url u{};
    if (!parse(url, &u) || provider(u) != EmbedProvider::steam) return {};
    const bool video = prefix(u.path, utf8_literal("/video/"));
    const u32 begin = video ? 7 : prefix(u.path, utf8_literal("/app/")) ? 5 : 0;
    if (!begin) return {};
    u32 end = begin;
    while (end < u.path.bytes && u.path.data[end] != '/') ++end;
    SteamVideoLocator result{{u.path.data + begin, end - begin}, {}};
    if (!steam_id(result.app)) return {};
    if (end < u.path.bytes) {
        const u32 start = ++end;
        while (end < u.path.bytes && u.path.data[end] != '/') ++end;
        if (video && end > start) {
            result.movie = {u.path.data + start, end - start};
            if (!steam_id(result.movie)) return {};
        }
        if (!video) for (u32 i = start; i < end; ++i)
            if (!ascii_is_alnum(u.path.data[i]) && u.path.data[i] != '_' && u.path.data[i] != '-') return {};
        if (end < u.path.bytes && end + 1 != u.path.bytes) return {};
    }
    return result;
}
TwitterVideoLocator twitter_video_locator(Utf8View url) noexcept {
    Url u{};
    if (!parse(url,&u) || provider(u)!=EmbedProvider::twitter) return {};
    TwitterVideoLocator out{};
    if (host(u,utf8_literal("platform.twitter.com"))) {
        if (!equal(u.path,utf8_literal("/embed/Tweet.html")) ||
            !parameter(u.query,utf8_literal("id"),&out.id)) return {};
    } else if(prefix(u.path,utf8_literal("/i/videos/tweet/"))) {
        out.id={u.path.data+16,u.path.bytes-16};
    } else {
        if(u.path.bytes<2 || u.path.data[0]!='/') return {};
        u32 end=1;
        while(end<u.path.bytes && u.path.data[end]!='/') ++end;
        if(end<2 || end>16) return {};
        for(u32 i=1;i<end;++i) if(!ascii_is_alnum(u.path.data[i]) && u.path.data[i]!='_') return {};
        const Utf8View rest{u.path.data+end,u.path.bytes-end};
        if(!prefix(rest,utf8_literal("/status/"))) return {};
        const u32 begin=end+8;
        end=begin;
        while(end<u.path.bytes && u.path.data[end]!='/') ++end;
        out.id={u.path.data+begin,end-begin};
        if(end<u.path.bytes && end+1!=u.path.bytes) {
            const Utf8View suffix{u.path.data+end,u.path.bytes-end};
            if(suffix.bytes!=8 || !prefix(suffix,utf8_literal("/video/")) || suffix.data[7]<'1' || suffix.data[7]>'4') return {};
            out.media_index=static_cast<u8>(suffix.data[7]-'0');
        }
    }
    return twitter_post_id(out.id) ? out : TwitterVideoLocator{};
}
bool vimeo_privacy_hash(Utf8View value) noexcept {
    if (!value.data || value.bytes != 10) return false;
    for (u32 i = 0; i < value.bytes; ++i) if (!ascii_is_hex_digit(value.data[i])) return false;
    return true;
}
Utf8View vimeo_video_id(Utf8View url, Utf8View* privacy_hash) noexcept {
    Url parsed{};
    if (!parse(url, &parsed)) return {};
    if (provider(parsed) == EmbedProvider::vimeo) {
        const bool player = host(parsed, utf8_literal("player.vimeo.com"));
        Utf8View parts[4]{}; u32 count = 0;
        if (!parsed.path.bytes || parsed.path.data[0] != '/') return {};
        for (u32 at = 1; at <= parsed.path.bytes;) {
            u32 end = at;
            while (end < parsed.path.bytes && parsed.path.data[end] != '/') ++end;
            if (at == end || count == 4) return {};
            parts[count++] = {parsed.path.data + at, end - at}; at = end + 1;
        }
        Utf8View privacy{};
        if (!parameter(parsed.query, utf8_literal("h"), &privacy)) return {};
        Utf8View id{};
        if (player) {
            if (count != 2 || !equal(parts[0], utf8_literal("video"))) return {};
            id = parts[1];
        } else if (count <= 2) {
            id = parts[0];
            if (count == 2) { if (privacy.data) return {}; privacy = parts[1]; }
        } else {
            const bool channel = count == 3 && (equal(parts[0], utf8_literal("channels")) || equal(parts[0], utf8_literal("ondemand")));
            const bool group = count == 4 && equal(parts[0], utf8_literal("groups")) && equal(parts[2], utf8_literal("videos"));
            const bool album = count == 4 && equal(parts[0], utf8_literal("album")) && equal(parts[2], utf8_literal("video")) && steam_id(parts[1]);
            if (!channel && !group && !album) return {};
            for (u32 i = 0; i < parts[1].bytes; ++i)
                if (!ascii_is_alnum(parts[1].data[i]) && parts[1].data[i] != '-' && parts[1].data[i] != '_') return {};
            id = parts[count - 1];
        }
        if (!steam_id(id) || (privacy.data && !vimeo_privacy_hash(privacy))) return {};
        if (privacy_hash) *privacy_hash = privacy;
        return id;
    }
    return {};
}
YoutubeCollectionLocator youtube_collection_locator(Utf8View url) noexcept {
    Url u{}; if (!parse(url, &u)) return {};
    const auto p = provider(u);
    if (p != EmbedProvider::youtube && p != EmbedProvider::youtube_music) return {};
    const auto video = embed_video_locator(url);
    if (!equal(u.path, utf8_literal("/watch")) && !equal(u.path, utf8_literal("/playlist")) && !equal(u.path, utf8_literal("/embed/videoseries")) && !video.id.bytes) return {};
    YoutubeCollectionLocator result{};
    if (!parameter(u.query, utf8_literal("list"), &result.list) || result.list.bytes < 3 || result.list.bytes > 128) return {};
    for (u32 i = 0; i < result.list.bytes; ++i) if (!ascii_is_alnum(result.list.data[i]) && result.list.data[i] != '-' && result.list.data[i] != '_') return {};
    if (!parameter(u.query, utf8_literal("v"), &result.video)) return {};
    if (result.video.bytes) {
        const auto item = embed_video_locator(url);
        if (item.id.bytes != 11) return {};
        result.video = item.id;
    }
    if (!result.video.bytes) result.video = video.id;
    if (prefix(result.list, utf8_literal("RD")) && !result.video.bytes) return {};
    return result;
}
Utf8View youtube_collection_source(Utf8View source, Utf8View content) noexcept {
    if (youtube_collection_locator(source).list.bytes) return source;
    const auto video = embed_video_locator(source);
    if ((video.provider != EmbedProvider::youtube && video.provider != EmbedProvider::youtube_music) ||
        video.id.bytes != 11 || !content.data) return source;
    const auto boundary = [](u8 c) noexcept {
        return c <= 32 || c >= 127 || c == '<' || c == '>' || c == '(' || c == ')' ||
               c == '[' || c == ']' || c == '"' || c == '\'' || c == '`' || c == '*';
    };
    Utf8View found{}, list{};
    for (u32 at = 0; at + 8 <= content.bytes;) {
        if ((at && !boundary(static_cast<u8>(content.data[at - 1]))) ||
            !ascii_equal_ignore_case({content.data + at, 8}, utf8_literal("https://"))) { ++at; continue; }
        u32 end = at + 8;
        while (end < content.bytes && !boundary(static_cast<u8>(content.data[end]))) ++end;
        u32 trimmed = end;
        while (trimmed > at && (content.data[trimmed - 1] == '.' || content.data[trimmed - 1] == ',' ||
                               content.data[trimmed - 1] == '!' || content.data[trimmed - 1] == ';')) --trimmed;
        const Utf8View candidate{content.data + at, trimmed - at};
        const auto queue = youtube_collection_locator(candidate);
        if (queue.list.bytes && equal(queue.video, video.id)) {
            if (found.bytes && !equal(list, queue.list)) return source;
            found = candidate;
            list = queue.list;
        }
        at = end;
    }
    return found.bytes ? found : source;
}
EmbedVideoLocator embed_video_locator(Utf8View url) noexcept {
    Url parsed{};
    if (!parse(url, &parsed)) return {};
    EmbedVideoLocator out{{}, 0, provider(parsed)};
    if (out.provider == EmbedProvider::vimeo) {
        out.id = vimeo_video_id(url); Utf8View time{};
        if (!parameter(parsed.fragment, utf8_literal("t"), &time) || (time.data && !time.bytes) ||
            !seconds(time, &out.start_seconds)) return {};
        return out;
    }
    if (out.provider == EmbedProvider::pornhub) {
        if (prefix(parsed.path, utf8_literal("/embed/")))
            out.id = {parsed.path.data + 7, parsed.path.bytes - 7};
        else if (equal(parsed.path, utf8_literal("/view_video.php")) &&
                 !parameter(parsed.query, utf8_literal("viewkey"), &out.id)) return {};
        if (!pornhub_video_id(out.id)) out.id = {};
        return out;
    }
    if (out.provider == EmbedProvider::twitter) { out.id=twitter_video_locator(url).id; return out; }
    if (out.provider == EmbedProvider::imgur) {
        auto path=parsed.path;
        if(path.bytes && path.data[0]=='/') {
            path={path.data+1,path.bytes-1};
            constexpr Utf8View endings[]{utf8_literal("/embed"),utf8_literal(".gifv"),utf8_literal(".mp4")};
            for(auto ending:endings) if(path.bytes>ending.bytes &&
                equal({path.data+path.bytes-ending.bytes,ending.bytes},ending)) {
                path.bytes-=ending.bytes;break;
            }
            if(imgur_video_id(path)) out.id=path;
        }
        return out;
    }
    if (out.provider == EmbedProvider::steam) { out.id = steam_video_locator(url).app; return out; }
    if (out.provider == EmbedProvider::streamable) {
        const u32 prefix_bytes = prefix(parsed.path, utf8_literal("/e/")) || prefix(parsed.path, utf8_literal("/o/")) ? 3 : 1;
        if (parsed.path.bytes > prefix_bytes) out.id = {parsed.path.data + prefix_bytes, parsed.path.bytes - prefix_bytes};
        Utf8View start{};
        if (!streamable_video_id(out.id) || !parameter(parsed.query, utf8_literal("start"), &start) ||
            !seconds(start, &out.start_seconds)) return {};
        return out;
    }
    if (out.provider == EmbedProvider::twitch) {
        if (host(parsed, utf8_literal("player.twitch.tv")) && (!parsed.path.bytes || equal(parsed.path, utf8_literal("/")))) {
            Utf8View channel{}, clip{};
            if (!parameter(parsed.query, utf8_literal("video"), &out.id) ||
                !parameter(parsed.query, utf8_literal("channel"), &channel) ||
                !parameter(parsed.query, utf8_literal("clip"), &clip) || clip.bytes || (channel.bytes && out.id.bytes)) return {};
            if (channel.bytes) {
                if (!twitch_channel_id(channel)) return {};
                out.id = channel; out.provider = EmbedProvider::twitch_live; return out;
            }
            if (out.id.bytes && out.id.data[0] == 'v') out.id = {out.id.data + 1, out.id.bytes - 1};
        } else if (!host(parsed, utf8_literal("clips.twitch.tv")) && prefix(parsed.path, utf8_literal("/videos/"))) {
            out.id = {parsed.path.data + 8, parsed.path.bytes - 8};
        }
        if (out.id.bytes) {
            Utf8View time{}, embed_time{}; u32 embedded_seconds{};
            if (!twitch_vod_id(out.id) || !parameter(parsed.query, utf8_literal("t"), &time) ||
                !parameter(parsed.query, utf8_literal("time"), &embed_time) || !seconds(time, &out.start_seconds) ||
                !seconds(embed_time, &embedded_seconds) ||
                (time.bytes && embed_time.bytes && out.start_seconds != embedded_seconds)) return {};
            if (embed_time.bytes) out.start_seconds = embedded_seconds;
            out.provider = EmbedProvider::twitch_vod; return out;
        }
        if (host(parsed, utf8_literal("clips.twitch.tv"))) {
            if (equal(parsed.path, utf8_literal("/embed"))) {
                if (!parameter(parsed.query, utf8_literal("clip"), &out.id)) return {};
            } else if (parsed.path.bytes > 1) out.id = {parsed.path.data + 1, parsed.path.bytes - 1};
        } else if (!host(parsed, utf8_literal("player.twitch.tv")) && parsed.path.bytes > 1) {
            u32 end = 1;
            while (end < parsed.path.bytes && parsed.path.data[end] != '/') ++end;
            const Utf8View rest{parsed.path.data + end, parsed.path.bytes - end};
            if (end > 1 && prefix(rest, utf8_literal("/clip/"))) out.id = {rest.data + 6, rest.bytes - 6};
            else if (!rest.bytes || equal(rest, utf8_literal("/"))) {
                const Utf8View channel{parsed.path.data + 1, end - 1};
                constexpr Utf8View reserved[]{utf8_literal("directory"), utf8_literal("downloads"), utf8_literal("search"),
                    utf8_literal("settings"), utf8_literal("login"), utf8_literal("signup"), utf8_literal("videos"), utf8_literal("p")};
                for (auto name : reserved) if (ascii_equal_ignore_case(channel, name)) return {};
                if (twitch_channel_id(channel)) { out.id = channel; out.provider = EmbedProvider::twitch_live; return out; }
            }
        }
        if (!twitch_clip_id(out.id)) out.id = {};
        return out;
    }
    if (out.provider == EmbedProvider::tiktok) {
        constexpr Utf8View paths[]{utf8_literal("/player/v1/"), utf8_literal("/embed/v2/"), utf8_literal("/embed/")};
        for (auto path : paths)
            if (prefix(parsed.path, path)) { out.id = {parsed.path.data + path.bytes, parsed.path.bytes - path.bytes}; break; }
        if (!out.id.bytes && prefix(parsed.path, utf8_literal("/@"))) {
            u32 end = 2;
            while (end < parsed.path.bytes && parsed.path.data[end] != '/') ++end;
            const Utf8View rest{parsed.path.data + end, parsed.path.bytes - end};
            if (end > 2 && prefix(rest, utf8_literal("/video/"))) out.id = {rest.data + 7, rest.bytes - 7};
        }
        u64 value = 0;
        bool valid = out.id.bytes && out.id.bytes <= 20 && out.id.data[0] != '0';
        for (u32 i = 0; valid && i < out.id.bytes; ++i) {
            const u32 digit = static_cast<u8>(out.id.data[i]) - '0';
            valid = digit <= 9 && value <= (~u64{0} - digit) / 10;
            if (valid) value = value * 10 + digit;
        }
        if (!valid) out.id = {};
        return out;
    }
    if (out.provider != EmbedProvider::youtube && out.provider != EmbedProvider::youtube_music) return out;
    if (host(parsed, utf8_literal("youtu.be"))) {
        if (parsed.path.bytes) out.id = {parsed.path.data + 1, parsed.path.bytes - 1};
    } else if (equal(parsed.path, utf8_literal("/watch"))) {
        if (!parameter(parsed.query, utf8_literal("v"), &out.id)) return {};
    } else {
        constexpr Utf8View paths[]{utf8_literal("/embed/"), utf8_literal("/shorts/"), utf8_literal("/live/")};
        for (auto path : paths)
            if (prefix(parsed.path, path)) out.id = {parsed.path.data + path.bytes, parsed.path.bytes - path.bytes};
    }
    if (!id_valid(out.id)) { out.id = {}; return out; }
    Utf8View start{}, time{};
    if (!parameter(parsed.query, utf8_literal("start"), &start) ||
        !parameter(parsed.query, utf8_literal("t"), &time) ||
        !seconds(start.bytes ? start : time, &out.start_seconds)) return {};
    return out;
}
} // namespace maicord
