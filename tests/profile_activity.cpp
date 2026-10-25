// Rich presence cards in the profile popout and full profile: card order,
// headings and lines, timer and progress math, asset URL selection and the
// artwork fallback, the card layout, in-place timer ticks, and the REST
// targets and responses for application icons and button metadata.
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/model/user_activity.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/ui/profile_activity.hpp"
#include "maicord/ui/activity_status.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/ui/profile_scene.hpp"
#include "maicord/ui/icon.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/media/asset.hpp"

namespace maicord::profile_activity_test {
u32 failures = 0;
void check(bool valid, const char* name) noexcept {
    test_check(&failures, valid, name);
}
u32 scratch[16384];
bool decode(Utf8View presence, UserActivitySet* set, u64 user = 42) noexcept {
    bool has = false;
    const auto scan = json_scan_structurals_avx1(presence, {scratch, 16384});
    return scan.ok() &&
           user_activities_decode({presence, {scratch, scan.count}}, {presence, 0, JsonValueKind::object}, user, set,
                                  &has) &&
           has;
}
bool same(Utf8View a, Utf8View b) noexcept {
    return utf8_equal(a, b);
}
Utf8View text(const UserActivity& a, UserActivityField field) noexcept {
    return user_activity_text(a, field);
}
template <u32 N> bool order_is(const UserActivitySet& set, const u8 (&expected)[N]) noexcept {
    u8 order[kUserActivityMaximum];
    const u32 count = profile_activity_order(set, {order, kUserActivityMaximum});
    if (count != N)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (order[i] != expected[i])
            return false;
    return true;
}

void test_order() noexcept {
    UserActivitySet set{};
    check(decode(utf8_literal(R"({"activities":[
        {"type":2,"name":"Spotify","created_at":1000,"party":{"id":"spotify:1"}},
        {"type":0,"name":"Old Game","created_at":500},
        {"type":0,"name":"New Game","details":"Level 1","created_at":400}]})"),
                 &set) &&
              order_is(set, {2, 0}),
          "order: playing outranks listening; one playing card, rich presence first");
    check(decode(utf8_literal(R"({"activities":[
        {"type":3,"name":"Cinema","created_at":1},
        {"type":1,"name":"Twitch","created_at":2},
        {"type":5,"name":"Arena","created_at":3}]})"),
                 &set) &&
              order_is(set, {2, 1, 0}),
          "order: competing, streaming, then the other types");
    check(decode(utf8_literal(R"({"activities":[
        {"type":3,"name":"First","application_id":"5","created_at":1},
        {"type":2,"name":"Second","application_id":"5","created_at":9},
        {"type":2,"name":"First","created_at":5}]})"),
                 &set) &&
              order_is(set, {1, 2}),
          "order: newer first; one card per application ID or name");
    check(decode(utf8_literal(R"({"activities":[{"type":2,"name":"A"},{"type":3,"name":"B"}]})"), &set) &&
              order_is(set, {0, 1}),
          "order: equal activities keep the presence order");
}

