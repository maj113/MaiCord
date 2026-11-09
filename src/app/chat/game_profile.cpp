#include "game_profile.hpp"
#include "game_lookup.hpp"
#include "composer/text_form.hpp"
#include "../account/account_access.hpp"
#include "../account/friends.hpp"
#include "../account/profile.hpp"
#include "../guild/channel_overview.hpp"
#include "../settings/modal.hpp"
#include "../settings/settings.hpp"
#include "../shell/content_input.hpp"
#include "../shell/state.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/guild/game_profile.hpp"
#include "maicord/ui/chat/game_media.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kPageOwner = 0x4D41474D50524F46ull; // "MAGMPROF"
constexpr UiPageKey kPage{kPageOwner, 1};
constexpr u32 kControlCapacity = 64;
constexpr u32 kFriendMaximum = 32;
constexpr u32 kTextBytes = 2048;
constexpr u32 kCoverDips = 160;
constexpr u32 kCoverColumns = 3;
enum : u64 {
    control_cover = 1,
    control_status,
    control_retry,
    control_summary,
    control_details,
    control_genres,
    control_publishers,
    control_developers,
    control_release,
    control_platforms,
    control_links,
    control_credit,
    control_friends,
    control_link_base = 100,
    control_friend_base = 200,
};

enum class Phase : u8 { loading, loaded, failed };

struct Owner : SettingsDialog {
    UiSettingsControl controls[kControlCapacity];
    UiSettingsControlPresentation presentation[kControlCapacity];
    UiSettingsAvatarMedia avatar_media[kControlCapacity];
    u32 control_count;
    u64 game_id, account_scope, profile_id;
    Phase phase;
    u32 games_revision, activities_revision;
    u32 title_bytes;
    char title[128];
    GameImageMedia cover;
    u64 friends[kFriendMaximum];
    u32 friend_count;
    // The links in the order shown: indices into game.links.
    u8 links[kGameProfileLinkMaximum];
    u32 link_count;
    char text[kTextBytes];
    TextBank bank;
    GameProfile game;
};
Owner* owner;

// The official client's genre names (SKU genres 1-69), '\0'-separated from genre 1 on.
constexpr char kGenreNames[] =
    "Action\0Action RPG\0Beat 'Em Up/Brawler\0Hack and Slash\0Platformer\0Stealth\0Survival\0Adventure\0"
    "Action-Adventure\0Metroidvania\0Open-World\0Psychological Horror\0Sandbox\0Survival Horror\0Visual Novel\0"
    "Driving/Racing\0Vehicular Combat\0Massively Multiplayer\0MMORPG\0Role-Playing\0Dungeon Crawler\0Roguelike\0"
    "Shooter\0Light-Gun\0Shoot 'Em Up\0FPS\0Dual-Joystick Shooter\0Simulation\0Flight Simulator\0Train Simulator\0"
    "Life Simulator\0Fishing\0Sports\0Baseball\0Basketball\0Billiards\0Bowling\0Boxing\0Football\0Golf\0Hockey\0"
    "Skateboarding/Skating\0Snowboarding/Skiing\0Soccer\0Track & Field\0Surfing/Wakeboarding\0Wrestling\0"
    "Strategy\0" "4X\0Artillery\0RTS\0Tower Defense\0Turn-Based Strategy\0Wargame\0MOBA\0Fighting\0Puzzle\0"
    "Card Game\0Education\0Fitness\0Gambling\0Music/Rhythm\0Party/Mini-Game\0Pinball\0Trivia/Board Game\0"
    "Tactical\0Indie\0Arcade\0Point and Click";
constexpr u32 kGenreCount = 69;

Utf8View genre_name(u32 genre) noexcept {
    if (!genre || genre > kGenreCount)
        return {};
    const char* at = kGenreNames;
    for (u32 i = 1; i < genre; ++i)
        at += c_string_length(at) + 1;
    return {at, c_string_length(at)};
}

