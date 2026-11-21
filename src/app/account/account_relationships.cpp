#include "maicord/app/account/account_relationships.hpp"

namespace maicord {
RestClientSubmitResult account_relationship_submit(RestClient* rest,
    RelationshipOperation operation, u64 user_id, Utf8View username, u64 request_id,
    RelationshipLocation location) noexcept {
    if (!rest || !request_id) return {RestClientSubmitError::invalid_input};
    u8 target[96], body[kRelationshipBodyMaximumBytes], context[96];
    const auto path = write_relationship_target(operation, user_id, {target, sizeof(target)});
    if (!path.ok()) return {path.error == RelationshipWriteError::output_too_small ?
        RestClientSubmitError::target_capacity : RestClientSubmitError::invalid_input};
    const auto json = write_relationship_body(operation, username, {body, sizeof(body)});
    if (!json.ok()) return {json.error == RelationshipWriteError::output_too_small ?
        RestClientSubmitError::body_capacity : RestClientSubmitError::invalid_input};
    const auto header = write_relationship_context(location, {context, sizeof(context)});
    if (!header.ok()) return {RestClientSubmitError::invalid_input};
    RestClientRequest request{{target, path.bytes}, {body, json.bytes},
        {kRelationshipsRouteId, 0, relationship_method(operation), RestMajorParameter::none},
        request_id, kRestClientDefaultTimeoutMs, json.bytes != 0};
    request.extra_headers = {context, header.bytes};
    // A full list (1,000 friends, 5,000 blocked users and the pending requests) outgrows the ordinary response
    // capacity. It may take as much as the decoder accepts.
    static_assert(kRelationshipsMaximumBytes <= kRestClientMaximumLargeResponseBytes);
    return operation == RelationshipOperation::list
        ? rest_client_submit_large_response(rest, request, kRelationshipsMaximumBytes)
        : rest_client_submit(rest, request);
}
} // namespace maicord
