// Offline capture of user-action CAPTCHA retries on the shipping REST owner:
// armed X-Captcha-* headers ride exactly one matching submission, and the
// invite accept adds its session body and X-Context-Properties header.
// No worker thread, socket, DNS lookup or live HTTP is created.
#include "../src/app/rest_client.cpp"
#include "maicord/app/account_guild_invite.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/guild_invite.hpp"
#include "rest_client_probe.hpp"
#include "test_output.hpp"

namespace maicord {
namespace action_captcha_transport {
u32 failures;
void check(bool pass, const char* label) noexcept {
    test_check(&failures, pass, label);
}
bool contains(ConstSlice<u8> haystack, Utf8View needle) noexcept {
    for (u32 i = 0; needle.bytes && i + needle.bytes <= haystack.count; ++i)
        if (bytes_equal(haystack.data + i, needle.data, needle.bytes))
            return true;
    return false;
}
struct Probe : RestClientProbe {
    // Drops the admitted command as if it had been sent, so another submit fits.
    void drain() noexcept {
        network_loop_poll(&client.worker.loop, 0);
        erase_command(runtime);
        client.request_state = request_state_empty;
    }
    ConstSlice<u8> headers() const noexcept {
        return {runtime->extra_headers, runtime->command.extra_headers_bytes};
    }
    RestClientSubmitResult send(u64 id, ConstSlice<u8> extra = {}) noexcept {
        static constexpr char target[] = "/api/v10/channels/1/messages";
        static constexpr char body[] = "{}";
        return rest_client_submit(&client, {{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
                                            {reinterpret_cast<const u8*>(body), 2},
                                            {0x4d534731, 1, RestMethod::post,
                                             RestMajorParameter::channel},
                                            id,
                                            kRestClientDefaultTimeoutMs,
                                            true,
                                            false,
                                            {},
                                            extra});
    }
};

constexpr Utf8View kCaptcha = utf8_literal("X-Captcha-Key: P1_solved.token\r\nX-Captcha-Rqtoken: rq.abc\r\n"
                                           "X-Captcha-Session-Id: 3f2a\r\n");
ConstSlice<u8> bytes(Utf8View text) noexcept {
    return {reinterpret_cast<const u8*>(text.data), text.bytes};
}

void retry() noexcept {
    Probe p;
    if (!p.initialize()) {
        check(false, "action captcha: REST owner initializes offline");
        return;
    }
    check(rest_client_arm_headers(&p.client, 777, bytes(kCaptcha)) && p.runtime->armed_request_id == 777,
          "action captcha: solved headers arm for one request ID");
    check(p.send(776).ok() && !p.headers().count && p.runtime->armed_request_id == 777,
          "action captcha: a different request carries no CAPTCHA headers and keeps the arm");
    p.drain();
    constexpr Utf8View context = utf8_literal("X-Context-Properties: eyJ4IjoxfQ==\r\n");
    check(p.send(777, bytes(context)).ok() && p.headers().count == context.bytes + kCaptcha.bytes &&
              contains(p.headers(), utf8_literal("X-Captcha-Key: P1_solved.token\r\n")) &&
              contains(p.headers(), utf8_literal("X-Captcha-Session-Id: 3f2a\r\n")) &&
              contains(p.headers(), context) && !p.runtime->armed_request_id && !p.runtime->armed_headers_bytes,
          "action captcha: the retried request carries its own and the armed headers once");
    u8 head[16 * 1024]{};
    const auto written = write_rest_request_head(
        {{p.runtime->request_target, p.runtime->command.target_bytes}, {}, p.runtime->command.body_bytes,
         RestMethod::post, RestAuthorization::none, true, false, nullptr, true, {}, p.headers()},
        {head, sizeof(head)});
    check(written.ok() && contains({head, written.bytes}, utf8_literal("\r\nX-Captcha-Rqtoken: rq.abc\r\n")) &&
              contains({head, written.bytes}, utf8_literal("X-Context-Properties: eyJ4IjoxfQ==\r\nX-Captcha-Key")) &&
              contains({head, written.bytes}, utf8_literal("X-Captcha-Session-Id: 3f2a\r\nContent-Type")),
          "action captcha: headers reach the wire head before the body headers");
    p.drain();
    check(p.send(777).ok() && !p.headers().count, "action captcha: a solution is never replayed twice");
    p.drain();
    check(!rest_client_arm_headers(&p.client, 9, bytes(utf8_literal("Authorization: x\r\n"))) &&
              !rest_client_arm_headers(&p.client, 9, bytes(utf8_literal("X-Captcha-Key: a\nb\r\n"))) &&
              !rest_client_arm_headers(&p.client, 9, bytes(utf8_literal("X-Captcha-Key: \r\n"))) &&
              !p.runtime->armed_request_id,
          "action captcha: only allowlisted, single-line header values arm");
    check(p.send(10, bytes(utf8_literal("Cookie: a=b\r\n"))).error == RestClientSubmitError::invalid_input,
          "action captcha: submissions reject headers outside the allowlist");
    check(rest_client_arm_headers(&p.client, 11, bytes(kCaptcha)) &&
              rest_client_arm_headers(&p.client, 0, {}) && !p.runtime->armed_request_id &&
              !p.runtime->armed_headers_bytes,
          "action captcha: cancel disarms and erases the held solution");
    u8 line[64]{};
    check(rest_append_extra_header({line, 64}, 0, utf8_literal("X-Captcha-Key"),
                                   bytes(utf8_literal("t"))) == 18 &&
              rest_append_extra_header({line, 8}, 0, utf8_literal("X-Captcha-Key"), bytes(utf8_literal("t"))) ==
                  ~0u,
          "action captcha: header lines are bounded by their output");
}

void invite() noexcept {
    GuildInviteRequest request{};
    copy_bytes(request.code, "abc-1", 5);
    request.code_bytes = 5;
    request.operation = GuildInviteOperation::accept;
    request.location_guild_id = 55;
    request.location_channel_id = 66;
    u8 target[kGuildInviteMaximumTargetBytes]{}, body[kGuildInviteMaximumBodyBytes]{}, header[512]{};
    const auto written = guild_invite_write_request(request, {target, sizeof(target)}, {body, sizeof(body)},
                                                    utf8_literal("9c3f0e2b1a"));
    check(written.ok() && utf8_equal({reinterpret_cast<char*>(body), written.body_bytes},
                                     utf8_literal("{\"session_id\":\"9c3f0e2b1a\"}")),
          "invite accept: body carries the Gateway session ID");
    check(!guild_invite_write_request(request, {target, sizeof(target)}, {body, sizeof(body)},
                                      utf8_literal("bad\"id"))
               .ok(),
          "invite accept: a malformed session ID is not serialized");
    const u32 header_bytes = guild_invite_write_context_header(request, {header, sizeof(header)});
    constexpr Utf8View prefix = utf8_literal("X-Context-Properties: ");
    char json[256]{};
    const auto decoded =
        header_bytes > prefix.bytes + 2
            ? base64_decode({reinterpret_cast<char*>(header) + prefix.bytes, header_bytes - prefix.bytes - 2},
                            {reinterpret_cast<u8*>(json), sizeof(json)})
            : Base64Result{0, 0, Base64Error::invalid_input};
    check(header_bytes && rest_extra_headers_valid({header, header_bytes}) && decoded.ok() &&
              utf8_equal({json, decoded.bytes},
                         utf8_literal("{\"location\":\"Desktop Invite Modal\",\"location_guild_id\":\"55\","
                                      "\"location_channel_id\":\"66\",\"location_channel_type\":0}")),
          "invite accept: context header is base64 JSON of the invite location");
    request.operation = GuildInviteOperation::resolve;
    check(!guild_invite_write_context_header(request, {header, sizeof(header)}),
          "invite resolve: no context header");
}
} // namespace action_captcha_transport
} // namespace maicord

extern "C" void WINAPI maicord_account_action_captcha_transport_test_entry() noexcept {
    maicord::action_captcha_transport::retry();
    maicord::action_captcha_transport::invite();
    ExitProcess(maicord::action_captcha_transport::failures ? 1u : 0u);
}
