// Offline harness over the shipping REST owner. This header includes
// "../src/app/rest_client.cpp" once per translation unit, so fixtures that
// share one unit compile the owner once; it drives the owner's private storage
// and transport helpers. IOCP and its mailbox are real; plaintext responses
// are supplied at the existing transport callback boundary. No credential,
// worker thread, socket, DNS lookup or live HTTP is created.
#pragma once

#include "../src/app/rest_client.cpp"
#include "maicord/protocol/writer.hpp"

namespace maicord {
namespace {

inline bool zeros(ConstSlice<u8> bytes) noexcept {
    for (u32 i = 0; i < bytes.count; ++i)
        if (bytes[i])
            return false;
    return true;
}

struct RestClientProbe {
    RestClient client{};
    RestClientRuntime* runtime{};
    bool initialize(u32 capacity = kRestClientMinimumResponseBytes) noexcept {
        const u32 bytes = allocation_bytes(capacity, false, true);
        runtime = static_cast<RestClientRuntime*>(
            VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!runtime)
            return false;
        client.runtime = runtime;
        runtime->owner = &client;
        runtime->allocation_bytes = bytes;
        runtime->response_capacity = runtime->default_response_capacity = capacity;
        runtime->request_body_capacity = kRestClientMaximumRequestBodyBytes;
        runtime->authorization = RestAuthorization::normal_user;
        runtime->clock = clock_initialize();
        assign_storage(runtime);
        runtime->default_body = runtime->request_body;
        client.phase = static_cast<LONG>(RestClientPhase::idle);
        client.response_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // This event is an admission sentinel, never used as a thread handle.
        client.worker.thread = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return client.response_event && client.worker.thread &&
               network_loop_initialize(&client.worker.loop).ok() &&
               tls_stream_initialize(&runtime->transport, L"discord.com",
                                     {{runtime->encrypted_storage, kEncryptedInputBytes},
                                      {runtime->handshake_output, kHandshakeOutputBytes},
                                      {runtime->record_output, kRecordOutputBytes}});
    }
    ~RestClientProbe() noexcept {
        if (runtime) {
            runtime->transport.outstanding_operations = 0;
            close_transport(runtime);
        }
        network_loop_release(&client.worker.loop);
        if (client.worker.thread)
            CloseHandle(client.worker.thread);
        if (client.response_event)
            CloseHandle(client.response_event);
        destroy_client_storage(&client, nullptr);
    }
    // Consumes the request signal posted by an admitted submission.
    bool signal() noexcept {
        const auto polled = network_loop_poll(&client.worker.loop, 1000);
        return polled.state == NetworkPollState::completion &&
               polled.completion.signal == &runtime->request_signal;
    }
    RestClientRequest retained() const noexcept {
        return {{runtime->request_target, runtime->command.target_bytes},
                {runtime->request_body, runtime->command.body_bytes},
                runtime->command.route,
                runtime->command.request_id,
                runtime->command.timeout_ms,
                runtime->command.json_body};
    }
    bool receive(u16 status, Utf8View body, bool rate = false) noexcept {
        if (!signal() || reject_restricted_request(&client, runtime))
            return false;
        client.request_state = request_state_active;
        const auto lease = rest_rate_acquire(runtime->scheduler, runtime->command.route, 1);
        if (!lease.acquired_request() ||
            !rest_transaction_acquire(&runtime->transaction, runtime->command.request_id, lease.lease,
                                      10'000, false) ||
            !rest_transaction_begin_send(&runtime->transaction, runtime->command.body_bytes))
            return false;
        request_plaintext_committed(runtime, {runtime->request_body, runtime->command.body_bytes});
        if (!zeros({runtime->request_body, runtime->command.body_bytes}) ||
            !rest_response_initialize(
                &runtime->response, {runtime->response_input, kRestMaximumHeaderBytes},
                {runtime->response_body, runtime->response_capacity}, RestMethod::post))
            return false;
        u8 bytes[384]{};
        ProtocolWriter writer{{bytes, sizeof(bytes)}, 0};
        protocol_writer_append_literal(&writer, "HTTP/1.1 ");
        protocol_writer_append_u64(&writer, status);
        protocol_writer_append_literal(&writer,
                                       " Fixture\r\nContent-Type: application/json\r\nContent-Length: ");
        protocol_writer_append_u64(&writer, body.bytes);
        if (rate)
            protocol_writer_append_literal(&writer, "\r\nRetry-After: 0.25");
        protocol_writer_append_literal(&writer, "\r\n\r\n");
        auto step = response_plaintext_received(runtime, {bytes, writer.used}, false);
        if (body.bytes)
            step = response_plaintext_received(runtime,
                                               {reinterpret_cast<const u8*>(body.data), body.bytes}, false);
        return step == TlsStreamReadAction::complete &&
               finalize_response(&client, runtime, runtime->response_step, 1);
    }
};

} // namespace
} // namespace maicord