void test_times() noexcept {
    char out[32];
    const auto clock = [&](u64 seconds) noexcept { return Utf8View{out, profile_activity_clock_text(seconds, {out, 32})}; };
    const auto track = [&](u64 seconds) noexcept { return Utf8View{out, profile_activity_track_text(seconds, {out, 32})}; };
    check(same(clock(5), utf8_literal("0:05")) && same(clock(754), utf8_literal("12:34")) &&
              same(clock(3723), utf8_literal("1:02:03")) && same(clock(36000), utf8_literal("10:00:00")),
          "times: timer chips read m:ss, then h:mm:ss");
    check(same(track(5), utf8_literal("00:05")) && same(track(195), utf8_literal("03:15")) &&
              same(track(3723), utf8_literal("01:02:03")),
          "times: progress ends read mm:ss, then hh:mm:ss");

    UserActivity game{};
    game.type = UserActivityType::playing;
    game.start_ms = 1'000'000;
    game.created_ms = 1'000'000;
    auto c = profile_activity_clock(game, 1'065'999);
    check(c.timer == ProfileActivityTimer::elapsed && c.seconds == 65,
          "times: elapsed since the start, on whole seconds");
    game.start_ms = 0;
    game.created_ms = 2'000'000;
    c = profile_activity_clock(game, 2'005'999);
    check(c.timer == ProfileActivityTimer::elapsed && c.seconds == 5, "times: without a start the creation counts");
    game.created_ms = 1'000'000;
    game.end_ms = 1'300'000;
    c = profile_activity_clock(game, 1'100'000);
    const auto ended = profile_activity_clock(game, 1'400'000);
    check(c.timer == ProfileActivityTimer::countdown && c.seconds == 200 &&
              ended.timer == ProfileActivityTimer::elapsed && ended.seconds == 400,
          "times: an end after creation counts down, then elapsed time resumes");
    game.created_ms = 1'500'000;
    game.start_ms = 1'000'000;
    c = profile_activity_clock(game, 1'400'000);
    check(c.timer == ProfileActivityTimer::elapsed && c.seconds == 300, "times: an earlier end stops the elapsed time");
    UserActivity song{};
    song.type = UserActivityType::listening;
    song.created_ms = 1'000'000;
    song.end_ms = 1'300'000;
    c = profile_activity_clock(song, 1'350'000);
    check(c.timer == ProfileActivityTimer::elapsed && c.seconds == 300,
          "times: listening never counts down and stops at the end");
    song.start_ms = 1'000'000;
    song.end_ms = 1'200'000;
    ProfileActivityProgress progress{};
    const bool shown = profile_activity_progress(song, 1'090'500, &progress);
    check(profile_activity_clock(song, 1'090'500).timer == ProfileActivityTimer::none && shown &&
              progress.elapsed == 90 && progress.duration == 200 && progress.fraction > .4524f &&
              progress.fraction < .4526f,
          "times: listening with both timestamps shows progress instead of a timer");
    profile_activity_progress(song, 900'000, &progress);
    const bool before = progress.elapsed == 0 && progress.fraction == 0;
    profile_activity_progress(song, 5'000'000, &progress);
    check(before && progress.elapsed == 200 && progress.fraction == 1 &&
              !profile_activity_progress(game, 1'090'500, &progress),
          "times: progress clamps to the track and is for listening and watching only");
}

void test_urls() noexcept {
    UserActivitySet set{};
    check(decode(utf8_literal(R"({"activities":[
        {"type":0,"name":"Mossrun","application_id":"500000000000000001",
         "assets":{"large_image":"500000000000000002","small_image":"mp:external/abc/https/x.png"}},
        {"type":2,"name":"Spotify","party":{"id":"spotify:1"},
         "assets":{"large_image":"spotify:ab67616d0000b273deadbeefdeadbeefdeadbeef","small_image":"twitch:someone"}},
        {"type":1,"name":"Twitch","application_id":"7","assets":{"large_image":"youtube:abc","small_image":"bad_key"}}]})"),
                 &set) &&
              set.count == 3,
          "urls: three activities decode");
    char url[512];
    MediaAttachmentFormat format{};
    u32 bytes = user_activity_image_url(set.items[0], true, {url, sizeof(url)}, &format);
    check(same({url, bytes}, utf8_literal("https://cdn.discordapp.com/app-assets/500000000000000001/"
                                          "500000000000000002.png?size=160")) &&
              format == MediaAttachmentFormat::png,
          "urls: application assets as PNG at 160");
    bytes = user_activity_image_url(set.items[0], false, {url, sizeof(url)}, &format);
    check(same({url, bytes},
               utf8_literal("https://media.discordapp.net/external/abc/https/x.png?format=webp&width=160&height=160")) &&
              format == MediaAttachmentFormat::webp,
          "urls: mp: keys through the media proxy as WebP");
    bytes = user_activity_image_url(set.items[1], true, {url, sizeof(url)}, &format);
    check(same({url, bytes}, utf8_literal("https://i.scdn.co/image/ab67616d0000b273deadbeefdeadbeefdeadbeef")) &&
              format == MediaAttachmentFormat::jpeg,
          "urls: spotify: album art from i.scdn.co as JPEG");
    check(!user_activity_image_url(set.items[1], false, {url, sizeof(url)}, &format) &&
              !user_activity_image_url(set.items[2], true, {url, sizeof(url)}, &format) &&
              !user_activity_image_url(set.items[2], false, {url, sizeof(url)}, &format),
          "urls: Twitch and YouTube previews and non-numeric keys have no URL");
    bytes = user_activity_application_icon_url(5, utf8_literal("abc123"), {url, sizeof(url)});
    check(same({url, bytes}, utf8_literal("https://cdn.discordapp.com/app-icons/5/abc123.png?size=160")) &&
              !user_activity_application_icon_url(5, utf8_literal("a/b"), {url, sizeof(url)}) &&
              !user_activity_application_icon_url(0, utf8_literal("abc"), {url, sizeof(url)}),
          "urls: application icons as PNG at 160");
    char long_key[400];
    u32 key = 0;
    const auto put = [&](Utf8View v) noexcept {
        copy_bytes(long_key + key, v.data, v.bytes);
        key += v.bytes;
    };
    put(utf8_literal(R"({"activities":[{"type":0,"name":"G","assets":{"large_image":"mp:external/)"));
    for (u32 i = 0; i < 28; ++i)
        put(utf8_literal("abcdefghij"));
    put(utf8_literal(R"(/x.png"}}]})"));
    UserActivitySet external{};
    check(decode({long_key, key}, &external) &&
              text(external.items[0], UserActivityField::large_image).bytes == 3 + 9 + 280 + 6 &&
              user_activity_image_url(external.items[0], true, {url, sizeof(url)}, &format),
          "urls: media proxy keys of about 300 characters are kept whole");
    const auto a = user_activity_artwork(set.items[0], false);
    const auto b = user_activity_artwork(set.items[2], true);
    const auto c = user_activity_artwork(set.items[2], false);
    UserActivitySet small_only{};
    decode(utf8_literal(R"({"activities":[{"type":0,"name":"G","application_id":"9","assets":{"small_image":"1"}}]})"),
           &small_only);
    const auto d = user_activity_artwork(small_only.items[0], false);
    const auto e = user_activity_artwork(small_only.items[0], true);
    check(a.large == UserActivityArt::large && a.badge && b.large == UserActivityArt::application && !b.badge &&
              c.large == UserActivityArt::none && !c.badge && d.large == UserActivityArt::small_image && !d.badge &&
              e.large == UserActivityArt::application && e.badge,
          "urls: artwork falls back to the application icon, then the small image in the large slot");
}

