#include "card.hpp"
#include "../account/account_presentation.hpp"
#include "../account/login/action_captcha.hpp"
#include "../shell/content_input.hpp"
#include "../shell/cursor.hpp"
#include "community.hpp"
#include "invite.hpp"
#include "../shell/input.hpp"
#include "../shell/window_events.hpp"
#include "../shell/state.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/guild/invite_card_cache.hpp"
#include "maicord/protocol/guild/assets.hpp"
#include "maicord/protocol/guild/community.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "maicord/ui/guild/card_scene.hpp"
#include "maicord/ui/guild/banner.hpp"
#include "maicord/ui/account/profile_media.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/chat/game_media.hpp"
#include "maicord/ui/text/emoji_catalog.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/ui/chat/messages/rich_layout.hpp"
#include "../chat/game_lookup.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord::app_detail {
namespace {
// Past closed, the phases follow UiGuildCardPhase.
enum class Phase : u8 { closed, loading, ready, private_profile, failed };
static_assert(static_cast<u8>(Phase::loading) - 1 == static_cast<u8>(UiGuildCardPhase::loading) &&
              static_cast<u8>(Phase::ready) - 1 == static_cast<u8>(UiGuildCardPhase::ready) &&
              static_cast<u8>(Phase::private_profile) - 1 == static_cast<u8>(UiGuildCardPhase::private_profile) &&
              static_cast<u8>(Phase::failed) - 1 == static_cast<u8>(UiGuildCardPhase::failed));

struct State {
    UiGuildCardScene scene;
    GuildProfile profile;
    u64 scope, guild_id, loaded_guild, loaded_ms;
    UiRect anchor;
    Phase phase;
    bool pressed;
    u8 media_logged; // media_failed bits: 1 icon, 2 badge, 4 banner
    bool has_icon_color;
    u32 icon_color; // the icon's dominant colour once read back (has_icon_color)
    float saturation; // the accessibility saturation the last projection applied
    bool animate_emoji; // "Play animated emoji" as the last projection read it
    MediaAssetKey icon_key, badge_key;
    char icon_url[192], badge_url[192];
    u32 icon_url_bytes, badge_url_bytes;
    UiGuildImageMedia banner;
    // The known games' pictures (the first five, then the ones "+N"'s tooltip lists, the first dimmed under it), as
    // of the game lookup's revision, and the names of the games after the fifth.
    GameImageMedia games[kUiGuildCardGames + kUiGuildCardGamesMore];
    u32 games_revision;
    char games_more_names[384];
    u32 scratch[8192];
};
State* g_card;

// The popout's join outlives the popout: a CAPTCHA opens the browser (the
// popout closes with the focus) and the server arrives over the Gateway later.
enum class JoinPhase : u8 { idle, requesting, awaiting, captcha, retry };
constexpr u64 kJoinArrivalMs = 20'000;
struct JoinState {
    u64 guild_id, scope, deadline_ms;
    JoinPhase phase;
};
JoinState g_join;

bool available() noexcept {
    return g_app.account_ui && !g_app.login.visible;
}
bool member_of(u64 guild_id) noexcept {
    return guild_id && g_app.account_ui && chat_state_store_find_guild(g_app.account_ui->chat_state, guild_id);
}
bool open_now() noexcept {
    return g_card && g_card->phase != Phase::closed && g_card->scene.active;
}
bool joining(u64 guild_id) noexcept {
    return guild_id && g_join.phase != JoinPhase::idle && g_join.guild_id == guild_id &&
           g_join.scope == g_app.plugin_account_scope_serial;
}
// An invite to the server already resolved in chat (a known invite).
const InviteCardEntry* known_invite(u64 guild_id) noexcept {
    const auto* cache = g_app.message_scene.invite_cards;
    for (u32 i = 0; cache && i < kInviteCardCacheEntries; ++i)
        if (cache->entries[i].state == InviteCardState::resolved && cache->entries[i].invite.guild_id == guild_id &&
            cache->entries[i].code_bytes)
            return &cache->entries[i];
    return nullptr;
}
UiGuildCardAction action() noexcept {
    const auto& c = *g_card;
    if (member_of(c.guild_id))
        return UiGuildCardAction::go_to_server;
    if (joining(c.guild_id))
        return UiGuildCardAction::joining;
    if ((c.profile.features & guild_profile_discoverable) || known_invite(c.guild_id))
        return UiGuildCardAction::join;
    return UiGuildCardAction::none;
}
// The icon (icons/{id}/{hash}.png) and the tag badge (clan-badges, the
// key user_guild_badge_media gives the same badge on author lines).
void resolve_media() noexcept {
    auto& c = *g_card;
    c.icon_key = c.badge_key = {};
    c.icon_url_bytes = c.badge_url_bytes = 0;
    c.has_icon_color = false;
    const auto& p = c.profile;
    // The profile's custom banner shows only on a discoverable server, from
    // the discovery-splash path (512 * dpr as on the official card, capped at
    // the 512 px the JPEG decoder accepts); otherwise the colour glow.
    ui_guild_card_banner_media(p.id, {p.banner, p.banner_bytes}, (p.features & guild_profile_discoverable) != 0,
                               g_app.renderer.dpi, &c.banner);
    if (p.icon_bytes) {
        // A 64 px icon request that may not animate: the CDN's still WebP
        // (an a_ icon's representative frame) at ui_media_proxy_size(64 * dpr).
        const u32 dpi = g_app.renderer.dpi ? g_app.renderer.dpi : 96u;
        TextBank url{c.icon_url, sizeof(c.icon_url), 0};
        url.add("https://cdn.discordapp.com/icons/");
        url.number(p.id);
        url.add("/");
        url.add({p.icon, p.icon_bytes});
        url.add(".webp?size=");
        url.number(ui_media_proxy_size((64u * dpi + 95u) / 96u));
        c.icon_url_bytes = url.used;
        c.icon_key = media_asset_embed(hash64_bytes(kHash64Seed, c.icon_url, url.used), 1, 0, MediaEmbedResource::image,
                                       MediaAttachmentFormat::webp, false, MediaResolutionTier::px512);
        c.icon_key.kind = MediaAssetKind::profile_image;
    }
    if (p.badge_bytes && p.badge_bytes < sizeof(UserAppearance{}.primary_badge.data)) {
        UserAppearance appearance{};
        appearance.primary_enabled = true;
        appearance.primary_guild_id = p.id;
        copy_bytes(appearance.primary_badge.data, p.badge, p.badge_bytes);
        appearance.primary_badge.bytes = p.badge_bytes;
        ProfileMedia media{};
        if (user_guild_badge_media(appearance, {}, {}, &media) && media.url.bytes <= sizeof(c.badge_url)) {
            copy_bytes(c.badge_url, media.url.data, media.url.bytes);
            c.badge_url_bytes = media.url.bytes;
            c.badge_key = media.key;
        }
    }
}
// The profile's games the lookup knows, by activity score (the official card leaves unknown ones out): the first
// five with their names, then the next ones' pictures and names for "+N" (the first dimmed under it) and its tooltip.
void project_games(UiGuildCardContent* content) noexcept {
    auto& c = *g_card;
    c.games_revision = game_lookup_revision();
    const u32 dpi = g_app.renderer.dpi ? g_app.renderer.dpi : 96u;
    TextBank more{c.games_more_names, sizeof(c.games_more_names), 0};
    u32 known = 0;
    for (u32 i = 0; i < c.profile.game_count; ++i) {
        const GameRecord* record = game_lookup_find(c.profile.games[i].id);
        if (!record || record->state != GameState::known)
            continue;
        const auto& game = record->game;
        if (known < kUiGuildCardGames + kUiGuildCardGamesMore && !game_icon_media(game, 24, dpi, &c.games[known]))
            reset_value(&c.games[known]);
        if (known < kUiGuildCardGames) {
            content->games[known] = c.games[known].key;
            content->game_names[known] = {game.name, game.name_bytes};
            content->game_high[known] = c.profile.games[i].level == kGuildProfileActivityHigh;
        } else if (known < kUiGuildCardGames + kUiGuildCardGamesMore) {
            content->games_more_icons[known - kUiGuildCardGames] = c.games[known].key;
            if (more.used)
                more.add("\n");
            more.add({game.name, game.name_bytes});
        }
        ++known;
    }
    for (u32 i = known; i < kUiGuildCardGames + kUiGuildCardGamesMore; ++i)
        reset_value(&c.games[i]);
    content->game_count = known;
    content->games_more_names = more.view();
}
// A trait's emoji as the official card resolves it: a custom one from the server's emoji (known while a member),
// else a Unicode one by name. An animated custom one plays while "Play animated emoji" is on, through the shared
// animation timeline (reduced motion holds it on its frame).
void project_trait_emoji(UiGuildCardContent* content) noexcept {
    const auto& p = g_card->profile;
    g_card->animate_emoji = message_rich_display().emoji == MessageMediaAnimation::always;
    for (u32 i = 0; i < content->trait_count; ++i) {
        const auto& trait = p.traits[i];
        if (trait.emoji_id) {
            const auto* emoji = chat_state_store_find_emoji(g_app.account_ui->chat_state, trait.emoji_id);
            if (emoji && emoji->guild_id == p.id)
                content->trait_emoji_media[i] =
                    media_asset_custom_emoji(trait.emoji_id, trait.emoji_animated && g_card->animate_emoji);
        } else if (trait.emoji_name_bytes) {
            content->trait_emoji[i] = emoji_shortcode_glyph({trait.emoji_name, trait.emoji_name_bytes});
        }
    }
}
bool project() noexcept {
    auto& c = *g_card;
    if (c.phase == Phase::closed || !available())
        return false;
    const auto& p = c.profile;
    UiGuildCardContent content{};
    content.guild_id = c.guild_id;
    content.phase = static_cast<UiGuildCardPhase>(static_cast<u8>(c.phase) - 1);
    if (c.phase == Phase::ready) {
        content.name = {p.name, p.name_bytes};
        content.description = {p.description, p.description_bytes};
        content.tag = {p.tag, p.tag_bytes};
        content.trait_count = minimum<u32>(p.trait_count, kUiGuildCardTraits);
        for (u32 i = 0; i < content.trait_count; ++i)
            content.traits[i] = {p.traits[i].label, p.traits[i].label_bytes};
        content.online_count = p.online_count;
        content.member_count = p.member_count;
        // The banner glows in the brand colour, else in the icon's colour once read back (under the accessibility
        // saturation, as the official client tints it), else the theme's.
        c.saturation = application_user_color_saturation();
        content.has_banner_color = p.has_brand_color || c.has_icon_color;
        content.banner_color = p.has_brand_color ? p.brand_color : ui_guild_card_desaturate(c.icon_color, c.saturation);
        content.server = {(p.features & guild_profile_verified) != 0, (p.features & guild_profile_partnered) != 0,
                          (p.features & guild_profile_community) != 0, (p.features & guild_profile_discoverable) != 0,
                          (p.features & guild_profile_manual_approval) && (p.features & guild_profile_verification_gate),
                          p.premium_tier, p.premium_subscription_count};
        project_trait_emoji(&content);
        project_games(&content);
        content.icon = c.icon_key;
        content.badge = c.badge_key;
        content.banner = c.banner.key;
        content.action = action();
    }
    if (!ui_guild_card_show(&c.scene, c.guild_id, content, c.anchor, application_viewport_dips(),
                            clock_monotonic_milliseconds(), application_reduced_motion(),
                            &g_app.account_ui->text_measure_cache)) {
        c.phase = Phase::closed;
        g_app.renderer.guild_card_scene = nullptr;
        return false;
    }
    g_app.renderer.guild_card_scene = &c.scene;
    application_request_frame();
    return true;
}
bool ensure() noexcept {
    if (!g_card) {
        g_card = static_cast<State*>(commit_pages(sizeof(State)));
        if (!g_card)
            return false;
        g_card->scope = g_app.plugin_account_scope_serial;
    }
    if (g_card->scope != g_app.plugin_account_scope_serial) {
        // Another account: nothing of the previous one's popout survives.
        application_guild_card_close();
        const auto scene = g_card->scene;
        clear_bytes(g_card, static_cast<u32>(sizeof(State)));
        g_card->scene = scene;
        g_card->scope = g_app.plugin_account_scope_serial;
    }
    return true;
}
void activate() noexcept {
    auto& c = *g_card;
    switch (action()) {
    case UiGuildCardAction::go_to_server: {
        const u64 guild = c.guild_id;
        application_guild_card_close();
        select_content_guild(guild);
        return;
    }
    case UiGuildCardAction::join:
        // As in the official client: a known invite wins (joined through the
        // invite), else a discoverable server is lurked (a lurker join, then
        // opened).
        if (const auto* invite = known_invite(c.guild_id)) {
            char code[kGuildInviteCodeMaximumBytes];
            const u32 bytes = invite->code_bytes;
            copy_bytes(code, invite->code, bytes);
            application_guild_card_close();
            application_guild_invite_open({code, bytes});
        } else if ((c.profile.features & guild_profile_discoverable) && application_guild_community_join(c.guild_id)) {
            if (g_join.phase == JoinPhase::captcha)
                application_action_captcha_cancel();
            g_join = {c.guild_id, g_app.plugin_account_scope_serial, 0, JoinPhase::requesting};
            project();
        }
        return;
    default:
        return;
    }
}
void retry() noexcept {
    auto& c = *g_card;
    c.phase = Phase::loading;
    reset_value(&c.profile);
    application_guild_community_profile(c.guild_id);
    project();
}
} // namespace