// Game platform types (Userdoccers): desktop 0, xbox 1, playstation 2, ios 3, android 4, nintendo 5, linux 6,
// macos 7, roblox 8. The official client folds Linux and macOS into Desktop and iOS and Android into Mobile.
struct Platform {
    u16 mask;
    const char* name;
};
constexpr Platform kPlatforms[] = {{(1u << 0) | (1u << 6) | (1u << 7), "Desktop"},
                                   {(1u << 3) | (1u << 4), "Mobile"},
                                   {1u << 1, "Xbox"},
                                   {1u << 2, "PlayStation"},
                                   {1u << 5, "Nintendo"},
                                   {1u << 8, "Roblox"}};

// Website categories the official client shows, in its order, then its store links: App Store (iPhone, then
// iPad) and Play Store (Android), each only on the store's own host.
struct LinkKind {
    u8 category;
    const char* title;
    const char* host; // stores: the URL must be on this host
};
constexpr LinkKind kLinkKinds[] = {{1, "Official web page", nullptr},
                                   {4, "Facebook", nullptr},
                                   {5, "X (Twitter)", nullptr},
                                   {8, "Instagram", nullptr},
                                   {9, "YouTube", nullptr},
                                   {19, "Bluesky", nullptr},
                                   {14, "Reddit", nullptr},
                                   {6, "Twitch", nullptr},
                                   {22, "Roblox", nullptr},
                                   {10, "App Store", "apps.apple.com"},
                                   {10, "App Store", "itunes.apple.com"},
                                   {11, "App Store", "apps.apple.com"},
                                   {11, "App Store", "itunes.apple.com"},
                                   {12, "Play Store", "play.google.com"}};
constexpr u32 kFirstStore = 9;

Utf8View literal(const char* text) noexcept {
    return {text, c_string_length(text)};
}
Utf8View view(const char* text, u32 bytes) noexcept {
    return {text, bytes};
}

bool web_url(Utf8View url) noexcept {
    return (url.bytes > 8 && bytes_equal(url.data, "https://", 8)) ||
           (url.bytes > 7 && bytes_equal(url.data, "http://", 7));
}
bool on_host(Utf8View url, const char* host) noexcept {
    const u32 scheme = url.bytes > 8 && bytes_equal(url.data, "https://", 8) ? 8u : 7u;
    const u32 length = c_string_length(host);
    return url.bytes >= scheme + length && bytes_equal(url.data + scheme, host, length) &&
           (url.bytes == scheme + length || url.data[scheme + length] == '/' || url.data[scheme + length] == '?' ||
            url.data[scheme + length] == ':');
}

// The links in the official client's order: listed categories by kLinkKinds, then at most one store link each.
void order_links() noexcept {
    auto& p = *owner;
    const auto& g = p.game;
    p.link_count = 0;
    bool app_store = false;
    for (u32 k = 0; k < sizeof(kLinkKinds) / sizeof(kLinkKinds[0]); ++k) {
        const auto& kind = kLinkKinds[k];
        const bool store = k >= kFirstStore;
        if (store && kind.category != 12 && app_store)
            continue;
        for (u32 i = 0; i < g.link_count; ++i) {
            const Utf8View url = view(g.links[i].url, g.links[i].url_bytes);
            if (g.links[i].category != kind.category || !web_url(url) || (kind.host && !on_host(url, kind.host)))
                continue;
            p.links[p.link_count++] = static_cast<u8>(i);
            app_store |= kind.category != 12 && store;
            if (store)
                break;
        }
    }
}
Utf8View link_title(const GameProfileLink& link) noexcept {
    for (const auto& kind : kLinkKinds)
        if (kind.category == link.category)
            return literal(kind.title);
    return {};
}

// "September 4, 2023" (the official client's "LL"), in UTC.
Utf8View release_text(u64 ms) noexcept {
    static constexpr const char* kMonths[12] = {"January", "February", "March",     "April",   "May",      "June",
                                                "July",    "August",   "September", "October", "November", "December"};
    auto& bank = owner->bank;
    const CivilDate date = civil_date_from_days(static_cast<i64>(ms / 86'400'000));
    if (date.month < 1 || date.month > 12 || date.year < 0)
        return {};
    const u32 start = bank.used;
    bank.add(literal(kMonths[date.month - 1]));
    bank.add(" ");
    bank.number(date.day);
    bank.add(", ");
    bank.number(static_cast<u64>(date.year));
    return bank.since(start);
}