void test_content() noexcept {
    UserActivitySet set{};
    ProfileActivityContent c{};
    decode(utf8_literal(R"({"activities":[
        {"type":0,"name":"Mossrun","application_id":"5","details":"Chapter 7","state":"Ascent",
         "party":{"id":"p","size":[2,4]},"buttons":["Watch","Join"],"flags":2},
        {"type":0,"name":"Mossrun","application_id":"5","state":"Ascent","flags":2},
        {"type":0,"name":"Lobby Game","application_id":"6","state":"Lobby","party":{"size":[1,4]},"flags":256}]})"),
           &set);
    check(profile_activity_content(set.items[0], false, false, &c) && same(c.heading.view(), utf8_literal("Playing")) &&
              same(c.title.view(), utf8_literal("Mossrun")) && c.line_count == 1 &&
              same(c.lines[0].view(), utf8_literal("Chapter 7")) &&
              same(c.party.view(), utf8_literal("Ascent (2 of 4)")) && c.action == ProfileActivityAction::custom &&
              c.chips && c.artwork && !c.progress,
          "content: a game shows its name, details and the party chip; its own buttons win");
    check(profile_activity_content(set.items[1], false, false, &c) && c.action == ProfileActivityAction::join &&
              same(c.hint.view(), utf8_literal("Mossrun is not detected")) && c.line_count == 1 &&
              same(c.lines[0].view(), utf8_literal("Ascent")) && !c.party.bytes &&
              profile_activity_content(set.items[1], false, true, &c) && c.action == ProfileActivityAction::none,
          "content: joinable games offer Ask to Join (not to yourself); state shows without a party");
    check(profile_activity_content(set.items[2], true, false, &c) && c.line_count == 1 &&
              same(c.lines[0].view(), utf8_literal("Lobby (1 of 4)")) && !c.party.bytes && !c.artwork && !c.chips,
          "content: embedded parties read in the state line; bots show no artwork or chips");

    decode(utf8_literal(R"({"activities":[
        {"type":2,"name":"Spotify","details":"Native Song","state":"First Artist; Second Artist",
         "party":{"id":"spotify:abc"},"timestamps":{"start":1,"end":2},"assets":{"large_text":"Album"}},
        {"type":2,"name":"Tunes","state":"Artist","assets":{"large_text":"Album"}},
        {"type":3,"name":"Cinema","details":"A Film","state":"Part 1",
         "assets":{"large_text":"Season 2, Episode 5"}}]})"),
           &set);
    check(profile_activity_content(set.items[0], false, false, &c) && c.spotify &&
              same(c.heading.view(), utf8_literal("Listening to Spotify")) &&
              same(c.title.view(), utf8_literal("Native Song")) && c.line_count == 1 &&
              same(c.lines[0].view(), utf8_literal("First Artist, Second Artist")) && c.progress &&
              c.action == ProfileActivityAction::none,
          "content: Spotify reads track and artists, without the album, under a progress bar");
    check(profile_activity_content(set.items[1], false, false, &c) && !c.spotify &&
              same(c.heading.view(), utf8_literal("Listening to")) && same(c.title.view(), utf8_literal("Tunes")) &&
              c.line_count == 2 && same(c.lines[1].view(), utf8_literal("Album")),
          "content: other listening shows name, state and large text under Listening to");
    check(profile_activity_content(set.items[2], false, false, &c) &&
              same(c.heading.view(), utf8_literal("Watching Cinema")) &&
              same(c.title.view(), utf8_literal("A Film")) && c.line_count == 1 &&
              same(c.episode.view(), utf8_literal("S2E5")) && c.clock_glyph == ProfileActivityGlyph::screen,
          "content: watching drops the large text line for an episode chip");

    decode(utf8_literal(R"({"activities":[
        {"type":1,"name":"Twitch","details":"Speedrun","url":"https://twitch.tv/someone"},
        {"type":0,"name":"Console Game","platform":"xbox"},
        {"type":0,"name":"Headless","session_id":"h:steam,123"}]})"),
           &set);
    check(profile_activity_content(set.items[0], false, false, &c) &&
              same(c.heading.view(), utf8_literal("Streaming Twitch")) && same(c.title.view(), utf8_literal("Speedrun")) &&
              c.action == ProfileActivityAction::watch && !c.chips,
          "content: Twitch streams offer Watch and show no chips");
    check(profile_activity_content(set.items[1], false, false, &c) &&
              same(c.heading.view(), utf8_literal("Playing on Xbox")) &&
              profile_activity_content(set.items[2], false, false, &c) &&
              same(c.heading.view(), utf8_literal("Playing on Steam")),
          "content: console and headless sessions name their platform");
    decode(utf8_literal(R"({"activities":[{"type":5,"name":"Arena","details":"Finals"},{"type":5,"name":"Arena"}]})"), &set);
    char heading[64];
    check(same({heading, user_activity_heading(set.items[0], {heading, 64})}, utf8_literal("Competing in Arena")) &&
              same({heading, user_activity_heading(set.items[1], {heading, 64})}, utf8_literal("Competing in")),
          "content: competing names the application only beside details");

    char out[64];
    const auto party = [&](Utf8View state, u32 size, u32 max) noexcept {
        return Utf8View{out, profile_activity_party_text(state, size, max, {out, 64})};
    };
    check(same(party({}, 2, 4), utf8_literal("2 of 4")) && same(party(utf8_literal("S"), 2, 0), utf8_literal("S (2)")) &&
              same(party(utf8_literal("S"), 0, 0), utf8_literal("S")) && !party({}, 0, 0).bytes,
          "content: party chip text");
    const auto episode = [&](Utf8View large) noexcept { return Utf8View{out, profile_activity_episode_text(large, {out, 64})}; };
    check(same(episode(utf8_literal("Season 1, Episode 12")), utf8_literal("S1E12")) &&
              same(episode(utf8_literal("x S 1 2, E 3")), utf8_literal("S2E3")) && !episode(utf8_literal("Ep 3")).bytes,
          "content: episode chip from large text");
}

