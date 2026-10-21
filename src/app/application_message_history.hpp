#pragma once

#include "account_ui_runtime.hpp"
#include "maicord/app/rest_client.hpp"

namespace maicord::app_detail {

// Shared production catch-up boundary: admit all writable decode storage before
// the REST bridge can change catch-up, timeline, users or members. Empty/error
// responses retain the bridge's existing classification without committing VM.
inline bool application_message_history_catchup_response(AccountUiRuntime& ui,
    MessageHistoryCatchup& catchup, MessageEventPump& pump,
    const RestClientResponse& response, Utf8View body, u64 now_ms,
    MessageHistoryRestResult* accepted, MessageHistoryDecodeScratch* decoded_scratch) noexcept {
    if (!accepted || !decoded_scratch) return false;
    MessageHistoryDecodeScratch scratch{};
    if (response.transport_ok() && response.status_code == 200 && body.data && body.bytes &&
        body.bytes == response.body_bytes && !application_message_decode_scratch(ui, &scratch, body)) return false;
    const auto result = message_history_catchup_accept_rest(&catchup, &pump, response, body, scratch,
        MessageContentAvailability::unknown, now_ms);
    *accepted = result;
    *decoded_scratch = scratch;
    return true;
}

bool pump_message_history() noexcept;
// Actor/lifecycle reset of the reaction-only PRIMARY admission turn.
void application_message_history_reset_reaction_turn() noexcept;
bool scroll_to_resident_message(u64 message_id) noexcept;

} // namespace maicord::app_detail
