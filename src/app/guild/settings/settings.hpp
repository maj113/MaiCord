#pragma once
#include "maicord/app/net/rest_client.hpp"
namespace maicord { struct GatewayClientEvent; }
#include "maicord/protocol/guild/assets.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/chat/unread_banner_scene.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/media/core/asset_key.hpp"

namespace maicord::app_detail {
// Server Settings: a full-window layer over the shell with permission-gated
// pages. Shares the channel settings lane and text input routing through
// application_channel_overview, which forwards while this layer is active.
// Reads a picked image file whole into a fresh VirtualAlloc allocation the caller frees; on failure
// *error says why (a file over the source limit names its size, written to `error_storage`).
bool application_image_file_read(const ConstSlice<wchar_t>& path, u8** data, u32* bytes, Utf8View* error,
                                 Slice<char> error_storage) noexcept;
// Probes a picked image for `profile` the way the account module prepares it at submit; `info` says why it fails.
// A `data_uri` buffer, sized by guild_image_data_uri_bytes, receives the prepared image's data URI.
bool application_guild_image_probe(GuildImageProfile profile, ConstSlice<u8> image, GuildImageInfo* info,
                                   Slice<u8> data_uri = {}) noexcept;
// Why an image failed its probe, as a sentence for that use. The picked file at `path` is `bytes` long; a
// too_large failure over the use's limit names it and its size, written to `storage` (which the result views).
Utf8View application_guild_image_error_text(GuildImageProfile profile, GuildImageError error, ConstSlice<wchar_t> path,
                                            u64 bytes, Slice<char> storage) noexcept;
bool application_guild_settings_available(u64 guild_id) noexcept;
// page: 0 opens the first permitted page, else a submenu page value (falls
// back to the first permitted page when that one is not allowed).
bool application_guild_settings_open(u64 guild_id, u8 page = 0) noexcept;
// The server menu's Server Settings submenu, in sidebar order: each permitted
// page as a row with action = action_base + its page value.
inline constexpr u32 kGuildSettingsPageCount = 18;
u32 application_guild_settings_page_items(u64 guild_id, u32 action_base, Slice<PopupItem> output) noexcept;
bool application_guild_settings_active() noexcept;
void application_guild_settings_close(bool restore_focus = true) noexcept;
void application_guild_settings_release() noexcept;
bool application_guild_settings_synchronize() noexcept;
// Inline editor engine while the page is open (accessibility SetValue).
struct SettingsLayer;
SettingsLayer* application_guild_settings_settings_layer() noexcept;
bool application_guild_settings_submit(u64 now) noexcept;
bool application_guild_settings_owns_response(u64 request_id) noexcept;
bool application_guild_settings_response(const RestClientResponse&, Utf8View) noexcept;
void application_guild_settings_gateway(const GatewayClientEvent&, const Utf8View&) noexcept;
u64 application_guild_settings_next_wake() noexcept;
u64 application_guild_settings_caret_wake() noexcept;
void application_guild_settings_tick(u64 now) noexcept;
// Test builds only: opens the page MAICORD_TEST_GUILD_SETTINGS names (its sidebar label) once the account is ready.
void application_guild_settings_test_open() noexcept;
// Signalled when an image file dialog completes; null while none is open.
HANDLE application_guild_settings_notification() noexcept;
// A bulk ban whose request failed: the Members page clears its selection.
void application_guild_settings_bulk_ban_failed(u64 guild_id) noexcept;
// The Server Profile preview card's pictures fetched by URL (its game icons
// and a discoverable server's custom banner), from index 0; false after the
// last. A key is current while the preview shows it.
bool application_guild_settings_preview_media(u32 index, MediaAssetKey* key, Utf8View* url) noexcept;
bool application_guild_settings_preview_key_current(const MediaAssetKey& key) noexcept;
// Opens the name form for a new channel from the server or category menu.
bool application_guild_settings_create_channel(u64 guild_id, u64 category_id, u8 type) noexcept;
// View Server as Role (chat_state_store_view_as_roles): start closes Server
// Settings and lands on a channel the role can see; end restores the
// account's permissions, optionally reopening Roles. The notice is the
// banner over a guild timeline while its view lasts (zero id otherwise);
// its title is written to `text`.
bool application_view_as_role_start(u64 guild_id, u64 role_id) noexcept;
bool application_view_as_role_end(u64 guild_id, bool back_to_settings) noexcept;
UiBannerNotice application_view_as_role_notice(u64 channel_id, Slice<char> text) noexcept;
} // namespace maicord::app_detail