const ProfilePart* find(const ProfileScene& s, ProfileMediaKind media, u32 nth = 0) noexcept {
    for (u32 i = 0; i < s.part_count; ++i)
        if (s.parts[i].media == media && !nth--)
            return &s.parts[i];
    return nullptr;
}
const ProfilePart* find_action(const ProfileScene& s, ProfileAction action, u32 nth = 0) noexcept {
    for (u32 i = 0; i < s.part_count; ++i)
        if (s.parts[i].action == action && !nth--)
            return &s.parts[i];
    return nullptr;
}
const ProfilePart* find_text(const ProfileScene& s, Utf8View value) noexcept {
    for (u32 i = 0; i < s.part_count; ++i)
        if (utf8_equal(s.parts[i].text, value))
            return &s.parts[i];
    return nullptr;
}
UiRect rect(const ProfileScene& s, const ProfilePart* part) noexcept {
    const auto* node = part ? ui_widget_manager_layout_node(s.widgets, part->widget) : nullptr;
    return node ? node->rectangle : UiRect{};
}
u32 count(const ProfileScene& s, ProfileMediaKind media) noexcept {
    u32 n = 0;
    for (u32 i = 0; i < s.part_count; ++i)
        n += s.parts[i].media == media;
    return n;
}
template <u32 N> void set_text(ProfileString<N>* out, Utf8View value) noexcept {
    copy_bytes(out->data, value.data, value.bytes);
    out->bytes = static_cast<u16>(value.bytes);
}

