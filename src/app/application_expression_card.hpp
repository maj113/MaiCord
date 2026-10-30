#pragma once
#include "application_emoji_tooltip.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/media/asset.hpp"

#include "maicord/core/win32_types.hpp"

namespace maicord {
struct GuildRecord;
struct UiExpressionCardScene;
} // namespace maicord

namespace maicord::app_detail {
// The emoji and sticker popout, opened by clicking an
// emoji or a sticker in a message: beside it (right, else left), closed by
// Escape, the wheel, a click outside (which still acts) or a second click on
// its source.
// - Unicode emoji and flags: the emoji, ":name:" and "A default emoji. You can
//   use this emoji everywhere on Discord."
// - Custom emoji: the emoji, its name and the sentence for whether it
//   is from this server, one of yours, or an unknown one, with Nitro; a joined
//   source server below ("This emoji is from"), linked to open it.
// - Standard stickers: their name, "This is a <pack> sticker." (or "... They
//   are no longer available."), up to three of the pack's other stickers and
//   View More Stickers (the composer's Stickers tab at that pack).
// - Server stickers: the sticker and the server sentences as for emoji.
// Sticker definitions come from the joined servers' sticker lists, else
// GET /stickers/{id}; packs from GET /sticker-packs (kept for the session),
// else GET /sticker-packs/{id} for a retired pack.

// A press over the message view that may become a click: the release toggles
// the popout when it is over the same expression and no text was selected by
// a drag. Neither consumes the message (a press on an emoji starts selection).
void application_expression_card_pointer_down(UiPoint point) noexcept;
void application_expression_card_pointer_up(UiPoint point) noexcept;
bool application_expression_card_open(const MessageExpression& expression) noexcept;
void application_expression_card_close() noexcept;
// Whether the expression's popout is open (its tooltip then stays hidden).
bool application_expression_card_shows(const MessageExpression& expression) noexcept;
bool application_expression_card_contains(UiPoint point) noexcept;
// Pointer, keys and wheel while open; true when consumed.
bool application_expression_card_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
// Per frame: the source message still current, the entrance animation.
void application_expression_card_synchronize(u64 now_ms) noexcept;
// The primary request lane: the sticker, pack list or pack being resolved.
bool application_expression_card_pending(u64 now) noexcept;
bool application_expression_card_submit(u64 request_id, u64 now) noexcept;
bool application_expression_card_response(const RestClientResponse& response, Utf8View body) noexcept;
// The picture and pack stickers to fetch by key while shown; the joined
// server whose icon the popout shows, or null.
u32 application_expression_card_media(MediaAssetKey* output, u32 capacity) noexcept;
const GuildRecord* application_expression_card_guild() noexcept;
void application_expression_card_release() noexcept;
} // namespace maicord::app_detail
