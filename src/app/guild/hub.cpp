#include "hub.hpp"
#include "banner.hpp"
#include "community.hpp"
#include "../account/account_access.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../account/activity.hpp"
#include "../account/friends.hpp"
#include "../account/profile.hpp"
#include "../account/self_status.hpp"
#include "../chat/game_lookup.hpp"
#include "../chat/game_profile.hpp"
#include "../settings/modal.hpp"
#include "../settings/settings.hpp"
#include "../settings/user_settings.hpp"
#include "../shell/content_page.hpp"
#include "../shell/owner.hpp"
#include "../shell/state.hpp"
#include "maicord/app/gateway/client.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/guild/detail_store.hpp"
#include "maicord/model/guild/permission_bits.hpp"
#include "maicord/model/notifications/message_notifications.hpp"
#include "maicord/protocol/user/account_settings.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/chat/game_media.hpp"
#include "maicord/ui/chat/messages/component_media.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/ui/guild/card_scene.hpp"
#include "maicord/ui/widgets/icon.hpp"

namespace maicord::app_detail {
namespace {
// The official client lists the first 20 entries of a leaderboard and the account's own after them, in a list
// 320 DIPs high of 40 DIP rows. The covers of a popular games widget fill one row (guild_hub_covers).
constexpr u32 kListed = 20;
constexpr u32 kRows = kListed + 1;
constexpr float kRowHeight = 40, kListHeight = 320;
constexpr u32 kCoversShown = kGuildSpaceGames;
// The official client shows a leaderboard or popular games only with three members who share their activity.
constexpr u32 kEnoughMembers = 3;
// Pictures: the banner, the server icon, the rows' (and the floating row's) avatars and games, the covers and
// the players.
constexpr u32 kMediaCapacity = 2 + (kRows + 1) * (1 + kGuildSpaceLeaderGames) + kCoversShown + kGuildSpacePlayers;
constexpr u32 kImageCapacity = 1 + (kRows + 1) * kGuildSpaceLeaderGames + kCoversShown;
// A leader's parts act with the entry's index added: its name opens the profile, its value and its games the
// profile's activity. A cover opens the game's profile.
enum Action : u32 {
    action_retry = 1,
    action_share,
    action_share_games,
    action_online,
    action_jump,
    action_profile = 0x100,
    action_activity = 0x200,
    action_games = 0x280,
    action_game = 0x300
};
enum class Phase : u8 { loading, loaded, failed };
// What the account must turn on to count, as the official client asks: nothing, activity sharing (in this server),
// personalization, or both.
enum class Need : u8 { none, sharing, personalization, all };

struct Owner {
    u64 guild_id, account_scope, profile_id;
    // The day the page was built on: the badge counts the days left of the week's competition.
    u32 users_revision, members_revision, games_revision, state, day;
    Phase phase;
    bool hydrate_failed;
    // What the account's own row asked for when it first showed (the official client's mount state): once given,
    // the row says that it counts from now on.
    bool mounted, mount_need, mount_invisible;
    float self_top;
    u32 query_bytes;
    char query[192];
    u32 media_count, image_count, people_count;
    UiSettingsAvatarMedia media[kMediaCapacity];
    GameImageMedia images[kImageCapacity];
    u64 people[kRows + 1 + kGuildSpacePlayers];
    char text[12288];
    TextBank bank;
    GuildSpace space;
    // The banner without a picture: the official client's glow of the server profile's brand colour (its GET
    // /guilds/{g}/profile, fresh for a minute), else of the server icon's colour.
    u64 brand_guild, brand_ms, icon_guild;
    u32 brand_rgb, icon_rgb;
    bool brand_known;
    float saturation;
    GuildProfile profile;
};
Owner* owner;
// The servers whose hub was their last view.
u64 remembered[8];
// The personalization consent: 0 unknown, 1 given, 2 refused. Each opening asks for it again (the owner's account
// may have changed it elsewhere); a change waits for its answer.
u8 consent;
bool consent_busy;
u64 consent_scope;

// The leaderboard's and popular games' texts for each Need after none. Both say the same for Need::all.
constexpr char kNeedTexts[] = "Share your activity in order to join the leaderboard.\0"
                              "Enable activity sharing to see your played games join the rankings.\0"
                              "Turn on personalization to join the leaderboard.\0"
                              "Turn on personalization to see your played games join the rankings.\0"
                              "Turn on activity sharing and personalization to join the rankings.";
constexpr char kStatNames[] = "Gaming Leaderboard\0Most Game Time\0Most Game Days\0Most Unique Games";
constexpr char kStatQuestions[] = "Who is on top of the leaderboard?\0Who has the most total game time?\0"
                                  "Who has played on the most days?\0Who has played the most unique games?";
constexpr char kTrouble[] = "Uh-oh, something went wrong. Please try again later.";

bool scope_current() noexcept {
    return owner && application_account_available() && owner->account_scope == g_app.plugin_account_scope_serial &&
           owner->profile_id == g_app.account.status.profile_id;
}

bool sharing_on(u64 guild_id, bool* known = nullptr) noexcept {
    GatewayActivitySharing sharing{};
    account_module_local_presence(g_app.account.instance, nullptr, &sharing);
    if (known)
        *known = sharing.known;
    return (!sharing.known || sharing.share) &&
           !application_user_settings_guild_restricted(GuildPrivacyList::activity_status, guild_id);
}
// The official client's requirement: nothing until the consent is known.
Need need(u64 guild_id) noexcept {
    if (consent == 0)
        return Need::none;
    const bool off = !sharing_on(guild_id), refused = consent == 2;
    return off && refused ? Need::all : off ? Need::sharing : refused ? Need::personalization : Need::none;
}
bool invisible() noexcept {
    const auto status = application_self_status_snapshot();
    return status.authoritative && status.status == GatewayUserStatus::invisible;
}
// What the page and the sharing dialog show of the account's settings: a change builds them again.
u32 state_key(u64 guild_id) noexcept {
    bool known = false;
    const bool on = sharing_on(guild_id, &known);
    return static_cast<u32>(consent | consent_busy << 2 | known << 3 | on << 4 | invisible() << 5);
}
Utf8View need_text(Need need_now, bool games) noexcept {
    return c_string_list_entry(kNeedTexts, minimum<u32>((static_cast<u32>(need_now) - 1) * 2 + games, 4));
}
Utf8View need_button(Need need_now) noexcept {
    return need_now == Need::sharing ? utf8_literal("Share Activity") : utf8_literal("Open Settings");
}

const GameRecord* known_game(u64 id) noexcept {
    game_lookup_want(id);
    const GameRecord* record = game_lookup_find(id);
    return record && record->state == GameState::known ? record : nullptr;
}
Utf8View game_name(const GameRecord& record) noexcept {
    return owner->bank.add({record.game.name, record.game.name_bytes});
}

// A picture's slot for a preview (0: none).
u32 add_media(const UiSettingsAvatarMedia& media) noexcept {
    auto& p = *owner;
    if (!media.key.id || p.media_count == kMediaCapacity)
        return 0;
    p.media[p.media_count] = media;
    return ++p.media_count;
}
u32 add_image(const GameImageMedia* image) noexcept {
    return image ? add_media({image->key, 0, {}, image->url_view()}) : 0;
}
// A member's avatar, or the CDN's default one for a user without an avatar (or not known yet).
u32 add_avatar(u64 user_id) noexcept {
    const auto* user = user_store_find(g_app.account_ui->user_store, user_id);
    return add_media(application_listed_user_avatar_media(user_id, user ? user_store_avatar_hash(*user) : Utf8View{}, 0));
}
// A game's icon or cover by its CDN URL.
u32 add_game(const GameRecord* record, bool cover, u32 dips) noexcept {
    auto& p = *owner;
    if (!record || p.image_count == kImageCapacity)
        return 0;
    auto* image = &p.images[p.image_count];
    if (!(cover ? game_cover_media : game_icon_media)(record->game, dips, g_app.renderer.dpi, image))
        return 0;
    ++p.image_count;
    return add_image(image);
}
// The hub's banner (header.custom_banner_hash) as the official client asks for it: WebP, at the CDN size that holds
// 1200 DIPs. Without one, the server's banner of the channel list.
u32 add_banner() noexcept {
    auto& p = *owner;
    if (!p.space.banner_bytes) {
        GuildBannerMedia banners[2];
        const u32 count = application_guild_banner_media(banners, 2);
        return count ? add_media({banners[count - 1].key, 0, {}, banners[count - 1].url}) : 0;
    }
    auto& image = p.images[p.image_count++];
    const u32 wanted = 1200 * g_app.renderer.dpi / 96;
    static constexpr u32 kSizes[]{1280, 1536, 2048, 3072, 4096};
    u32 size = 4096;
    for (u32 i = 4; i-- > 0;)
        if (wanted <= kSizes[i])
            size = kSizes[i];
    TextWriter url{{image.url, sizeof(image.url)}};
    url.add("https://cdn.discordapp.com/guild-space/");
    url.number(p.guild_id);
    url.add("/banner/");
    url.add({p.space.banner, p.space.banner_bytes});
    url.add(".webp?size=");
    url.number(size);
    image.url_bytes = url.ok ? url.used : 0;
    image.key = message_component_media_key(hash64_words(kHash64Seed, image.url, image.url_bytes),
                                            MediaAttachmentFormat::webp);
    return image.url_bytes ? add_image(&image) : 0;
}

UiStoreNode& leaf(UiStoreBuilder& b, UiStoreKind kind, float size) noexcept {
    auto& n = b.leaf(kind);
    n.rw = n.rh = size;
    return n;
}
void icon(UiStoreBuilder& b, UiIcon glyph, float size, UiStoreInk ink = UiStoreInk::subtle) noexcept {
    auto& n = leaf(b, UiStoreKind::icon, size);
    n.icon = static_cast<u8>(glyph);
    n.ink = static_cast<u8>(ink);
}
// A round picture (an avatar) or a rounded one of `radius`, with its tooltip.
void picture(UiStoreBuilder& b, u32 slot, float size, u16 radius = 999, Utf8View tip = {}) noexcept {
    auto& n = b.open(UiStoreKind::preview);
    n.variant = static_cast<u8>(UiStoreAspect::fixed);
    n.rw = n.rh = size;
    n.aux = radius;
    n.value = slot;
    n.text2 = tip;
    b.close();
}
UiStoreNode& row(UiStoreBuilder& b, float gap) noexcept {
    auto& n = b.open(UiStoreKind::row);
    n.gap = gap;
    n.flags = ui_store_center;
    return n;
}
// A widget's card and its title row: the glyph, the title (wrapped to the card) and, at the end, a badge (the days
// left) and its tooltip.
void card(UiStoreBuilder& b, UiIcon glyph, Utf8View title, Utf8View badge = {}, Utf8View badge_tip = {}) noexcept {
    auto& c = b.open(UiStoreKind::card);
    c.pad_x = c.pad_y = 16;
    c.gap = 12;
    row(b, 8);
    icon(b, glyph, 20, UiStoreInk::strong);
    b.text(title, UiStoreType::body_bold, UiStoreInk::strong, ui_store_wrap);
    if (badge.bytes) {
        b.spacer(0, 0, true);
        b.chip(badge, UiStoreTone::brand).text2 = badge_tip;
    }
    b.close();
}
// "N member(s)" and the like, appended to the bank: the count, then the plural word without its "s" for one.
Utf8View plural(u64 n, const Utf8View& word) noexcept {
    auto& bank = owner->bank;
    const u32 start = bank.used;
    bank.number(n);
    bank.append(word.data, word.bytes - (n == 1));
    return bank.since(start);
}
// "+N": the games or members a cluster does not show.
Utf8View more(u64 n) noexcept {
    auto& bank = owner->bank;
    const u32 start = bank.used;
    bank.add("+");
    bank.number(n);
    return bank.since(start);
}
// A time as the official client writes it in a sentence: within two days by the calendar ("today at 5:00 pm"),
// else the date and the time, in lower case.
Utf8View when(u64 seconds, bool calendar) noexcept {
    auto& bank = owner->bank;
    const u32 start = bank.used;
    const u64 now = clock_unix_milliseconds() / 1000;
    char buffer[96];
    const auto put = [&](TimestampFormatResult r) noexcept {
        if (r.ok)
            bank.append(buffer, r.bytes);
    };
    if (calendar)
        put(format_timestamp_calendar(seconds, now, {buffer, sizeof(buffer)}));
    else {
        put(format_timestamp_local(seconds, 'd', now, {buffer, sizeof(buffer)}));
        bank.add(" ");
        put(format_timestamp_local(seconds, 't', now, {buffer, sizeof(buffer)}));
    }
    for (u32 i = start; i < bank.used; ++i)
        if (owner->text[i] >= 'A' && owner->text[i] <= 'Z')
            owner->text[i] = static_cast<char>(owner->text[i] + 32);
    return bank.since(start);
}

// A widget without three members who count, or whose data did not come.
void notice(UiStoreBuilder& b, Utf8View text, Utf8View button = {}, u32 action = action_share) noexcept {
    b.text(text, UiStoreType::small, UiStoreInk::subtle, ui_store_wrap);
    if (button.bytes)
        b.button(button, UiStoreButton::secondary, action);
}
void not_enough(UiStoreBuilder& b, Need need_now, bool games) noexcept {
    auto& bank = owner->bank;
    const u32 start = bank.used;
    if (need_now == Need::none) {
        bank.add("There need to be at least 3 server members sharing their gaming activity to have ");
        bank.add(games ? utf8_literal("popular games") : utf8_literal("a leaderboard"));
        bank.add(" enabled.");
    } else {
        bank.add("Share your gaming activity and turn on personalization to help unlock this ");
        bank.add(games ? utf8_literal("panel") : utf8_literal("leaderboard"));
        bank.add(". It appears once 3 members have both settings on.");
    }
    b.text(utf8_literal("Not enough data"), UiStoreType::label_bold, UiStoreInk::strong);
    notice(b, bank.since(start), need_now == Need::none ? Utf8View{} : utf8_literal("Share My Activity"),
           games ? action_share_games : action_share);
}
// The skeleton of a list still loading: rows of a rank, an avatar and two lines.
void skeleton(UiStoreBuilder& b) noexcept {
    for (u32 i = 0; i < 5; ++i) {
        auto& r = row(b, 12);
        r.pad_x = 8;
        r.pad_y = 4;
        leaf(b, UiStoreKind::bar, 16);
        leaf(b, UiStoreKind::bar, 32).variant = 1;
        auto& lines = b.open(UiStoreKind::column);
        lines.gap = 6;
        lines.flags = ui_store_flex;
        b.leaf(UiStoreKind::bar).rw = 120;
        auto& second = b.leaf(UiStoreKind::bar);
        second.rw = 80;
        second.rh = 10;
        b.close();
        b.close();
    }
}

// The member's name in the server (nickname, else the user's), appended to the bank, from its record (null: none).
Utf8View member_display_name(const GuildMemberRecord* member, u64 user_id) noexcept {
    Utf8View name = member ? guild_member_store_nickname(*member) : Utf8View{};
    if (!name.bytes) {
        const auto& users = g_app.account_ui->user_store;
        const auto* user = user_store_find(users, user_id);
        name = user ? user_store_name(users, *user) : utf8_literal("Unknown User");
    }
    return owner->bank.add(name);
}
inline Utf8View display_name(u64 user_id) noexcept {
    return member_display_name(
        guild_member_store_find(g_app.account_ui->guild_member_store, owner->guild_id, user_id), user_id);
}
// The name in the colour of the member's role; "(You)" after the account's own.
Utf8View member_name(UiStoreBuilder& b, u64 user_id, bool you) noexcept {
    auto& bank = owner->bank;
    const u32 start = bank.used;
    const auto* member = guild_member_store_find(g_app.account_ui->guild_member_store, owner->guild_id, user_id);
    member_display_name(member, user_id);
    if (you)
        bank.add(" (You)");
    const Utf8View name = bank.since(start);
    auto& n = b.text(name, UiStoreType::label_bold, UiStoreInk::strong);
    if (member && (member->role_color & 0xFFFFFF)) {
        n.ink = static_cast<u8>(UiStoreInk::custom);
        n.group = member->role_color & 0xFFFFFF;
    }
    return name;
}
// Hours and minutes as the official client writes a time played ("15h", "8h 35m", "35m"), with " total" for the
// time under the days or games played.
Utf8View duration(u64 minutes, bool total) noexcept {
    auto& bank = owner->bank;
    const u32 start = bank.used;
    if (minutes >= 60) {
        bank.number(minutes / 60);
        bank.add("h");
    }
    if (minutes % 60 || minutes < 60 || total) {
        if (minutes >= 60)
            bank.add(" ");
        bank.number(minutes % 60);
        bank.add("m");
    }
    if (total)
        bank.add(" total");
    return bank.since(start);
}
void note_person(u64 user_id) noexcept {
    auto& p = *owner;
    if (user_id && p.people_count < sizeof(p.people) / sizeof(p.people[0]))
        p.people[p.people_count++] = user_id;
}
// The games a leader played (those known), as the official client says them: "Played A", "Played A and B" (of
// two), "Played A, B and C" (of three), else "Played A, B and N other games"; or "No games played yet".
inline Utf8View played(const GameRecord* const* games, u32 count, u32 total) noexcept {
    auto& bank = owner->bank;
    if (!count)
        return utf8_literal("No games played yet");
    const u32 start = bank.used;
    const bool all = count == total;
    bank.add("Played ");
    game_name(*games[0]);
    if (count > 1) {
        bank.add(all && count == 2 ? utf8_literal(" and ") : utf8_literal(", "));
        game_name(*games[1]);
        bank.add(" and ");
        if (all && count == 3)
            game_name(*games[2]);
        else if (!all)
            plural(total - 2, utf8_literal(" other games"));
    }
    return bank.since(start);
}
// The value: hours and minutes played, or days or games played over the time played.
inline void leader_value(UiStoreBuilder& b, const GuildSpaceLeader& leader, u8 stat) noexcept {
    static constexpr UiIcon kStatIcons[]{UiIcon::timer, UiIcon::timer, UiIcon::calendar, UiIcon::controller};
    row(b, 4);
    icon(b, kStatIcons[stat <= 3 ? stat : 0], 12);
    b.text(stat == 2   ? plural(leader.value, utf8_literal(" days"))
           : stat == 3 ? plural(leader.value, utf8_literal(" games"))
           : stat == 1 ? duration(leader.value / 60'000, false)
                       : utf8_literal("\xE2\x80\x94"),
           UiStoreType::label_bold, UiStoreInk::strong);
    b.close();
    if (stat == 2 || stat == 3)
        b.text(duration(leader.seconds / 60, true), UiStoreType::small, UiStoreInk::subtle);
}
// One entry: the medal (1-3) or the rank, the avatar, the name over the game played last; the value; the games
// played (three icons, or two and how many more). The floating copy of the account's row is one button that
// scrolls the list to it.
void leader_row(UiStoreBuilder& b, const GuildSpaceLeader& leader, u32 index, u32 rank, u8 stat, bool floating) noexcept {
    auto& bank = owner->bank;
    const bool you = leader.user_id == g_app.account_ui->chat_state.self_user_id;
    note_person(leader.user_id);
    u32 gates = 0;
    application_friends_notification_gates(leader.user_id, &gates);
    const bool blocked = (gates & notification_gate_blocked) != 0,
               hidden = blocked || (gates & notification_gate_ignored) != 0;
    auto& r = row(b, 8);
    if (you)
        r.fill = static_cast<u8>(UiStoreFill::selected);
    if (floating) {
        r.action = action_jump;
        r.text = utf8_literal("Jump to you");
    }
    // The identity and the value are each 36 DIPs of content (a 32 DIP avatar beside a name over a line, or two lines)
    // in kRowHeight: 2 DIPs above and below.
    auto& identity = row(b, 12);
    identity.flags |= ui_store_flex;
    identity.pad_x = 8;
    identity.pad_y = 2;
    if (rank <= 3) {
        static constexpr u8 kMedals[]{4, 6, 7};
        auto& medal = leaf(b, UiStoreKind::swatch, 24);
        medal.variant = kMedals[rank - 1];
        medal.text = bank.number(rank);
    } else
        b.text(bank.number(rank), UiStoreType::label_bold, UiStoreInk::subtle, ui_store_center).rw = 24;
    if (hidden) {
        auto& plate = b.open(UiStoreKind::row);
        plate.rw = 32;
        plate.flags = ui_store_center | ui_store_justify;
        icon(b, blocked ? UiIcon::ban : UiIcon::eye_off, 16);
        b.close();
    } else
        picture(b, add_avatar(leader.user_id), 32);
    b.open(UiStoreKind::column).flags = ui_store_flex;
    Utf8View name{};
    if (hidden)
        name = b.text(blocked ? utf8_literal("Blocked User") : utf8_literal("Ignored User"), UiStoreType::label_bold,
                      UiStoreInk::subtle)
                   .text;
    else
        name = member_name(b, leader.user_id, you);
    const GameRecord* games[kGuildSpaceLeaderGames];
    u32 known = 0;
    // The game played last is the first of the list, when it is known: looked up once with the others.
    const GameRecord* last = nullptr;
    for (u32 i = 0; i < leader.game_count; ++i)
        if (const auto* game = known_game(leader.games[i])) {
            if (!i)
                last = game;
            games[known++] = game;
        }
    if (last && !hidden) {
        const u32 start = bank.used;
        bank.add("Last played ");
        game_name(*last);
        b.text(bank.since(start), UiStoreType::small, UiStoreInk::subtle);
    }
    b.close();
    if (!floating) {
        const u32 start = bank.used;
        bank.add("Rank ");
        bank.number(rank);
        bank.add(", ");
        bank.add(name);
        identity.action = action_profile + index;
        identity.text = bank.since(start);
    }
    b.close();
    auto& value = b.open(UiStoreKind::column);
    value.rw = 104;
    value.pad_x = 8;
    value.pad_y = 2;
    if (!floating) {
        value.action = action_activity + index;
        value.text = utf8_literal("View recent activity");
    }
    leader_value(b, leader, stat);
    b.close();
    const u32 total = maximum<u32>(leader.game_total, leader.game_count);
    const u32 icons = minimum<u32>(known, total > 3 ? 2 : 3);
    auto& cluster = row(b, 4);
    cluster.rw = 3 * 24 + 2 * 4 + 8;
    cluster.pad_x = 4;
    cluster.pad_y = 8;
    cluster.text2 = played(games, known, total);
    if (!floating) {
        cluster.action = action_games + index;
        cluster.text = utf8_literal("View recent activity");
    }
    for (u32 i = 0; i < icons; ++i)
        picture(b, add_game(games[i], false, 24), 24, 6);
    if (total > icons)
        b.text(more(total - icons), UiStoreType::small_bold, UiStoreInk::subtle);
    b.close();
    b.close();
}

// The account's row when the leaderboard does not list it: what it must turn on, that being invisible hides its
// games, that it counts from now on, or that it has yet to play.
void unranked(UiStoreBuilder& b, Need need_now) noexcept {
    auto& p = *owner;
    const u64 self = g_app.account_ui->chat_state.self_user_id;
    const bool hidden = invisible();
    note_person(self);
    auto& r = row(b, 12);
    r.fill = static_cast<u8>(UiStoreFill::selected);
    r.pad_x = 8;
    r.pad_y = 8;
    r.text = utf8_literal("Unranked");
    b.text("?", UiStoreType::label_bold, UiStoreInk::subtle, ui_store_center).rw = 24;
    picture(b, add_avatar(self), 32);
    b.open(UiStoreKind::column).flags = ui_store_flex;
    member_name(b, self, false);
    Utf8View body = utf8_literal("Play some games to join the leaderboard! Progress will be shown starting tomorrow.");
    bool counted = false;
    if (need_now != Need::none)
        body = need_text(need_now, false);
    else if (hidden)
        body = utf8_literal("Being invisible hides your gaming activity. Go Online to join the leaderboard.");
    else if (p.mount_invisible || p.mount_need) {
        counted = true;
        body = p.mount_invisible ? utf8_literal("You\xE2\x80\x99re Online! We\xE2\x80\x99re counting your activity now "
                                                "and you\xE2\x80\x99ll show up on the leaderboard tomorrow.")
                                 : utf8_literal("Activity Sharing is on. We\xE2\x80\x99re counting your activity now "
                                                "and you\xE2\x80\x99ll show up on the leaderboard tomorrow.");
    }
    row(b, 4);
    if (counted)
        icon(b, UiIcon::circle_check, 12, UiStoreInk::positive);
    b.text(body, UiStoreType::small, UiStoreInk::subtle, ui_store_wrap | ui_store_flex);
    b.close();
    b.close();
    if (need_now != Need::none)
        b.button(need_button(need_now), UiStoreButton::secondary, action_share);
    else if (hidden)
        b.button("Go Online", UiStoreButton::secondary, action_online);
    b.close();
}

void leaderboard(UiStoreBuilder& b, const GuildSpaceData& data, Need need_now) noexcept {
    auto& p = *owner;
    auto& bank = p.bank;
    const u64 self = g_app.account_ui->chat_state.self_user_id;
    // Once the requirement is known (the consent answered), as the official client's row reads it when it mounts.
    if (!p.mounted && consent) {
        p.mounted = true;
        p.mount_need = need_now != Need::none;
        p.mount_invisible = invisible();
    }
    // An account that does not count leaves the list: the rows after it move up a rank. The list holds the first
    // 20 entries and the account's own after them.
    const bool hide_self = need_now != Need::none;
    u32 listed[kRows], count = 0, entries = 0, own = ~0u, self_at = ~0u;
    for (u32 i = 0; i < data.leader_count; ++i) {
        const bool you = data.leaders[i].user_id == self;
        if (you && hide_self)
            continue;
        ++entries;
        if (you)
            own = i;
        if (count < kListed) {
            if (you)
                self_at = count;
            listed[count++] = i;
        }
    }
    if (own != ~0u && self_at == ~0u) {
        self_at = count;
        listed[count++] = own;
    }
    // The badge: the days left in the week's competition; its tooltip: the question and its end.
    Utf8View left{}, tip{};
    const u8 stat = data.stat <= 3 ? data.stat : 0;
    if (data.week_start) {
        const u64 end = static_cast<u64>(data.week_start) + 7 * 86'400, now = clock_unix_milliseconds() / 1000;
        u32 start = bank.used;
        if (now >= end)
            bank.add("Ended");
        else {
            bank.number((end - now + 86'399) / 86'400);
            bank.add("d left");
        }
        left = bank.since(start);
        start = bank.used;
        if (now >= end) {
            bank.add("This competition ended ");
            when(end, now - end < 2 * 86'400);
            bank.add(". The next competition is ");
            bank.add(c_string_list_entry(kStatNames, data.next_stat <= 3 ? data.next_stat : 0));
        } else {
            bank.add(c_string_list_entry(kStatQuestions, stat));
            bank.add(" This competition ends ");
            when(end, end - now < 2 * 86'400);
        }
        bank.add(".");
        tip = bank.since(start);
    }
    const bool enough = entries >= kEnoughMembers;
    card(b, UiIcon::trophy, c_string_list_entry(kStatNames, enough ? stat : 0), left, tip);
    if (!enough) {
        not_enough(b, need_now, false);
        b.close();
        return;
    }
    // The list scrolls inside the card; the account's row floats under it while scrolled out of view.
    auto& list = b.open(UiStoreKind::column);
    list.flags = ui_store_pane;
    list.rh = minimum(kListHeight, count * kRowHeight);
    for (u32 k = 0; k < count; ++k) {
        const auto& leader = data.leaders[listed[k]];
        leader_row(b, leader, listed[k], hide_self || !leader.rank ? k + 1 : leader.rank, stat, false);
    }
    b.close();
    p.self_top = self_at * kRowHeight;
    if (self_at != ~0u && count * kRowHeight > kListHeight) {
        const float scroll = application_content_page_pane_offset();
        if (p.self_top < scroll - .5f || p.self_top + kRowHeight > scroll + kListHeight + .5f) {
            const auto& leader = data.leaders[listed[self_at]];
            leader_row(b, leader, listed[self_at], hide_self || !leader.rank ? self_at + 1 : leader.rank, stat, true);
        } else
            b.spacer(0, kRowHeight);
    }
    if (own == ~0u)
        unranked(b, need_now);
    b.leaf(UiStoreKind::rule);
    auto& foot = row(b, 6);
    if (data.computed_ms) {
        const u32 start = bank.used;
        bank.add("Last updated ");
        when(data.computed_ms / 1000, true);
        foot.text2 = bank.since(start);
    }
    icon(b, UiIcon::refresh, 16);
    b.text("Standings update daily", UiStoreType::small, UiStoreInk::subtle);
    b.close();
    b.close();
}

void popular_games(UiStoreBuilder& b, const GuildSpaceWidget& widget, const GuildSpaceData& data, Need need_now,
                   float width) noexcept {
    auto& bank = owner->bank;
    card(b, UiIcon::controller,
         widget.default_title_bytes ? Utf8View{widget.default_title, widget.default_title_bytes}
                                    : utf8_literal("Popular Games"));
    if (!data.game_count || data.total_users < kEnoughMembers) {
        not_enough(b, need_now, true);
        b.close();
        return;
    }
    // The covers in one row of the card, each over its name and its players; a cover opens the game's profile.
    const auto covers = guild_hub_covers(width);
    const float cell = covers.cell;
    row(b, 12).flags = 0;
    for (u32 i = 0; i < data.game_count && i < covers.count; ++i) {
        const auto& game = data.games[i];
        const auto* record = known_game(game.id);
        const Utf8View name = record ? game_name(*record) : utf8_literal("Unknown Game");
        auto& column = b.open(UiStoreKind::column);
        column.rw = cell;
        column.gap = 4;
        if (record) {
            column.action = action_game + i;
            column.text2 = name;
            const u32 start = bank.used;
            bank.add(name);
            bank.add(", view game profile");
            column.text = bank.since(start);
        }
        auto& cover = b.open(UiStoreKind::preview);
        cover.variant = static_cast<u8>(UiStoreAspect::fixed);
        cover.rh = cell * 4 / 3;
        cover.aux = 8;
        cover.value = add_game(record, true, static_cast<u32>(cell));
        b.close();
        b.text(name, UiStoreType::small_bold, UiStoreInk::strong);
        auto& players = row(b, 4);
        const u32 start = bank.used;
        plural(game.players, utf8_literal(" members"));
        bank.add(" played ");
        bank.add(name);
        players.text2 = bank.since(start);
        icon(b, UiIcon::user, 14);
        b.text(bank.number(game.players), UiStoreType::small, UiStoreInk::subtle);
        b.close();
        b.close();
    }
    b.close();
    if (need_now != Need::none) {
        auto& share = b.open(UiStoreKind::card);
        share.pad_x = share.pad_y = 12;
        share.gap = 8;
        b.text("Share my activity", UiStoreType::label_bold, UiStoreInk::strong);
        notice(b, need_text(need_now, true), need_button(need_now), action_share_games);
        b.close();
    }
    // The players: up to four avatars (two in a card under 480 DIPs, as the official client's compact footer), each
    // naming its member, then how many more.
    const u32 faces = width < 480 ? 2 : kGuildSpacePlayers;
    b.leaf(UiStoreKind::rule);
    row(b, 8);
    const u32 start = bank.used;
    plural(data.total_users, utf8_literal(" members"));
    bank.add(" played ");
    plural(data.total_games, utf8_literal(" games"));
    bank.add(" in the last 7 days");
    b.text(bank.since(start), UiStoreType::small, UiStoreInk::subtle, ui_store_wrap | ui_store_flex);
    u32 avatars = 0;
    for (u32 i = 0; i < data.player_count && avatars < faces; ++i)
        if (const u32 slot = add_avatar(data.players[i])) {
            note_person(data.players[i]);
            picture(b, slot, 24, 999, display_name(data.players[i]));
            ++avatars;
        }
    if (data.total_users > avatars)
        b.text(more(data.total_users - avatars), UiStoreType::small_bold, UiStoreInk::subtle);
    b.close();
    b.close();
}

// The banner card: the hub's banner (or a plain plate) under the official client's scrim, and over its foot the
// server's icon, name and counts in the scrim's white ink, in every theme.
void header(UiStoreBuilder& b) noexcept {
    auto& p = *owner;
    const auto& ui = *g_app.account_ui;
    const auto* guild = chat_state_store_find_guild(ui.chat_state, p.guild_id);
    const auto* details = guild_detail_store_find(ui.guild_details, p.guild_id);
    u32 members = details ? details->member_count : 0, online = 0;
    bool online_known = false;
    for (const auto& list : ui.guild_member_store.lists)
        if (list.guild_id == p.guild_id && list.member_count_known && list.member_count >= members) {
            members = list.member_count;
            online = list.online_count;
            online_known = list.online_count_known;
        }
    auto& banner = b.open(UiStoreKind::preview);
    banner.variant = static_cast<u8>(UiStoreAspect::fixed);
    banner.rh = 240;
    banner.pad_x = banner.pad_y = 16;
    banner.fill = static_cast<u8>(UiStoreFill::scrim);
    banner.value = add_banner();
    // No picture: the glow of the brand colour, else of the icon's colour (under the accessibility saturation, as
    // the server card tints it), else of the theme's server banner colour (bit 24).
    p.saturation = application_user_color_saturation();
    const bool brand = p.brand_guild == p.guild_id && p.brand_known, tinted = brand || p.icon_guild == p.guild_id;
    if (!banner.value) {
        banner.ink = static_cast<u8>(UiStoreInk::custom);
        banner.group = brand ? p.brand_rgb : tinted ? ui_guild_card_desaturate(p.icon_rgb, p.saturation) : 1u << 24;
    }
    constexpr auto ink = UiStoreInk::on_scrim;
    row(b, 10).anchor = static_cast<u8>(UiStoreAnchor::bottom_left);
    const Utf8View name = guild ? p.bank.add(chat_state_store_guild_name(ui.chat_state, *guild)) : Utf8View{};
    // The server's icon (the official client's large guild icon, 50 DIPs), or its initials on the plate as in the
    // server list. The icon, the dots and the boost glyph are foreground: they draw over the banner's scrim.
    auto& plate = b.open(UiStoreKind::preview);
    plate.variant = static_cast<u8>(UiStoreAspect::fixed);
    plate.flags = ui_store_foreground;
    plate.rw = plate.rh = 50;
    plate.aux = 8;
    plate.value = add_media(application_guild_icon_media(p.guild_id));
    if (!plate.value) {
        char initials[16];
        b.text(p.bank.add(ui_admin_initials(name, {initials, sizeof(initials)})), UiStoreType::heading,
               UiStoreInk::strong)
            .anchor = static_cast<u8>(UiStoreAnchor::center);
    }
    b.close();
    b.open(UiStoreKind::column).gap = 4;
    b.text(name, UiStoreType::heading, ink);
    row(b, 4);
    // The counts in the official client's small bold type, 12 DIPs apart.
    const auto dot = [&](u8 variant, Utf8View text) noexcept {
        auto& mark = leaf(b, UiStoreKind::swatch, 8);
        mark.variant = variant;
        mark.flags = ui_store_foreground;
        b.text(text, UiStoreType::eyebrow, ink);
        b.spacer(4, 0);
    };
    if (online_known)
        dot(5, plural(online, utf8_literal(" Online")));
    dot(6, plural(members, utf8_literal(" Members")));
    icon(b, UiIcon::boost, 12, ink);
    b.node().flags = ui_store_foreground;
    b.text(plural(details ? details->premium_subscription_count : 0, utf8_literal(" Boosts")), UiStoreType::eyebrow,
           ink);
    b.close();
    b.close();
    b.close();
    b.close();
}

void widget(UiStoreBuilder& b, u32 index, Need need_now, float width) noexcept {
    const auto& w = owner->space.widgets[index];
    const auto& data = owner->space.data[index];
    // A locked widget (a power-up the server has not unlocked) shows only to its managers, as a preview.
    if (w.locked || w.type == GuildSpaceWidgetType::other)
        return;
    if (w.type == GuildSpaceWidgetType::image_text) {
        card(b, UiIcon::file_text,
             w.title_bytes ? Utf8View{w.title, w.title_bytes} : Utf8View{w.default_title, w.default_title_bytes});
        b.text({w.text, w.text_bytes}, UiStoreType::paragraph, UiStoreInk::normal, ui_store_wrap);
        b.close();
    } else if (!data.ready) {
        card(b, w.type == GuildSpaceWidgetType::leaderboard ? UiIcon::trophy : UiIcon::controller,
             w.default_title_bytes ? Utf8View{w.default_title, w.default_title_bytes}
             : w.type == GuildSpaceWidgetType::leaderboard ? c_string_list_entry(kStatNames, 0)
                                                           : utf8_literal("Popular Games"));
        if (owner->hydrate_failed)
            notice(b, utf8_literal(kTrouble));
        else
            skeleton(b);
        b.close();
    } else if (w.type == GuildSpaceWidgetType::leaderboard)
        leaderboard(b, data, need_now);
    else
        popular_games(b, w, data, need_now, width);
}

void build(UiStoreBuilder& b, float width) noexcept {
    if (!owner || !g_app.account_ui)
        return;
    auto& p = *owner;
    p.media_count = p.image_count = p.people_count = 0;
    p.bank = {p.text, sizeof(p.text), 0};
    p.users_revision = g_app.account_ui->user_store.revision;
    p.members_revision = g_app.account_ui->guild_member_store.revision;
    p.games_revision = game_lookup_revision();
    p.state = state_key(p.guild_id);
    p.day = static_cast<u32>(clock_unix_milliseconds() / 86'400'000);
    b.node().gap = 16;
    header(b);
    if (p.phase != Phase::loaded) {
        auto& c = b.open(UiStoreKind::card);
        c.pad_x = c.pad_y = 16;
        c.gap = 12;
        if (p.phase == Phase::loading)
            skeleton(b);
        else {
            notice(b, utf8_literal(kTrouble));
            b.button("Retry", UiStoreButton::secondary, action_retry);
        }
        b.close();
        return;
    }
    // The main column and the side column (a third), stacked at full width when narrow (the split's own rule).
    const Need need_now = need(p.guild_id);
    const auto columns = guild_hub_columns(width);
    auto& split = b.open(UiStoreKind::split);
    split.gap = 16;
    split.rw = columns.side;
    for (u8 column = 0; column < 2; ++column) {
        b.open(UiStoreKind::column).gap = 16;
        for (u32 i = 0; i < p.space.count; ++i)
            if ((p.space.widgets[i].column != 0) == (column != 0))
                widget(b, i, need_now, column ? columns.side : columns.main);
        b.close();
    }
    b.close();
    // The members shown changed: the visible-author hydration pins them and asks for those it does not know.
    g_app.account_ui->author_hydration.interest_current = false;
}

void request() noexcept {
    owner->phase = Phase::loading;
    owner->space.count = 0;
    application_guild_community_hub(GuildCommunityOperation::space_get, owner->guild_id);
}

inline const GuildSpaceLeader* leader_at(u32 index) noexcept {
    for (u32 i = 0; i < owner->space.count; ++i)
        if (owner->space.widgets[i].type == GuildSpaceWidgetType::leaderboard && index < owner->space.data[i].leader_count)
            return &owner->space.data[i].leaders[index];
    return nullptr;
}

bool sharing_open(u64 guild_id, Need need_now, bool games) noexcept;

void activate(u32 action, const UiRect& anchor) noexcept {
    if (!owner)
        return;
    auto& p = *owner;
    if (action == action_retry) {
        request();
        application_content_page_invalidate();
    } else if (action == action_share || action == action_share_games) {
        // The official client asks for what the account must turn on in its sharing dialog.
        sharing_open(p.guild_id, need(p.guild_id), action == action_share_games);
    } else if (action == action_online) {
        application_self_status_request(GatewayUserStatus::online);
    } else if (action == action_jump) {
        application_content_page_pane_scroll_to(maximum(0.f, p.self_top - (kListHeight - kRowHeight) * .5f));
    } else if (action >= action_game) {
        const u32 i = action - action_game;
        for (u32 w = 0; w < p.space.count; ++w)
            if (p.space.widgets[w].type == GuildSpaceWidgetType::popular_games && i < p.space.data[w].game_count) {
                application_game_profile_open(p.space.data[w].games[i].id);
                return;
            }
    } else if (const auto* leader = leader_at(action & 0x7F)) {
        // The name opens the member's profile; the value and the games, its recent activity.
        if (application_profile_open(leader->user_id, anchor) && action >= action_activity)
            application_profile_expand(leader->user_id, ProfileTab::activity);
    }
}

const UiSettingsAvatarMedia* media(u32 slot) noexcept {
    return owner && slot && slot <= owner->media_count ? &owner->media[slot - 1] : nullptr;
}

// Names, avatars, games and their covers arrive after the hub's own answers; the account's settings change what
// it asks for. Another account or server closes it.
bool stale() noexcept {
    if (!scope_current() || g_app.account_ui->chat_state.selected_guild_id != owner->guild_id) {
        application_content_page_close();
        return false;
    }
    auto& p = *owner;
    // Without a brand colour, the banner takes the icon's colour once the icon is resident.
    bool banner = false;
    if (!(p.brand_guild == p.guild_id && p.brand_known) && p.icon_guild != p.guild_id) {
        const auto icon = application_guild_icon_media(p.guild_id);
        if (icon.key.id && renderer_media_dominant_color(&g_app.renderer, icon.key, &p.icon_rgb)) {
            p.icon_guild = p.guild_id;
            banner = true;
        }
    }
    const auto& ui = *g_app.account_ui;
    return banner || ui.user_store.revision != p.users_revision ||
           ui.guild_member_store.revision != p.members_revision || game_lookup_revision() != p.games_revision ||
           state_key(p.guild_id) != p.state || clock_unix_milliseconds() / 86'400'000 != p.day ||
           p.saturation != application_user_color_saturation();
}

constexpr ContentPageOwner kPage{kNavigationHubRowId, build, activate, media, stale, 0, 0};

bool page_open() noexcept {
    return owner && application_content_page() == &kPage;
}

// The widgets that need data, in one request.
inline void hydrate() noexcept {
    auto& p = *owner;
    TextWriter query{{p.query, sizeof(p.query)}};
    for (u32 i = 0; i < p.space.count; ++i)
        if (p.space.widgets[i].hydrate) {
            if (query.used)
                query.add("&widget_ids=");
            query.add({p.space.widgets[i].id, p.space.widgets[i].id_bytes});
        }
    p.query_bytes = query.ok ? query.used : 0;
    if (p.query_bytes)
        application_guild_community_hub(GuildCommunityOperation::space_hydrate, p.guild_id);
}

// The official client's Activity Sharing dialog: the switches the account must turn on to count (activity
// sharing, in this server, and personalization); turning personalization off asks first.
constexpr u64 kSharingOwner = 0x4D41475348415245ull; // "MAGSHARE"
constexpr UiPageKey kSharingPage{kSharingOwner, 1};
enum : u64 { control_share = 1, control_guild, control_consent, control_confirm };
struct Sharing : SettingsDialog {
    UiSettingsControl controls[3];
    UiSettingsFooter footer;
    u64 guild_id, account_scope, profile_id;
    u32 state;
    Need need;
    bool games, confirm;
    char guild_name[128];
    u32 guild_name_bytes;
};
Sharing* dialog;

bool sharing_project() noexcept {
    if (!dialog || !dialog->scene.active)
        return true;
    auto& d = *dialog;
    u32 n = 0;
    d.state = state_key(d.guild_id);
    d.content.footer = nullptr;
    if (d.confirm) {
        d.content.description = {};
        d.controls[n++] = {control_confirm,
                           utf8_literal("Turn off personalization?"),
                           utf8_literal("By turning this off you lose access to things like personalized "
                                        "recommendations and suggestions. You won't be able to recover them in the "
                                        "future!"),
                           UiSettingsControlKind::notice,
                           true,
                           false,
                           {},
                           0,
                           {},
                           nullptr};
        d.footer = {{}, utf8_literal("Yes, I'm sure"), utf8_literal("No, take me back!"), true, !consent_busy,
                    consent_busy, false};
        d.content.footer = &d.footer;
    } else {
        d.content.description =
            d.games ? utf8_literal("Enable settings to contribute to Popular Games. These can be changed at any time "
                                   "in Settings.")
                    : utf8_literal("Enable settings to contribute to the leaderboard. These can be changed at any "
                                   "time in Settings.");
        if (d.need != Need::personalization) {
            auto& share = d.controls[n++];
            share = {control_share,
                     utf8_literal("Share my activity"),
                     utf8_literal("Share activity information from games and connected apps I use, including when "
                                  "and how I engage."),
                     UiSettingsControlKind::toggle,
                     true,
                     false,
                     {},
                     0,
                     {},
                     nullptr};
            application_activity_settings_project(SettingBinding::share_activity, &share);
            const bool restricted =
                application_user_settings_guild_restricted(GuildPrivacyList::activity_status, d.guild_id);
            d.controls[n++] = {control_guild,
                               utf8_literal("Share my activity with this server"),
                               {d.guild_name, d.guild_name_bytes},
                               UiSettingsControlKind::toggle,
                               share.checked && share.enabled,
                               share.checked && !restricted,
                               {},
                               0,
                               {},
                               nullptr};
        }
        if (d.need != Need::sharing)
            d.controls[n++] = {control_consent,
                               utf8_literal("Use data to personalize my Discord experience"),
                               utf8_literal("Allows Discord to use information, such as who you talk to and what "
                                            "games you play, to personalize Discord for you."),
                               UiSettingsControlKind::toggle,
                               consent != 0 && !consent_busy,
                               consent == 1,
                               {},
                               0,
                               {},
                               nullptr};
    }
    d.content.controls = {d.controls, n};
    ++d.content.revision;
    return settings_modal_update(d);
}
bool sharing_repaint() noexcept {
    return settings_modal_present(sharing_project());
}
void consent_set(bool grant) noexcept {
    if (application_guild_community_hub(grant ? GuildCommunityOperation::consent_grant
                                              : GuildCommunityOperation::consent_revoke,
                                        dialog->guild_id))
        consent_busy = true;
}
void sharing_activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    if (!dialog)
        return;
    auto& d = *dialog;
    const auto target = activation.value.target;
    const bool checked = activation.value.checked;
    if (target.kind == UiSettingsTargetKind::close) {
        application_guild_hub_sharing_close(true);
        return;
    }
    if (target.kind == UiSettingsTargetKind::save && d.confirm && !consent_busy) {
        consent_set(false);
        d.confirm = false;
    } else if (target.kind == UiSettingsTargetKind::reset)
        d.confirm = false;
    else if (target.kind != UiSettingsTargetKind::control)
        return;
    else if (target.control_id == control_share) {
        bool saved = false;
        application_activity_settings_activate(SettingBinding::share_activity, checked, 0, &saved);
    } else if (target.control_id == control_guild)
        application_user_settings_set_guild_restricted(GuildPrivacyList::activity_status, d.guild_id, !checked);
    else if (target.control_id == control_consent) {
        if (checked)
            consent_set(true);
        else
            d.confirm = true;
    }
    if (target.kind == UiSettingsTargetKind::control)
        ui_settings_scene_animate_toggle(&d.scene, target, checked, clock_monotonic_milliseconds());
    sharing_repaint();
}
constexpr SettingsLayerHooks kSharingHooks{settings_hook_editor_none,
                                           settings_hook_edited_none,
                                           sharing_activate,
                                           settings_hook_ignore_choice,
                                           settings_hook_close<application_guild_hub_sharing_close>,
                                           settings_hook<sharing_repaint>,
                                           nullptr,
                                           settings_hook_not_editable};

bool sharing_open(u64 guild_id, Need need_now, bool games) noexcept {
    if (need_now == Need::none || !application_account_available() || !g_app.renderer_ready || g_app.text_form ||
        application_guild_hub_sharing_active() ||
        (!dialog && !(dialog = settings_modal_allocate<Sharing>(kSharingHooks))))
        return false;
    settings_modal_yield();
    auto& d = *dialog;
    d.guild_id = guild_id;
    d.account_scope = g_app.plugin_account_scope_serial;
    d.profile_id = g_app.account.status.profile_id;
    d.need = need_now;
    d.games = games;
    d.confirm = false;
    const auto& ui = *g_app.account_ui;
    const auto* guild = chat_state_store_find_guild(ui.chat_state, guild_id);
    const Utf8View name = guild ? chat_state_store_guild_name(ui.chat_state, *guild) : Utf8View{};
    d.guild_name_bytes = utf8_copy_truncated(d.guild_name, sizeof(d.guild_name), name);
    if (!settings_dialog_begin(d, kSharingPage, utf8_literal("Activity Sharing")))
        return false;
    if (settings_dialog_show(d, kSharingPage, 0, false, sharing_project) && sharing_repaint())
        return true;
    application_guild_hub_sharing_close(false);
    return false;
}
} // namespace

void application_guild_hub_navigation(NavigationView* view) noexcept {
    if (!g_app.account_ui)
        return;
    const auto& ui = *g_app.account_ui;
    const auto& state = ui.chat_state;
    const u64 guild_id = state.home_selected ? 0 : state.selected_guild_id;
    const auto* details = guild_id ? guild_detail_store_find(ui.guild_details, guild_id) : nullptr;
    const auto* guild = details ? chat_state_store_find_guild(state, guild_id) : nullptr;
    constexpr u64 manage = kPermissionManageGuild | kPermissionAdministrator;
    const bool hub = guild && guild_space_visible(details->space, (guild->base_permissions & manage) != 0);
    const bool boost = guild && details->premium_progress_bar_enabled;
    const u16 boosts = boost ? static_cast<u16>(minimum<u32>(details->premium_subscription_count, 0xFFFF)) : 0;
    const u16 goal = boost ? static_cast<u16>(guild_boost_goal(details->features)) : 0;
    if (hub != view->hub_row || boost != view->boost_row || boosts != view->boost_count || goal != view->boost_goal) {
        view->hub_row = hub;
        view->boost_row = boost;
        view->boost_count = boosts;
        view->boost_goal = goal;
        ++view->events_revision;
    }
}

bool application_guild_hub_open(u64 guild_id) noexcept {
    if (!application_account_available() || !g_app.renderer_ready || !guild_id ||
        (!owner && !(owner = owner_allocate<Owner>())))
        return false;
    auto& p = *owner;
    p.guild_id = guild_id;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.hydrate_failed = p.mounted = false;
    request();
    // The official client's page asks for the server profile when it mounts, unless it has one from the last minute.
    if (p.brand_guild != guild_id || clock_monotonic_milliseconds() - p.brand_ms > 60'000)
        application_guild_community_hub(GuildCommunityOperation::guild_profile_get, guild_id);
    if (consent_scope != p.account_scope) {
        consent_scope = p.account_scope;
        consent = 0;
        consent_busy = false;
    }
    application_guild_community_hub(GuildCommunityOperation::consent_get, guild_id);
    if (!application_guild_hub_remembered(guild_id)) {
        static u32 next;
        remembered[next++ % 8] = guild_id;
    }
    return application_content_page_open(&kPage);
}

bool application_guild_hub_remembered(u64 guild_id) noexcept {
    for (const u64 id : remembered)
        if (id && id == guild_id)
            return true;
    return false;
}

void application_guild_hub_forget(u64 guild_id) noexcept {
    for (u64& id : remembered)
        if (id == guild_id)
            id = 0;
}

u32 application_guild_hub_people(u64 guild_id, u64* ids, u32 count, u32 capacity) noexcept {
    if (!page_open() || owner->guild_id != guild_id)
        return count;
    for (u32 i = 0; i < owner->people_count && count < capacity; ++i)
        ids[count++] = owner->people[i];
    return count;
}

Utf8View application_guild_hub_query() noexcept {
    return owner ? Utf8View{owner->query, owner->query_bytes} : Utf8View{};
}

void application_guild_hub_response(GuildCommunityOperation op, u64 guild_id, bool ok, u16 status,
                                    Utf8View body) noexcept {
    if (!g_app.account_ui)
        return;
    const Slice<u32> scratch{g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount};
    if (op == GuildCommunityOperation::consent_get) {
        AccountConsents consents{};
        if (ok && account_settings_decode_consents(body, scratch, &consents)) {
            consent = consents.personalization ? 1 : 2;
            log_u64(LogLevel::info, "hub", "personalization_consent", consents.personalization);
        }
    } else if (op != GuildCommunityOperation::space_get && op != GuildCommunityOperation::space_hydrate) {
        // The sharing dialog's change: the official client takes it as given once the server accepts it.
        consent_busy = false;
        if (ok)
            consent = op == GuildCommunityOperation::consent_grant ? 1 : 2;
        else
            log_u64(LogLevel::warning, "hub", "consent_update_status", status);
        if (application_guild_hub_sharing_active())
            sharing_repaint();
    } else if (!page_open() || owner->guild_id != guild_id) {
        return;
    } else if (op == GuildCommunityOperation::space_get) {
        auto& p = *owner;
        // A server that never set its hub up answers 404: an empty hub.
        if (ok ? guild_space_decode(body, scratch, &p.space) : status == 404) {
            if (!ok)
                reset_value(&p.space);
            p.phase = Phase::loaded;
            hydrate();
        } else
            p.phase = Phase::failed;
    } else
        owner->hydrate_failed = !ok || !guild_space_hydrate_decode(body, scratch, &owner->space);
    application_content_page_invalidate();
}

void application_guild_hub_profile(u64 guild_id, bool ok, Utf8View body) noexcept {
    if (!ok || !owner || owner->guild_id != guild_id || !g_app.account_ui)
        return;
    auto& p = *owner;
    const Slice<u32> scratch{g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount};
    if (!guild_profile_decode(body, scratch, &p.profile) || p.profile.id != guild_id)
        return;
    p.brand_guild = guild_id;
    p.brand_ms = clock_monotonic_milliseconds();
    p.brand_known = p.profile.has_brand_color;
    p.brand_rgb = p.profile.brand_color;
    application_content_page_invalidate();
}

void application_guild_hub_release() noexcept {
    if (page_open())
        application_content_page_close();
    if (owner)
        owner_release(owner);
    reset_value(&remembered);
}

bool application_guild_hub_sharing_active() noexcept {
    return dialog && dialog->scene.active;
}

void application_guild_hub_sharing_close(bool restore_focus) noexcept {
    if (!application_guild_hub_sharing_active())
        return;
    settings_modal_suspend(*dialog);
    settings_modal_close(*dialog, restore_focus && application_account_available());
}

void application_guild_hub_sharing_release() noexcept {
    application_guild_hub_sharing_close(false);
    if (dialog)
        settings_modal_free(dialog);
}

bool application_guild_hub_sharing_synchronize() noexcept {
    if (!application_guild_hub_sharing_active())
        return true;
    auto& d = *dialog;
    if (!application_account_available() || d.account_scope != g_app.plugin_account_scope_serial ||
        d.profile_id != g_app.account.status.profile_id) {
        application_guild_hub_sharing_close(false);
        return true;
    }
    return state_key(d.guild_id) != d.state ? sharing_repaint() : settings_modal_refresh(d);
}

SettingsLayer* application_guild_hub_sharing_settings_layer() noexcept {
    return dialog ? &dialog->layer : nullptr;
}

u64 application_guild_hub_sharing_caret_wake() noexcept {
    return application_guild_hub_sharing_active() ? settings_layer_next_wake(dialog->layer) : 0;
}
} // namespace maicord::app_detail
