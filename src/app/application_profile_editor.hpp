#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/model/profile_editor.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/profile_dialog_scene.hpp"
#include "maicord/ui/profile_scene.hpp"

namespace maicord::app_detail {
// User Settings > Profiles: the official client's profile editor ("Main Profile" and
// "Per-server Profiles" tabs) with the live preview card, the image cropper
// and the decoration/effect pickers. It presents as the settings modal with
// the Profiles page selected; other pages hand back to the settings host.
// Requests use the DETAILS lane through application_channel_overview, which
// forwards to this owner while it is open.

// Opens the editor on a tab (guild: that server, or the first one when zero);
// false when it cannot.
bool application_profile_editor_open(ProfileEditorTab tab, u64 guild_id = 0) noexcept;

// Settings-modal host contract (see application_channel_overview).
bool application_profile_editor_active() noexcept;
void application_profile_editor_close(bool restore_focus = true) noexcept;
void application_profile_editor_release() noexcept;
bool application_profile_editor_synchronize() noexcept;
bool application_profile_editor_input(UiInputKind, u32, bool) noexcept;
struct SettingsLayer;
SettingsLayer* application_profile_editor_settings_layer() noexcept;
bool application_profile_editor_window_message(UINT, WPARAM, LPARAM) noexcept;
bool application_profile_editor_submit(u64 now) noexcept;
bool application_profile_editor_owns_response(u64 request_id) noexcept;
bool application_profile_editor_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_profile_editor_next_wake() noexcept;
u64 application_profile_editor_caret_wake() noexcept;
void application_profile_editor_tick(u64 now) noexcept;
bool application_profile_editor_edit_focused() noexcept;
MessageComposer* application_profile_editor_edit_model() noexcept;
TextInputTarget application_profile_editor_edit_target() noexcept;
void application_profile_editor_edit_cancel_native_group(TextInputTarget) noexcept;
UiRect application_profile_editor_edit_bounds() noexcept;
bool application_profile_editor_edit_changed(bool content) noexcept;
u32 application_profile_editor_owner_bytes() noexcept;
// Local preview frames of picked images (the media pump routes them here).
bool application_profile_editor_frame_current(MediaAssetKey key) noexcept;
void application_profile_editor_frame_uploaded(MediaAssetKey key) noexcept;
void application_profile_editor_frame_rejected(MediaAssetKey key) noexcept;
// The preview card and the dialog while they show (media requests).
ProfileScene* application_profile_editor_preview() noexcept;
const UiProfileDialogScene* application_profile_editor_dialog() noexcept;
} // namespace maicord::app_detail
