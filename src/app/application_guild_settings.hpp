#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Server Settings: a full-window layer over the shell with permission-gated
// pages. Shares the channel settings lane and text input routing through
// application_channel_overview, which forwards while this layer is active.
// Reads a picked image file whole into a fresh VirtualAlloc allocation the caller frees; on failure
// *error says why.
bool application_image_file_read(ConstSlice<wchar_t> path, u8** data, u32* bytes, Utf8View* error) noexcept;
bool application_guild_settings_available(u64 guild_id) noexcept;
// page: 0 opens the first permitted page, else a submenu page value (falls
// back to the first permitted page when that one is not allowed).
bool application_guild_settings_open(u64 guild_id, u8 page = 0) noexcept;
// The server menu's Server Settings submenu, in sidebar order: each permitted
// page as a row with action = action_base + its page value.
inline constexpr u32 kGuildSettingsPageCount = 12;
u32 application_guild_settings_page_items(u64 guild_id, u32 action_base, Slice<PopupItem> output) noexcept;
bool application_guild_settings_active() noexcept;
void application_guild_settings_close(bool restore_focus = true) noexcept;
void application_guild_settings_release() noexcept;
bool application_guild_settings_synchronize() noexcept;
bool application_guild_settings_input(UiInputKind, u32, bool) noexcept;
// Inline editor engine while the page is open (accessibility SetValue).
struct SettingsLayer;
SettingsLayer* application_guild_settings_settings_layer() noexcept;
bool application_guild_settings_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_guild_settings_submit(u64 now) noexcept;
bool application_guild_settings_owns_response(u64 request_id) noexcept;
bool application_guild_settings_response(const RestClientResponse&, Utf8View) noexcept;
void application_guild_settings_gateway(const GatewayClientEvent&, Utf8View) noexcept;
u64 application_guild_settings_next_wake() noexcept;
u64 application_guild_settings_caret_wake() noexcept;
void application_guild_settings_tick(u64 now) noexcept;
// Signalled when an image file dialog completes; null while none is open.
HANDLE application_guild_settings_notification() noexcept;
bool application_guild_settings_edit_focused() noexcept;
MessageComposer* application_guild_settings_edit_model() noexcept;
TextInputTarget application_guild_settings_edit_target() noexcept;
void application_guild_settings_edit_cancel_native_group(TextInputTarget) noexcept;
UiRect application_guild_settings_edit_bounds() noexcept;
bool application_guild_settings_edit_changed(bool content) noexcept;
u32 application_guild_settings_owner_bytes() noexcept;
// A bulk ban whose request failed: the Members page clears its selection.
void application_guild_settings_bulk_ban_failed(u64 guild_id) noexcept;
// Opens the name form for a new channel from the server or category menu.
bool application_guild_settings_create_channel(u64 guild_id, u64 category_id, u8 type) noexcept;
} // namespace maicord::app_detail
