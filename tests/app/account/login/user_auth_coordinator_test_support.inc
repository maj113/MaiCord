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

// Shared synchronous transport operations; case-specific observations stay in the mocks.
RestClientResult start_client(RestClient* client) noexcept {
    client->runtime = &fixture;
    client->worker.thread = reinterpret_cast<HANDLE>(1);
    fixture.phase = RestClientPhase::idle;
    fixture.error = RestClientError::none;
    RestClientResult result{}; result.phase = RestClientPhase::idle; return result;
}

RestClientResult client_status() noexcept {
    RestClientResult result{};
    result.phase = fixture.ready ? RestClientPhase::response_ready : fixture.phase;
    result.error = fixture.error;
    return result;
}

RestClientTakeResult consume_response(RestClientResponse* response,
                                       RestUserAuthConsumeFunction consume, void* context) noexcept {
    *response = fixture.metadata;
    if (!consume(context, fixture.metadata, secure_buffer_view(fixture.response)))
        return {0, RestClientTakeError::invalid_input};
    fixture.ready = false;
    secure_buffer_clear(&fixture.response);
    return {response->body_bytes, RestClientTakeError::none};
}

RestClientResult stop_client(RestClient* client) noexcept {
    RestClientResult result{};
    result.phase = fixture.fail_stop      ? RestClientPhase::draining
                   : fixture.joined_error ? RestClientPhase::failed
                                          : RestClientPhase::stopped;
    result.error = fixture.joined_error ? RestClientError::worker : RestClientError::none;
    if (!fixture.fail_stop) {
        *client = {}; fixture.ready = false;
        secure_buffer_clear(&fixture.request); secure_buffer_clear(&fixture.response); secure_buffer_clear(&fixture.headers);
    }
    return result;
}

RestClientSubmitResult record_request(const RestUserAuthRequest& value) noexcept {
    fixture.operation = value.operation; fixture.request_id = value.request_id; ++fixture.submissions;
    secure_buffer_clear(&fixture.headers);
    if (value.captcha_key) {
        const auto utf8 = [](const SecureBuffer* buffer) noexcept -> Utf8View {
            return buffer ? Utf8View{reinterpret_cast<const char*>(buffer->data), buffer->bytes} : Utf8View{};
        };
        const auto written = user_auth_write_captcha_headers(
            {utf8(value.captcha_key), utf8(value.captcha_rqtoken), utf8(value.captcha_session_id)},
            {fixture.headers.data, fixture.headers.capacity});
        if (!written.ok()) return {RestClientSubmitError::invalid_input};
        fixture.headers.bytes = written.bytes;
    }
    fixture.ready = false;
    return {RestClientSubmitError::none};
}

void stop_receipt(RestClientResponse* receipt) noexcept {
    if (receipt) {
        *receipt = {};
        if (fixture.ready && fixture.metadata.transport_ok() && fixture.metadata.status_code == 429) {
            receipt->request_id = fixture.metadata.request_id;
            receipt->status_code = 429;
            receipt->retry_at_ms = fixture.metadata.retry_at_ms;
        }
    }
}