bool application_guild_card_open(u64 guild_id, UiRect anchor) noexcept {
    if (!available() || !guild_id || !ensure())
        return false;
    auto& c = *g_card;
    if (open_now() && c.guild_id == guild_id) {
        // A second click on the same chip closes it, as a popout toggles.
        application_guild_card_close();
        return true;
    }
    c.guild_id = guild_id;
    c.anchor = anchor;
    c.pressed = false;
    c.media_logged = 0;
    // A profile read moments ago opens at once (the official client keeps it a while).
    const bool cached = c.loaded_guild == guild_id && c.profile.id == guild_id &&
                        clock_monotonic_milliseconds() - c.loaded_ms < 60'000;
    if (cached) {
        c.phase = Phase::ready;
    } else {
        c.phase = Phase::loading;
        reset_value(&c.profile);
        c.icon_key = c.badge_key = {};
        c.icon_url_bytes = c.badge_url_bytes = 0;
        reset_value(&c.banner);
        if (!application_guild_community_profile(guild_id))
            c.phase = Phase::failed;
    }
    return project();
}

void application_guild_card_close() noexcept {
    if (!g_card || g_card->phase == Phase::closed)
        return;
    g_card->phase = Phase::closed;
    g_card->pressed = false;
    ui_guild_card_close(&g_card->scene);
    if (g_app.renderer.guild_card_scene == &g_card->scene)
        g_app.renderer.guild_card_scene = nullptr;
    application_request_frame();
}

