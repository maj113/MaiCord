#include "maicord/protocol/browser_sign_in.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/http1.hpp"
#include "json_write.hpp"


namespace maicord {
namespace {
using namespace protocol_detail;
constexpr Utf8View kOrigin = utf8_literal("https://discord.com");
constexpr Utf8View kCallback = utf8_literal("https://discord.com/handoff?done=true");
bool folded(Utf8View value, const char* literal) noexcept {
    return http_ascii_equal_case_insensitive({reinterpret_cast<const u8*>(value.data), value.bytes}, literal);
}
bool port_valid(u16 port) noexcept {
    return port >= kBrowserSignInFirstPort && port < kBrowserSignInFirstPort + kBrowserSignInPortCount;
}
bool key_valid(Utf8View key) noexcept {
    if (!key.data || key.bytes != kBrowserSignInKeyBytes) return false;
    for (u32 i = 0; i < key.bytes; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (key.data[i] != '-') return false; }
        else if (ascii_hex_value(key.data[i]) < 0) return false;
    }
    return true;
}
bool destination(Slice<u8> output, Utf8View first = {}, Utf8View second = {}) noexcept {
    return output.data && byte_range_valid({output.data, output.count}) &&
        !byte_ranges_overlap({output.data, output.count}, {reinterpret_cast<const u8*>(first.data), first.bytes}) &&
        !byte_ranges_overlap({output.data, output.count}, {reinterpret_cast<const u8*>(second.data), second.bytes});
}
bool decode_uri(Utf8View source, Slice<u8> output, u32* count) noexcept {
    u32 used = 0;
    for (u32 i = 0; i < source.bytes; ++i) {
        u8 value = static_cast<u8>(source.data[i]);
        if (value == '%') {
            if (source.bytes - i < 3) return false;
            const int high = ascii_hex_value(source.data[i + 1]), low = ascii_hex_value(source.data[i + 2]);
            if (high < 0 || low < 0) return false;
            value = static_cast<u8>(high * 16 + low); i += 2;
        } else if (value == '+') value = ' ';
        if (!value || value == '\r' || value == '\n' || used == output.count) return false;
        output[used++] = value;
    }
    if (!utf8_validate({reinterpret_cast<const char*>(output.data), used}).valid) return false;
    *count = used; return true;
}
BrowserSignInParseResult reject(BrowserSignInError error) noexcept {
    return {{}, error, BrowserSignInParseState::rejected};
}
struct Payload {
    JsonIndexedDocument document;
    BrowserSignInRequest request;
    u32 fields, args_fields;
};
bool bounded_string(JsonValue value, u32 maximum, bool nonempty, JsonString* output) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string)) return false;
    const auto measured = json_string_decode_utf8(string, {});
    // A zero-sized query reports output_too_small for a nonempty valid string.
    if (measured.error != JsonStringDecodeError::none && measured.error != JsonStringDecodeError::output_too_small) return false;
    if (measured.bytes > maximum || (nonempty && !measured.bytes)) return false;
    *output = string; return true;
}
bool args_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* payload = static_cast<Payload*>(opaque);
    const u32 bit = json_string_equals_ascii(name, utf8_literal("handoffToken")) ? 1u :
        json_string_equals_ascii(name, utf8_literal("fingerprint")) ? 2u : 0u;
    if (!bit || (payload->args_fields & bit)) return false;
    payload->args_fields |= bit;
    if (value.kind == JsonValueKind::null_value) return true;
    JsonString string{};
    if (!bounded_string(value, bit == 1 ? kBrowserSignInMaximumTokenBytes : 512, bit == 1, &string)) return false;
    if (bit == 1) { payload->request.token = string; payload->request.has_token = true; }
    return true;
}
bool payload_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* payload = static_cast<Payload*>(opaque);
    const u32 bit = json_string_equals_ascii(name, utf8_literal("cmd")) ? 1u :
        json_string_equals_ascii(name, utf8_literal("args")) ? 2u :
        json_string_equals_ascii(name, utf8_literal("nonce")) ? 4u :
        json_string_equals_ascii(name, utf8_literal("evt")) ? 8u : 0u;
    if (!bit || (payload->fields & bit)) return false;
    payload->fields |= bit;
    if (bit == 1) { JsonString command{}; return json_value_as_string(value, &command) && json_string_equals_ascii(command, utf8_literal("BROWSER_HANDOFF")); }
    if (bit == 2) return value.kind == JsonValueKind::object &&
        json_visit_indexed_object(payload->document, value, 3, args_member, payload).ok();
    if (bit == 4) return bounded_string(value, kBrowserSignInMaximumNonceBytes, true, &payload->request.nonce);
    return value.kind == JsonValueKind::null_value;
}
} // namespace

