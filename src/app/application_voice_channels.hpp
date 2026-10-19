#pragma once
#include "maicord/app/gateway_client.hpp"
#include "maicord/ui/voice_channel_rows.hpp"

namespace maicord::app_detail {
// Voice channel extras in the channel list: each voice channel's status and
// session timer (server values from Request Channel Info and its updates),
// "Set a channel status" for the connected channel and "Invite to Voice".

// Gateway mailbox observer: sessions, CHANNEL_INFO and the start time and
// status updates, guild and channel deletes.
void application_voice_channels_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept;
// Voice cadence: asks for the selected guild's values once per session and
// follows the connected channel.
void application_voice_channels_pump() noexcept;
const UiVoiceChannelExtras& application_voice_channels_extras() noexcept;
// Channel list targets: the status line and the invite row. Coordinates are DIPs.
bool application_voice_channels_click(float x, float y) noexcept;
// Updates hover feedback; true over a clickable target.
bool application_voice_channels_hover(float x, float y) noexcept;
bool application_voice_channels_hand_cursor(float x, float y) noexcept;
// The status PUT's outcome (from the stage queue).
void application_voice_channels_status_result(u64 channel_id, bool ok, u32 status_code) noexcept;
void application_voice_channels_release() noexcept;
} // namespace maicord::app_detail