void test_layout() noexcept {
    // Times are relative to a fixed clock the tick supplies.
    constexpr u64 now = 1'700'000'100'000ull;
    UserActivitySet set{};
    const bool decoded = decode(utf8_literal(R"({"activities":[
        {"type":2,"name":"Spotify","details":"Native Song","state":"First Artist; Second Artist",
         "party":{"id":"spotify:abc"},"session_id":"aaaa","created_at":"1700000000000",
         "timestamps":{"start":1700000000000,"end":1700000200000},
         "assets":{"large_image":"spotify:ab67616d0000b273deadbeefdeadbeefdeadbeef"}},
        {"type":0,"name":"Mossrun","application_id":"500000000000000001","details":"Chapter 7","state":"Ascent",
         "created_at":"1700000000000","timestamps":{"start":1700000000000},"party":{"id":"p","size":[2,4]},
         "session_id":"bbbb","buttons":["Watch","Join"],
         "assets":{"large_image":"500000000000000002","large_text":"High Ridge",
                   "small_image":"500000000000000003","small_text":"Wanderer"}}]})"),
                                &set);
    UserStore users{};
    UserProfile profile{};
    profile.user_id = 42;
    set_text(&profile.username, utf8_literal("activity"));
    set_text(&profile.display_name, utf8_literal("Activity Person"));
    ProfileScene s{};
    const UiRect viewport{0, 0, 1280, 720};
    bool ok = decoded && user_activity_upsert(&users.activities, set) &&
              profile_scene_open(&s, &profile, nullptr, &users, 1, {300, 200, 340, 240}, viewport) &&
              profile_scene_tick(&s, now);
    const auto* card = find(s, ProfileMediaKind::activity_card);
    const auto* image = find(s, ProfileMediaKind::activity_large);
    const auto* badge_part = find(s, ProfileMediaKind::activity_small);
    const auto c = rect(s, card), art = rect(s, image), corner = rect(s, badge_part);
    check(ok && count(s, ProfileMediaKind::activity_card) == 1 && find_text(s, utf8_literal("Playing")) &&
              find_text(s, utf8_literal("Mossrun")) && !find_text(s, utf8_literal("Native Song")) &&
              find_action(s, ProfileAction::activity) && card && card->action == ProfileAction::card,
          "layout: the popout shows the first card in order (the game) with the next peeking");
    check(ui_rect_width(c) == 268 && art.left == c.left + 8 && art.top == c.top + 8 + 16 + 8 &&
              ui_rect_width(art) == 60 && ui_rect_height(art) == 60 &&
              ui_rect_equal(corner, {art.right + 4 - 24, art.bottom + 4 - 24, art.right + 4, art.bottom + 4}) &&
              rect(s, find_text(s, utf8_literal("Mossrun"))).left == art.right + 8,
          "layout: popout cards pad 8 around a 60 DIP artwork with its 24 DIP badge 4 past the corner");
    check(same(image->hint, utf8_literal("High Ridge")) && same(badge_part->hint, utf8_literal("Wanderer")) &&
              image->action == ProfileAction::card,
          "layout: artwork hover text rides on the images");
    const auto* party = find_text(s, utf8_literal("Ascent (2 of 4)"));
    const auto* clock = find_text(s, utf8_literal("1:40"));
    check(party && clock && clock->tone == ProfileTone::positive && rect(s, clock).top == rect(s, party).top &&
              rect(s, clock).left > rect(s, party).right,
          "layout: the party chip, then the green elapsed timer, in one chip row");
    const auto b0 = rect(s, find_action(s, ProfileAction::activity_button, 0));
    const auto b1 = rect(s, find_action(s, ProfileAction::activity_button, 1));
    check(ui_rect_width(b0) == 252 && ui_rect_height(b0) == 32 && b1.top == b0.bottom + 8 && b0.left == c.left + 8 &&
              b0.top == art.bottom + 12 && c.bottom == b1.bottom + 8 &&
              find_text(s, utf8_literal("Watch")) && find_text(s, utf8_literal("Join")),
          "layout: popout buttons stack full width below the body");
    check(s.activity_timed && s.activity_set && s.activity_set->count == 2, "layout: a visible timer ticks");

    // Ticks rewrite same-length text in place and leave the widgets alone.
    const auto widget = clock->widget;
    const u32 revision = s.revision;
    const bool ticked = profile_scene_tick(&s, now + 1000);
    clock = find_text(s, utf8_literal("1:41"));
    check(ticked && clock && ui_widget_handle_equal(clock->widget, widget) && s.revision == revision + 1 &&
              !profile_scene_tick(&s, now + 1500),
          "tick: one in-place update per second");
    check(profile_scene_tick(&s, now + 500'000) && find_text(s, utf8_literal("10:00")),
          "tick: a longer time lays the card out again");

    // The full profile lists both cards in order, 8 apart, with 100 DIP art.
    s.presentation = ProfilePresentation::modal;
    s.tab = 0;
    ok = profile_scene_layout(&s, viewport, true) && profile_scene_tick(&s, now + 1'000'000);
    const auto first = rect(s, find(s, ProfileMediaKind::activity_card, 0));
    const auto second = rect(s, find(s, ProfileMediaKind::activity_card, 1));
    const auto big = rect(s, find(s, ProfileMediaKind::activity_large, 0));
    const auto spotify_art = find(s, ProfileMediaKind::activity_large, 1);
    check(ok && count(s, ProfileMediaKind::activity_card) == 2 && second.top == first.bottom + 8 &&
              ui_rect_width(big) == 100 && big.left == first.left + 12 && big.top == first.top + 12 + 16 + 8 &&
              rect(s, find(s, ProfileMediaKind::activity_small, 0)).right == big.right + 4,
          "layout: the full profile shows every card, 8 apart, padded 12 around 100 DIP art");
    const auto m0 = rect(s, find_action(s, ProfileAction::activity_button, 0));
    const auto m1 = rect(s, find_action(s, ProfileAction::activity_button, 1));
    check(m0.left == big.right + 12 && m1.left == m0.right + 8 && m0.top == m1.top &&
              ui_rect_width(m0) == 5 * 7 + 32 && m0.bottom <= first.bottom - 12,
          "layout: full profile buttons sit under the text at their natural width");
    const auto* track = find(s, ProfileMediaKind::activity_progress);
    check(track && track->color.red == 1 && find_text(s, utf8_literal("03:20")) &&
              find_text(s, utf8_literal("Listening to Spotify")) &&
              find_text(s, utf8_literal("First Artist, Second Artist")),
          "layout: Spotify shows artists and a progress bar at its end");
    ProfileMedia media{};
    s.activity_art_radius = 4;
    const auto spotify_index = static_cast<u32>(spotify_art - s.parts);
    check(spotify_art && profile_media_resolve(s, spotify_index, &media) &&
              same(media.url.view(), utf8_literal("https://i.scdn.co/image/ab67616d0000b273deadbeefdeadbeefdeadbeef")) &&
              media_asset_embed_format(media.key) == MediaAttachmentFormat::jpeg && media.mask_radius == 4,
          "layout: album art resolves to i.scdn.co with the concentric corner radius");
    const auto* badge = find(s, ProfileMediaKind::activity_small);
    const auto badge_rect = rect(s, badge);
    check(badge && profile_media_resolve(s, static_cast<u32>(badge - s.parts), &media) && media.circular &&
              ui_rect_equal(media.mask, badge_rect) && media.mask_radius == ui_rect_width(badge_rect) * .5f,
          "layout: the small image is masked to its own circle, not the profile card");

    // Away from the Activity tab nothing ticks; mutual friends, listed
    // outside the profile card, are masked to their own circles.
    profile.mutual_friend_count = profile.mutual_friends_count = 1;
    profile.mutual_friends[0].id = 7;
    set_text(&profile.mutual_friends[0].icon, utf8_literal("abc123"));
    s.tab = 1;
    ok = profile_scene_layout(&s, viewport, true);
    check(ok && !s.activity_timed && !profile_scene_tick(&s, now + 2'000'000), "layout: hidden timers do not tick");
    const auto* friend_face = find(s, ProfileMediaKind::peer_avatar);
    const auto face = rect(s, friend_face);
    check(friend_face && profile_media_resolve(s, static_cast<u32>(friend_face - s.parts), &media) &&
              face.left > s.card.right && ui_rect_equal(media.mask, face) &&
              media.mask_radius == ui_rect_width(face) * .5f,
          "layout: mutual friends in the full profile carry their own circular mask");
    profile.mutual_friend_count = profile.mutual_friends_count = 0;

    // A card without art asks for its application's icon, then shows it.
    UserActivitySet plain{};
    decode(utf8_literal(R"({"activities":[{"type":0,"name":"Iconic","application_id":"77"}]})"), &plain);
    user_activity_upsert(&users.activities, plain);
    ++users.revision;
    s.tab = 0;
    ok = profile_scene_layout(&s, viewport, true);
    check(ok && s.missing_icon_count == 1 && s.missing_icons[0] == 77, "layout: a missing icon is reported");
    ProfileApplication apps[1]{};
    apps[0].id = 77;
    set_text(&apps[0].icon, utf8_literal("cafe"));
    s.applications = {apps, 1};
    ok = profile_scene_layout(&s, viewport, true);
    const auto* icon = find(s, ProfileMediaKind::activity_large);
    check(ok && !s.missing_icon_count && icon &&
              profile_media_resolve(s, static_cast<u32>(icon - s.parts), &media) &&
              same(media.url.view(), utf8_literal("https://cdn.discordapp.com/app-icons/77/cafe.png?size=160")),
          "layout: the application icon stands in for missing art");
    profile_scene_release(&s);
    user_activity_release(&users.activities);
}