BrowserSignInParseResult browser_sign_in_parse_request(ConstSlice<u8> input, u16 port,
    Slice<u8> decoded_query, Slice<u32> structural_scratch) noexcept {
    const ConstSlice<u8> decoded{decoded_query.data, decoded_query.count};
    const ConstSlice<u8> index{reinterpret_cast<const u8*>(structural_scratch.data), structural_scratch.count * static_cast<u32>(sizeof(u32))};
    if (!port_valid(port) || !input.data || !byte_range_valid(input) || !decoded_query.data ||
        decoded_query.count < kBrowserSignInMaximumBodyBytes || !byte_range_valid(decoded) ||
        !structural_scratch.data || structural_scratch.count < kBrowserSignInStructuralCount ||
        structural_scratch.count > ~u32{0} / sizeof(u32) || !byte_range_valid(index) ||
        byte_ranges_overlap(input, decoded) || byte_ranges_overlap(input, index) || byte_ranges_overlap(decoded, index))
        return reject(BrowserSignInError::invalid_input);
    if (input.count > kBrowserSignInMaximumHeaderBytes + kBrowserSignInMaximumBodyBytes) return reject(BrowserSignInError::capacity);
    u32 head_bytes = 0;
    for (u32 i = 0; i < input.count; ++i) {
        if (input[i] == '\n' && (!i || input[i - 1] != '\r')) return reject(BrowserSignInError::http);
        if (i + 3 < input.count && input[i] == '\r' && input[i + 1] == '\n' && input[i + 2] == '\r' && input[i + 3] == '\n') { head_bytes = i + 4; break; }
    }
    if (!head_bytes) return input.count >= kBrowserSignInMaximumHeaderBytes ? reject(BrowserSignInError::capacity) : BrowserSignInParseResult{};
    if (head_bytes > kBrowserSignInMaximumHeaderBytes) return reject(BrowserSignInError::capacity);
    const char* text = reinterpret_cast<const char*>(input.data);
    u32 line_end = 0;
    while (line_end + 1 < head_bytes && !(text[line_end] == '\r' && text[line_end + 1] == '\n')) ++line_end;
    u32 method_end = 0; while (method_end < line_end && text[method_end] != ' ') ++method_end;
    u32 target_end = method_end + 1; while (target_end < line_end && text[target_end] != ' ') ++target_end;
    if (!method_end || method_end >= line_end || target_end <= method_end + 1 || target_end >= line_end ||
        !utf8_equal({text + target_end + 1, line_end - target_end - 1}, utf8_literal("HTTP/1.1"))) return reject(BrowserSignInError::http);
    const Utf8View method{text, method_end}, target{text + method_end + 1, target_end - method_end - 1};
    const bool post = utf8_equal(method, utf8_literal("POST")), options = utf8_equal(method, utf8_literal("OPTIONS")), get = utf8_equal(method, utf8_literal("GET"));
    if (!post && !options && !get) return reject(BrowserSignInError::route);
    for (u32 i = 0; i < target.bytes; ++i) if (static_cast<u8>(target.data[i]) <= 0x20 || static_cast<u8>(target.data[i]) >= 0x7f) return reject(BrowserSignInError::route);
    Utf8View host{}, origin{}, content_type{}, request_method{}, request_headers{}, private_network{};
    u32 length = 0, fields = 0, seen = 0;
    u32 cursor = line_end + 2;
    while (cursor < head_bytes - 2) {
        const u32 begin = cursor; while (cursor + 1 < head_bytes && !(text[cursor] == '\r' && text[cursor + 1] == '\n')) ++cursor;
        const u32 end = cursor; cursor += 2;
        if (++fields > 64 || begin == end || text[begin] == ' ' || text[begin] == '\t') return reject(BrowserSignInError::http);
        u32 colon = begin; while (colon < end && text[colon] != ':') ++colon;
        if (colon == begin || colon == end) return reject(BrowserSignInError::http);
        for (u32 i = begin; i < colon; ++i) {
            const char c = text[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return reject(BrowserSignInError::http);
        }
        for (u32 i = colon + 1; i < end; ++i) if ((static_cast<u8>(text[i]) < 0x20 && text[i] != '\t') || text[i] == 0x7f) return reject(BrowserSignInError::http);
        const Utf8View name{text + begin, colon - begin}, value = http_trim_ows({text + colon + 1, end - colon - 1});
        u32 bit = folded(name, "host") ? 1u : folded(name, "origin") ? 2u : folded(name, "content-length") ? 4u :
            folded(name, "content-type") ? 8u : folded(name, "access-control-request-method") ? 16u :
            folded(name, "access-control-request-headers") ? 32u : folded(name, "access-control-request-private-network") ? 64u : 0u;
        if (folded(name, "transfer-encoding") || folded(name, "expect") || folded(name, "authorization") || folded(name, "cookie")) return reject(BrowserSignInError::framing);
        if (bit && (seen & bit)) return reject(BrowserSignInError::framing);
        seen |= bit;
        if (bit == 1) host = value;
        else if (bit == 2) origin = value;
        else if (bit == 4) {
            if (!value.bytes) return reject(BrowserSignInError::framing);
            for (u32 i = 0; i < value.bytes; ++i) {
                const char c = value.data[i]; if (c < '0' || c > '9' || length > kBrowserSignInMaximumBodyBytes / 10) return reject(BrowserSignInError::framing);
                length = length * 10 + static_cast<u32>(c - '0');
                if (length > kBrowserSignInMaximumBodyBytes) return reject(BrowserSignInError::capacity);
            }
        } else if (bit == 8) content_type = value;
        else if (bit == 16) request_method = value;
        else if (bit == 32) request_headers = value;
        else if (bit == 64) private_network = value;
    }
    u8 expected_host[32]; u32 host_bytes = 0;
    write_literal(expected_host, &host_bytes, "127.0.0.1:"); host_bytes += decimal_write(expected_host + host_bytes, port);
    if (!utf8_equal(host, {reinterpret_cast<const char*>(expected_host), host_bytes})) return reject(BrowserSignInError::host);
    if ((post || options || origin.bytes) && !utf8_equal(origin, kOrigin)) return reject(BrowserSignInError::origin);
    if (post && (!(seen & 4) || !length ||
        !(folded(content_type, "application/json") || folded(content_type, "application/json; charset=utf-8")))) return reject(BrowserSignInError::framing);
    if (!post && length) return reject(BrowserSignInError::framing);
    const u32 total = head_bytes + length;
    if (input.count < total) return {};
    if (input.count != total) return reject(BrowserSignInError::framing);
    BrowserSignInRequest request{};
    if (post || options) {
        if (!utf8_equal(target, utf8_literal("/rpc?v=1"))) return reject(BrowserSignInError::route);
        request.kind = post ? BrowserSignInRequestKind::post : BrowserSignInRequestKind::preflight;
        if (options) {
            if (!utf8_equal(request_method, utf8_literal("POST")) ||
                (request_headers.bytes && !folded(http_trim_ows(request_headers), "content-type")) ||
                (private_network.bytes && !utf8_equal(private_network, utf8_literal("true")))) return reject(BrowserSignInError::preflight);
            request.private_network = private_network.bytes != 0;
            return {request, BrowserSignInError::none, BrowserSignInParseState::complete};
        }
    }
    Utf8View payload{text + head_bytes, length};
    if (get) {
        if (target.bytes < 6 || !utf8_equal({target.data, 5}, utf8_literal("/rpc?"))) return reject(BrowserSignInError::route);
        Utf8View encoded_payload{}, callback{}, version{}; u32 query_seen = 0;
        u32 query = 5;
        while (query < target.bytes) {
            const u32 begin = query; while (query < target.bytes && target.data[query] != '&') ++query;
            const u32 end = query; if (query < target.bytes) ++query;
            if (query == target.bytes && target.data[target.bytes - 1] == '&') return reject(BrowserSignInError::route);
            u32 equals = begin; while (equals < end && target.data[equals] != '=') ++equals;
            if (equals == begin || equals == end) return reject(BrowserSignInError::route);
            const Utf8View name{target.data + begin, equals - begin}, value{target.data + equals + 1, end - equals - 1};
            const u32 bit = utf8_equal(name, utf8_literal("v")) ? 1u : utf8_equal(name, utf8_literal("payload")) ? 2u : utf8_equal(name, utf8_literal("callback")) ? 4u : 0u;
            if (!bit || (query_seen & bit)) return reject(BrowserSignInError::route);
            query_seen |= bit;
            if (bit == 1) version = value; else if (bit == 2) encoded_payload = value; else callback = value;
        }
        if (query_seen != 7 || !utf8_equal(version, utf8_literal("1"))) return reject(BrowserSignInError::route);
        u8 decoded_callback[64]; u32 callback_bytes = 0;
        if (!decode_uri(callback, {decoded_callback, sizeof(decoded_callback)}, &callback_bytes) ||
            !utf8_equal({reinterpret_cast<const char*>(decoded_callback), callback_bytes}, kCallback)) return reject(BrowserSignInError::route);
        u32 decoded_bytes = 0;
        if (!decode_uri(encoded_payload, {decoded_query.data, kBrowserSignInMaximumBodyBytes}, &decoded_bytes) || !decoded_bytes) return reject(BrowserSignInError::payload);
        payload = {reinterpret_cast<const char*>(decoded_query.data), decoded_bytes};
        request.kind = BrowserSignInRequestKind::redirect;
    }
    const auto scan = json_scan_structurals_avx1(payload, structural_scratch);
    if (!scan.ok()) return reject(BrowserSignInError::json);
    Payload context{{payload, {structural_scratch.data, scan.count}}, request, 0, 0};
    if (!json_visit_root_object(payload, {structural_scratch.data, scan.count}, 3, payload_member, &context).ok() ||
        (context.fields & 7u) != 7u) return reject(BrowserSignInError::payload);
    return {context.request, BrowserSignInError::none, BrowserSignInParseState::complete};
}

BrowserSignInWriteResult browser_sign_in_write_key(ConstSlice<u8> random, Slice<u8> output) noexcept {
    if (!random.data || random.count != 16 || !byte_range_valid(random) || !destination(output) ||
        byte_ranges_overlap(random, {output.data, output.count})) return {0, BrowserSignInError::invalid_input};
    if (output.count < kBrowserSignInKeyBytes) return {kBrowserSignInKeyBytes, BrowserSignInError::capacity};
    constexpr char digits[] = "0123456789abcdef"; u32 cursor = 0;
    for (u32 i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) output[cursor++] = '-';
        const u8 byte = i == 6 ? static_cast<u8>((random[i] & 15u) | 0x40u) :
            i == 8 ? static_cast<u8>((random[i] & 63u) | 0x80u) : random[i];
        output[cursor++] = static_cast<u8>(digits[byte >> 4]); output[cursor++] = static_cast<u8>(digits[byte & 15]);
    }
    return {cursor, BrowserSignInError::none};
}
BrowserSignInWriteResult browser_sign_in_write_url(u16 port, Utf8View key, Slice<u8> output) noexcept {
    if (!port_valid(port) || !key_valid(key) || !destination(output, key)) return {0, BrowserSignInError::invalid_input};
    // Nested internal redirect: no arbitrary scheme, origin, callback or path.
    constexpr char prefix[] = "https://discord.com/login?redirect_to=%2Fhandoff%3Frpc%3D";
    const u32 required = sizeof(prefix) - 1 + 4 + sizeof("%26key%3D") - 1 + key.bytes;
    if (output.count < required) return {required, BrowserSignInError::capacity};
    u32 cursor = 0; write_literal(output.data, &cursor, prefix); cursor += decimal_write(output.data + cursor, port);
    write_literal(output.data, &cursor, "%26key%3D"); copy_bytes(output.data + cursor, key.data, key.bytes); cursor += key.bytes;
    return {cursor, BrowserSignInError::none};
}
BrowserSignInWriteResult browser_sign_in_write_exchange(Utf8View key, Utf8View token, Slice<u8> output) noexcept {
    EncodedString key_string{}, token_string{};
    if (!key_valid(key) || !encoded_string(key, kBrowserSignInKeyBytes, &key_string) ||
        !encoded_string(token, kBrowserSignInMaximumTokenBytes, &token_string) || !destination(output, key, token)) return {0, BrowserSignInError::invalid_input};
    const u32 required = sizeof("{\"key\":,\"handoff_token\":}") - 1 + key_string.bytes + token_string.bytes;
    if (required > kBrowserSignInMaximumBodyBytes) return {required, BrowserSignInError::capacity};
    if (required > output.count) return {required, BrowserSignInError::capacity};
    u32 cursor = 0; write_literal(output.data, &cursor, "{\"key\":"); write_encoded(output.data, &cursor, key_string);
    write_literal(output.data, &cursor, ",\"handoff_token\":"); write_encoded(output.data, &cursor, token_string); output[cursor++] = '}';
    return {cursor, BrowserSignInError::none};
}
BrowserSignInWriteResult browser_sign_in_write_response(const BrowserSignInRequest& request, Slice<u8> output) noexcept {
    if (!destination(output, request.nonce.encoded) ||
        byte_ranges_overlap({output.data, output.count}, {reinterpret_cast<const u8*>(&request), sizeof(request)})) return {0, BrowserSignInError::invalid_input};
    if (request.kind == BrowserSignInRequestKind::redirect) {
        constexpr char response[] = "HTTP/1.1 301 Moved Permanently\r\nLocation: https://discord.com/handoff?done=true\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        if (output.count < sizeof(response) - 1) return {sizeof(response) - 1, BrowserSignInError::capacity};
        copy_bytes(output.data, response, sizeof(response) - 1); return {sizeof(response) - 1, BrowserSignInError::none};
    }
    if (request.kind == BrowserSignInRequestKind::preflight) {
        constexpr char head[] = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: https://discord.com\r\nAccess-Control-Allow-Methods: POST\r\nAccess-Control-Allow-Headers: Content-Type\r\nVary: Origin, Access-Control-Request-Method, Access-Control-Request-Headers\r\nCache-Control: no-store\r\n";
        constexpr char network[] = "Access-Control-Allow-Private-Network: true\r\n";
        constexpr char tail[] = "Content-Length: 0\r\nConnection: close\r\n\r\n";
        const u32 required = sizeof(head) + sizeof(tail) - 2 + (request.private_network ? sizeof(network) - 1 : 0);
        if (output.count < required) return {required, BrowserSignInError::capacity};
        u32 cursor = 0; write_literal(output.data, &cursor, head); if (request.private_network) write_literal(output.data, &cursor, network); write_literal(output.data, &cursor, tail);
        return {cursor, BrowserSignInError::none};
    }
    if (request.kind != BrowserSignInRequestKind::post) return {0, BrowserSignInError::invalid_input};
    char nonce[kBrowserSignInMaximumNonceBytes]{};
    const auto decoded = json_string_decode_utf8(request.nonce, {nonce, sizeof(nonce)});
    EncodedString encoded{};
    if (!decoded.ok() || !decoded.bytes || !encoded_string({nonce, decoded.bytes}, true, &encoded)) { secure_clear_bytes(nonce, sizeof(nonce)); return {0, BrowserSignInError::invalid_input}; }
    // The reply echoes cmd and nonce; the handler returns nothing, so data and evt are null.
    const u32 body_bytes = sizeof("{\"cmd\":\"BROWSER_HANDOFF\",\"data\":null,\"evt\":null,\"nonce\":}") - 1 + encoded.bytes;
    constexpr char head[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: https://discord.com\r\nVary: Origin\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: ";
    u8 length[10]; const u32 length_bytes = decimal_write(length, body_bytes);
    const u32 required = sizeof(head) - 1 + length_bytes + 4 + body_bytes;
    if (output.count < required) { secure_clear_bytes(nonce, sizeof(nonce)); return {required, BrowserSignInError::capacity}; }
    u32 cursor = 0; write_literal(output.data, &cursor, head); copy_bytes(output.data + cursor, length, length_bytes); cursor += length_bytes;
    write_literal(output.data, &cursor, "\r\n\r\n{\"cmd\":\"BROWSER_HANDOFF\",\"data\":null,\"evt\":null,\"nonce\":"); write_encoded(output.data, &cursor, encoded); output[cursor++] = '}';
    secure_clear_bytes(nonce, sizeof(nonce)); return {cursor, BrowserSignInError::none};
}
} // namespace maicord
