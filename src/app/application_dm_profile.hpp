#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/media/asset_key.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/dm_profile_panel.hpp"
#include "maicord/ui/profile_scene.hpp"

namespace maicord::app_detail {
// The DM profile panel: the recipient's profile (ProfilePresentation::panel)
// in the side slot of a one-to-one DM, shown while the preference is on and
// the window is at least kUiDmProfilePanelMinimumWindowWidth wide. Profiles
// come from GET /users/{id}/profile, one request in flight, kept fresh for a
// minute after it ends (refused or rate-limited users wait the same minute);
// a fresh popout profile of the same user is reused instead of fetched.

// The side slot's plan inputs for the selected conversation.
UiSidePanelInput application_side_panel_input() noexcept;
// Per frame, after the shell layout: shows, hides or lays out the panel in
// the slot and publishes it to the renderer (renderer.dm_profile_panel, which
// media, tooltips and wheel scrolling read).
void application_dm_profile_synchronize() noexcept;
// Loop tick: advances shown activity timers at their second boundary.
void application_dm_profile_tick(u64 now) noexcept;
// The shown panel's scene, or null.
ProfileScene* application_dm_profile_scene() noexcept;
bool application_dm_profile_pending(u64 now) noexcept;
bool application_dm_profile_submit(u64 request_id, u64 now) noexcept;
bool application_dm_profile_response(const RestClientResponse& response, Utf8View body) noexcept;
// Pointer input inside the panel (content DIPs); false elsewhere.
bool application_dm_profile_hover(UiPoint point) noexcept;
bool application_dm_profile_click(UiPoint point) noexcept;
void application_dm_profile_release() noexcept;
} // namespace maicord::app_detail