void application_guild_card_response(u64 guild_id, bool join, const RestClientResponse& response,
                                     Utf8View body) noexcept {
    const bool ok = response.succeeded();
    if (join) {
        if (!joining(guild_id) || g_join.phase != JoinPhase::requesting)
            return;
        if (ok) {
            // The server arrives over the Gateway (GUILD_CREATE to this
            // session); application_guild_card_join_poll opens it.
            g_join.phase = JoinPhase::awaiting;
            g_join.deadline_ms = clock_monotonic_milliseconds() + kJoinArrivalMs;
        } else if (response.transport_ok() && response.status_code == 400 && response.rejection.captcha &&
                   application_action_captcha_capture(ApplicationActionCaptchaKind::guild_join, guild_id, response,
                                                      body) &&
                   application_action_captcha_verify(ApplicationActionCaptchaKind::guild_join, guild_id)) {
            // The server asks for a CAPTCHA first: its challenge opens in the
            // browser and the solved token arms one retry of the same join.
            g_join.phase = JoinPhase::captcha;
        } else {
            log_u64(LogLevel::warning, "guild_card", "join_status", response.status_code);
            reset_value(&g_join);
        }
        if (open_now() && g_card->guild_id == guild_id)
            project();
        return;
    }
    if (!g_card || g_card->guild_id != guild_id || g_card->phase == Phase::closed)
        return;
    auto& c = *g_card;
    if (ok && guild_profile_decode(body, {c.scratch, 8192}, &c.profile) && c.profile.id == guild_id) {
        c.phase = Phase::ready;
        c.loaded_guild = guild_id;
        c.loaded_ms = clock_monotonic_milliseconds();
        resolve_media();
    } else {
        const u64 code = response.transport_ok() ? guild_asset_error_code(body, {c.scratch, 8192}) : 0;
        if (ok)
            log_write(LogLevel::warning, "guild_card", "profile_decode_failed");
        reset_value(&c.profile);
        c.phase = code == kGuildProfilePrivateErrorCode ? Phase::private_profile : Phase::failed;
    }
    project();
}

