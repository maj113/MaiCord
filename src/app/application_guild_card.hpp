#pragma once
#include "maicord/app/account_media.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/media/asset.hpp"
#include "maicord/ui/geometry.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
struct UiGuildCardScene;
}

namespace maicord::app_detail {
// Server tag popout: a server tag chip (message author line, profile card)
// opens the server profile popout beside it, filled from
// GET /guilds/{id}/profile: icon, name, tag and badge, online and member
// counts, founding month, description and traits, and Go to Server (member)
// or Join (an invite seen in chat, or a discoverable server). A limited
// profile (50001) shows "Private Server"; other failures retry on click.
bool application_guild_card_open(u64 guild_id, UiRect anchor) noexcept;
void application_guild_card_close() noexcept;
// The community lane's answer for the card: its profile, or its join.
void application_guild_card_response(u64 guild_id, bool join, const RestClientResponse& response,
                                     Utf8View body) noexcept;
// Pointer, wheel and Escape while open; true when consumed.
bool application_guild_card_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// Per frame: entrance, spinner phase, membership (a finished join navigates).
void application_guild_card_synchronize(u64 now_ms) noexcept;
// The popout's Join, which outlives the popout: the official client's lurking join
// (PUT /guilds/{id}/members/@me?lurker=true&session_id=...), then the
// server's GUILD_CREATE opens it. A CAPTCHA answer opens the challenge in the
// browser and its solved token arms one retry. Polled per frame (the server's
// arrival and the return from the browser repaint) and from the community lane.
void application_guild_card_join_poll(u64 now_ms) noexcept;
// True once for the join retry the solved CAPTCHA must arm.
bool application_guild_card_join_take_captcha(u64 guild_id) noexcept;
// Paint-only spinner phase while loading; null otherwise.
UiGuildCardScene* application_guild_card_spinner_scene() noexcept;
// The icon and tag badge (CDN URLs) while shown.
struct GuildCardMedia {
    Utf8View url; // Borrowed until the next guild card call.
    MediaAssetKey key;
};
u32 application_guild_card_media(GuildCardMedia* output, u32 capacity) noexcept;
bool application_guild_card_key_current(MediaAssetKey key) noexcept;
// A refused or failed fetch of the icon, badge or banner: logged once per
// opening with its state and codec/HTTP errors (the banner is a JPEG).
void application_guild_card_media_failed(MediaAssetKey key, const AccountMediaRequestResult& result) noexcept;
void application_guild_card_release() noexcept;
} // namespace maicord::app_detail
