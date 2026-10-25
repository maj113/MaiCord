#include "maicord/protocol/browser_sign_in.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord {
namespace {
bool same(Utf8View a, Utf8View b) noexcept { return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes); }
u32 append(u8* output, u32 used, Utf8View text) noexcept { copy_bytes(output + used, text.data, text.bytes); return used + text.bytes; }
u32 number(u32 value, u8* output) noexcept {
    u32 divisor = 1, used = 0; while (value / divisor >= 10) divisor *= 10;
    do { output[used++] = static_cast<u8>('0' + value / divisor); value %= divisor; divisor /= 10; } while (divisor); return used;
}
u32 post(u8* output, Utf8View payload, Utf8View headers = utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\n"),
    Utf8View route = utf8_literal("/rpc?v=1")) noexcept {
    u32 used = append(output, 0, utf8_literal("POST ")); used = append(output, used, route);
    used = append(output, used, utf8_literal(" HTTP/1.1\r\n")); used = append(output, used, headers);
    used = append(output, used, utf8_literal("Content-Length: ")); used += number(payload.bytes, output + used);
    used = append(output, used, utf8_literal("\r\n\r\n")); return append(output, used, payload);
}
u32 uri(u8* output, Utf8View text) noexcept {
    constexpr char digits[] = "0123456789ABCDEF"; u32 used = 0;
    for (u32 i = 0; i < text.bytes; ++i) {
        const u8 c = static_cast<u8>(text.data[i]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') output[used++] = c;
        else { output[used++] = '%'; output[used++] = static_cast<u8>(digits[c >> 4]); output[used++] = static_cast<u8>(digits[c & 15]); }
    }
    return used;
}
bool contains(ConstSlice<u8> text, Utf8View value) noexcept {
    for (u32 i = 0; i + value.bytes <= text.count; ++i) if (bytes_equal(text.data + i, value.data, value.bytes)) return true;
    return false;
}
} // namespace