// The popout's Spotify card and a game card's glyphs; ticks once a second.
void test_popout_spotify() noexcept {
    constexpr u64 now = 1'700'000'100'000ull;
    UserActivitySet set{};
    const bool decoded = decode(utf8_literal(R"({"activities":[
        {"type":2,"name":"Spotify","details":"Native Song","state":"First Artist; Second Artist",
         "party":{"id":"spotify:abc"},"timestamps":{"start":1700000013000,"end":1700000377000},
         "assets":{"large_image":"spotify:ab67616d0000b273deadbeefdeadbeefdeadbeef","large_text":"Album"}}]})"),
                                &set);
    UserStore users{};
    UserProfile profile{};
    profile.user_id = 42;
    set_text(&profile.username, utf8_literal("listener"));
    set_text(&profile.display_name, utf8_literal("Listener"));
    ProfileScene s{};
    const UiRect viewport{0, 0, 1280, 720};
    const bool ok = decoded && user_activity_upsert(&users.activities, set) &&
                    profile_scene_open(&s, &profile, nullptr, &users, 1, {300, 200, 340, 240}, viewport) &&
                    profile_scene_tick(&s, now);
    const auto c = rect(s, find(s, ProfileMediaKind::activity_card));
    const auto art = rect(s, find(s, ProfileMediaKind::activity_large));
    const auto* title = find_text(s, utf8_literal("Native Song"));
    const auto* artists = find_text(s, utf8_literal("First Artist, Second Artist"));
    const auto* elapsed = find_text(s, utf8_literal("01:27"));
    const auto* total = find_text(s, utf8_literal("06:04"));
    const auto* bar = find(s, ProfileMediaKind::activity_progress);
    check(ok && find_text(s, utf8_literal("Listening to Spotify")) && ui_rect_width(art) == 60 &&
              art.left == c.left + 8 && title && artists && (title->style & text_style_bold) &&
              rect(s, title).left == art.right + 8 && rect(s, artists).top == rect(s, title).bottom,
          "spotify: header, 60 DIP album art, bold track and its artists");
    check(elapsed && total && bar && rect(s, elapsed).top == rect(s, total).top &&
              rect(s, bar).left > rect(s, elapsed).right && rect(s, bar).right < rect(s, total).left &&
              bar->color.red > .2389f && bar->color.red < .2391f && !find(s, ProfileMediaKind::activity_small) &&
              s.activity_timed,
          "spotify: elapsed and total times frame the progress bar; no timer chip");
    const u32 revision = s.revision;
    const float before = bar->color.red;
    check(!profile_scene_tick(&s, now + 400) && s.revision == revision && bar->color.red == before &&
              profile_scene_tick(&s, now + 1000) && s.revision == revision + 1 &&
              find_text(s, utf8_literal("01:28")) && bar->color.red > before,
          "spotify: the bar and elapsed time redraw only when the shown second changes");

    UserActivitySet game{};
    decode(utf8_literal(R"j({"activities":[{"type":0,"name":"Turbo Ball","details":"Duel 3:24 (2 - 2)",
        "state":"Ranked","timestamps":{"start":1700000080000},"application_id":"9",
        "assets":{"large_image":"11","small_image":"12"}}]})j"),
           &game);
    user_activity_upsert(&users.activities, game);
    ++users.revision;
    bool controller = false;
    const bool laid = profile_scene_layout(&s, viewport, false) && profile_scene_tick(&s, now);
    for (u32 i = 0; i < s.part_count; ++i)
        controller |= s.parts[i].icon == static_cast<u16>(UiIcon::controller) && s.parts[i].tone == ProfileTone::positive;
    const auto* clock = find_text(s, utf8_literal("0:20"));
    check(laid && find_text(s, utf8_literal("Playing")) && find_text(s, utf8_literal("Duel 3:24 (2 - 2)")) &&
              find_text(s, utf8_literal("Ranked")) && clock && clock->tone == ProfileTone::positive && controller &&
              find(s, ProfileMediaKind::activity_small),
          "game: details, state, the small image and a green timer with the controller glyph");
    profile_scene_release(&s);
    user_activity_release(&users.activities);
}