bool application_guild_card_window_event(const WindowEvent& event) noexcept {
    if (!open_now())
        return false;
    auto& c = *g_card;
    if (event.type == EventType::key_down) {
        if ((event.flags & ev_system) != 0 || event.key.key != key::escape)
            return false;
        application_guild_card_close();
        return true;
    }
    if (input_vertical_wheel(event) || event.type == EventType::resize || event.type == EventType::cancel_input ||
        input_focus_lost(event)) {
        // Its anchor moves or the window goes away: the popout closes.
        application_guild_card_close();
        return false;
    }
    if (event.type == EventType::pointer_leave) {
        if (ui_guild_card_set_hover(&c.scene, -1e7f, -1e7f))
            application_request_frame();
        return false;
    }
    const bool move = event.type == EventType::pointer_move, cursor = event.type == EventType::cursor_query;
    const bool press = input_down(event), release = input_release(event);
    const bool secondary_press = input_press(event, PointerButton::right);
    if (!move && !cursor && !press && !release && !secondary_press && !input_release(event, PointerButton::right))
        return false;
    UiPoint point{};
    if (cursor) {
        if (!client_cursor_dip(event, &point))
            return false;
    } else
        point = {client_dip_x(event), client_dip_y(event)};
    const bool over = ui_guild_card_contains(c.scene, point.x, point.y);
    const auto hit = ui_guild_card_hit(c.scene, point.x, point.y);
    if (move) {
        if (ui_guild_card_set_hover(&c.scene, point.x, point.y))
            application_request_frame();
        return over;
    }
    if (cursor) {
        if (!over)
            return false;
        application_set_cursor(ui_action_cursor(hit != UiGuildCardControl::none));
        return true;
    }
    if (press) {
        if (!over) {
            // Outside the popout: it closes and the click is trapped, as in the official client.
            application_guild_card_close();
            return true;
        }
        c.pressed = hit != UiGuildCardControl::none;
        if (ui_guild_card_set_pressed(&c.scene, hit == UiGuildCardControl::action))
            application_request_frame();
        return true;
    }
    if (release) {
        const bool pressed = c.pressed;
        c.pressed = false;
        if (ui_guild_card_set_pressed(&c.scene, false))
            application_request_frame();
        if (!over)
            return pressed;
        if (pressed && hit == UiGuildCardControl::action)
            activate();
        else if (pressed && hit == UiGuildCardControl::retry)
            retry();
        return true;
    }
    if (secondary_press && !over)
        application_guild_card_close();
    return over;
}

