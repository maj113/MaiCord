#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord::app_detail {
// Message components (buttons, selects) and the modals they open. One
// interaction is in flight at a time on the DETAILS lane; its control shows a
// loading state until the Gateway reports success or failure.

// A click on a component widget of the main message scene. Returns true when
// the widget belongs to a component (consumed, even if inert).
bool application_message_component_click(u64 message_id, UiWidgetHandle widget, UiRect anchor) noexcept;
// Popup selections for an open select menu.
bool application_message_components_popup_action(u32 action) noexcept;
// Gateway interaction_* dispatches (payload is the verbatim event object).
void application_message_components_gateway(GatewayClientDispatchKind kind, Utf8View body) noexcept;
// A message update/create resolves a deferred component response.
void application_message_components_message(u64 message_id) noexcept;

// DETAILS lane.
bool application_message_components_pending(u64 now_ms) noexcept;
bool application_message_components_submit(u64 request_id, u64 now_ms) noexcept;
bool application_message_components_response(const RestClientResponse& response, Utf8View body) noexcept;
void application_message_components_tick(u64 now_ms) noexcept;
u64 application_message_components_next_wake() noexcept;
void application_message_components_release() noexcept;

// Frame time: age-restricted channels show their content only after the
// "Age-restricted channel" confirmation, remembered per server for this
// session and account (the official client likewise asks again after a restart).
void application_age_gate_synchronize() noexcept;
} // namespace maicord::app_detail