// The status line under a DM or member row's name.
void test_status_line() noexcept {
    UserStore users{};
    UserActivitySet set{};
    decode(utf8_literal(R"({"activities":[{"type":2,"name":"Spotify","details":"Track","state":"Grave; Choir",
        "party":{"id":"spotify:1"}},{"type":0,"name":"Turbo Ball"}]})"),
           &set);
    UserAppearance line{};
    line.user_id = 42;
    line.fields = appearance_activity;
    line.activity.bytes = static_cast<u16>(user_activity_summary(set.items[0], {line.activity.data, 256}));
    char storage[256];
    UiActivityStatus status{};
    bool ok = user_activity_upsert(&users.activities, set) && user_appearance_upsert(&users.appearance, line);
    const auto* record = user_appearance_find(users.appearance, 42);
    check(ok && ui_user_activity_status(users, 42, record, {storage, sizeof(storage)}, &status) &&
              status.glyph == UiActivityGlyph::music && same(status.text, utf8_literal("Grave, Choir")) &&
              !status.emoji.bytes,
          "status line: the first activity's glyph and short text replace its summary");
    UserAppearance custom = line;
    custom.fields = appearance_activity | appearance_custom_status;
    set_text(&custom.activity, utf8_literal("Out for lunch"));
    set_text(&custom.status_emoji, utf8_literal("\xF0\x9F\x8D\x9C"));
    ok = user_appearance_upsert(&users.appearance, custom);
    record = user_appearance_find(users.appearance, 42);
    check(ok && ui_user_activity_status(users, 42, record, {storage, sizeof(storage)}, &status) &&
              status.glyph == UiActivityGlyph::music && same(status.text, utf8_literal("Out for lunch")) &&
              same(status.emoji, utf8_literal("\xF0\x9F\x8D\x9C")),
          "status line: a custom status's text and emoji follow the activity glyph");
    check(ui_user_activity_status(users, 42, record, {}, &status) && status.glyph == UiActivityGlyph::music &&
              same(status.text, utf8_literal("Out for lunch")),
          "status line: without storage the glyph and custom status resolve");
    user_activity_upsert(&users.activities, {42});
    check(ui_user_activity_status(users, 42, record, {storage, sizeof(storage)}, &status) &&
              status.glyph == UiActivityGlyph::none && same(status.text, utf8_literal("Out for lunch")),
          "status line: a custom status alone has no glyph");
    ok = user_appearance_upsert(&users.appearance, line);
    record = user_appearance_find(users.appearance, 42);
    check(ok && record && !(record->fields & appearance_custom_status) &&
              ui_user_activity_status(users, 42, record, {storage, sizeof(storage)}, &status) &&
              status.glyph == UiActivityGlyph::none && same(status.text, utf8_literal("Listening to Spotify")),
          "status line: without rich activities the summary shows as it was");
    UserAppearance bot{};
    bot.user_id = 43;
    bot.fields = appearance_bot;
    bot.bot = true;
    UserActivitySet robot = set;
    robot.user_id = 43;
    check(user_appearance_upsert(&users.appearance, bot) && user_activity_upsert(&users.activities, robot) &&
              ui_user_activity_status(users, 43, nullptr, {storage, sizeof(storage)}, &status) &&
              status.glyph == UiActivityGlyph::none && same(status.text, utf8_literal("Grave, Choir")) &&
              !ui_user_activity_status(users, 44, nullptr, {storage, sizeof(storage)}, &status),
          "status line: bots get no glyph and users without presence no line");
    check(ui_activity_glyph(UserActivityType::playing) == UiActivityGlyph::game &&
              ui_activity_glyph(UserActivityType::competing) == UiActivityGlyph::game &&
              ui_activity_glyph(UserActivityType::watching) == UiActivityGlyph::screen &&
              ui_activity_glyph(UserActivityType::streaming) == UiActivityGlyph::screen &&
              ui_activity_glyph_icon(UiActivityGlyph::game) == UiIcon::controller &&
              ui_activity_glyph_icon(UiActivityGlyph::music) == UiIcon::music,
          "status line: a controller for games, a note for music, a screen for watching");
    const UiRect row{10, 20, 200, 36};
    check(ui_rect_equal(ui_activity_glyph_rectangle(row), {10, 22, 22, 34}) &&
              ui_rect_equal(ui_activity_glyph_text(row, UiActivityGlyph::music), {26, 20, 200, 36}) &&
              ui_rect_equal(ui_activity_glyph_text(row, UiActivityGlyph::none), row),
          "status line: a 12 DIP glyph, then the text 4 DIP after it");
    user_activity_release(&users.activities);
    user_appearance_release(&users.appearance);
}

void test_requests() noexcept {
    u8 target[256];
    const u64 ids[] = {5, 6};
    u32 bytes = user_activity_applications_target({ids, 2}, {target, sizeof(target)});
    const u64 zero[] = {5, 0};
    const u64 many[] = {1, 2, 3, 4};
    check(same({reinterpret_cast<const char*>(target), bytes},
               utf8_literal("/api/v10/applications/public?application_ids=5&application_ids=6")) &&
              !user_activity_applications_target({zero, 2}, {target, sizeof(target)}) &&
              !user_activity_applications_target({many, 4}, {target, sizeof(target)}),
          "requests: public applications by ID, at most three");
    bytes = user_activity_metadata_target(42, utf8_literal("abc123"), 5, {target, sizeof(target)});
    const bool metadata =
        same({reinterpret_cast<const char*>(target), bytes},
             utf8_literal("/api/v10/users/42/sessions/abc123/activities/5/metadata"));
    bytes = user_activity_metadata_target(42, utf8_literal("h:xbox,1"), 0, {target, sizeof(target)});
    check(metadata &&
              same({reinterpret_cast<const char*>(target), bytes},
                   utf8_literal("/api/v10/users/42/sessions/h:xbox,1/activities/0/metadata")) &&
              !user_activity_metadata_target(42, utf8_literal("a/b"), 5, {target, sizeof(target)}) &&
              !user_activity_metadata_target(42, {}, 5, {target, sizeof(target)}),
          "requests: activity metadata by session and application (0 for none)");
    struct Seen {
        u64 ids[4];
        char icons[4][16];
        u32 bytes[4], count;
    } seen{};
    const auto visit = [](void* context, u64 id, Utf8View icon) noexcept {
        auto& v = *static_cast<Seen*>(context);
        if (v.count < 4 && icon.bytes <= 16) {
            v.ids[v.count] = id;
            copy_bytes(v.icons[v.count], icon.data, icon.bytes);
            v.bytes[v.count++] = icon.bytes;
        }
    };
    check(user_activity_applications_decode(
              utf8_literal(R"([{"id":"5","name":"X","icon":"abc"},{"id":"6","icon":null}])"), {scratch, 16384},
              visit, &seen) &&
              seen.count == 2 && seen.ids[0] == 5 && same({seen.icons[0], seen.bytes[0]}, utf8_literal("abc")) &&
              seen.ids[1] == 6 && !seen.bytes[1] &&
              !user_activity_applications_decode(utf8_literal(R"({"id":"5"})"), {scratch, 16384}, visit, &seen),
          "requests: application icons decode, null icons are empty");
    char url[256];
    const auto body = utf8_literal(R"({"button_urls":["https://example.invalid/a b","https://example.invalid/b"]})");
    const auto unsafe = utf8_literal(R"j({"button_urls":["javascript:alert(1)"]})j");
    bytes = user_activity_button_url(body, {scratch, 16384}, 1, {url, sizeof(url)});
    check(same({url, bytes}, utf8_literal("https://example.invalid/b")) &&
              !user_activity_button_url(body, {scratch, 16384}, 0, {url, sizeof(url)}) &&
              !user_activity_button_url(unsafe, {scratch, 16384}, 0, {url, sizeof(url)}) &&
              !user_activity_button_url(body, {scratch, 16384}, 2, {url, sizeof(url)}),
          "requests: button URLs must be plain http(s) links");
}

void test_wire() noexcept {
    UserActivitySet set{};
    UserActivitySet restored{};
    u8 wire[4096];
    const bool decoded = decode(utf8_literal(R"({"activities":[{"type":2,"name":"Spotify","session_id":"abcdef",
        "platform":"ps5","party":{"id":"spotify:1"}}]})"),
                                &set);
    const u32 written = decoded ? user_activities_write(set, {wire, sizeof(wire)}) : 0;
    const bool read = written && user_activities_read({wire, written}, 42, &restored);
    const auto& a = restored.items[0];
    check(read && same(text(a, UserActivityField::session), utf8_literal("abcdef")) &&
              a.platform == UserActivityPlatform::ps5 && (a.detail & user_activity_has_party) &&
              (a.detail & user_activity_spotify_party) && user_activity_is_spotify(a),
          "wire: session, platform and party facts survive the mailbox");
    wire[4] = 0x11; // the first activity's detail byte
    check(!user_activities_read({wire, written}, 42, &restored), "wire: unknown detail bits are rejected");
}

