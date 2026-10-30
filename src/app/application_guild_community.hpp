#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/model/scheduled_event_store.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
struct NavigationView;
}

namespace maicord::app_detail {
// Community features of a server: the rules form (membership screening),
// onboarding questions with "Channels & Roles", and scheduled events (the
// events store, the channel-list entry, list/detail/create modals, Interested
// and event cards). Modals run in the shared settings-modal framework and are
// hosted by application_channel_overview like the server lifecycle modals;
// requests use the DETAILS lane through that host.

// Gateway: READY resets, guild snapshots replace, dispatches update.
void application_guild_community_gateway(const GatewayClientEvent&, ConstSlice<u8> payload) noexcept;
// Null until the first event arrives.
const ScheduledEventStore* application_guild_community_events() noexcept;
// Host projection for the channel list's events row.
void application_guild_community_navigation(NavigationView*) noexcept;

// The selected server holds the composer until its rules are accepted.
bool application_guild_community_composer_gated() noexcept;
// Composer placeholder while gated; empty otherwise.
Utf8View application_guild_community_composer_notice() noexcept;
// A press in a gated composer opens the rules instead; true when consumed.
bool application_guild_community_composer_press() noexcept;
// After joining (invite flow) or selecting a server: rules or onboarding.
void application_guild_community_joined(u64 guild_id) noexcept;
// The server tag popout's requests on this lane: GET /guilds/{id}/profile and
// the join of a discoverable server; answers go to the guild card.
bool application_guild_community_profile(u64 guild_id) noexcept;
bool application_guild_community_join(u64 guild_id) noexcept;

bool application_guild_community_open_rules(u64 guild_id) noexcept;
bool application_guild_community_open_onboarding(u64 guild_id, bool revisit) noexcept;
bool application_guild_community_open_events(u64 guild_id) noexcept;
bool application_guild_community_open_event(u64 guild_id, u64 event_id) noexcept;
bool application_guild_community_open_create(u64 guild_id) noexcept;
// Server menu rows: Channels & Roles and Create event, where they apply.
u32 application_guild_community_menu_items(u64 guild_id, u32 onboarding_action, u32 create_event_action,
                                           Slice<PopupItem> output) noexcept;
bool application_guild_community_can_create(u64 guild_id) noexcept;
// Fetches a linked event the store does not know (event cards).
void application_guild_community_want_event(u64 guild_id, u64 event_id) noexcept;
void application_guild_community_frame() noexcept;

// Settings-modal host contract (see application_channel_overview).
bool application_guild_community_active() noexcept;
void application_guild_community_close(bool restore_focus = true) noexcept;
void application_guild_community_release() noexcept;
bool application_guild_community_synchronize() noexcept;
struct SettingsLayer;
SettingsLayer* application_guild_community_settings_layer() noexcept;
bool application_guild_community_submit(u64 now) noexcept;
bool application_guild_community_owns_response(u64 request_id) noexcept;
bool application_guild_community_response(const RestClientResponse&, Utf8View) noexcept;
u64 application_guild_community_next_wake() noexcept;
u64 application_guild_community_caret_wake() noexcept;
u32 application_guild_community_owner_bytes() noexcept;

} // namespace maicord::app_detail
