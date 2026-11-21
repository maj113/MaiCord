#pragma once

#include "maicord/app/net/rest_client.hpp"
#include "maicord/protocol/user/relationships.hpp"

namespace maicord {
// Submit to the account's existing bounded REST owner. The transport copies
// target/body/header synchronously; no caller view, queue or model mutation is retained.
// Completion is consumed through the ordinary account rest_take mailbox. `location` adds the
// X-Context-Properties header (write_relationship_context).
RestClientSubmitResult account_relationship_submit(RestClient* rest,
    RelationshipOperation operation, u64 user_id, Utf8View username, u64 request_id,
    RelationshipLocation location = RelationshipLocation::none) noexcept;
} // namespace maicord