// Real-shaped presences (synthetic values): Spotify's 40-character album art
// key, a media proxy key near the 320-byte budget and an application asset
// survive the mailbox and the shared store byte for byte, URLs included.
void test_art_store() noexcept {
    char json[2048];
    u32 used = 0;
    const auto put = [&](Utf8View v) noexcept {
        copy_bytes(json + used, v.data, v.bytes);
        used += v.bytes;
    };
    put(utf8_literal(R"({"activities":[{"type":2,"name":"Spotify","flags":48,"id":"spotify:1",)"
                     R"("details":"Synthetic Song - Sped Up Version","state":"First Artist; Second Artist",)"
                     R"("session_id":"0123456789abcdef0123456789abcdef","sync_id":"0123456789abcdefghijkl",)"
                     R"("party":{"id":"spotify:4343"},"created_at":1700000000300,)"
                     R"("timestamps":{"start":1700000000000,"end":1700000200000},)"
                     R"("assets":{"large_image":"spotify:ab67616d0000b2730123456789abcdef01234567",)"
                     R"j("large_text":"Synthetic Album (Sped Up Version)"}},)j"
                     R"({"type":3,"name":"Cinema","assets":{"large_image":"mp:external/)"));
    for (u32 i = 0; i < 27; ++i)
        put(utf8_literal("abcdefghij"));
    put(utf8_literal(R"(/https/example.invalid/poster.png","large_text":"Season 1"}},)"
                     R"({"type":0,"name":"Mossrun","application_id":"500000000000000001",)"
                     R"("assets":{"large_image":"500000000000000002","small_image":"500000000000000003"}}]})"));
    UserActivitySet set{}, mailbox{}, stored{};
    u8 wire[kUserActivityRecordMaximumBytes];
    const bool decoded = decode({json, used}, &set, 4343) && set.count == 3;
    const u32 written = decoded ? user_activities_write(set, {wire, sizeof(wire)}) : 0;
    UserStore users{};
    const bool kept = written && user_activities_read({wire, written}, 4343, &mailbox) &&
                      user_activity_upsert(&users.activities, mailbox) &&
                      user_activity_find(users.activities, 4343, &stored) && stored.count == 3;
    bool same_urls = kept;
    for (u32 i = 0; same_urls && i < 3; ++i)
        for (u32 large = 0; large < 2; ++large) {
            char a[512], b[512];
            MediaAttachmentFormat fa{}, fb{};
            const u32 na = user_activity_image_url(set.items[i], large, {a, sizeof(a)}, &fa);
            const u32 nb = user_activity_image_url(stored.items[i], large, {b, sizeof(b)}, &fb);
            same_urls = na == nb && fa == fb && bytes_equal(a, b, na) &&
                        same(text(set.items[i], UserActivityField::large_image),
                             text(stored.items[i], UserActivityField::large_image));
        }
    char url[512];
    MediaAttachmentFormat format{};
    const u32 art = kept ? user_activity_image_url(stored.items[0], true, {url, sizeof(url)}, &format) : 0;
    check(kept && same_urls && text(stored.items[1], UserActivityField::large_image).bytes == 3 + 9 + 270 + 33 &&
              same({url, art}, utf8_literal("https://i.scdn.co/image/ab67616d0000b2730123456789abcdef01234567")) &&
              format == MediaAttachmentFormat::jpeg,
          "store: long art keys round-trip the mailbox and the store with identical URLs");
    user_activity_release(&users.activities);
}

// The artwork placeholder names the activity: a note for music, the
// controller only for games.
void test_placeholders() noexcept {
    using T = UserActivityType;
    const auto icon = [](T type) noexcept { return profile_activity_glyph_icon(profile_activity_placeholder(type)); };
    check(icon(T::listening) == UiIcon::music && icon(T::playing) == UiIcon::controller &&
              icon(T::watching) == UiIcon::monitor && icon(T::streaming) == UiIcon::video &&
              icon(T::competing) == UiIcon::crown,
          "placeholder: one glyph per activity type");
}

void run() noexcept {
    test_order();
    test_times();
    test_urls();
    test_content();
    test_layout();
    test_popout_spotify();
    test_status_line();
    test_requests();
    test_wire();
    test_art_store();
    test_placeholders();
}
} // namespace maicord::profile_activity_test

extern "C" __declspec(noreturn) void WINAPI maicord_profile_activity_entry() noexcept {
    maicord::profile_activity_test::run();
    ExitProcess(maicord::profile_activity_test::failures ? 1 : 0);
}
