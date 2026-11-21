#pragma once
#include "maicord/app/net/rest_client.hpp"
#include "maicord/protocol/guild/community.hpp"

namespace maicord {
struct NavigationView;
}

namespace maicord::app_detail {
struct SettingsLayer;
// The hub's widget columns in a page `width` DIPs wide: the main column and the side column (a third), 16 apart.
// Under 640 DIPs the columns stack and each is the page's full width.
struct GuildHubColumns {
    float main, side;
};
inline GuildHubColumns guild_hub_columns(float width) noexcept {
    if (width < 640)
        return {width, width};
    const float side = (width - 16) / 3;
    return {width - side - 16, side};
}
// The Popular Games covers in a card `width` DIPs wide, as the official client lays them out: as many as fit at
// 96 DIPs or more each, 12 apart inside the card's padding of 16. At least one, at most kGuildSpaceGames.
struct GuildHubCovers {
    u32 count;
    float cell;
};
inline GuildHubCovers guild_hub_covers(float width) noexcept {
    const float inner = width - 32;
    const float fit = (inner + 12) / 108;
    const u32 count = fit < 1 ? 1 : fit >= kGuildSpaceGames ? kGuildSpaceGames : static_cast<u32>(fit);
    const float cell = (inner - 12.f * static_cast<float>(count - 1)) / static_cast<float>(count);
    return {count, cell > 1 ? cell : 1};
}
// The Server Hub (the official client's guild space) and the Boost Goal row. A server shows the hub when its guild
// experiment and hub settings allow it (guild_space_visible). The hub is a content page (application_content_page)
// in place of the chat column and the member list, opened from the hub's row in the channel list (the official
// client's /channels/{g}/guild-space route). Each opening fetches GET /guilds/{g}/space, then
// GET /guilds/{g}/space/widgets/hydrate for the widgets that need data, as the official client does each time the
// page mounts. Each opening also fetches the account's consents (GET /users/@me/consent): the official client reads
// them once a session, but the account may change personalization elsewhere meanwhile. Requests use the
// community lane (application_guild_community_hub). A server whose hub was showing when another server was chosen
// shows it again when chosen again, until a channel of it is chosen (for the session).

// The channel list's Boost Goal and Server Hub rows for the selected server.
void application_guild_hub_navigation(NavigationView*) noexcept;
bool application_guild_hub_open(u64 guild_id) noexcept;
// The hub was the server's last view.
bool application_guild_hub_remembered(u64 guild_id) noexcept;
// A channel of the server was chosen: the hub is no longer its last view.
void application_guild_hub_forget(u64 guild_id) noexcept;
// Appends the members the open hub of `guild_id` shows (leaders, the account, players) after ids[count], up to
// `capacity`, and returns the new count: the visible-author hydration pins them and asks the Gateway for those it
// does not know, as the official client does.
u32 application_guild_hub_people(u64 guild_id, u64* ids, u32 count, u32 capacity) noexcept;
// The hydrate request's widget ids, joined by "&widget_ids=".
Utf8View application_guild_hub_query() noexcept;
// The answer to one of the hub's requests (`ok` for a 2xx status).
void application_guild_hub_response(GuildCommunityOperation, u64 guild_id, bool ok, u16 status,
                                    Utf8View body) noexcept;
// The account's teardown: closes the page and frees the hub.
void application_guild_hub_release() noexcept;
// The Activity Sharing dialog the hub's buttons open (a settings modal of the channel overview's host): the
// switches of activity sharing, of sharing in the server (privacy.activity_restricted_guild_ids) and of
// personalization (POST /users/@me/consent, as the official client sends it).
bool application_guild_hub_sharing_active() noexcept;
void application_guild_hub_sharing_close(bool restore_focus) noexcept;
void application_guild_hub_sharing_release() noexcept;
bool application_guild_hub_sharing_synchronize() noexcept;
SettingsLayer* application_guild_hub_sharing_settings_layer() noexcept;
u64 application_guild_hub_sharing_caret_wake() noexcept;
} // namespace maicord::app_detail
