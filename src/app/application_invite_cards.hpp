#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord::app_detail {
// Invite cards under messages with invite links: resolves visible codes on
// the DETAILS lane through a lazily allocated session cache and routes the
// card's Join action to the native join dialog.

// Frame time: discovers visible invite links, maintains membership flags and
// refreshes card text when resolutions change.
void application_invite_cards_synchronize(u64 now_ms) noexcept;
// A click on an invite card widget; true when consumed.
bool application_invite_card_click(u64 message_id, UiWidgetHandle widget, float x, float y) noexcept;

bool application_invite_cards_pending(u64 now_ms) noexcept;
bool application_invite_cards_submit(u64 request_id, u64 now_ms) noexcept;
bool application_invite_cards_response(const RestClientResponse& response, Utf8View body) noexcept;
u64 application_invite_cards_next_wake() noexcept;
void application_invite_cards_release() noexcept;
} // namespace maicord::app_detail