const GameRecord* known_game() noexcept {
    const GameRecord* record = owner ? game_lookup_find(owner->game_id) : nullptr;
    return record && record->state == GameState::known ? record : nullptr;
}

bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.gateway_ready && g_app.account.api.guild_admin;
}
bool scope_current() noexcept {
    return owner && application_account_available() && owner->account_scope == g_app.plugin_account_scope_serial &&
           owner->profile_id == g_app.account.status.profile_id;
}

// Friends whose presence shows them playing the game now. The official client lists the friends its content
// inventory says played it recently; MaiCord has presences, so it lists the friends playing it at the moment.
void collect_friends() noexcept {
    auto& p = *owner;
    p.friend_count = 0;
    const auto* relationships = application_friends_relationships();
    if (!relationships || !g_app.account_ui)
        return;
    const auto& activities = g_app.account_ui->user_store.activities;
    p.activities_revision = activities.revision;
    for (u32 i = 0; i < relationships->count && p.friend_count < kFriendMaximum; ++i) {
        const auto& record = relationships->records[i];
        UserActivitySet set{};
        if (record.type != RelationshipType::friend_ || !user_activity_find(activities, record.user_id, &set))
            continue;
        for (u32 a = 0; a < set.count; ++a)
            if (set.items[a].application_id == p.game_id && set.items[a].type == UserActivityType::playing) {
                p.friends[p.friend_count++] = record.user_id;
                break;
            }
    }
}

u32 push(const UiSettingsControl& control, UiSettingsControlPresentation presentation = {
                                                UiSettingsVariant::standard, 0, kUiSettingsNoDefault}) noexcept {
    auto& p = *owner;
    if (p.control_count >= kControlCapacity)
        return ~0u;
    p.presentation[p.control_count] = presentation;
    reset_value(&p.avatar_media[p.control_count]);
    p.controls[p.control_count] = control;
    return p.control_count++;
}
UiSettingsControl row(u64 id, Utf8View label, Utf8View description, UiSettingsControlKind kind,
                      Utf8View action_label = {}) noexcept {
    return {id, label, description, kind, true, false, {}, 0, action_label, nullptr};
}
// A details line: its label and value, or "-" when the game has none.
void detail(u64 id, Utf8View label, Utf8View value) noexcept {
    push(row(id, label, value.bytes ? value : utf8_literal("-"), UiSettingsControlKind::notice));
}

void project_details() noexcept {
    auto& p = *owner;
    const auto& g = p.game;
    auto& bank = p.bank;
    push(row(control_details, utf8_literal("Details"), {}, UiSettingsControlKind::category));
    u32 start = bank.used;
    for (u32 i = 0; i < g.genre_count; ++i)
        if (const Utf8View name = genre_name(g.genres[i]); name.bytes) {
            if (bank.used != start)
                bank.add(", ");
            bank.add(name);
        }
    detail(control_genres, g.genre_count == 1 ? utf8_literal("Genre") : utf8_literal("Genres"), bank.since(start));
    detail(control_publishers, g.publisher_count == 1 ? utf8_literal("Publisher") : utf8_literal("Publishers"),
           view(g.publishers, g.publishers_bytes));
    detail(control_developers, g.developer_count == 1 ? utf8_literal("Developer") : utf8_literal("Developers"),
           view(g.developers, g.developers_bytes));
    detail(control_release, utf8_literal("Release Date"), g.release_ms ? release_text(g.release_ms) : Utf8View{});
    start = bank.used;
    u32 platforms = 0;
    for (const auto& platform : kPlatforms)
        if (g.platforms & platform.mask) {
            if (platforms++)
                bank.add(", ");
            bank.add(literal(platform.name));
        }
    detail(control_platforms, platforms > 1 ? utf8_literal("Platforms") : utf8_literal("Platform"),
           bank.since(start));
    if (!p.link_count) {
        detail(control_links, utf8_literal("Links"), {});
    } else {
        push(row(control_links, utf8_literal("Links"), {}, UiSettingsControlKind::notice));
        for (u32 i = 0; i < p.link_count; ++i) {
            const auto& link = g.links[p.links[i]];
            push(row(control_link_base + i, link_title(link), view(link.url, link.url_bytes),
                     UiSettingsControlKind::action, utf8_literal("Open")));
        }
    }
    push(row(control_credit, utf8_literal("Game metadata by"), {}, UiSettingsControlKind::footnote,
             utf8_literal("IGDB")));
}

