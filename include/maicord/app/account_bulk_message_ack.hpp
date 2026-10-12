#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/bulk_message_ack.hpp"

namespace maicord {
inline constexpr u64 kBulkMessageAckRouteId = 0x42554C4B41434B01ull;
// Copies a typed batch into the PRIMARY REST worker's storage synchronously.
// Generic REST metadata responses follow the normal restriction policy.
// Null entries or a zero count cancels only this request ID; it never sends a body.
RestClientSubmitResult account_bulk_message_ack_submit(RestClient* primary,
                                                       const BulkMessageAckEntry* entries, u32 count,
                                                       u64 request_id) noexcept;
} // namespace maicord
