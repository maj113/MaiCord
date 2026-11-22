// A big bot's application command index over the shipping REST owner. Discord sends it gzip-compressed and chunked,
// and it inflates past the 256 KiB response the details client is configured for. IOCP is real; plaintext reaches
// the transport callbacks directly, with no credential, worker thread, socket or live request.
// Part of rest_client_fixtures.cpp.
#include "../../../support/rest_client_probe.hpp"
#include "../../../fixtures/large_command_index.hpp"
#include "../../../../src/app/chat/composer/command_request.hpp"
#include "maicord/app/account/user_account_coordinator.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/crc32.hpp"
#include "maicord/media/image/png_encode.hpp"
#include "../../../support/test_output.hpp"

namespace maicord {
namespace command_index_transport {
u32 run() noexcept {
    u32 failures = 0;
    static u8 json[kLargeCommandIndexBytes], gzip[kLargeCommandIndexBytes], output[kLargeCommandIndexBytes];
    alignas(4) static u8 workspace[kZlibCompressWorkspaceBytes];
    const u32 json_bytes = write_large_command_index({json, sizeof(json)});
    // A gzip member around the deflate stream of a zlib one written 8 bytes in: the gzip header covers the zlib
    // header, and the CRC-32 and size replace the Adler-32.
    const u32 zlib_bytes =
        json_bytes ? zlib_compress({json, json_bytes}, {workspace, sizeof(workspace)}, {gzip + 8, sizeof(gzip) - 16})
                   : 0;
    constexpr u8 header[]{0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    copy_bytes(gzip, header, sizeof(header));
    byte_store_le32(gzip + zlib_bytes + 4, crc32_extend(0, json, json_bytes));
    byte_store_le32(gzip + zlib_bytes + 8, json_bytes);
    const u32 gzip_bytes = zlib_bytes + 12;
    RestClientProbe probe;
    if (zlib_bytes < 6 || !probe.initialize(kUserAccountAuthorizedResponseBytes)) {
        test_check(&failures, false, "command index transport: fixture and owner initialize");
        return failures;
    }
    auto* r = probe.runtime;
    // A bot's profile card in a DM asks for the DM's index.
    constexpr auto target = utf8_literal("/api/v9/channels/9/application-command-index");
    test_check(&failures,
               app_detail::submit_command_index(&probe.client, false, 0, 9, 4001).ok() &&
                   utf8_equal({reinterpret_cast<const char*>(r->request_target), r->command.target_bytes}, target) &&
                   r->command.route.route_id == app_detail::kCommandIndexRouteId &&
                   r->command.route.method == RestMethod::get && r->command.route.major == RestMajorParameter::channel &&
                   r->command.route.major_key == 9 && !r->command.body_bytes,
               "a DM's command index is a GET of the channel's index");
    const auto lease = rest_rate_acquire(r->scheduler, r->command.route, 1);
    probe.client.request_state = request_state_active;
    bool delivered = probe.signal() && lease.acquired_request() &&
                     rest_transaction_acquire(&r->transaction, 4001, lease.lease, 10'000, true) &&
                     rest_transaction_begin_send(&r->transaction, 17);
    memset(r->request_head, 'h', 17);
    request_plaintext_committed(r, {r->request_head, 17});
    delivered = delivered && rest_response_initialize(&r->response, {r->response_input, kRestMaximumHeaderBytes},
                                                      {r->response_body, r->response_capacity}, RestMethod::get);
    const auto feed = [&](const void* bytes, u32 count) noexcept {
        return response_plaintext_received(r, {static_cast<const u8*>(bytes), count}, false);
    };
    constexpr char head[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nContent-Encoding: gzip\r\n\r\n";
    auto action = delivered ? feed(head, sizeof(head) - 1) : TlsStreamReadAction::reject;
    for (u32 offset = 0; action == TlsStreamReadAction::continue_read && offset < gzip_bytes; offset += 4096) {
        const u32 count = minimum(4096u, gzip_bytes - offset);
        constexpr char hex[] = "0123456789abcdef";
        const char line[]{hex[(count >> 12) & 15], hex[(count >> 8) & 15], hex[(count >> 4) & 15], hex[count & 15],
                          '\r', '\n'};
        action = feed(line, sizeof(line));
        action = action == TlsStreamReadAction::continue_read ? feed(gzip + offset, count) : action;
        action = action == TlsStreamReadAction::continue_read ? feed("\r\n", 2) : action;
    }
    action = action == TlsStreamReadAction::continue_read ? feed("0\r\n\r\n", 5) : action;
    RestClientResponse response{};
    delivered = action == TlsStreamReadAction::complete &&
                finalize_response(&probe.client, r, r->response_step, 1) &&
                rest_client_take_response(&probe.client, &response, {output, sizeof(output)}).ok() &&
                response.status_code == 200 && response.body_bytes == json_bytes &&
                bytes_equal(output, json, json_bytes);
    test_check(&failures, delivered,
               "a big bot's gzip command index inflates whole past the configured 256 KiB response");
    return failures;
}
} // namespace command_index_transport
} // namespace maicord
