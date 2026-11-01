// Synchronous REST boundary for the user-auth coordinator tests. Include at
// namespace scope after the test namespace, with MAICORD_AUTH_TEST_NS naming
// the namespace that holds `fixture` and user_auth_coordinator_test_support.inc.
// The REST client stays mocked so the real networking implementation is not
// pulled into the test.
namespace maicord {
RestClientResult rest_client_start_user_auth_capacity(RestClient* client, u32 response, u32 capacity) noexcept {
    MAICORD_AUTH_TEST_NS::fixture.request_capacity = capacity;
    return rest_client_start_user_auth(client, response);
}
RestClientResult rest_client_start_user_auth(RestClient* client, u32) noexcept {
    return MAICORD_AUTH_TEST_NS::start_client(client);
}
RestClientSubmitResult rest_client_submit_user_auth(RestClient*, const RestUserAuthRequest& value) noexcept {
    using namespace MAICORD_AUTH_TEST_NS;
    if (fixture.fail_submit) return {RestClientSubmitError::busy};
    if (!value.body || !value.body->locked ||
        (fixture.request_capacity && value.body->bytes > fixture.request_capacity) ||
        !secure_buffer_assign(&fixture.request, secure_buffer_view(*value.body)).ok())
        return {RestClientSubmitError::invalid_input};
    return record_request(value);
}
RestClientResult rest_client_status(const RestClient*) noexcept { return MAICORD_AUTH_TEST_NS::client_status(); }
RestClientTakeResult rest_client_consume_user_auth_response(RestClient*, RestClientResponse* response,
                                                            RestUserAuthConsumeFunction consume,
                                                            void* context) noexcept {
    return MAICORD_AUTH_TEST_NS::consume_response(response, consume, context);
}
HANDLE rest_client_notification_handle(const RestClient*) noexcept {
    ++MAICORD_AUTH_TEST_NS::fixture.notification_reads;
    return reinterpret_cast<HANDLE>(1);
}
RestClientResult rest_client_stop_user_auth(RestClient* client, u32 timeout_ms, RestClientResponse* receipt) noexcept {
    MAICORD_AUTH_TEST_NS::stop_receipt(receipt);
    return rest_client_stop(client, timeout_ms);
}
RestClientResult rest_client_stop(RestClient* client, u32) noexcept { return MAICORD_AUTH_TEST_NS::stop_client(client); }
} // namespace maicord