bool project() noexcept {
    if (!owner || !owner->scene.active)
        return true;
    auto& p = *owner;
    p.control_count = 0;
    p.bank = {p.text, kTextBytes, 0};
    const GameRecord* record = known_game();
    p.games_revision = game_lookup_revision();
    // The cover art leads, captioned with the game's name.
    if (record && game_cover_media(record->game, kCoverDips, g_app.renderer.dpi, &p.cover)) {
        const u32 index = push(row(control_cover, view(p.title, p.title_bytes), {}, UiSettingsControlKind::action),
                               {UiSettingsVariant::standard, ui_settings_presentation_avatar, kCoverColumns, 0, 0,
                                ui_settings_more_cell});
        if (index != ~0u) {
            p.avatar_media[index].key = p.cover.key;
            p.avatar_media[index].url = p.cover.url_view();
        }
    }
    switch (p.phase) {
    case Phase::loading: {
        UiSettingsControl loading = row(control_status, utf8_literal("Loading"), {}, UiSettingsControlKind::notice);
        loading.checked = true;
        push(loading);
        break;
    }
    case Phase::failed:
        push(row(control_status, utf8_literal("Couldn't load this game's details"),
                 utf8_literal("Check your connection and try again."), UiSettingsControlKind::notice));
        push(row(control_retry, utf8_literal("Try again"), {}, UiSettingsControlKind::action, utf8_literal("Retry")));
        p.controls[p.control_count - 1].enabled = connected();
        break;
    case Phase::loaded:
        if (p.game.summary_bytes)
            push(row(control_summary, {}, view(p.game.summary, p.game.summary_bytes), UiSettingsControlKind::notice));
        project_details();
        break;
    }
    collect_friends();
    if (p.friend_count) {
        push(row(control_friends, utf8_literal("Friends Who Play"), {}, UiSettingsControlKind::category));
        const auto& users = g_app.account_ui->user_store;
        for (u32 i = 0; i < p.friend_count; ++i) {
            const auto* user = user_store_find(users, p.friends[i]);
            const u32 index = push(row(control_friend_base + i,
                                       user ? user_store_name(users, *user) : utf8_literal("A friend"),
                                       utf8_literal("Playing Now"), UiSettingsControlKind::action),
                                   {UiSettingsVariant::standard, ui_settings_presentation_avatar, kUiSettingsNoDefault});
            if (index != ~0u)
                p.avatar_media[index] = application_user_avatar_media(p.friends[i]);
        }
    }
    p.content.controls = {p.controls, p.control_count};
    p.content.presentation = p.presentation;
    p.content.avatar_media = p.avatar_media;
    ++p.content.revision;
    return settings_modal_update(p);
}
bool repaint() noexcept {
    return settings_modal_present(project());
}

void request() noexcept {
    owner->phase = Phase::loading;
    game_lookup_profile(owner->game_id);
}

void activate(SettingsLayer*, const SettingsLayerActivation& activation) noexcept {
    if (!owner)
        return;
    auto& p = *owner;
    const auto target = activation.value.target;
    if (target.kind == UiSettingsTargetKind::close) {
        application_game_profile_close();
        return;
    }
    if (target.kind != UiSettingsTargetKind::control)
        return;
    const u64 id = target.control_id;
    if (id == control_retry && p.phase == Phase::failed && connected()) {
        request();
        repaint();
    } else if (id == control_credit) {
        application_open_content_url(utf8_literal("https://www.igdb.com/about"));
    } else if (id >= control_link_base && id < control_link_base + p.link_count) {
        const auto& link = p.game.links[p.links[id - control_link_base]];
        application_open_content_url(view(link.url, link.url_bytes));
    } else if (id >= control_friend_base && id < control_friend_base + p.friend_count) {
        // The friend's full profile, over the chat this modal leaves.
        const u64 user = p.friends[id - control_friend_base];
        application_game_profile_close(false);
        if (application_profile_open(user, activation.anchor))
            application_profile_expand(user);
    }
}
constexpr SettingsLayerHooks kHooks{settings_hook_editor_none,
                                    settings_hook_edited_none,
                                    activate,
                                    settings_hook_ignore_choice,
                                    settings_hook_close<application_game_profile_close>,
                                    settings_hook<repaint>,
                                    nullptr,
                                    settings_hook_not_editable};

