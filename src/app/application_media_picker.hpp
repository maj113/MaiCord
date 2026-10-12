#pragma once

#include "maicord/app/account_media.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/input_router.hpp"

namespace maicord::app_detail {

struct ApplicationMediaPicker;

bool application_media_picker_open(UiMediaPickerKind kind) noexcept;
// The official client's reaction popout: the Emoji tab without the GIF/Sticker/Emoji
// selector, beside the anchor, adding the chosen emoji as a reaction to the
// message (Shift keeps it open). Opening on the open message toggles it shut.
bool application_media_picker_open_reaction(u64 message_id, UiRect anchor) noexcept;
// Reaction admission/status changed; refreshes an open reaction popout.
void application_media_picker_reaction_changed() noexcept;
void application_media_picker_close() noexcept;
void application_media_picker_release() noexcept;
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
u32 application_media_picker_owner_bytes() noexcept;

} // namespace maicord::app_detail
