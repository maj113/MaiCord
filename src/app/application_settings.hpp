#pragma once
#include "maicord/protocol/oauth.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/page_registry.hpp"
#include "maicord/app/plugin_host.hpp"
#include "maicord/ui/scroll_host.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
// Settings avatar rows: the cached user's avatar key and hash, or an empty
// record (initials only) when the user or their avatar is unknown.
UiSettingsAvatarMedia application_user_avatar_media(u64 user_id) noexcept;
// Server identity rows: the guild icon under the rail's key (same CDN asset
// and atlas entry), or an empty record (initials only) without an icon.
UiSettingsAvatarMedia application_guild_icon_media(u64 guild_id) noexcept;
// A user listed by a REST response (audit log users): their avatar hash, or
// the default avatar (embed/avatars/{n}.png; n is discriminator % 5 for
// legacy tags, else (id >> 22) % 6) under the profile card's key. The hash
// field then carries that CDN path.
UiSettingsAvatarMedia application_listed_user_avatar_media(u64 user_id, const Utf8View& avatar_hash,
                                                           u16 discriminator) noexcept;
void application_preferences_initialize() noexcept;
void application_preferences_apply() noexcept;
// Suspends media animations while reduced motion, a live resize, an inactive
// or a minimized window holds them; resumes them otherwise.
void application_animation_activity_synchronize() noexcept;
// WM_ACTIVATE: the official client pauses chat and identity animations while unfocused.
void application_animation_window_active(bool active) noexcept;
// The main window lost activation (WM_ACTIVATE); shown unactivated counts as active.
bool application_window_inactive() noexcept;
HANDLE application_preferences_notification() noexcept;
void application_preferences_poll() noexcept;
void application_preferences_shutdown() noexcept;
HANDLE application_media_auth_notification() noexcept;
void application_media_auth_poll() noexcept;
void application_media_auth_shutdown() noexcept;
bool application_settings_active() noexcept;
// Appearance, or search results that can show its controls, is selected.
bool application_settings_appearance_selected() noexcept;
// Monotonic owner lifetime, retained after release; never wraps or resets.
u64 application_settings_lifetime() noexcept;
bool application_preferences_member_visibility(bool visible) noexcept;
// The DM profile panel's remembered choice (dm_profile_panel.hpp).
bool application_preferences_dm_profile_visibility(bool visible) noexcept;
// Persists splitter widths once after a completed drag; true when queued.
bool application_preferences_persist_pane_widths() noexcept;
bool application_settings_commit_preferences() noexcept;
void application_settings_cancel_pointer() noexcept;
bool application_settings_open() noexcept;
// The profile editor hands the modal back on a category switch: Settings
// never visibly closed, so its entrance does not replay.
bool application_settings_resume() noexcept;
void application_settings_close(bool restore_focus = true) noexcept;
void application_settings_release() noexcept;
bool application_settings_synchronize() noexcept;
UiScrollBinding application_settings_scroll_binding(bool sidebar) noexcept;
bool application_settings_input(UiInputKind kind, u32 code, bool repeated) noexcept;
bool application_settings_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
void application_settings_choice_account_invalidated() noexcept;
// UI-thread contribution registry. Allocate lazily; unregister owner before
// unloading any borrowed descriptors. Registration alone does not open settings.
UiPageRegistry* application_settings_pages() noexcept;
NativePluginHost* application_settings_plugin_host() noexcept;
NativePluginHost* application_settings_plugin_host_current() noexcept;
bool application_settings_open_plugin_page(UiPageKey) noexcept;
// Opens Settings at a catalog panel or setting key ("appearance.theme"),
// scrolling to and flashing a setting.
bool application_settings_open_key(Utf8View key) noexcept;
// Opens Settings at a registered page (the profile editor hands back here).
bool application_settings_open_page(UiPageKey page) noexcept;
bool application_settings_focus_search() noexcept;
void application_settings_refresh_pages() noexcept;
} // namespace maicord::app_detail