bool not_found_valid(TextFormOwnerKey) noexcept {
    return true;
}
bool not_found_save(TextFormOwnerKey, Utf8View) noexcept {
    return true;
}

// The official client's dialog for a mention of a game it cannot show.
bool open_not_found() noexcept {
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Game could not be found");
    request.context = utf8_literal("The game is either missing or has been removed.");
    request.accept = utf8_literal("Okay");
    request.owner = {g_app.plugin_account_scope_serial, 0, 0};
    request.valid = not_found_valid;
    request.save = not_found_save;
    request.confirmation_only = true;
    request.single_action = true;
    request.accept_focused = true;
    return application_text_form_open(request);
}

} // namespace

bool application_game_profile_open(u64 game_id) noexcept {
    const GameRecord* record = game_lookup_find(game_id);
    if (!record || record->state != GameState::known)
        return open_not_found();
    if (!application_account_available() || !g_app.renderer_ready || g_app.text_form ||
        application_channel_overview_active())
        return false;
    if (!owner && !(owner = settings_modal_allocate<Owner>(kHooks)))
        return false;
    settings_modal_yield();
    auto& p = *owner;
    p.game_id = game_id;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    reset_value(&p.game);
    p.link_count = 0;
    p.title_bytes = utf8_copy_truncated(p.title, sizeof(p.title), view(record->game.name, record->game.name_bytes));
    request();
    if (!settings_dialog_begin(p, kPage, view(p.title, p.title_bytes)))
        return false;
    if (settings_dialog_show(p, kPage, 0, false, project) && repaint())
        return true;
    application_game_profile_close(false);
    return false;
}

bool application_game_profile_active() noexcept {
    return owner && owner->scene.active;
}

void application_game_profile_close(bool restore_focus) noexcept {
    if (!application_game_profile_active())
        return;
    game_lookup_profile_cancel();
    settings_modal_suspend(*owner);
    settings_modal_close(*owner, restore_focus && application_account_available());
}

void application_game_profile_release() noexcept {
    application_game_profile_close(false);
    if (owner)
        settings_modal_free(owner);
}

bool application_game_profile_synchronize() noexcept {
    if (!application_game_profile_active())
        return true;
    if (!scope_current()) {
        application_game_profile_close(false);
        return true;
    }
    // A friend starting or stopping the game changes who plays it.
    if (g_app.account_ui->user_store.activities.revision != owner->activities_revision)
        return repaint();
    return settings_modal_refresh(*owner);
}

SettingsLayer* application_game_profile_settings_layer() noexcept {
    return owner ? &owner->layer : nullptr;
}

u64 application_game_profile_caret_wake() noexcept {
    return application_game_profile_active() ? settings_layer_next_wake(owner->layer) : 0;
}

void application_game_profile_response(u64 game_id, bool ok, Utf8View body) noexcept {
    if (!application_game_profile_active() || owner->game_id != game_id || owner->phase != Phase::loading)
        return;
    auto& p = *owner;
    const Slice<u32> scratch{g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount};
    if (ok && game_profile_decode(body, scratch, game_id, &p.game)) {
        p.phase = Phase::loaded;
        order_links();
    } else if (ok && !known_game()) {
        // The server no longer knows the game: the official client's not-found dialog instead.
        application_game_profile_close(false);
        open_not_found();
        return;
    } else {
        p.phase = Phase::failed;
    }
    repaint();
}

void application_game_profile_games_changed() noexcept {
    if (application_game_profile_active() && owner->games_revision != game_lookup_revision())
        repaint();
}

} // namespace maicord::app_detail
