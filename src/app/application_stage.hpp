#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/model/stage_instance_store.hpp"
#include "maicord/ui/call_scene.hpp"

namespace maicord::app_detail {
// Stage channels on the client: live stage instances (topic, privacy), the
// local user's role and actions, the call view's stage projection and the
// stage REST requests (DETAILS lane, one at a time, in order).

// Gateway mailbox observer: READY, stage instance snapshots, guild deletes.
void application_stage_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept;
const StageInstanceStore& application_stage_instances() noexcept;
bool application_stage_channel(u64 channel_id) noexcept;
// Speakers are the call view's tiles; everyone else is the audience.
bool application_stage_speaker(u32 voice_flags) noexcept;
// Stage content for the call view of channel_id (inactive for other channels).
UiCallStage application_stage_content(u64 channel_id) noexcept;
// Stage control from the call view (user_id for per-row/per-tile controls).
bool application_stage_activate(u64 channel_id, UiCallControl control, u64 user_id) noexcept;

// Voice cadence: an invitation to speak opens the call view on its banner.
void application_stage_pump() noexcept;

// DETAILS lane.
bool application_stage_pending(u64 now_ms) noexcept;
bool application_stage_submit(u64 request_id, u64 now_ms) noexcept;
bool application_stage_response(const RestClientResponse& response, Utf8View body) noexcept;
// lane_idle: no DETAILS request is in flight (queued requests wake only then).
u64 application_stage_next_wake(bool lane_idle) noexcept;
void application_stage_release() noexcept;
} // namespace maicord::app_detail