void application_guild_card_join_poll(u64 now_ms) noexcept {
    if (g_join.phase == JoinPhase::idle)
        return;
    const u64 guild = g_join.guild_id;
    if (g_join.scope != g_app.plugin_account_scope_serial || !available()) {
        reset_value(&g_join);
        return;
    }
    // The lurked server is here: switch to it at once, as the official client does.
    if ((g_join.phase == JoinPhase::requesting || g_join.phase == JoinPhase::awaiting) && member_of(guild)) {
        reset_value(&g_join);
        if (open_now() && g_card->guild_id == guild)
            application_guild_card_close();
        select_content_guild(guild);
        return;
    }
    if (g_join.phase == JoinPhase::awaiting && now_ms >= g_join.deadline_ms) {
        log_write(LogLevel::warning, "guild_card", "join_arrival_timeout");
        reset_value(&g_join);
    } else if (g_join.phase == JoinPhase::captcha) {
        switch (application_action_captcha_phase(ApplicationActionCaptchaKind::guild_join, guild)) {
        case ApplicationActionCaptchaPhase::solved:
            g_join.phase = application_guild_community_join(guild) ? JoinPhase::retry : JoinPhase::idle;
            break;
        case ApplicationActionCaptchaPhase::waiting:
        case ApplicationActionCaptchaPhase::captured:
            return;
        default:
            reset_value(&g_join);
            break;
        }
    } else {
        return;
    }
    if (open_now() && g_card->guild_id == guild)
        project();
}

bool application_guild_card_join_take_captcha(u64 guild_id) noexcept {
    if (!joining(guild_id) || g_join.phase != JoinPhase::retry)
        return false;
    g_join.phase = JoinPhase::requesting;
    return true;
}

