// Also built with MAICORD_REST_STORAGE_BENCHMARK=1 as the manual MaiCordRestStorageBenchmark
// (cmake/benchmarks.cmake).
// Exercise the shipping private owner, including publication and transport close.
#include "../src/app/rest_client.cpp"
#include "fixtures/rest_gzip_fixtures.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/thread_browser.hpp"
#if defined(MAICORD_REST_STORAGE_BENCHMARK)
#include <psapi.h>
#endif

namespace maicord {
namespace {
struct RestStorageProbe {
    RestClient client{};
    RestClientRuntime* runtime = nullptr;
    bool initialize(u32 capacity, bool normal_user = false, RestSharedState* shared = nullptr) noexcept {
        const u32 bytes = allocation_bytes(capacity, false, normal_user, shared != nullptr);
        runtime = static_cast<RestClientRuntime*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!runtime) return false;
        client.runtime = runtime;
        runtime->owner = &client;
        runtime->allocation_bytes = bytes;
        runtime->response_capacity = capacity;
        runtime->authorization = normal_user ? RestAuthorization::normal_user : RestAuthorization::none;
        runtime->clock = clock_initialize();
        assign_storage(runtime, shared);
        client.shared = shared;
        // A real connection has written the handshake output before any HTTP
        // response arrives. Include that first resident page in the fixture.
        *reinterpret_cast<volatile u8*>(runtime->handshake_output) = 0;
        client.response_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return client.response_event &&
            tls_stream_initialize(&runtime->transport, L"discord.com", {
                {runtime->encrypted_storage, kEncryptedInputBytes},
                {runtime->handshake_output, kHandshakeOutputBytes},
                {runtime->record_output, kRecordOutputBytes}});
    }
    ~RestStorageProbe() noexcept {
        if (client.response_event) CloseHandle(client.response_event);
        destroy_client_storage(&client, nullptr);
    }
    bool begin_request(bool reusable, u32 request_bytes = 0) noexcept {
        runtime->transport.state = TlsStreamState::ready;
        const RestRouteKey route{1, 0, request_bytes ? RestMethod::post : RestMethod::get, RestMajorParameter::none};
        const auto acquired = rest_rate_acquire(runtime->scheduler, route, 1);
        runtime->transaction = {};
        if (!acquired.acquired_request() ||
            !rest_transaction_acquire(&runtime->transaction, 1, acquired.lease, 1000, reusable) ||
            !rest_transaction_begin_send(&runtime->transaction, runtime->request_head_capacity + request_bytes)) return false;
        // A completed encrypted write releases and erases the outgoing header
        // before the same bytes become incoming HTTP-header storage.
        memset(runtime->request_head, 0xA5, runtime->request_head_capacity);
        const u32 half = runtime->request_head_capacity / 2;
        request_plaintext_committed(runtime, {runtime->request_head, half});
        if (runtime->request_head[0] || runtime->request_head[half] != 0xA5) return false;
        request_plaintext_committed(runtime, {runtime->request_head + half, runtime->request_head_capacity - half});
        for (u32 i = 0; i < runtime->request_head_capacity; ++i) if (runtime->request_head[i]) return false;
        runtime->command.route = route;
        runtime->command.body_bytes = request_bytes;
        for (u32 i = 0; i < request_bytes; ++i) runtime->request_body[i] = static_cast<u8>(i * 37u + 1);
        for (u32 i = 0; i < request_bytes; i += 16384)
            request_plaintext_committed(runtime, {runtime->request_body + i, minimum(16384u, request_bytes - i)});
        for (u32 i = 0; i < request_bytes; ++i) if (runtime->request_body[i]) return false;
        if (runtime->write_commit_failed || runtime->transaction.phase != RestTransactionPhase::receiving) return false;
        client.request_state = request_state_active;
        if (!rest_response_initialize(&runtime->response,
            {runtime->response_input, kRestMaximumHeaderBytes},
            {runtime->response_body, runtime->response_capacity}, RestMethod::get)) return false;
        return true;
    }
    bool prepare(const RestGzipFixture& fixture, bool reusable, bool corrupt = false,
                 u32 request_bytes = 0) noexcept {
        if (!begin_request(reusable, request_bytes)) return false;
        u8 header[256];
        ProtocolWriter writer{{header, sizeof(header)}, 0};
        protocol_writer_append_literal(&writer, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: ");
        protocol_writer_append_u64(&writer, fixture.bytes);
        if (reusable) protocol_writer_append_literal(&writer, "\r\n\r\n");
        else protocol_writer_append_literal(&writer, "\r\nConnection: close\r\n\r\n");
        if (response_plaintext_received(runtime, {header, writer.used}, false) != TlsStreamReadAction::continue_read ||
            response_plaintext_received(runtime, {fixture.encoded, fixture.bytes}, false) != TlsStreamReadAction::complete) return false;
        if (corrupt) runtime->response_body[fixture.bytes - 8] ^= 1; // CRC failure after output was produced.
        return true;
    }
    bool finish() noexcept { return finalize_response(&client, runtime, runtime->response_step, 1); }
    bool correct(const RestGzipFixture& fixture) noexcept {
        if (runtime->completed.body_bytes != fixture.decoded_bytes || client.request_state != request_state_ready) return false;
        for (u32 i = 0; i < fixture.decoded_bytes; ++i) {
            const u8 expected = i == 0 || i + 1 == fixture.decoded_bytes ? '"' : static_cast<u8>('a' + (i - 1) % 26);
            if (runtime->response_body[i] != expected) return false;
        }
        return true;
    }
    u64 committed() noexcept {
        MEMORY_BASIC_INFORMATION region{};
        VirtualQuery(runtime, &region, sizeof(region));
        u64 bytes = region.State == MEM_COMMIT ? region.RegionSize : 0;
        if (runtime->decompression) {
            VirtualQuery(runtime->decompression, &region, sizeof(region));
            if (region.AllocationBase != runtime && region.State == MEM_COMMIT) bytes += region.RegionSize;
        }
        return bytes;
    }
    void consume() noexcept { release_completed_response(&client, runtime, runtime->completed.body_bytes); }
};
NetworkWorkerStep publish_stop_rate_receipt(void* context, NetworkLoop*,
                                            const NetworkWorkerEvent& event) noexcept {
    if (event.kind != NetworkWorkerEventKind::stop_requested)
        return {INFINITE, false};
    auto* probe = static_cast<RestStorageProbe*>(context);
    // Publication occurs only after the real Stop call has posted its worker
    // stop signal. No socket or auth decoder participates in this fixture.
    probe->runtime->completed = {};
    probe->runtime->completed.request_id = 77;
    probe->runtime->completed.status_code = 429;
    probe->runtime->completed.retry_at_ms = 900'000'123;
    probe->runtime->completed.body_bytes = 64;
    InterlockedExchange(&probe->client.request_state, request_state_ready);
    return {INFINITE, true};
}
} // namespace

void test_rest_response_storage(void (*check)(bool, const char*) noexcept) noexcept {
    {
        constexpr auto mfa =
            utf8_literal(R"({"code":60003,"mfa":{"ticket":"synthetic","methods":[{"type":"totp"}]}})");
        constexpr auto invalid = utf8_literal(R"({"code":50014})");
        // Deterministic gzip encodings of the two synthetic JSON receipts.
        constexpr u8 mfa_gzip[]{
            0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xab, 0x56, 0x4a, 0xce, 0x4f,
            0x49, 0x55, 0xb2, 0x32, 0x33, 0x30, 0x30, 0x30, 0xd6, 0x51, 0xca, 0x4d, 0x4b, 0x54, 0xb2,
            0xaa, 0x56, 0x2a, 0xc9, 0x4c, 0xce, 0x4e, 0x2d, 0x51, 0xb2, 0x52, 0x2a, 0xae, 0xcc, 0x2b,
            0xc9, 0x48, 0x05, 0x72, 0x95, 0x80, 0x72, 0xa9, 0x25, 0x19, 0xf9, 0x29, 0xc5, 0x4a, 0x56,
            0xd1, 0x40, 0x05, 0x95, 0x05, 0x40, 0x4d, 0x4a, 0x25, 0xf9, 0x25, 0x05, 0x4a, 0xb5, 0xb1,
            0xb5, 0xb5, 0x00, 0xc4, 0xf6, 0x71, 0x61, 0x47, 0x00, 0x00, 0x00};
        constexpr u8 invalid_gzip[]{0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xab, 0x56,
                                    0x4a, 0xce, 0x4f, 0x49, 0x55, 0xb2, 0x32, 0x35, 0x30, 0x30, 0x34, 0xa9,
                                    0x05, 0x00, 0xe9, 0x8b, 0x1c, 0xf7, 0x0e, 0x00, 0x00, 0x00};
        for (u32 scenario = 0; scenario < 11; ++scenario) {
            RestSharedState shared{};
            RestStorageProbe probe;
            RestClient peer{};
            peer.shared = &shared;
            bool valid =
                rest_rate_scheduler_initialize(&shared.scheduler, {shared.buckets, kRateBucketCount},
                                               {shared.bindings, kRateBindingCount}) &&
                probe.initialize(4096, true, &shared) && probe.begin_request(true);
            if (valid) {
                const bool gzip = scenario == 1 || scenario == 3 || scenario == 6 || scenario == 8;
                const bool ordinary = scenario == 2 || scenario == 3;
                Utf8View body = ordinary         ? invalid
                                : scenario == 4  ? utf8_literal("{")
                                : scenario == 5  ? utf8_literal(R"({"code":60003})")
                                : scenario == 10 ? Utf8View{}
                                                 : mfa;
                ConstSlice<u8> wire{reinterpret_cast<const u8*>(body.data), body.bytes};
                if (gzip)
                    wire = ordinary ? ConstSlice<u8>{invalid_gzip, sizeof(invalid_gzip)}
                                    : ConstSlice<u8>{mfa_gzip, sizeof(mfa_gzip)};
                u8 head[256]{};
                ProtocolWriter writer{{head, sizeof(head)}, 0};
                protocol_writer_append_literal(
                    &writer,
                    "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: ");
                protocol_writer_append_u64(&writer, wire.count);
                if (gzip)
                    protocol_writer_append_literal(&writer, "\r\nContent-Encoding: gzip");
                if (scenario == 7)
                    protocol_writer_append_literal(&writer, "\r\nContent-Encoding: deflate");
                protocol_writer_append_literal(&writer, "\r\n\r\n");
                const auto received =
                    response_plaintext_received(probe.runtime, {head, writer.used}, false);
                valid = wire.count ? received == TlsStreamReadAction::continue_read &&
                                         response_plaintext_received(probe.runtime, wire, false) ==
                                             TlsStreamReadAction::complete
                                   : received == TlsStreamReadAction::complete;
                if (scenario == 6)
                    probe.runtime->response_body[wire.count - 8] ^= 1;
                if (scenario == 8)
                    shared.authentication_rejected = 1;
                if (scenario == 9)
                    probe.client.authentication_rejected = 1;
                const bool finished = valid && probe.finish();
                const bool elevation = scenario == 0 || scenario == 1 || scenario == 8 || scenario == 9;
                const bool rejected = !elevation || scenario == 8 || scenario == 9;
                valid =
                    valid && finished == (scenario != 6 && scenario != 7) &&
                    rest_client_status(&probe.client).authentication_rejected == rejected &&
                    rest_client_status(&peer).authentication_rejected == (!elevation || scenario == 8) &&
                    probe.client.requests == 0 && probe.client.responses == (finished ? 1 : 0);
                if (finished)
                    valid = valid && probe.runtime->completed.status_code == 401 &&
                            probe.runtime->completed.transport_ok() &&
                            !probe.runtime->completed.rate_limited &&
                            (scenario == 10 ||
                             bytes_equal(probe.runtime->response_body, body.data, body.bytes));
                if (elevation)
                    valid = valid && probe.runtime->completed.rejection.has_code &&
                            probe.runtime->completed.rejection.code == 60003;
            }
            check(valid, "rest401_mfa_elevation_preserves_session_plain_and_gzip_while_invalid_decode_"
                         "failures_and_existing_peer_rejections_stay_latched_without_replay");
        }
    }
    {
        const struct {
            u16 status;
            Utf8View body;
            bool challenge;
        } responses[]{
            {503, utf8_literal("{}"), false},
            {200, utf8_literal("{}"), false},
            {403, utf8_literal(R"({"code":50001})"), false},
            {503,
             utf8_literal(
                 R"({"captcha_key":["required"],"captcha_service":"hcaptcha","captcha_sitekey":"synthetic"})"),
             true}};
        for (const auto& response : responses) {
            RestStorageProbe probe;
            bool valid = probe.initialize(4096) && probe.begin_request(true);
            if (valid) {
                const RestRouteKey route = probe.runtime->command.route;
                u8 head[256]{};
                ProtocolWriter writer{{head, sizeof(head)}, 0};
                valid = protocol_writer_append_literal(&writer, "HTTP/1.1 ") &&
                        protocol_writer_append_u64(&writer, response.status) &&
                        protocol_writer_append_literal(
                            &writer, " Response\r\nContent-Type: application/json\r\nContent-Length: ") &&
                        protocol_writer_append_u64(&writer, response.body.bytes) &&
                        protocol_writer_append_literal(&writer, "\r\nRetry-After: 60\r\n\r\n") &&
                        response_plaintext_received(probe.runtime, {head, writer.used}, false) ==
                            TlsStreamReadAction::continue_read &&
                        response_plaintext_received(
                            probe.runtime,
                            {reinterpret_cast<const u8*>(response.body.data), response.body.bytes},
                            false) == TlsStreamReadAction::complete &&
                        probe.finish() && probe.runtime->completed.transport_ok() &&
                        probe.runtime->completed.status_code == response.status &&
                        probe.runtime->completed.retry_at_ms == (response.challenge ? 0 : 60'001) &&
                        !probe.runtime->completed.rate_limited &&
                        !probe.runtime->completed.global_rate_limit &&
                        api_rejection_requires_action(probe.runtime->completed.rejection) ==
                            response.challenge &&
                        probe.runtime->completed.rate.retry_after_ms == 60'000 &&
                        (probe.runtime->completed.rate.fields & rate_limit_field_retry_after) &&
                        !probe.runtime->scheduler->global_reset_at_ms && probe.client.requests == 0 &&
                        probe.client.responses == 1;
                if (valid) {
                    auto acquired = rest_rate_acquire(probe.runtime->scheduler, route, 2);
                    valid = acquired.acquired_request() &&
                            rest_rate_cancel(probe.runtime->scheduler, &acquired.lease);
                }
            }
            check(valid, "rest_non429_retry_floor_survives_transport_without_enabling_rate_replay_"
                         "and_challenges_clear_the_floor");
        }
    }
    {
        constexpr Utf8View bodies[]{
            utf8_literal("{"),
            utf8_literal(
                R"({"captcha_key":["required"],"captcha_service":"hcaptcha","captcha_sitekey":"synthetic"})")};
        constexpr const char* cases[][3]{
            {"rest429_auth_malformed_unknown_floor_without_replay",
             "rest429_auth_malformed_preserves_header_floor_without_replay",
             "rest429_ordinary_malformed_preserves_header_floor_without_replay"},
            {"rest429_auth_captcha_unknown_floor_without_replay",
             "rest429_auth_captcha_preserves_header_floor_without_replay",
             "rest429_ordinary_captcha_is_terminal_and_preserves_scheduler_floor"}};
        for (u32 body_index = 0; body_index < 2; ++body_index) {
            const auto body = bodies[body_index];
            for (u32 variant = 0; variant < 3; ++variant) {
                const bool header = variant != 0;
                const bool auth = variant != 2;
                const bool action_required = !auth && body_index == 1;
                RestStorageProbe probe;
                bool valid = probe.initialize(4096) && probe.begin_request(true);
                if (valid) {
                    const RestRouteKey route = probe.runtime->command.route;
                    probe.runtime->user_auth_mode = auth;
                    u8 head[256]{};
                    ProtocolWriter writer{{head, sizeof(head)}, 0};
                    protocol_writer_append_literal(&writer,
                                                   "HTTP/1.1 429 Too Many Requests\r\nContent-Type: "
                                                   "application/json\r\nContent-Length: ");
                    protocol_writer_append_u64(&writer, body.bytes);
                    if (header)
                        protocol_writer_append_literal(&writer, "\r\nRetry-After: 1.25");
                    protocol_writer_append_literal(&writer, "\r\n\r\n");
                    valid = response_plaintext_received(probe.runtime, {head, writer.used}, false) ==
                                TlsStreamReadAction::continue_read &&
                            response_plaintext_received(
                                probe.runtime, {reinterpret_cast<const u8*>(body.data), body.bytes},
                                false) == TlsStreamReadAction::complete &&
                            probe.finish() && probe.client.request_state == request_state_ready &&
                            probe.runtime->completed.transport_ok() &&
                            probe.runtime->completed.status_code == 429 &&
                            probe.runtime->completed.rate_limited == !action_required &&
                            probe.runtime->completed.retry_at_ms == (header && !action_required ? 1251 : 0) &&
                            api_rejection_requires_action(probe.runtime->completed.rejection) ==
                                action_required &&
                            ((probe.runtime->completed.rate.fields & rate_limit_field_retry_after) != 0) ==
                                header &&
                            (!header || probe.runtime->completed.rate.retry_after_ms == 1250) &&
                            probe.client.requests == 0 && probe.client.responses == 1;
                    if (valid && action_required) {
                        // The response cannot authorize replay of verification,
                        // while its genuine header still limits a later action.
                        const auto acquired = rest_rate_acquire(probe.runtime->scheduler, route, 2);
                        valid = acquired.kind == RestRateAcquireKind::waiting &&
                                acquired.wait_until_ms == 1251 && probe.client.requests == 0 &&
                                probe.client.responses == 1;
                    }
                }
                check(valid, cases[body_index][variant]);
            }
        }
    }
    {
        RestStorageProbe probe;
        bool valid = probe.initialize(4096);
        if (valid) {
            probe.runtime->user_auth_mode = true;
            probe.runtime->completed = {};
            probe.runtime->completed.request_id = 55;
            probe.runtime->completed.status_code = 429;
            probe.runtime->completed.retry_at_ms = 123;
            RestClientResponse receipt{};
            // In-progress publication is never a safe receipt to inspect.
            probe.client.request_state = request_state_publishing;
            capture_user_auth_stop_receipt(&probe.client, &receipt);
            valid = !receipt.status_code && !receipt.retry_at_ms;
            probe.client.request_state = request_state_ready;
            capture_user_auth_stop_receipt(&probe.client, &receipt);
            valid = valid && receipt.request_id == 55 && receipt.status_code == 429 &&
                    receipt.retry_at_ms == 123 && !receipt.body_bytes;
            probe.runtime->completed.status_code = 200;
            receipt = {};
            capture_user_auth_stop_receipt(&probe.client, &receipt);
            valid = valid && !receipt.status_code && !receipt.request_id;
            probe.client.request_state = request_state_active;
            valid =
                valid && network_worker_start(&probe.client.worker, publish_stop_rate_receipt, &probe).ok();
            if (probe.client.worker.thread) {
                const auto stopped = rest_client_stop_user_auth(&probe.client, 3000, &receipt);
                valid = valid && stopped.ok() && !probe.client.runtime && !probe.client.worker.thread &&
                        receipt.request_id == 77 && receipt.status_code == 429 &&
                        receipt.retry_at_ms == 900'000'123 && !receipt.body_bytes;
            }
        }
        check(
            valid,
            "rest_auth_stop_reads_only_published429_metadata_including_receipt_published_during_real_join");
    }
    {
        // The shipping Details owner has the same shared rates/body arena as
        // the ordinary account lane. Exercise the largest valid browser target
        // in that storage without a credential, worker or socket.
        RestSharedState shared{};
        RestStorageProbe details;
        bool valid = details.initialize(256 * 1024, true, &shared);
        char name[400];
        for (u32 at = 0; at < sizeof(name); at += 4) {
            name[at] = static_cast<char>(0xF0);
            name[at + 1] = static_cast<char>(0x9F);
            name[at + 2] = static_cast<char>(0x98);
            name[at + 3] = static_cast<char>(0x80);
        }
        const ThreadBrowserRequest request{~u64{0}, 10, {name, sizeof(name)}, 0, ThreadBrowserMode::search,
                                           {}};
        if (valid) {
            auto* r = details.runtime;
            r->encrypted_storage[0] = 0xA5;
            r->request_body[0] = 0x5A;
            const auto written =
                thread_browser_write_target(request, {r->request_target, kRestClientMaximumTargetBytes});
            valid = written.ok() && written.bytes == 1259 && written.bytes > 1024 &&
                    r->request_target[written.bytes - 1] == '0' && r->encrypted_storage[0] == 0xA5 &&
                    r->request_body[0] == 0x5A && r->request_body == r->response_body &&
                    r->scheduler == &shared.scheduler && !r->extended_body;
            details.client.current_request_id = 701;
            details.client.request_state = request_state_pending;
            valid = valid && !rest_client_cancel(&details.client, 700) &&
                details.client.request_state == request_state_pending &&
                rest_client_cancel(&details.client, 701) &&
                details.client.request_state == request_state_cancel_pending;
            r->completed.request_id = 702;
            r->completed.status_code = 200;
            details.client.current_request_id = 702;
            details.client.request_state = request_state_ready;
            valid = valid && !rest_client_cancel(&details.client, 702) &&
                details.client.request_state == request_state_ready && r->completed.request_id == 702 &&
                r->completed.status_code == 200;
        }
        check(valid, "rest_storage_browser_maximum_unicode_target_shared_details_layout_and_exact_cancel_preserve_ready_receipt");
    }
    const u32 capacities[]{4096, 4097, 65535, 65536, 262143, 262144, 262145, kRestClientMaximumResponseBytes};
    for (u32 capacity : capacities) for (u32 normal_user = 0; normal_user < 2; ++normal_user)
    for (u32 sharing = 0; sharing < 2; ++sharing) {
        RestSharedState shared{};
        RestStorageProbe layout;
        bool valid = layout.initialize(capacity, normal_user != 0, sharing ? &shared : nullptr);
        if (valid) {
            const auto* r = layout.runtime;
            const Slice<u8> spans[]{
                {r->request_target, kRestClientMaximumTargetBytes},
                {r->request_body, maximum(kRestClientMaximumRequestBodyBytes, capacity)},
                {r->encrypted_storage, kEncryptedInputBytes}, {r->record_output, kRecordOutputBytes},
                {r->response_input, kRestMaximumHeaderBytes},
                {r->handshake_output, kHandshakeOutputBytes}};
            const auto begin = reinterpret_cast<usize>(r) + sizeof(*r) + (sharing ? 0 : sizeof(RestLocalRates));
            const auto end = reinterpret_cast<usize>(r) + r->allocation_bytes;
            valid = (sharing ? r->scheduler == &shared.scheduler : reinterpret_cast<usize>(r->scheduler) == reinterpret_cast<usize>(r) + sizeof(*r)) &&
                (reinterpret_cast<usize>(r->handshake_output) & 4095) == 0 && r->request_body == r->response_body &&
                r->request_head >= r->response_input &&
                r->request_head + r->request_head_capacity == r->response_input + kRestMaximumHeaderBytes;
            for (u32 i = 0; valid && i < 6; ++i) {
                const usize first = reinterpret_cast<usize>(spans[i].data), last = first + spans[i].count;
                valid = first >= begin && last <= end;
                for (u32 j = 0; valid && j < i; ++j) {
                    const usize other = reinterpret_cast<usize>(spans[j].data);
                    valid = last <= other || first >= other + spans[j].count;
                }
            }
            if (normal_user) valid = valid &&
                r->user_super_properties >= r->response_body + capacity &&
                reinterpret_cast<const u8*>(r->user_timezone) + kRestUserTimezoneMaximumBytes <= r->handshake_output;
        }
        check(valid, "rest_storage_page_aligned_scratch_nonoverlapping_at_capacity_boundaries");
    }
    for (const auto& fixture : rest_gzip_fixtures) for (u32 sharing = 0; sharing < 2; ++sharing) {
        RestSharedState shared{};
        rest_rate_scheduler_initialize(&shared.scheduler, {shared.buckets, 32}, {shared.bindings, 64});
        RestStorageProbe probe;
        const u32 capacity = maximum(kHandshakeOutputBytes, fixture.decoded_bytes);
        bool valid = probe.initialize(capacity, true, sharing ? &shared : nullptr);
        const u64 before = valid ? probe.committed() : 0;
        for (u32 cycle = 0; valid && cycle < 4; ++cycle) {
            // Simulate handshake-output reuse before another response/reconnect.
            probe.runtime->handshake_output[0] = 0xA5;
            valid = probe.prepare(fixture, (cycle & 1) == 0, false,
                cycle & 1 ? kRestClientMaximumRequestBodyBytes : 17) && probe.finish() && probe.correct(fixture);
            if (!valid) break;
            valid = probe.committed() == before + (capacity > kHandshakeOutputBytes ? capacity : 0) &&
                (capacity <= kHandshakeOutputBytes ? probe.runtime->decompression == probe.runtime->handshake_output
                                                  : probe.runtime->decompression != probe.runtime->handshake_output);
            probe.consume();
        }
        check(valid, "rest_gzip_storage_exact_output_keepalive_close_reconnect_and_commitment");
        if (!valid) continue;
        valid = probe.prepare(fixture, true, true) && !probe.finish();
        const auto* scratch = capacity <= kHandshakeOutputBytes ? probe.runtime->handshake_output : probe.runtime->decompression;
        for (u32 i = 0; valid && i < capacity; ++i) valid = scratch[i] == 0;
        check(valid, "rest_gzip_crc_failure_erases_entire_scratch");
        cancel_rate_lease(probe.runtime);
        erase_response_storage(probe.runtime);
        valid = probe.prepare(fixture, true);
        probe.client.request_state = request_state_cancel_active;
        valid = valid && probe.finish() && probe.runtime->completed.error == RestClientError::cancelled &&
            probe.runtime->completed.body_bytes == 0;
        check(valid, "rest_gzip_active_cancel_closes_without_exposing_output");
    }
    RestStorageProbe limited;
    bool valid = limited.initialize(4096);
    if (valid) {
        limited.runtime->handshake_output[4096] = 0x5A;
        valid = limited.prepare(rest_gzip_fixtures[4], true) && !limited.finish() &&
            limited.runtime->decompression == limited.runtime->handshake_output && limited.runtime->handshake_output[4096] == 0x5A;
        for (u32 i = 0; valid && i < 4096; ++i) valid = limited.runtime->handshake_output[i] == 0;
    }
    check(valid, "rest_gzip_scratch_reuse_preserves_response_capacity_and_guard");
    RestStorageProbe identity;
    valid = identity.initialize(4096) && identity.begin_request(true, kRestClientMaximumRequestBodyBytes);
    if (valid) {
        constexpr char header[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\n\r\n";
        constexpr char body[] = "{\"ok\":true}";
        valid = response_plaintext_received(identity.runtime,
            {reinterpret_cast<const u8*>(header), sizeof(header) - 1}, false) == TlsStreamReadAction::continue_read &&
            response_plaintext_received(identity.runtime,
            {reinterpret_cast<const u8*>(body), sizeof(body) - 1}, false) == TlsStreamReadAction::complete && identity.finish();
        RestClientResponse response{};
        u8 output[16]{};
        const auto short_take = rest_client_take_response(&identity.client, &response, {output, 1});
        valid = valid && short_take.error == RestClientTakeError::output_too_small &&
            identity.client.request_state == request_state_ready;
        const auto taken = rest_client_take_response(&identity.client, &response, {output, sizeof(output)});
        valid = valid && taken.ok() && taken.body_bytes == sizeof(body) - 1 &&
            identity.client.request_state == request_state_empty;
        for (u32 i = 0; valid && i < sizeof(body) - 1; ++i)
            valid = output[i] == static_cast<u8>(body[i]) && identity.runtime->response_body[i] == 0;
    }
    check(valid, "rest_identity_response_survives_request_cleanup_and_short_output_retry");
    RestStorageProbe small;
    valid = small.initialize(kRestClientMinimumResponseBytes);
    for (u32 i = 0; valid && i < 3; ++i) {
        valid = small.prepare(rest_gzip_fixtures[i], true, false,
                              kRestClientMaximumRequestBodyBytes) &&
                small.finish() && small.correct(rest_gzip_fixtures[i]);
        small.consume();
    }
    check(valid, "rest_maximum_request_preserves_small_and_empty_responses");
    if (valid) {
        auto* r = small.runtime;
        const RestRouteKey route{2, 0, RestMethod::post, RestMajorParameter::none};
        const auto acquired = rest_rate_acquire(r->scheduler, route, 1);
        valid = acquired.acquired_request() &&
            rest_transaction_acquire(&r->transaction, 2, acquired.lease, 1000, true) &&
            rest_transaction_begin_send(&r->transaction, kRestClientMaximumRequestBodyBytes);
        r->command.body_bytes = kRestClientMaximumRequestBodyBytes;
        for (u32 i = 0; i < r->command.body_bytes; ++i) r->request_body[i] = 0xA5;
        request_plaintext_committed(r, {r->request_body, 16384});
        valid = valid && r->request_body[0] == 0 && r->request_body[16384] == 0xA5;
        small.client.request_state = request_state_cancel_active;
        cancel_rate_lease(r);
        publish_request_error(&small.client, r, RestClientError::send, 0);
        valid = valid && r->completed.error == RestClientError::cancelled && r->completed.body_bytes == 0;
        for (u32 i = 0; valid && i < kRestClientMaximumRequestBodyBytes; ++i)
            valid = r->request_body[i] == 0;
    }
    check(valid, "rest_partial_send_cancel_erases_unsent_suffix_in_shared_storage");
    RestStorageProbe headers;
    valid = headers.initialize(kHandshakeOutputBytes, true);
    const auto receive_text = [&](Utf8View text) noexcept {
        return response_plaintext_received(headers.runtime,
            {reinterpret_cast<const u8*>(text.data), text.bytes}, false);
    };
    for (u32 cycle = 0; valid && cycle < 2; ++cycle) {
        valid = headers.begin_request(true) &&
            receive_text(utf8_literal("HTTP/1.1 200 OK\r\nX-Padding: ")) == TlsStreamReadAction::continue_read;
        u8 padding[512];
        memset(padding, 'a', sizeof(padding));
        for (u32 part = 0; valid && part < 60; ++part)
            valid = response_plaintext_received(headers.runtime, {padding, sizeof(padding)}, false) ==
                TlsStreamReadAction::continue_read;
        valid = valid && receive_text(utf8_literal("\r\nContent-Length: 2\r\n\r\n{}")) == TlsStreamReadAction::complete &&
            headers.finish() && headers.runtime->completed.body_bytes == 2 &&
            bytes_equal(headers.runtime->response_body, "{}", 2) &&
            headers.runtime->response.parsed_head.head.header_bytes > headers.runtime->request_head_capacity;
        if (!valid) break;
        headers.consume();
        for (u32 byte = 0; valid && byte < kRestMaximumHeaderBytes; ++byte)
            valid = headers.runtime->response_input[byte] == 0;
    }
    check(valid, "rest_shared_headers_keep_response_capacity_and_clear_before_reuse");
    if (valid) {
        auto* r = headers.runtime;
        const RestRouteKey route{3, 0, RestMethod::get, RestMajorParameter::none};
        const auto acquired = rest_rate_acquire(r->scheduler, route, 1);
        valid = acquired.acquired_request() &&
            rest_transaction_acquire(&r->transaction, 3, acquired.lease, 1000, true) &&
            rest_transaction_begin_send(&r->transaction, r->request_head_capacity);
        memset(r->request_head, 0xA5, r->request_head_capacity);
        request_plaintext_committed(r, {r->request_head, r->request_head_capacity / 2});
        valid = valid && r->request_head[0] == 0 && r->request_head[r->request_head_capacity - 1] == 0xA5;
        headers.client.request_state = request_state_cancel_active;
        cancel_rate_lease(r);
        publish_request_error(&headers.client, r, RestClientError::send, 0);
        valid = valid && r->completed.error == RestClientError::cancelled;
        for (u32 byte = 0; valid && byte < kRestMaximumHeaderBytes; ++byte)
            valid = r->response_input[byte] == 0;
    }
    check(valid, "rest_partial_header_cancel_clears_unsent_header_suffix");
}

#if defined(MAICORD_REST_STORAGE_BENCHMARK)
bool rest_storage_benchmark() noexcept {
    const Clock clock = clock_initialize();
    const u32 uploads[]{0, kRestClientMaximumRequestBodyBytes};
    for (const u32 upload : uploads)
    for (const auto& fixture : rest_gzip_fixtures) {
        const u32 capacity = maximum(kHandshakeOutputBytes, fixture.decoded_bytes);
        for (u32 mode = 0; mode < 2; ++mode) for (u32 round = 0; round < 9; ++round) {
            const u32 calls = fixture.decoded_bytes >= 65536 ? 32 : 512;
            i64 elapsed = 0;
            u64 committed = 0;
            u32 page_faults = 0;
            RestStorageProbe warm;
            if (mode && !warm.initialize(capacity)) return false;
            for (u32 call = 0; call < calls; ++call) {
                RestStorageProbe cold;
                auto& probe = mode ? warm : cold;
                if ((!mode && !probe.initialize(capacity)) || !probe.prepare(fixture, true, false, upload)) return false;
                PROCESS_MEMORY_COUNTERS before{};
                if (call == 0) K32GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
                const i64 start = clock_now();
                if (!probe.finish()) return false;
                elapsed += clock_now() - start;
                if (call == 0) {
                    PROCESS_MEMORY_COUNTERS after{};
                    K32GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
                    page_faults = after.PageFaultCount - before.PageFaultCount;
                }
                if (!probe.correct(fixture)) return false;
                committed = probe.committed();
                probe.consume();
            }
            TraceRecord line("rest_gzip_storage");
            line.field("raw_bytes", fixture.decoded_bytes);
            line.field("request_bytes", upload);
            line.field("mode", mode ? "reuse" : "first");
            line.field("round", round);
            line.field("calls", calls);
            line.field("us", clock_microseconds(clock, 0, elapsed));
            line.field("committed", committed);
            line.field("first_call_faults", page_faults);
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
        }
    }
    return true;
}
#endif
} // namespace maicord

#if defined(MAICORD_REST_STORAGE_BENCHMARK)
extern "C" void maicord_rest_storage_entry() noexcept {
    ExitProcess(maicord::rest_storage_benchmark() ? 0 : 1);
}
#endif
