#include "maicord/app/account_relationships.hpp"

namespace maicord {
RestClientSubmitResult account_relationship_submit(RestClient* rest,
    RelationshipOperation operation, u64 user_id, Utf8View username, u64 request_id) noexcept {
    if (!rest || !request_id) return {RestClientSubmitError::invalid_input};
    u8 target[96]{}, body[256]{};
    const auto path = write_relationship_target(operation, user_id, {target, sizeof(target)});
    if (!path.ok()) return {path.error == RelationshipWriteError::output_too_small ?
        RestClientSubmitError::target_capacity : RestClientSubmitError::invalid_input};
    const auto json = write_relationship_body(operation, username, {body, sizeof(body)});
    if (!json.ok()) return {json.error == RelationshipWriteError::output_too_small ?
        RestClientSubmitError::body_capacity : RestClientSubmitError::invalid_input};
    return rest_client_submit(rest, {target, path.bytes}, {body, json.bytes},
        {kRelationshipsRouteId, 0, relationship_method(operation), RestMajorParameter::none},
        request_id, json.bytes != 0);
}
} // namespace maicord
