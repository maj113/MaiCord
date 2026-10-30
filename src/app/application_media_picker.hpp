#pragma once

#include "maicord/model/message_composer.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
struct RestClientResponse;

enum class UiMediaPickerKind : u8;
struct UiRect;

struct AccountMediaCatalogPreviewRequest;
struct MediaAssetKey;

struct GatewayClientEvent;
}

namespace maicord::app_detail {

bool application_media_picker_open(UiMediaPickerKind kind) noexcept;
// The composer's Stickers tab scrolled to a pack once it has loaded (the
// sticker popout's View More Stickers).
bool application_media_picker_open_sticker_pack(u64 pack_id) noexcept;
// The composer's inline /gif strip or /sticker list for query, above anchor
// (the commands popup, else the composer); it lasts while the command's option
// bar shows. Results: a 300 ms debounced GET
// /gifs/search (animated WebP, 20 results; a query answered this session
// shows again without a request), or the eight best sendable stickers. An
// empty query hides them; the open expression picker suppresses them.
bool application_media_picker_inline(UiMediaPickerKind kind, Utf8View query, UiRect anchor) noexcept;
void application_media_picker_inline_close() noexcept;
// While inline results show: Up/Down step the selection (wrapping), Enter
// sends the selected result and Tab inserts a GIF's link (sends a sticker).
// *handled is false when the key keeps its composer meaning.
bool application_media_picker_inline_key(u32 key, bool* handled) noexcept;
// The joined server whose session sticker list holds the sticker; zero when none.
u64 application_media_picker_sticker_server(u64 sticker_id) noexcept;
// The official client's reaction popout: the Emoji tab without the GIF/Sticker/Emoji
// selector, beside the anchor, adding the chosen emoji as a reaction to the
// message (Shift keeps it open). Opening on the open message toggles it shut.
bool application_media_picker_open_reaction(u64 message_id, UiRect anchor) noexcept;
// Reaction admission/status changed; refreshes an open reaction popout.
void application_media_picker_reaction_changed() noexcept;
void application_media_picker_close() noexcept;
// Also drops the session's server sticker lists.
void application_media_picker_release() noexcept;
// Keeps each server's sticker list (READY guilds, GUILD_CREATE,
// GUILD_STICKERS_UPDATE) for the Stickers tab; READY starts over.
void application_media_picker_gateway(const GatewayClientEvent&, Utf8View body) noexcept;
bool application_media_picker_active() noexcept;
bool application_media_picker_synchronize() noexcept;
void application_media_picker_favorites_changed() noexcept;
// Re-projects open emoji sections after a favorites/frecency snapshot.
void application_media_picker_emoji_preferences_changed() noexcept;
bool application_media_picker_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
bool application_media_picker_input(UiInputKind kind, u32 key, bool repeated) noexcept;
void application_media_picker_tick(u64 now_ms) noexcept;
u64 application_media_picker_next_wake() noexcept;
u64 application_media_picker_caret_wake() noexcept;

// The account's ordinary primary REST worker handles submission and completion.
// Closing cancels queued requests but keeps the in-flight request until it completes.
bool application_media_picker_pending(u64 now_ms) noexcept;
bool application_media_picker_submit(u64 request_id, u64 now_ms) noexcept;
bool application_media_picker_response(const RestClientResponse&, Utf8View body) noexcept;

MessageComposer* application_media_picker_edit_model() noexcept;
TextInputTarget application_media_picker_edit_target() noexcept;
UiRect application_media_picker_edit_bounds() noexcept;
bool application_media_picker_edit_focused() noexcept;
bool application_media_picker_edit_changed(bool content = true) noexcept;
void application_media_picker_edit_cancel_native_group(const TextInputTarget&) noexcept;

// Resolve only against the currently shown picker and its visible cells.
// Catalog keys never stand in for attachment IDs or provider URL hashes.
bool application_media_picker_preview_owner_current(MediaAssetKey key, u64 scope, u32 generation) noexcept;
bool application_media_picker_preview_current(MediaAssetKey key, u64 scope, u32 generation) noexcept;
bool application_media_picker_preview_request(u32 visible_cell,
                                              AccountMediaCatalogPreviewRequest* output) noexcept;
bool application_media_picker_preview_visible(MediaAssetKey key) noexcept;
bool application_media_picker_preview_rejected(MediaAssetKey key,u64 scope,u32 generation) noexcept;

} // namespace maicord::app_detail
