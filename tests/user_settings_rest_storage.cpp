// Offline controlled completion fixture over the shipping owner implementation.
// IOCP is real; HTTP plaintext/commit callbacks are driven explicitly. There
// are no credentials, worker threads, sockets, DNS queries or live requests.
#include "../src/app/rest_client.cpp"
#include "fixtures/rest_gzip_fixtures.hpp"
#include "maicord/protocol/writer.hpp"
#include "rest_client_probe.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool pass, const char* label) noexcept {
    test_check(&failures, pass, label);
}
bool freed(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION memory{};
    return VirtualQuery(pointer, &memory, sizeof(memory)) && memory.State == MEM_FREE;
}
struct Probe : RestClientProbe {
    RestClientRequest request(RestMethod method, u64 id, ConstSlice<u8> body = {}) noexcept {
        static constexpr char target[] = "/api/v10/users/@me/settings-proto/2";
        return {{reinterpret_cast<const u8*>(target), sizeof(target) - 1}, body,
            {kRestUserSettingsRouteId, 0, method, RestMajorParameter::none}, id, 0,
            method == RestMethod::patch, true};
    }
    bool begin_commits() noexcept {
        if (!signal()) return false;
        client.request_state = request_state_active;
        const auto acquired = rest_rate_acquire(runtime->scheduler, runtime->command.route, 1);
        if (!acquired.acquired_request() || !rest_transaction_acquire(&runtime->transaction,
            runtime->command.request_id, acquired.lease, 1000, true) ||
            !rest_transaction_begin_send(&runtime->transaction, runtime->command.body_bytes + 17)) return false;
        memset(runtime->request_head, 'h', 17);
        request_plaintext_committed(runtime, {runtime->request_head, 17});
        for (u32 offset = 0; offset < runtime->command.body_bytes; offset += 16384)
            request_plaintext_committed(runtime, {runtime->request_body + offset,
                minimum(16384u, runtime->command.body_bytes - offset)});
        return !runtime->write_commit_failed && runtime->transaction.phase == RestTransactionPhase::receiving &&
            zeros({runtime->request_body, runtime->command.body_bytes}) &&
            rest_response_initialize(&runtime->response, {runtime->response_input, kRestMaximumHeaderBytes},
                {runtime->response_body, runtime->response_capacity}, runtime->command.route.method);
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
            TlsStreamReadAction::complete && finalize_response(&client, runtime, runtime->response_step, 1);
    }
    bool gzip_response(const RestGzipFixture& gzip) noexcept {
        u8 head[192]{};
        ProtocolWriter writer{{head, sizeof(head)}, 0};
        protocol_writer_append_literal(&writer, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: ");
        protocol_writer_append_u64(&writer, gzip.bytes);
        protocol_writer_append_literal(&writer, "\r\n\r\n");
        return response_plaintext_received(runtime, {head, writer.used}, false) == TlsStreamReadAction::continue_read &&
            response_plaintext_received(runtime, {gzip.encoded, gzip.bytes}, false) == TlsStreamReadAction::complete &&
            finalize_response(&client, runtime, runtime->response_step, 1);
    }
};
}

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
    r->authorization = RestAuthorization::bearer;
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
    for (u32 cycle = 0; cycle < 2; ++cycle) {
        auto request = cycle ? probe.request(RestMethod::get, 1002) : patch;
        const auto submitted = rest_client_submit_user_settings(&probe.client, request, 2 * 1024 * 1024);
        const void* bank = r->extended_body;
        check(submitted.ok() && bank && r->request_body == r->response_body &&
            r->extended_body_bytes == 2 * 1024 * 1024 && r->allocation_bytes == baseline &&
            (cycle || bytes_equal(r->request_body, payload, sizeof(payload))),
            "large settings admission copies into bounded transaction owned alias without fixed growth");
        if (!submitted.ok()) return failures + 1;
        check(rest_client_submit_user_settings(&probe.client, request, 2 * 1024 * 1024).error ==
            RestClientSubmitError::busy && r->extended_body == bank,
            "busy settings admission cannot replace existing bank");
        check(probe.begin_commits() && probe.plain_response({payload, sizeof(payload)}),
            "large PATCH commit and chunked GET use existing callback pipeline");
        RestClientResponse response{};
        const auto small = rest_client_take_response(&probe.client, &response, {output, 16});
        check(small.error == RestClientTakeError::output_too_small && small.body_bytes == sizeof(payload) &&
            probe.client.request_state == request_state_ready && r->extended_body == bank &&
            !rest_client_cancel(&probe.client, request.request_id),
            "short output and late cancellation retain ready exact ticket and bank");
        const auto take = rest_client_take_response(&probe.client, &response, {output, sizeof(output)});
        check(take.ok() && response.request_id == request.request_id && response.body_bytes == sizeof(payload) &&
            bytes_equal(output, payload, sizeof(payload)) && !r->extended_body && freed(bank) &&
            r->request_body == r->default_body && r->response_body == r->default_body &&
            r->response_capacity == 256 * 1024 && r->request_body_capacity == 64 * 1024 &&
            probe.client.request_state == request_state_empty && !probe.client.current_request_id,
            "consumption returns exact large response and frees bank before idle restoring default capacities");
    }
    // A retained handshake-backed inflater must grow before a later large response.
    for (u32 cycle = 0; cycle < 2; ++cycle) {
        const auto request = probe.request(RestMethod::get, 2001 + cycle);
        const u32 capacity = cycle ? 2 * 1024 * 1024 : 256 * 1024;
        const auto& fixture = rest_gzip_fixtures[cycle ? 5 : 1];
        const auto submit = rest_client_submit_user_settings(&probe.client, request, capacity);
        check(submit.ok() && probe.begin_commits() && probe.gzip_response(fixture) &&
            r->decompression_capacity >= capacity &&
            (cycle ? r->decompression != r->handshake_output : r->decompression == r->handshake_output),
            "retained inflater grows from handshake scratch to bounded settings capacity");
        if (probe.client.request_state == request_state_ready) release_completed_response(&probe.client, r, r->completed.body_bytes);
        check(!r->extended_body && r->decompression_capacity <= 256 * 1024,
            "large transaction inflater allocation released at consumption");
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
    check(rest_client_take_response(&probe.client, &response, {}).ok() && !r->extended_body && freed(cancelled_bank),
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
    check(r->completed.request_id == 3002 && r->completed.error == RestClientError::cancelled &&
        r->extended_body == pending_bank && zeros({r->extended_body, r->extended_body_bytes}) &&
        rest_client_take_response(&probe.client, &response, {}).ok() && freed(pending_bank),
        "pending cancellation publishes exact ticket without connecting and releases after take");
    // Opt-in large bodies (image data URIs, multipart uploads): a bank exists
    // only for an oversized body, the response keeps its configured capacity,
    // and small bodies stay on the fixed arena.
    {
        static constexpr char target[] = "/api/v10/guilds/10";
        static constexpr char boundary[] = "MaiCordFormBoundary0000000000001";
        RestClientRequest large{{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                                {payload, sizeof(payload)},
                                {0x4741535345540000ull, 10, RestMethod::patch,
                                 RestMajorParameter::guild},
                                5001, 120'000, true, false};
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
        const auto take = rest_client_take_response(&probe.client, &large_response, {output, sizeof(output)});
        check(take.ok() && large_response.request_id == 5001 && large_response.body_bytes == 4096 && !r->extended_body &&
                  freed(bank) && r->request_body == r->default_body && r->request_body_capacity == 64 * 1024 &&
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
        probe.signal(), "joined teardown fixture retains pending bank");
    const void* destroy_bank = r->extended_body;
    close_transport(r);
    check(drained(r), "teardown connection has drained every borrow");
    destroy_client_storage(&probe.client, nullptr);
    probe.runtime = nullptr;
    check(!probe.client.runtime && freed(destroy_bank), "joined storage teardown releases pending extended bank");
    return failures;
}
}

extern "C" void maicord_user_settings_rest_storage_entry() noexcept {
    ExitProcess(maicord::user_settings_rest_storage_fixture() ? 1u : 0u);
}
