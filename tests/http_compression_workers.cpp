#include "maicord/media/deflate.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/app/user_account_coordinator.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {
namespace {
HANDLE endpoint_wait_event;
volatile LONG endpoint_waits;
void observe_endpoint_wait(const char* record) noexcept {
    constexpr char event[] = "gate_wait ";
    constexpr char reason[] = " reason=endpoint ";
    for (u32 i = 0; i < sizeof(event) - 1; ++i)
        if (record[i] != event[i])
            return;
    for (u32 i = 0; record[i]; ++i) {
        u32 j = 0;
        while (j < sizeof(reason) - 1 && record[i + j] == reason[j])
            ++j;
        if (j == sizeof(reason) - 1) {
            if (InterlockedIncrement(&endpoint_waits) == 2)
                SetEvent(endpoint_wait_event);
            return;
        }
    }
}
} // namespace
void test_http_compression_workers(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr u8 raw[] = {123, 34,  99, 111, 110, 116, 101, 110, 116, 34, 58,  34,  72,  101, 108,
                          108, 111, 44, 32,  101, 109, 111, 106, 105, 32, 240, 159, 152, 128, 34,
                          44,  34,  99, 111, 100, 101, 34,  58,  52,  48, 48,  48,  50,  125};
    constexpr u8 encoded[] = {31,  139, 8,   0,   0,   0,   0,   0,   2,   255, 171, 86,  74, 206, 207, 43,
                              73,  205, 43,  81,  178, 82,  242, 72,  205, 201, 201, 215, 81, 72,  205, 205,
                              207, 202, 84,  248, 48,  127, 70,  131, 146, 14,  80,  54,  37, 85,  201, 202,
                              196, 192, 192, 192, 168, 22,  0,   46,  180, 124, 166, 44,  0,  0,   0};
    constexpr u8 optional[] = {31,  139, 8,   30,  0,   0,   0,   0,   2,   255, 3,   0,   97,  98,
                               99,  110, 97,  109, 101, 0,   99,  111, 109, 109, 101, 110, 116, 0,
                               244, 110, 171, 86,  74,  206, 207, 43,  73,  205, 43,  81,  178, 82,
                               242, 72,  205, 201, 201, 215, 81,  72,  205, 205, 207, 202, 84,  248,
                               48,  127, 70,  131, 146, 14,  80,  54,  37,  85,  201, 202, 196, 192,
                               192, 192, 168, 22,  0,   46,  180, 124, 166, 44,  0,   0,   0};
    constexpr u8 zlib_body[] = {120, 156, 171, 86,  74,  206, 207, 43,  73, 205, 43,  81,  178,
                                82,  242, 72,  205, 201, 201, 215, 81,  72, 205, 205, 207, 202,
                                84,  248, 48,  127, 70,  131, 146, 142, 82, 114, 126, 74,  170,
                                146, 149, 137, 129, 129, 129, 81,  45,  0,  97,  103, 15,  12};
    DeflateWorkspace workspace{};
    u8 output[1024]{}, changed[256]{};
    for (u32 kind = 0; kind < 2; ++kind) {
        const ConstSlice<u8> source =
            kind ? ConstSlice<u8>{optional, sizeof(optional)} : ConstSlice<u8>{encoded, sizeof(encoded)};
        auto r = gzip_inflate(source, {output, sizeof(output)}, &workspace);
        check(r.ok() && r.output_bytes == sizeof(raw) && bytes_equal(raw, output, sizeof(raw)),
              "gzip validates body and optional header CRC");
        for (u32 n = 1; n < source.count; ++n)
            check(!gzip_inflate({source.data, n}, {output, sizeof(output)}, &workspace).ok(),
                  "truncated gzip is never published as valid JSON");
        check(gzip_inflate(source, {output, sizeof(raw) - 1}, &workspace).error ==
                  ZlibInflateError::output_capacity,
              "gzip output bound rejects expansion overflow");
    }
    copy_bytes(changed, encoded, sizeof(encoded));
    changed[sizeof(encoded) - 8] ^= 1;
    check(gzip_inflate({changed, sizeof(encoded)}, {output, sizeof(output)}, &workspace).error ==
              ZlibInflateError::checksum,
          "gzip payload checksum mismatch is rejected");
    copy_bytes(changed, encoded, sizeof(encoded));
    changed[sizeof(encoded) - 4] ^= 1;
    check(gzip_inflate({changed, sizeof(encoded)}, {output, sizeof(output)}, &workspace).error ==
              ZlibInflateError::output_size,
          "gzip trailer length mismatch is rejected");
    copy_bytes(changed, encoded, sizeof(encoded));
    copy_bytes(changed + sizeof(encoded), encoded, sizeof(encoded));
    auto joined = gzip_inflate({changed, 2 * sizeof(encoded)}, {output, sizeof(output)}, &workspace);
    check(joined.ok() && joined.output_bytes == 2 * sizeof(raw) && bytes_equal(output, raw, sizeof(raw)) &&
              bytes_equal(output + sizeof(raw), raw, sizeof(raw)),
          "concatenated gzip members decode with independent dictionaries");
    const DeflateInputSpan span{zlib_body, sizeof(zlib_body)};
    auto z = zlib_inflate({&span, 1}, {output, sizeof(output)}, ~0u, &workspace);
    check(z.ok() && z.output_bytes == sizeof(raw) && bytes_equal(output, raw, sizeof(raw)),
          "bounded zlib decoding supports unknown HTTP body length");

    RestRateBucket buckets[2]{};
    RestRateBinding bindings[2]{};
    RestRateScheduler rates{};
    rest_rate_scheduler_initialize(&rates, {buckets, 2}, {bindings, 2});
    const RestRouteKey route_a{11, 7, RestMethod::get, RestMajorParameter::channel};
    const RestRouteKey route_b{12, 7, RestMethod::get, RestMajorParameter::channel};
    constexpr char head_text[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nX-RateLimit-Limit: 10\r\nX-RateLimit-Remaining: "
        "8\r\nX-RateLimit-Reset-After: 5\r\nX-RateLimit-Bucket: shared\r\n\r\n";
    auto head =
        parse_rest_response_head({reinterpret_cast<const u8*>(head_text), sizeof(head_text) - 1});
    auto a = rest_rate_acquire(&rates, route_a, 100);
    auto b = rest_rate_acquire(&rates, route_b, 100);
    check(rest_rate_observe(&rates, &a.lease, head, nullptr, 100, 100).ok() &&
              rest_rate_observe(&rates, &b.lease, head, nullptr, 100, 100).ok(),
          "two routes can learn the same account bucket");
    a = rest_rate_acquire(&rates, route_a, 200);
    b = rest_rate_acquire(&rates, route_b, 200);
    head.rate.remaining = 6;
    check(rest_rate_observe(&rates, &b.lease, head, nullptr, 201, 201).ok(),
          "later server operation completes first");
    head.rate.remaining = 7;
    check(rest_rate_observe(&rates, &a.lease, head, nullptr, 202, 202).ok() &&
              buckets[0].remaining == 6,
          "out-of-order responses cannot restore already-consumed rate quota");

    UserAccountCoordinator account{};
    account.state.phase = UserAccountCoordinatorPhase::running;
    RestClient* clients[2]{&account.rest, &account.details};
    auto& shared = account.rest_shared;
    endpoint_wait_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    InterlockedExchange(&endpoint_waits, 0);
    check(endpoint_wait_event != nullptr, "shared endpoint wait observation event initializes");
    rest_client_set_trace(observe_endpoint_wait);
    bool started = true;
    for (auto& client : clients)
        started = rest_client_start_public(client, 4096, &shared).ok() && started;
    check(started, "two credential-free REST workers start without making a request");
    if (started) {
        check(clients[0]->shared == &shared && clients[1]->shared == &shared,
              "REST owners start with one shared scheduler");
        RestClient excess{};
        const auto rejected = rest_client_start_public(&excess, 4096, &shared);
        check(!rejected.ok() && !excess.runtime && !excess.worker.thread && !excess.response_event &&
                  !excess.shared && shared.clients[0] == clients[0] && shared.clients[1] == clients[1],
              "full shared scheduler rejects startup without leaking a worker or replacing peers");
        AcquireSRWLockExclusive(shared.lock.native());
        shared.scheduler.global_reset_at_ms = clock_monotonic_milliseconds() + 60000;
        ReleaseSRWLockExclusive(shared.lock.native());
        constexpr char target[] = "/api/v10/gateway";
        for (u32 n = 0; n < 2; ++n) {
            RestClientRequest request{{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                                      {},
                                      {n + 1, 0, RestMethod::get, RestMajorParameter::none},
                                      n + 1,
                                      1000,
                                      false};
            check(rest_client_submit(clients[n], request).ok(),
                  "independent owners admit bounded reads concurrently");
        }
        for (u32 order = 0; order < 2; ++order) {
            const u32 n = 1 - order;
            check(rest_client_cancel(clients[n], n + 1),
                  "each blocked read can be cancelled by its own ID");
            check(rest_client_wait_response(clients[n], 3000).ok(),
                  "cancel wakes a rate-blocked worker without network traffic");
            RestClientResponse response{};
            auto taken = user_account_coordinator_take(&account, &response, {output, sizeof(output)});
            check(taken.ok() && response.request_id == n + 1 &&
                      response.error == RestClientError::cancelled &&
                      rest_client_status(clients[n]).connections == 0,
                  "cancelled completions retain ownership and global limit prevents DNS/TLS");
        }
        // The global limit remains an offline safety backstop. The trace barrier
        // proves both real workers entered the endpoint gate before that limit.
        constexpr char endpoint[] = "/api/v10/channels/1/messages";
        constexpr char history[] = "/api/v10/channels/1/messages?before=2";
        constexpr char message[] = "/api/v10/channels/1/messages?nonce=3";
        const u64 floor = clock_monotonic_milliseconds() + 120000;
        AcquireSRWLockExclusive(shared.lock.native());
        shared.endpoint_gate.entries[0] = {
            hash64_counted_bytes(kHash64Seed, endpoint, sizeof(endpoint) - 1),
            hash64_counted_bytes(~kHash64Seed, endpoint, sizeof(endpoint) - 1),
            floor,
            sizeof(endpoint) - 1,
            1,
            0};
        const u32 acquired_before = shared.scheduler.acquired;
        ReleaseSRWLockExclusive(shared.lock.native());
        ResetEvent(endpoint_wait_event);
        InterlockedExchange(&endpoint_waits, 0);
        const RestClientRequest requests[]{{{reinterpret_cast<const u8*>(history), sizeof(history) - 1},
                                            {},
                                            {101, 1, RestMethod::get, RestMajorParameter::channel},
                                            101,
                                            1000,
                                            false},
                                           {{reinterpret_cast<const u8*>(message), sizeof(message) - 1},
                                            {},
                                            {102, 1, RestMethod::post, RestMajorParameter::channel},
                                            102,
                                            1000,
                                            true}};
        bool submitted = true;
        for (u32 n = 0; n < 2; ++n)
            submitted = rest_client_submit(clients[n], requests[n]).ok() && submitted;
        const bool gated = submitted && endpoint_wait_event &&
                           WaitForSingleObject(endpoint_wait_event, 3000) == WAIT_OBJECT_0;
        AcquireSRWLockExclusive(shared.lock.native());
        const bool no_leases = shared.scheduler.acquired == acquired_before && !shared.mutation_owner &&
                               shared.endpoint_gate.entries[0].retry_at_ms == floor &&
                               shared.endpoint_gate.entries[0].in_flight == 0;
        ReleaseSRWLockExclusive(shared.lock.native());
        check(gated && no_leases && rest_client_status(clients[0]).connections == 0 &&
                  rest_client_status(clients[1]).connections == 0 &&
                  rest_client_status(clients[0]).requests == 0 &&
                  rest_client_status(clients[1]).requests == 0,
              "shared endpoint floor blocks GET and POST query variants before any lease, DNS or send");
        for (u32 n = 0; n < 2; ++n) {
            RestClientResponse response{};
            const bool cancelled =
                rest_client_cancel(clients[n], n + 101) && rest_client_wait_response(clients[n], 3000).ok();
            const auto taken = user_account_coordinator_take(&account, &response, {output, sizeof(output)});
            check(cancelled && taken.ok() && response.request_id == n + 101 &&
                      response.error == RestClientError::cancelled &&
                      shared.endpoint_gate.entries[0].retry_at_ms == floor,
                  "each endpoint-gated worker cancels without clearing the shared server floor");
        }
        InterlockedExchange(&shared.authentication_rejected, 1);
        check(rest_client_status(clients[0]).authentication_rejected &&
                  rest_client_status(clients[1]).authentication_rejected,
              "an authentication failure is immediately shared by both owners");
    }
    for (auto* client : clients)
        if (client->worker.thread)
            rest_client_stop(client, 3000);
    check(!shared.clients[0] && !shared.clients[1] && !clients[0]->runtime && !clients[1]->runtime,
          "shared worker teardown unregisters peers before releasing storage");
    const u64 reset = shared.scheduler.global_reset_at_ms;
    const auto endpoint_floor = shared.endpoint_gate.entries[0];
    RestClient replacement{};
    const auto restarted = rest_client_start_public(&replacement, 4096, &shared);
    check(restarted.ok() && shared.clients[0] == &replacement &&
              shared.scheduler.global_reset_at_ms == reset &&
              bytes_equal(&shared.endpoint_gate.entries[0], &endpoint_floor, sizeof(endpoint_floor)) &&
              rest_client_status(&replacement).authentication_rejected,
          "replacement worker retains shared rate limits, endpoint floors and authentication rejection");
    if (replacement.worker.thread)
        rest_client_stop(&replacement, 3000);
    rest_client_set_trace(nullptr);
    if (endpoint_wait_event) CloseHandle(endpoint_wait_event);
    endpoint_wait_event = nullptr;
}
} // namespace maicord
