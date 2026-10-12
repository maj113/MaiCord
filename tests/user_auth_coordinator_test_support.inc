// Shared helpers for the user-auth coordinator tests. Include inside the test
// namespace after its `fixture` object, which must provide `response`,
// `metadata` and `ready` members for the synchronous REST boundary.
u32 failures{};
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
bool body(Utf8View value) noexcept {
    return secure_buffer_assign(&fixture.response, {reinterpret_cast<const u8*>(value.data), value.bytes}).ok();
}
void response(u16 status, u64 request_id, u64 retry_at = 0,
              RestClientError error = RestClientError::none) noexcept {
    fixture.metadata = {}; fixture.metadata.status_code = status; fixture.metadata.request_id = request_id;
    fixture.metadata.retry_at_ms = retry_at; fixture.metadata.error = error;
    fixture.metadata.body_bytes = fixture.response.bytes; fixture.ready = true;
}
bool erased(const UserAuthCoordinatorRuntime& runtime) noexcept {
    for (u32 i = 0; i < runtime.secrets.capacity; ++i) if (runtime.secrets.data[i]) return false;
    return !runtime.retained_request_bytes && !runtime.token_bytes && !runtime.ticket_bytes;
}
bool same_metadata(const UserAuthCoordinatorResult& a, const UserAuthCoordinatorResult& b) noexcept {
    return a.generation == b.generation && a.request_id == b.request_id && a.retry_at_ms == b.retry_at_ms &&
        a.native_error == b.native_error && a.response_bytes == b.response_bytes && a.decode_error_offset == b.decode_error_offset &&
        a.http_status == b.http_status && a.error == b.error && a.phase == b.phase && a.submit_error == b.submit_error &&
        a.transport_error == b.transport_error && a.decode_error == b.decode_error && a.scan_error == b.scan_error &&
        a.read_error == b.read_error && a.transition_error == b.transition_error && a.response_kind == b.response_kind &&
        a.content_type == b.content_type && a.content_encoding == b.content_encoding && a.body_kind == b.body_kind;
}