void test_browser_sign_in(void (*check)(bool, const char*) noexcept) noexcept {
    u8 input[kBrowserSignInMaximumHeaderBytes + kBrowserSignInMaximumBodyBytes]{};
    u8 query[kBrowserSignInMaximumBodyBytes]{};
    u32 scratch[kBrowserSignInStructuralCount]{};
    u8 output[2048]{};
    constexpr Utf8View payload = utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{\"handoffToken\":\"synthetic-token\",\"fingerprint\":\"synthetic-fingerprint\"},\"nonce\":\"nonce-1\"}");
    const auto parse = [&](u32 bytes) noexcept { return browser_sign_in_parse_request({input, bytes}, 6463, {query, sizeof(query)}, {scratch, kBrowserSignInStructuralCount}); };
    u32 bytes = post(input, payload);
    auto parsed = parse(bytes);
    char token[64]{};
    const auto decoded = json_string_decode_utf8(parsed.request.token, {token, sizeof(token)});
    check(parsed.ok() && parsed.request.kind == BrowserSignInRequestKind::post && parsed.request.has_token && decoded.ok() &&
        same({token, decoded.bytes}, utf8_literal("synthetic-token")), "browser sign-in accepts observed POST with omitted evt and keeps token as a borrowed view");
    auto written = browser_sign_in_write_response(parsed.request, {output, sizeof(output)});
    check(written.ok() && contains({output, written.bytes}, utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"data\":null,\"evt\":null,\"nonce\":\"nonce-1\"}")) &&
        contains({output, written.bytes}, utf8_literal("Access-Control-Allow-Origin: https://discord.com\r\n")) &&
        !contains({output, written.bytes}, utf8_literal("synthetic-token")), "RPC acknowledgement exactly preserves command/nonce and null data/evt without returning a credential");
    const u32 complete_bytes = bytes;
    check(parse(complete_bytes - 1).state == BrowserSignInParseState::need_input &&
        parse(15).state == BrowserSignInParseState::need_input, "fragmented header and bounded content-length body wait for remaining input");
    input[complete_bytes] = 'x';
    check(!parse(complete_bytes + 1).ok(), "browser sign-in rejects trailing/pipelined bytes rather than accepting ambiguous framing");
    constexpr Utf8View rejected_payloads[] = {
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"cmd\":\"BROWSER_HANDOFF\",\"args\":{},\"nonce\":\"n\"}"),
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{\"handoffToken\":\"a\",\"handoffToken\":\"b\"},\"nonce\":\"n\"}"),
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{},\"nonce\":\"\"}"),
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":[],\"nonce\":\"n\"}"),
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{\"handoffToken\":[]},\"nonce\":\"n\"}"),
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{\"token\":\"session-shaped\"},\"nonce\":\"n\"}"),
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{},\"nonce\":\"n\",\"evt\":\"READY\"}"),
        utf8_literal("{\"cmd\":\"AUTHENTICATE\",\"args\":{},\"nonce\":\"n\"}"),
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{\"handoffToken\":\"\"},\"nonce\":\"n\"}"),
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{},\"nonce\":\"\\uD800\"}"),
    };
    for (Utf8View invalid : rejected_payloads) check(!parse(post(input, invalid)).ok(), "browser sign-in rejects duplicate, foreign and malformed RPC schemas");
    constexpr Utf8View no_session[] = {
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{},\"nonce\":\"n\"}"),
        utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{\"handoffToken\":null,\"fingerprint\":null},\"nonce\":\"n\",\"evt\":null}"),
    };
    for (Utf8View absent : no_session) { const auto value = parse(post(input, absent)); check(value.ok() && !value.request.has_token, "unauthenticated official-browser callback never invents an authenticated candidate"); }
    constexpr Utf8View rejected_headers[] = {
        utf8_literal("Host: localhost:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\n"),
        utf8_literal("Host: 127.0.0.1:6464\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com.evil.invalid\r\nContent-Type: application/json\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nContent-Type: application/json\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: null\r\nContent-Type: application/json\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nContent-Length: 0\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nAuthorization: synthetic\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nCookie: synthetic\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: text/plain\r\n"),
        utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\n folded: value\r\n"),
    };
    for (Utf8View headers : rejected_headers) check(!parse(post(input, payload, headers)).ok(), "POST admission rejects foreign origins, hosts, secret headers and ambiguous HTTP framing");
    check(!parse(post(input, payload, utf8_literal("Host: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\n"), utf8_literal("/rpc?v=1&client_id=other"))).ok(), "browser receiver does not implement unrelated RPC routes or client authorization");

    constexpr Utf8View preflight = utf8_literal("OPTIONS /rpc?v=1 HTTP/1.1\r\nHost: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nAccess-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: content-type\r\nAccess-Control-Request-Private-Network: true\r\n\r\n");
    copy_bytes(input, preflight.data, preflight.bytes); parsed = parse(preflight.bytes);
    written = browser_sign_in_write_response(parsed.request, {output, sizeof(output)});
    check(parsed.ok() && parsed.request.kind == BrowserSignInRequestKind::preflight && parsed.request.private_network && written.ok() &&
        contains({output, written.bytes}, utf8_literal("Access-Control-Allow-Private-Network: true\r\n")) &&
        contains({output, written.bytes}, utf8_literal("Access-Control-Allow-Headers: Content-Type\r\n")) &&
        !contains({output, written.bytes}, utf8_literal("Authorization")), "preflight narrowly admits official POST and only requested legacy private-network opt-in");
    constexpr Utf8View bad_preflight = utf8_literal("OPTIONS /rpc?v=1 HTTP/1.1\r\nHost: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nAccess-Control-Request-Method: POST\r\nAccess-Control-Request-Headers: content-type, authorization\r\n\r\n");
    copy_bytes(input, bad_preflight.data, bad_preflight.bytes);
    check(!parse(bad_preflight.bytes).ok(), "private-network/CORS preflight never admits an Authorization header");
    constexpr Utf8View callback = utf8_literal("https://discord.com/handoff?done=true");
    u32 get_bytes = append(input, 0, utf8_literal("GET /rpc?v=1&payload=")); get_bytes += uri(input + get_bytes, payload);
    get_bytes = append(input, get_bytes, utf8_literal("&callback=")); get_bytes += uri(input + get_bytes, callback);
    get_bytes = append(input, get_bytes, utf8_literal(" HTTP/1.1\r\nHost: 127.0.0.1:6463\r\n\r\n"));
    parsed = parse(get_bytes); written = browser_sign_in_write_response(parsed.request, {output, sizeof(output)});
    check(parsed.ok() && parsed.request.kind == BrowserSignInRequestKind::redirect && parsed.request.has_token && written.ok() &&
        contains({output, written.bytes}, utf8_literal("HTTP/1.1 301 Moved Permanently\r\nLocation: https://discord.com/handoff?done=true\r\n")), "legacy navigation accepts only fixed official callback and returns observed301; exchange remains separate");
    // An attacker-controlled callback may never become the response Location.
    u32 malicious = append(input, 0, utf8_literal("GET /rpc?v=1&payload=")); malicious += uri(input + malicious, payload);
    malicious = append(input, malicious, utf8_literal("&callback=https%3A%2F%2Fevil.invalid%2Fhandoff%3Fdone%3Dtrue HTTP/1.1\r\nHost: 127.0.0.1:6463\r\n\r\n"));
    check(!parse(malicious).ok(), "legacy callback does not introduce an open redirect or trusted origin claim");
    constexpr Utf8View malformed_uri = utf8_literal("GET /rpc?v=1&payload=%GG&callback=https%3A%2F%2Fdiscord.com%2Fhandoff%3Fdone%3Dtrue HTTP/1.1\r\nHost: 127.0.0.1:6463\r\n\r\n");
    copy_bytes(input, malformed_uri.data, malformed_uri.bytes); check(!parse(malformed_uri.bytes).ok(), "malformed percent encoding cannot reach the JSON decoder");

    u8 random[16]{}; u8 key[kBrowserSignInKeyBytes]{};
    written = browser_sign_in_write_key({random, sizeof(random)}, {key, sizeof(key)});
    constexpr Utf8View expected_key = utf8_literal("00000000-0000-4000-8000-000000000000");
    check(written.ok() && same({reinterpret_cast<const char*>(key), written.bytes}, expected_key), "browser binding key has genuine UUIDv4 version/variant from supplied random bytes");
    written = browser_sign_in_write_url(6463, expected_key, {output, sizeof(output)});
    check(written.ok() && same({reinterpret_cast<const char*>(output), written.bytes}, utf8_literal("https://discord.com/login?redirect_to=%2Fhandoff%3Frpc%3D6463%26key%3D00000000-0000-4000-8000-000000000000")), "browser action uses fixed official login with encoded internal handoff destination");
    check(!browser_sign_in_write_url(6473, expected_key, {output, sizeof(output)}).ok() &&
        !browser_sign_in_write_url(6463, utf8_literal("foreign-key"), {output, sizeof(output)}).ok(), "browser URL rejects unsupported ports and arbitrary key/URL material");
    written = browser_sign_in_write_exchange(expected_key, utf8_literal("a\"b\\c"), {output, sizeof(output)});
    check(written.ok() && same({reinterpret_cast<const char*>(output), written.bytes}, utf8_literal("{\"key\":\"00000000-0000-4000-8000-000000000000\",\"handoff_token\":\"a\\\"b\\\\c\"}")), "exchange preserves retained key and safely serializes the distinct handoff credential");
    memset(output, 0x5a, sizeof(output));
    const auto insufficient = browser_sign_in_write_exchange(expected_key, utf8_literal("synthetic"), {output, 1});
    bool unchanged = true; for (u8 c : output) if (c != 0x5a) unchanged = false;
    check(insufficient.error == BrowserSignInError::capacity && unchanged, "exchange writer preflights capacity atomically");
    const auto overlapping = browser_sign_in_write_exchange(expected_key, {reinterpret_cast<const char*>(output), 1}, {output, sizeof(output)});
    check(overlapping.error == BrowserSignInError::invalid_input, "exchange output cannot overwrite its borrowed secret input");
    bytes = post(input, payload);
    check(browser_sign_in_parse_request({input, bytes}, 6463, {input, kBrowserSignInMaximumBodyBytes}, {scratch, kBrowserSignInStructuralCount}).error == BrowserSignInError::invalid_input,
        "query scratch cannot overlap request input");
    u8 large[kBrowserSignInMaximumBodyBytes]{};
    u32 large_bytes = append(large, 0, utf8_literal("{\"cmd\":\"BROWSER_HANDOFF\",\"args\":{\"handoffToken\":\""));
    const u32 token_begin = large_bytes;
    memset(large + large_bytes, 'a', kBrowserSignInMaximumTokenBytes); large_bytes += kBrowserSignInMaximumTokenBytes;
    const u32 token_end = large_bytes;
    large_bytes = append(large, large_bytes, utf8_literal("\"},\"nonce\":\"n\"}"));
    parsed = parse(post(input, {reinterpret_cast<const char*>(large), large_bytes}));
    check(parsed.ok() && parsed.request.has_token && parsed.request.token.encoded.bytes == kBrowserSignInMaximumTokenBytes,
        "exact bounded4096-byte handoff candidate is admitted");
    large[token_end] = 'a'; large_bytes = append(large, token_end + 1, utf8_literal("\"},\"nonce\":\"n\"}"));
    check(!parse(post(input, {reinterpret_cast<const char*>(large), large_bytes})).ok(), "over-limit handoff candidate is rejected before publication");
    memset(input, 'x', kBrowserSignInMaximumHeaderBytes);
    check(parse(kBrowserSignInMaximumHeaderBytes).error == BrowserSignInError::capacity, "unfinished header at its exact limit is rejected without waiting forever");
    constexpr Utf8View body_oversize = utf8_literal("POST /rpc?v=1 HTTP/1.1\r\nHost: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nContent-Length: 8193\r\n\r\n");
    copy_bytes(input, body_oversize.data, body_oversize.bytes);
    check(parse(body_oversize.bytes).error == BrowserSignInError::capacity, "oversized body is rejected from its declared length without a body allocation");
    constexpr Utf8View bad_lengths[] = {
        utf8_literal("POST /rpc?v=1 HTTP/1.1\r\nHost: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nContent-Length: 12x\r\n\r\n"),
        utf8_literal("POST /rpc?v=1 HTTP/1.1\r\nHost: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nContent-Length:  \r\n\r\n"),
        utf8_literal("POST /rpc?v=1 HTTP/1.1\r\nHost: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\n{}"),
    };
    for (Utf8View head : bad_lengths) {
        copy_bytes(input, head.data, head.bytes);
        check(parse(head.bytes).error == BrowserSignInError::framing, "non-numeric, empty and repeated Content-Length are framing errors");
    }
    constexpr Utf8View huge_length = utf8_literal("POST /rpc?v=1 HTTP/1.1\r\nHost: 127.0.0.1:6463\r\nOrigin: https://discord.com\r\nContent-Type: application/json\r\nContent-Length: 99999999999\r\n\r\n");
    copy_bytes(input, huge_length.data, huge_length.bytes);
    check(parse(huge_length.bytes).error == BrowserSignInError::capacity, "a Content-Length too large for any body is rejected without overflow");
    // Nested escaping can exceed the exchange body cap even below the decoded
    // token limit. Preflight must preserve both candidate and output.
    memset(large + token_begin, '\x01', kBrowserSignInMaximumTokenBytes);
    const auto expanded = browser_sign_in_write_exchange(expected_key,
        {reinterpret_cast<const char*>(large + token_begin), kBrowserSignInMaximumTokenBytes}, {input, sizeof(input)});
    check(expanded.error == BrowserSignInError::capacity && expanded.bytes > kBrowserSignInMaximumBodyBytes,
        "exchange JSON expansion cannot exceed the8KiB transport contract");
    secure_clear_bytes(large, sizeof(large));
    secure_clear_bytes(input, sizeof(input)); secure_clear_bytes(query, sizeof(query)); secure_clear_bytes(output, sizeof(output)); secure_clear_bytes(token, sizeof(token));
}
} // namespace maicord