void application_guild_card_synchronize(u64 now_ms) noexcept {
    application_guild_card_join_poll(now_ms);
    if (!open_now())
        return;
    auto& c = *g_card;
    if (!available() || c.scope != g_app.plugin_account_scope_serial) {
        application_guild_card_close();
        return;
    }
    if (c.phase == Phase::ready && c.scene.action != action())
        project();
    // The profile's games stay wanted while shown; their arrival re-projects the row.
    if (c.phase == Phase::ready) {
        for (u32 i = 0; i < c.profile.game_count; ++i)
            game_lookup_want(c.profile.games[i].id);
        if (c.profile.game_count && c.games_revision != game_lookup_revision())
            project();
    }
    // Without a brand colour (or a banner picture) the banner takes the icon's colour once it is resident.
    if (c.phase == Phase::ready && !c.profile.has_brand_color && !c.banner.url_bytes && c.icon_url_bytes &&
        !c.has_icon_color && renderer_media_dominant_color(&g_app.renderer, c.icon_key, &c.icon_color)) {
        c.has_icon_color = true;
        project();
    }
    // The accessibility saturation changed under an icon-coloured banner.
    if (c.phase == Phase::ready && c.has_icon_color && !c.profile.has_brand_color &&
        c.saturation != application_user_color_saturation())
        project();
    // "Play animated emoji" changed under the traits' emoji.
    if (c.phase == Phase::ready && c.animate_emoji != (message_rich_display().emoji == MessageMediaAnimation::always))
        project();
    if (ui_guild_card_tick(&c.scene, now_ms, application_reduced_motion()) ||
        ui_guild_card_next_wake(c.scene, application_reduced_motion()))
        application_request_frame();
}

UiGuildCardScene* application_guild_card_spinner_scene() noexcept {
    return open_now() && ui_guild_card_spinner_visible(g_card->scene) ? &g_card->scene : nullptr;
}

u32 application_guild_card_media(GuildCardMedia* output, u32 capacity) noexcept {
    if (!open_now() || g_card->phase != Phase::ready)
        return 0;
    u32 count = 0;
    if (count < capacity && g_card->icon_url_bytes)
        output[count++] = {{g_card->icon_url, g_card->icon_url_bytes}, g_card->icon_key};
    if (count < capacity && g_card->badge_url_bytes)
        output[count++] = {{g_card->badge_url, g_card->badge_url_bytes}, g_card->badge_key};
    if (count < capacity && g_card->banner.url_bytes)
        output[count++] = {g_card->banner.url_view(), g_card->banner.key};
    for (const auto& game : g_card->games)
        if (count < capacity && game.url_bytes)
            output[count++] = {game.url_view(), game.key};
    return count;
}

u32 application_guild_card_emoji_media(MediaAssetKey* output, u32 capacity) noexcept {
    if (!open_now() || g_card->phase != Phase::ready)
        return 0;
    const auto& s = g_card->scene;
    u32 count = 0;
    for (u32 i = 0; i < s.visible_traits && count < capacity; ++i)
        if (media_asset_key_valid(s.trait_emoji_keys[i]))
            output[count++] = s.trait_emoji_keys[i];
    return count;
}

bool application_guild_card_key_current(MediaAssetKey key) noexcept {
    if (!open_now() || g_card->phase != Phase::ready)
        return false;
    for (const auto& game : g_card->games)
        if (game.url_bytes && media_asset_key_equal(key, game.key))
            return true;
    return (g_card->icon_url_bytes && media_asset_key_equal(key, g_card->icon_key)) ||
           (g_card->badge_url_bytes && media_asset_key_equal(key, g_card->badge_key)) ||
           (g_card->banner.url_bytes && media_asset_key_equal(key, g_card->banner.key));
}

void application_guild_card_media_failed(MediaAssetKey key, const AccountMediaRequestResult& result) noexcept {
    if (!open_now() || g_card->phase != Phase::ready ||
        (result.state != AccountMediaRequestState::failed && result.state != AccountMediaRequestState::unsupported &&
         result.state != AccountMediaRequestState::invalid))
        return;
    auto& c = *g_card;
    const u8 bit = media_asset_key_equal(key, c.icon_key)       ? 1
                   : media_asset_key_equal(key, c.badge_key)    ? 2
                   : media_asset_key_equal(key, c.banner.key)   ? 4
                                                                : 0;
    if (!bit || (c.media_logged & bit))
        return;
    c.media_logged |= bit;
    log_u64(LogLevel::warning, "guild_card", "media_failed",
            u64{bit} | u64{static_cast<u8>(result.state)} << 8 | u64{static_cast<u8>(result.error)} << 16 |
                u64{static_cast<u8>(result.http_error)} << 24 | u64{static_cast<u8>(result.client_error)} << 32 |
                u64{static_cast<u8>(result.jpeg_error)} << 40 | u64{static_cast<u8>(result.webp_error)} << 48 |
                u64{static_cast<u8>(result.media_url_error)} << 56);
}

void application_guild_card_release() noexcept {
    reset_value(&g_join);
    if (!g_card)
        return;
    if (g_app.renderer.guild_card_scene == &g_card->scene)
        g_app.renderer.guild_card_scene = nullptr;
    pages_release(g_card);
    g_card = nullptr;
}
} // namespace maicord::app_detail
