#pragma once

#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/reaction_users.hpp"

namespace maicord {
inline constexpr u64 kPollVoteRouteId = 0x504f4c4c564f5445ull;
inline constexpr u64 kPollVotersRouteId = 0x504f4c4c55534552ull;
inline constexpr u64 kPollMessageRouteId = 0x504f4c4c4d534747ull;
inline constexpr u32 kPollMaximumTargetBytes = 256;
inline constexpr u32 kPollMaximumRequestBodyBytes = 160;
inline constexpr u32 kPollMaximumResponseBytes = 2u*1024u*1024u;
enum class PollOperation : u8 { refresh, vote, remove_vote, voters };
// All request identity and answer IDs are values copied by the existing REST
// owner before publication. Clear vote uses PUT with an empty answer_ids array.
struct PollRequest {
    u64 channel_id{}, message_id{}, after_user_id{};
    u32 answer_ids[kGatewayMessagePollMaximumAnswers]{};
    u32 answer_id{}, limit{};
    u8 answer_count{};
    PollOperation operation{};
    u8 reserved[6]{};
};
enum class PollWriteError : u8 { none, invalid_input, output_capacity };
struct PollWriteResult {
    u64 route_id{};
    u32 target_bytes{}, body_bytes{};
    RestMethod method{};
    PollWriteError error{};
    bool ok() const noexcept { return error==PollWriteError::none; }
};
PollWriteResult write_poll_request(const PollRequest&,
    Slice<u8> target, Slice<u8> body) noexcept;
enum class PollDecodeError : u8 { none, invalid_input, scan, json, identity, poll, users };
struct PollMessageDecodeResult {
    GatewayEventDecodeResult poll{};
    u32 text_bytes{}, error_offset{};
    PollDecodeError error{};
    JsonScanError scan_error{};
    JsonReadError read_error{};
    bool ok() const noexcept { return error==PollDecodeError::none; }
};
// Validates a complete single-message identity and decodes only its poll. It
// never rewrites unrelated resident message content from a possibly stale GET.
// Poll text borrows caller decoded text storage, never HTTP response bytes.
PollMessageDecodeResult decode_poll_message(Utf8View input,
    u64 expected_channel_id, u64 expected_message_id, Slice<u32> structural_scratch,
    u32 maximum_depth, Slice<char> text_output, const GatewayMessagePollScratch&,
    GatewayMessagePoll* output) noexcept;
struct PollVotersDecodeResult {
    ReactionUsersDecodeResult users{};
    u32 error_offset{};
    PollDecodeError error{};
    JsonScanError scan_error{};
    JsonReadError read_error{};
    bool ok() const noexcept { return error==PollDecodeError::none; }
};
// Poll voter responses are an object with a required users array, unlike the
// reaction endpoint's bare array. Shared user decoding retains bounded fields.
PollVotersDecodeResult decode_poll_voters(Utf8View input,
    Slice<u32> page_structural_scratch, Slice<u32> object_structural_scratch,
    u32 maximum_depth, Slice<ReactionUser>, Slice<char> text_output) noexcept;
static_assert(sizeof(PollRequest)==80);
} // namespace maicord
