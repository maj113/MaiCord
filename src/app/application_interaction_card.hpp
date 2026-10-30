#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/ui/tooltip.hpp"

namespace maicord::app_detail {
// The popout of a slash command chip: click the chip of "<user> used <command>"
// and a card above it reads "/command option: value ..." (the interaction's
// command with the options it was run with). The
// card opens at once with "/command"; GET .../interaction-data fills in the
// options. The card is the pinned tooltip: the tooltip host shows it, and it
// closes with a press or wheel anywhere (a second click on the chip toggles
// it), Escape, or when the message leaves the view.
bool application_interaction_card_toggle(u64 message_id) noexcept;
void application_interaction_card_close() noexcept;
bool application_interaction_card_open() noexcept;
// A pointer press or wheel: closes the card; the press on the chip that was
// open then does not reopen it.
void application_interaction_card_press() noexcept;
// The pinned tooltip while open: the chip's current rectangle and the text.
bool application_interaction_card_target(u64 scope, UiTooltipTarget* target, UiTooltipStyle* style) noexcept;
// The interaction-data request lane.
bool application_interaction_card_pending(u64 now) noexcept;
bool application_interaction_card_submit(u64 request_id, u64 now) noexcept;
bool application_interaction_card_response(const RestClientResponse& response, Utf8View body) noexcept;
} // namespace maicord::app_detail
