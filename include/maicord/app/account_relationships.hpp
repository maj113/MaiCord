#pragma once

#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/relationships.hpp"

namespace maicord {
// Submit to the account's existing bounded REST owner. The transport copies
// target/body synchronously; no caller view, queue or model mutation is retained.
// Completion is consumed through the ordinary account rest_take mailbox.
RestClientSubmitResult account_relationship_submit(RestClient* rest,
    RelationshipOperation operation, u64 user_id, Utf8View username, u64 request_id) noexcept;
} // namespace maicord
