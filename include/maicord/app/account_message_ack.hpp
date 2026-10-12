#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/message_ack.hpp"

namespace maicord {
inline constexpr u64 kMessageAckRouteId = 0x4D534741434B0001ull;
// The caller builds validated ACK JSON in its existing response scratch. The
// REST owner copies it synchronously; no view or second large body is retained.
RestClientSubmitResult account_message_ack_submit(RestClient* rest,
    u64 channel_id, u64 message_id, ConstSlice<u8> body, u64 request_id) noexcept;
} // namespace maicord
