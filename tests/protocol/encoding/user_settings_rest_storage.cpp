// Offline controlled completion fixture over the shipping owner implementation.
// IOCP is real; HTTP plaintext/commit callbacks are driven explicitly. There
// are no credentials, worker threads, sockets, DNS queries or live requests.
// Part of rest_client_fixtures.cpp.
#include "../../support/rest_client_probe.hpp"
#include "../../fixtures/rest_gzip_fixtures.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/media/image/png_encode.hpp"
#include "../../support/test_output.hpp"
#include "../../support/released_memory.hpp"

namespace maicord {
namespace user_settings_storage {
u32 failures;
void check(bool pass, const char* label) noexcept {
    test_check(&failures, pass, label);
}
struct Probe : RestClientProbe {
    RestClientRequest request(RestMethod method, u64 id, ConstSlice<u8> body = {}) noexcept {
        static constexpr char target[] = "/api/v9/users/@me/settings-proto/2";
        return {{reinterpret_cast<const u8*>(target), sizeof(target) - 1}, body,
            {kRestUserSettingsRouteId, 0, method, RestMajorParameter::none}, id, 0,
            method == RestMethod::patch};
    }
    bool begin_commits() noexcept {
        if (!signal()) return false;
        client.request_state = request_state_active;
        const auto acquired = rest_rate_acquire(runtime->scheduler, runtime->command.route, 1);
        if (!acquired.acquired_request()) return false;
        rest_transaction_acquire(&runtime->transaction, runtime->command.request_id, acquired.lease, 1000, true);
        rest_transaction_begin_send(&runtime->transaction, runtime->command.body_bytes + 17);
        memset(runtime->request_head, 'h', 17);
        request_plaintext_committed(runtime, {runtime->request_head, 17});
        for (u32 offset = 0; offset < runtime->command.body_bytes; offset += 16384)
            request_plaintext_committed(runtime, {runtime->request_body + offset,
                minimum(16384u, runtime->command.body_bytes - offset)});
        rest_response_initialize(&runtime->response, {runtime->response_input, kRestMaximumHeaderBytes},
            {runtime->response_body, runtime->response_capacity}, runtime->command.route.method);
        return runtime->transaction.phase == RestTransactionPhase::receiving &&
            zeros({runtime->request_body, runtime->command.body_bytes});
    }
    bool plain_response(ConstSlice<u8> payload) noexcept {
        constexpr char head[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n";
        if (response_plaintext_received(runtime, {reinterpret_cast<const u8*>(head), sizeof(head) - 1}, false) !=
            TlsStreamReadAction::continue_read) return false;
        // Deliberately split size lines and trailers across plaintext callbacks.
        for (u32 offset = 0; offset < payload.count;) {
            const u32 count = minimum(8192u, payload.count - offset);
            char line[16]{};
            u32 digits = 0, value = count;
            do { line[digits++] = "0123456789abcdef"[value & 15]; value >>= 4; } while (value);
            for (u32 i = 0; i < digits / 2; ++i) { char c = line[i]; line[i] = line[digits - i - 1]; line[digits - i - 1] = c; }
            for (u32 i = 0; i < digits; ++i)
                if (response_plaintext_received(runtime, {reinterpret_cast<const u8*>(line + i), 1}, false) !=
                    TlsStreamReadAction::continue_read) return false;
            constexpr u8 crlf[]{'\r','\n'};
            if (response_plaintext_received(runtime, {crlf, 2}, false) != TlsStreamReadAction::continue_read ||
                response_plaintext_received(runtime, {payload.data + offset, count}, false) != TlsStreamReadAction::continue_read ||
                response_plaintext_received(runtime, {crlf, 2}, false) != TlsStreamReadAction::continue_read) return false;
            offset += count;
        }
        constexpr char end[] = "0\r\n\r\n";
        return response_plaintext_received(runtime, {reinterpret_cast<const u8*>(end), sizeof(end) - 1}, false) ==
            TlsStreamReadAction::complete && finalize_response(&client, runtime, 1);
    }
    bool compressed_response(ConstSlice<u8> encoded, bool gzip) noexcept {
        u8 head[192]{};
        ProtocolWriter writer{{head, sizeof(head)}, 0};
        protocol_writer_append_literal(&writer, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: ");
        if (gzip) protocol_writer_append_literal(&writer, "gzip");
        else protocol_writer_append_literal(&writer, "deflate");
        protocol_writer_append_literal(&writer, "\r\nContent-Length: ");
        protocol_writer_append_u64(&writer, encoded.count);
        protocol_writer_append_literal(&writer, "\r\n\r\n");
        return response_plaintext_received(runtime, {head, writer.used}, false) == TlsStreamReadAction::continue_read &&
            response_plaintext_received(runtime, encoded, false) == TlsStreamReadAction::complete &&
            finalize_response(&client, runtime, 1);
    }
    bool gzip_response(const RestGzipFixture& gzip) noexcept {
        return compressed_response({gzip.encoded, gzip.bytes}, true);
    }
};
u32 user_settings_rest_storage_fixture() noexcept {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    static u8 payload[1017200], output[sizeof(payload)];
    memset(payload, 'a', sizeof(payload)); payload[0] = '"'; payload[sizeof(payload) - 1] = '"';
    Probe probe;
    check(probe.initialize(256 * 1024), "settings owner fixture initializes without network work");
    if (!probe.runtime || !probe.client.worker.thread) return failures + 1;
    auto* r = probe.runtime;
    const u32 baseline = r->allocation_bytes;
    const auto patch = probe.request(RestMethod::patch, 1001, {payload, sizeof(payload)});
    check(rest_client_submit(&probe.client, patch).error == RestClientSubmitError::body_capacity &&
        !r->extended_body && probe.client.request_state == request_state_empty,
        "ordinary admission keeps 64KiB body ceiling");
    auto wrong = patch;
    wrong.route.route_id++;
    check(rest_client_submit_user_settings(&probe.client, wrong, 2 * 1024 * 1024).error ==
        RestClientSubmitError::invalid_input, "settings normalized route identity fenced");
    wrong = patch; wrong.route.method = RestMethod::post;
    check(rest_client_submit_user_settings(&probe.client, wrong, 2 * 1024 * 1024).error ==
        RestClientSubmitError::invalid_input, "settings mutation method fenced");
    wrong = patch; wrong.route.major_key = 1;
    check(rest_client_submit_user_settings(&probe.client, wrong, 2 * 1024 * 1024).error ==
        RestClientSubmitError::invalid_input, "settings major parameter fenced");
    wrong = patch; wrong.target.count--;
    check(rest_client_submit_user_settings(&probe.client, wrong, 2 * 1024 * 1024).error ==
        RestClientSubmitError::invalid_input, "settings exact origin target fenced");
    r->authorization = RestAuthorization::none;
    check(rest_client_submit_user_settings(&probe.client, patch, 2 * 1024 * 1024).error ==
        RestClientSubmitError::invalid_input, "settings normal user authorization fenced");
    r->authorization = RestAuthorization::normal_user;
    check(rest_client_submit_user_settings(&probe.client, patch, kRestClientMaximumResponseBytes + 1).error ==
        RestClientSubmitError::invalid_input, "settings capacity bound fenced");
    wrong = patch; wrong.body.count = kRestUserSettingsMaximumBodyBytes + 1;
    check(rest_client_submit_user_settings(&probe.client, wrong, 2 * 1024 * 1024).error ==
        RestClientSubmitError::body_capacity && !r->extended_body,
        "settings oversized body rejects before copying or allocation");
    wrong = patch; wrong.route.method = RestMethod::get;
    check(rest_client_submit_user_settings(&probe.client, wrong, 2 * 1024 * 1024).error ==
        RestClientSubmitError::invalid_input, "settings GET cannot submit a mutation body");
    // The PATCH body leaves the fixed arena for a bank. The GET response commits more of the default body
    // as its bytes arrive, and consumption decommits it again.
    const u32 default_body = r->default_body_bytes;
    const auto grown_released = [&]() noexcept {
        MEMORY_BASIC_INFORMATION region{};
        return r->body_committed == default_body &&
            VirtualQuery(r->default_body + default_body, &region, sizeof(region)) && region.State == MEM_RESERVE &&
            region.AllocationBase == r->default_body && default_body + region.RegionSize == kRestClientMaximumLargeResponseBytes;
    };
    for (u32 cycle = 0; cycle < 2; ++cycle) {
        auto request = cycle ? probe.request(RestMethod::get, 1002) : patch;
        const auto submitted = rest_client_submit_user_settings(&probe.client, request, 2 * 1024 * 1024);
        const void* bank = r->extended_body;
        check(submitted.ok() && r->request_body == r->response_body && r->response_capacity == 2 * 1024 * 1024 &&
            r->allocation_bytes == baseline && r->body_committed == default_body &&
            (cycle ? !bank && r->request_body == r->default_body
                   : bank && r->extended_body_bytes == 2 * 1024 * 1024 &&
                         bytes_equal(r->request_body, payload, sizeof(payload))),
            "large settings admission copies a large body into a bank and commits nothing ahead of a large response");
        if (!submitted.ok()) return failures + 1;
        check(rest_client_submit_user_settings(&probe.client, request, 2 * 1024 * 1024).error ==
            RestClientSubmitError::busy && r->extended_body == bank,
            "busy settings admission cannot replace existing bank");
        check(probe.begin_commits() && probe.plain_response({payload, sizeof(payload)}),
            "large PATCH commit and chunked GET use existing callback pipeline");
        check(r->body_committed == (cycle ? (sizeof(payload) + 65535) & ~65535u : default_body),
            "a large response commits the default body in 64 KiB steps as its bytes arrive");
        RestClientResponse response{};
        const auto small = rest_client_take_response(&probe.client, &response, {output, 16});
        check(small.error == RestClientTakeError::output_too_small && small.body_bytes == sizeof(payload) &&
            probe.client.request_state == request_state_ready && r->extended_body == bank &&
            !rest_client_cancel(&probe.client, request.request_id),
            "short output and late cancellation retain ready exact ticket and bank");
        const auto bank_mark = memory_mark(bank);
        const auto take = rest_client_take_response(&probe.client, &response, {output, sizeof(output)});
        check(take.ok() && response.request_id == request.request_id && response.body_bytes == sizeof(payload) &&
            bytes_equal(output, payload, sizeof(payload)) && !r->extended_body &&
            (cycle || memory_released(bank_mark)) && grown_released() &&
            r->request_body == r->default_body && r->response_body == r->default_body &&
            r->response_capacity == 256 * 1024 && r->request_body_capacity == 64 * 1024 &&
            probe.client.request_state == request_state_empty && !probe.client.current_request_id,
            "consumption returns exact large response, frees bank or grown pages and restores default capacities");
    }
    // A gzip body commits through the size at its end. A body that inflates past the committed default body
    // without one (deflate) commits the capacity and inflates again.
    for (u32 cycle = 0; cycle < 2; ++cycle) {
        const auto request = probe.request(RestMethod::get, 2001 + cycle);
        const u32 capacity = cycle ? 2 * 1024 * 1024 : 256 * 1024;
        const auto& fixture = rest_gzip_fixtures[cycle ? 5 : 1];
        const auto submit = rest_client_submit_user_settings(&probe.client, request, capacity);
        check(submit.ok() && probe.begin_commits() && probe.gzip_response(fixture) &&
            r->completed.body_bytes == fixture.decoded_bytes && r->decompression == r->handshake_output &&
            r->body_committed == (cycle ? fixture.decoded_bytes : default_body),
            "a gzip large response keeps the handshake scratch and grows the body to its stated size");
        if (probe.client.request_state == request_state_ready) release_completed_response(&probe.client, r);
        check(!r->extended_body && r->decompression_capacity <= 256 * 1024 && grown_released(),
            "large transaction inflater and grown body released at consumption");
    }
    {
        alignas(4) static u8 workspace[kZlibCompressWorkspaceBytes];
        const u32 bound = static_cast<u32>(zlib_compress_bound(sizeof(payload)));
        auto* deflated = static_cast<u8*>(VirtualAlloc(nullptr, bound, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        const u32 deflated_bytes = deflated ? zlib_compress({payload, sizeof(payload)}, {workspace, sizeof(workspace)},
                                                            {deflated, bound}) : 0;
        const auto request = probe.request(RestMethod::get, 2003);
        check(deflated_bytes && rest_client_submit_user_settings(&probe.client, request, 2 * 1024 * 1024).ok() &&
            probe.begin_commits() && probe.compressed_response({deflated, deflated_bytes}, false) &&
            r->completed.body_bytes == sizeof(payload) && bytes_equal(r->response_body, payload, sizeof(payload)) &&
            r->body_committed == 2 * 1024 * 1024,
            "a deflate response past the default body commits the capacity and inflates again");
        if (probe.client.request_state == request_state_ready) release_completed_response(&probe.client, r);
        check(grown_released(), "the deflate response's grown body is released at consumption");
        if (deflated) VirtualFree(deflated, 0, MEM_RELEASE);
    }
    const auto cancelled = probe.request(RestMethod::patch, 3001, {payload, sizeof(payload)});
    check(rest_client_submit_user_settings(&probe.client, cancelled, 2 * 1024 * 1024).ok() && probe.signal(),
        "cancel fixture admitted offline");
    const void* cancelled_bank = r->extended_body;
    probe.client.request_state = request_state_active;
    r->transaction.phase = RestTransactionPhase::sending;
    r->transaction.request_bytes = sizeof(payload);
    request_plaintext_committed(r, {r->request_body, 16384});
    r->transport.outstanding_operations = 1;
    check(!rest_client_cancel(&probe.client, 3000) && rest_client_cancel(&probe.client, 3001),
        "active cancellation claims only exact request ticket");
    cancel_request(&probe.client, r);
    finish_drain(&probe.client, r);
    check(r->draining && r->extended_body == cancelled_bank && probe.client.request_state == request_state_cancel_active,
        "ambiguous partial write retains bank through outstanding transport drain");
    r->transport.outstanding_operations = 0;
    finish_drain(&probe.client, r);
    check(r->completed.request_id == 3001 && r->completed.error == RestClientError::cancelled &&
        r->completed.body_bytes == 0 && zeros({r->extended_body, r->extended_body_bytes}),
        "drained cancellation publishes exact ticket and erases entire unsent suffix");
    RestClientResponse response{};
    const auto cancelled_mark = memory_mark(cancelled_bank);
    check(rest_client_take_response(&probe.client, &response, {}).ok() && !r->extended_body && memory_released(cancelled_mark),
        "cancelled response consumption frees bank");
    // Consume the cancel IOCP signal before exercising another pending ticket.
    const auto cancelled_signal = network_loop_poll(&probe.client.worker.loop, 1000);
    check(cancelled_signal.state == NetworkPollState::completion &&
        cancelled_signal.completion.signal == &r->cancel_signal, "active cancel notification delivered on existing loop");
    const auto pending = probe.request(RestMethod::patch, 3002, {payload, sizeof(payload)});
    check(rest_client_submit_user_settings(&probe.client, pending, 2 * 1024 * 1024).ok() &&
        rest_client_cancel(&probe.client, 3002) && probe.signal(),
        "queued large cancellation claims pending ticket before activation");
    const void* pending_bank = r->extended_body;
    activate_request(&probe.client, r, &probe.client.worker.loop, 1);
    const auto pending_mark = memory_mark(pending_bank);
    check(r->completed.request_id == 3002 && r->completed.error == RestClientError::cancelled &&
        r->extended_body == pending_bank && zeros({r->extended_body, r->extended_body_bytes}) &&
        rest_client_take_response(&probe.client, &response, {}).ok() && memory_released(pending_mark),
        "pending cancellation publishes exact ticket without connecting and releases after take");
    // Opt-in large bodies (image data URIs, multipart uploads): a bank exists
    // only for an oversized body, the response keeps its configured capacity,
    // and small bodies stay on the fixed arena.
    {
        static constexpr char target[] = "/api/v9/guilds/10";
        static constexpr char boundary[] = "MaiCordFormBoundary0000000000001";
        RestClientRequest large{{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                                {payload, sizeof(payload)},
                                {0x4741535345540000ull, 10, RestMethod::patch,
                                 RestMajorParameter::guild},
                                5001, 120'000, true};
        check(rest_client_submit(&probe.client, large).error == RestClientSubmitError::body_capacity &&
                  !r->extended_body,
              "ordinary submit keeps the 64 KiB ceiling for the same body");
        auto oversized = large;
        oversized.body.count = kRestClientMaximumLargeRequestBodyBytes + 1;
        check(rest_client_submit_large(&probe.client, oversized).error ==
                      RestClientSubmitError::body_capacity &&
                  !r->extended_body && probe.client.request_state == request_state_empty,
              "large bodies over 12 MiB are refused before allocation");
        auto bad = large;
        bad.multipart_boundary = {reinterpret_cast<const u8*>("bad boundary"), 12};
        check(rest_client_submit_large(&probe.client, bad).error == RestClientSubmitError::invalid_input,
              "multipart boundaries outside [A-Za-z0-9_-] are refused");
        bad.multipart_boundary = {reinterpret_cast<const u8*>(boundary), sizeof(boundary) - 1};
        bad.json_body = false;
        check(rest_client_submit_large(&probe.client, bad).error == RestClientSubmitError::invalid_input,
              "a multipart boundary needs a body");
        large.multipart_boundary = {reinterpret_cast<const u8*>(boundary), sizeof(boundary) - 1};
        const auto submitted = rest_client_submit_large(&probe.client, large);
        const void* bank = r->extended_body;
        check(submitted.ok() && bank && r->extended_body_bytes == sizeof(payload) && r->request_body == bank &&
                  r->response_body == bank && r->response_capacity == 256 * 1024 &&
                  r->allocation_bytes == baseline && bytes_equal(r->request_body, payload, sizeof(payload)) &&
                  r->command.multipart_boundary_bytes == sizeof(boundary) - 1 &&
                  bytes_equal(r->multipart_boundary, boundary, sizeof(boundary) - 1) &&
                  r->command.timeout_ms == 120'000,
              "a large body is copied into a transaction bank sized for it, with the boundary and timeout");
        check(submitted.ok() && probe.begin_commits() && probe.plain_response({payload, 4096}),
              "the large upload commits through the ordinary write pipeline and receives a response");
        RestClientResponse large_response{};
        const auto bank_mark = memory_mark(bank);
        const auto take = rest_client_take_response(&probe.client, &large_response, {output, sizeof(output)});
        check(take.ok() && large_response.request_id == 5001 && large_response.body_bytes == 4096 && !r->extended_body &&
                  memory_released(bank_mark) && r->request_body == r->default_body && r->request_body_capacity == 64 * 1024 &&
                  !r->command.multipart_boundary_bytes && zeros({r->multipart_boundary, sizeof(r->multipart_boundary)}),
              "taking the response erases and releases the bank and restores the fixed arena");
        auto small = large;
        small.request_id = 5002;
        small.body.count = 1024;
        check(rest_client_submit_large(&probe.client, small).ok() && !r->extended_body &&
                  r->request_body == r->default_body && probe.signal() &&
                  rest_client_cancel(&probe.client, 5002),
              "a small body on the large entry stays on the fixed arena");
        activate_request(&probe.client, r, &probe.client.worker.loop, 1);
        check(rest_client_take_response(&probe.client, &large_response, {}).ok() && large_response.request_id == 5002,
              "the small large-entry request cancels like any other");
    }
    // Make signal posting fail without a worker and verify synchronous rollback.
    const auto loop = probe.client.worker.loop;
    probe.client.worker.loop = {};
    check(rest_client_submit_user_settings(&probe.client, cancelled, 2 * 1024 * 1024).error ==
        RestClientSubmitError::signal && !r->extended_body && probe.client.request_state == request_state_empty &&
        r->request_body == r->default_body && r->response_capacity == 256 * 1024,
        "failed signal submission erases releases and restores bank before idle");
    probe.client.worker.loop = loop;
    // Destroy after the connection owner has no outstanding borrows: pending
    // or ready transactions need not be taken before joined storage teardown.
    const auto destroy_request = probe.request(RestMethod::get, 4001);
    check(rest_client_submit_user_settings(&probe.client, destroy_request, 2 * 1024 * 1024).ok() &&
        probe.signal() && commit_response_body(r, 1024 * 1024) && r->body_committed == 1024 * 1024,
        "joined teardown fixture retains a pending response that grew its body");
    const auto destroy_bank = memory_mark(r->default_body);
    close_transport(r);
    check(tls_stream_drained(&r->transport), "teardown connection has drained every borrow");
    destroy_client_storage(&probe.client);
    probe.runtime = nullptr;
    check(!probe.client.runtime && memory_released(destroy_bank),
        "joined storage teardown releases the body reservation of a pending extended response");
    return failures;
}
} // namespace user_settings_storage
} // namespace maicord
