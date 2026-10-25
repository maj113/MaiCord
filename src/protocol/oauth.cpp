#include "maicord/protocol/oauth.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/protocol/http1.hpp"
#include "maicord/core/ascii.hpp"

namespace maicord {
namespace {
using E = OAuthError;
struct Erase { void* data; usize bytes; ~Erase() noexcept { secure_clear_bytes(data, bytes); } };
bool proof(Utf8View text) noexcept {
    if (!text.data || text.bytes != kOAuthProofBytes) return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (!ascii_is_alnum(text.data[i]) && text.data[i] != '-' && text.data[i] != '_') return false;
    return true;
}
bool opaque(Utf8View text, u32 limit) noexcept {
    if (!text.data || !text.bytes || text.bytes > limit) return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if (static_cast<u8>(text.data[i]) <= 32 || static_cast<u8>(text.data[i]) >= 127) return false;
    return true;
}
bool add(ProtocolWriter& w, Utf8View text) noexcept {
    return protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(text.data), text.bytes);
}
bool parameter(ProtocolWriter& w, Utf8View name, Utf8View value) noexcept {
    return add(w, name) && protocol_writer_append_percent_encoded(&w, value);
}
u32 redirect(u16 port, Slice<u8> out) noexcept {
    ProtocolWriter w{out, 0};
    return port >= 1024 && add(w, utf8_literal("http://127.0.0.1:")) && protocol_writer_append_u64(&w, port) &&
        add(w, utf8_literal("/oauth2callback")) ? w.used : 0;
}
u32 finish(const ProtocolWriter& w, bool valid, Slice<u8> out) noexcept {
    if (!valid || !out.data || out.count < w.used) return 0;
    copy_bytes(out.data, w.output.data, w.used);
    return w.used;
}
bool decode(Utf8View in, Slice<u8> out, u32* bytes) noexcept {
    u32 used = 0;
    for (u32 i = 0; i < in.bytes; ++i) {
        u8 c = static_cast<u8>(in.data[i]);
        if (c == '%') {
            if (i + 2 >= in.bytes) return false;
            const int a = ascii_hex_value(in.data[i + 1]), b = ascii_hex_value(in.data[i + 2]);
            if (a < 0 || b < 0) return false;
            c = static_cast<u8>(a * 16 + b); i += 2;
        } else if (c == '+') c = ' ';
        if (c <= 32 || c >= 127 || used == out.count) return false;
        out[used++] = c;
    }
    *bytes = used;
    return used != 0;
}
struct Fields { JsonValue values[6]{}; u32 seen{}; };
bool token_field(void* context, JsonString name, JsonValue value) noexcept {
    auto& f = *static_cast<Fields*>(context);
    static constexpr Utf8View names[]{utf8_literal("access_token"), utf8_literal("refresh_token"),
        utf8_literal("expires_in"), utf8_literal("token_type"), utf8_literal("scope"), utf8_literal("error")};
    for (u32 i = 0; i < 6; ++i) if (json_name_is(name, names[i])) {
        if (f.seen & (1u << i)) return false;
        f.seen |= 1u << i; f.values[i] = value;
    }
    return true;
}
bool equal(JsonValue value, Utf8View expected) noexcept {
    JsonString s{};
    return json_value_as_string(value, &s) && json_string_equals_ascii(s, expected);
}
bool token(JsonValue value, char* out, u16* bytes) noexcept {
    JsonString text{};
    if (!json_value_as_string(value, &text)) return false;
    const auto decoded = json_string_decode_utf8(text, {out, kOAuthTokenBytes});
    if (!decoded.ok() || !opaque({out, decoded.bytes}, kOAuthTokenBytes)) return false;
    *bytes = static_cast<u16>(decoded.bytes);
    return true;
}
} // namespace
Utf8View oauth_scope(OAuthProvider provider) noexcept {
    return provider==OAuthProvider::spotify ? utf8_literal("user-read-private") : Utf8View{};
}
bool oauth_client_valid(OAuthProvider provider, Utf8View client) noexcept {
    if(provider==OAuthProvider::spotify) {
        if(!client.data || client.bytes!=32) return false;
        for(u32 i=0;i<client.bytes;++i) if(!ascii_is_hex_digit(client.data[i])) return false;
        return true;
    }
    return false;
}
bool oauth_token_valid(Utf8View token) noexcept { return opaque(token, kOAuthTokenBytes); }
u32 oauth_authorize(OAuthProvider provider, Utf8View client, u16 port, Utf8View state, Utf8View challenge, Slice<u8> out) noexcept {
    if (!oauth_client_valid(provider,client) || !proof(state) || !proof(challenge)) return 0;
    u8 staged[1024], uri[64]; Erase clear{staged, sizeof(staged)};
    const auto uri_bytes = redirect(port, {uri, sizeof(uri)});
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    const bool ok = uri_bytes && add(w, utf8_literal("https://accounts.spotify.com/authorize?response_type=code")) &&
        parameter(w, utf8_literal("&client_id="), client) &&
        parameter(w, utf8_literal("&redirect_uri="), {reinterpret_cast<char*>(uri), uri_bytes}) &&
        parameter(w, utf8_literal("&scope="), oauth_scope(provider)) && parameter(w, utf8_literal("&state="), state) &&
        parameter(w, utf8_literal("&code_challenge="), challenge) &&
        add(w, utf8_literal("&code_challenge_method=S256"));
    return finish(w, ok, out);
}
u32 oauth_exchange_body(OAuthProvider provider, Utf8View client, u16 port, Utf8View code, Utf8View verifier, Slice<u8> out) noexcept {
    if (!oauth_client_valid(provider,client) || !proof(verifier) || !opaque(code, kOAuthCodeBytes)) return 0;
    u8 staged[4096], uri[64]; Erase clear{staged, sizeof(staged)};
    const auto uri_bytes = redirect(port, {uri, sizeof(uri)});
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    const bool ok = uri_bytes && add(w, utf8_literal("grant_type=authorization_code")) &&
        parameter(w, utf8_literal("&client_id="), client) &&
        parameter(w, utf8_literal("&redirect_uri="), {reinterpret_cast<char*>(uri), uri_bytes}) &&
        parameter(w, utf8_literal("&code="), code) && parameter(w, utf8_literal("&code_verifier="), verifier);
    return finish(w, ok, out);
}
u32 oauth_refresh_body(OAuthProvider provider, Utf8View client, Utf8View refresh, Slice<u8> out) noexcept {
    if (!oauth_client_valid(provider,client) || !opaque(refresh, kOAuthTokenBytes)) return 0;
    u8 staged[4096]; Erase clear{staged, sizeof(staged)};
    ProtocolWriter w{{staged, sizeof(staged)}, 0};
    return finish(w, add(w, utf8_literal("grant_type=refresh_token")) &&
        parameter(w, utf8_literal("&client_id="), client) && parameter(w, utf8_literal("&refresh_token="), refresh), out);
}
OAuthCallback oauth_callback(OAuthProvider provider, Utf8View input, u16 port, Utf8View expected, Slice<u8> output) noexcept {
    const auto reject = [](E e) noexcept { return OAuthCallback{0, e, true}; };
    if (!oauth_scope(provider).bytes || (!input.data && input.bytes) || input.bytes > 8192 || port < 1024 || !proof(expected)) return reject(E::invalid_input);
    u32 head = 0;
    for (u32 i = 0; i + 3 < input.bytes; ++i)
        if (bytes_equal(input.data + i, "\r\n\r\n", 4)) { head = i + 4; break; }
    if (!head) return input.bytes == 8192 ? reject(E::capacity) : OAuthCallback{};
    if (head != input.bytes) return reject(E::malformed);
    u32 end = 0;
    while (end + 1 < head && !bytes_equal(input.data + end, "\r\n", 2)) ++end;
    constexpr auto begin = utf8_literal("GET /oauth2callback?");
    constexpr auto version = utf8_literal(" HTTP/1.1");
    if (end <= begin.bytes + version.bytes || !utf8_starts_with(input, begin) ||
        !utf8_equal({input.data + end - version.bytes, version.bytes}, version)) return reject(E::malformed);
    const Utf8View query{input.data + begin.bytes, end - begin.bytes - version.bytes};
    u8 host[32]; ProtocolWriter host_writer{{host, sizeof(host)}, 0};
    add(host_writer, utf8_literal("127.0.0.1:")); protocol_writer_append_u64(&host_writer, port);
    bool host_seen = false, length_seen = false;
    u32 fields = 0;
    for (u32 p = end + 2; p < head - 2;) {
        u32 colon = p, line = p;
        while (line + 1 < head && !bytes_equal(input.data + line, "\r\n", 2)) ++line;
        while (colon < line && input.data[colon] != ':') ++colon;
        if (++fields > 32 || colon == p || colon == line) return reject(E::malformed);
        for (u32 i = p; i < colon; ++i)
            if (!ascii_is_alnum(input.data[i]) && input.data[i] != '-') return reject(E::malformed);
        for (u32 i = colon + 1; i < line; ++i)
            if ((static_cast<u8>(input.data[i]) < 32 && input.data[i] != '\t') || static_cast<u8>(input.data[i]) >= 127)
                return reject(E::malformed);
        const Utf8View name{input.data + p, colon - p};
        const auto value = http_trim_ows(Utf8View{input.data + colon + 1, line - colon - 1});
        if (ascii_equal_ignore_case(name, utf8_literal("host"))) {
            if (host_seen || !utf8_equal(value, {reinterpret_cast<char*>(host), host_writer.used})) return reject(E::malformed);
            host_seen = true;
        } else if (ascii_equal_ignore_case(name, utf8_literal("content-length"))) {
            if (length_seen || !utf8_equal(value, utf8_literal("0"))) return reject(E::malformed);
            length_seen = true;
        } else if (ascii_equal_ignore_case(name, utf8_literal("transfer-encoding")) ||
                   ascii_equal_ignore_case(name, utf8_literal("origin"))) return reject(E::malformed);
        p = line + 2;
    }
    if (!host_seen) return reject(E::malformed);
    u8 code[kOAuthCodeBytes], temporary[kOAuthCodeBytes];
    Erase a{code, sizeof(code)}, b{temporary, sizeof(temporary)};
    u32 seen = 0, code_bytes = 0, count = 0;
    for (u32 p = 0; p < query.bytes;) {
        u32 last = p, equals = p;
        while (last < query.bytes && query.data[last] != '&') ++last;
        while (equals < last && query.data[equals] != '=') ++equals;
        if (++count > 16 || equals == p || equals == last) return reject(E::malformed);
        const Utf8View key{query.data + p, equals - p};
        const u32 bit = utf8_equal(key, utf8_literal("state")) ? 1 : utf8_equal(key, utf8_literal("code")) ? 2 :
            utf8_equal(key, utf8_literal("error")) ? 4 : utf8_equal(key, utf8_literal("iss")) ? 8 : 0;
        if (bit) {
            u32 bytes = 0;
            if (seen & bit || !decode({query.data + equals + 1, last - equals - 1}, {temporary, sizeof(temporary)}, &bytes))
                return reject(E::malformed);
            seen |= bit;
            if (bit == 1) {
                u32 different = bytes ^ expected.bytes;
                for (u32 i = 0; i < expected.bytes && i < bytes; ++i)
                    different |= temporary[i] ^ static_cast<u8>(expected.data[i]);
                if (different) return reject(E::state_mismatch);
            } else if (bit == 2) { code_bytes = bytes; copy_bytes(code, temporary, bytes); }
            else if (bit == 8 && !utf8_equal({reinterpret_cast<char*>(temporary), bytes}, utf8_literal("https://accounts.spotify.com")))
                return reject(E::malformed);
        }
        p = last + 1;
    }
    if (!(seen & 1) || (!(seen & 2) == !(seen & 4))) return reject(E::malformed);
    if (seen & 4) return reject(E::denied);
    if (!output.data || output.count < code_bytes) return reject(E::capacity);
    copy_bytes(output.data, code, code_bytes);
    return {code_bytes, E::none, true};
}
OAuthError oauth_tokens(OAuthProvider provider, Utf8View json, u16 status, Slice<u32> scratch, OAuthTokens* out) noexcept {
    if (!oauth_scope(provider).bytes || !out || !json.data || !json.bytes || json.bytes > 16384) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Fields f{};
    if (!json_visit_indexed_object(doc, {json, 0, JsonValueKind::object}, 8, token_field, &f).ok()) return E::malformed;
    if (status != 200) {
        if (equal(f.values[5], utf8_literal("invalid_request")))
            return E::invalid_request;
        if (equal(f.values[5], utf8_literal("invalid_grant"))) return E::invalid_grant;
        if (equal(f.values[5], utf8_literal("invalid_scope"))) return E::scope;
        if (equal(f.values[5], utf8_literal("invalid_client")) || equal(f.values[5], utf8_literal("unauthorized_client"))) return E::invalid_client;
        if (equal(f.values[5], utf8_literal("access_denied"))) return E::denied;
        return E::unavailable;
    }
    u64 expires = 0;
    if (f.seen & 32 || (f.seen & 13) != 13 || !equal(f.values[3], utf8_literal("Bearer")) ||
        !json_value_to_u64(f.values[2], &expires) || !expires || expires > 86400) return E::malformed;
    if (f.seen & 16) {
        char text[1024]; JsonString scope{};
        if (!json_value_as_string(f.values[4], &scope)) return E::malformed;
        const auto decoded = json_string_decode_utf8(scope, {text, sizeof(text)});
        if (!decoded.ok()) return E::scope;
        bool allowed = false;
        for (u32 p = 0; p < decoded.bytes;) {
            u32 end = p;
            while (end < decoded.bytes && text[end] != ' ') ++end;
            const Utf8View value{text + p, end - p};
            allowed |= utf8_equal(value, oauth_scope(provider));
            p = end + 1;
        }
        if (!allowed) return E::scope;
    }
    OAuthTokens staged{}; Erase clear{&staged, sizeof(staged)};
    if (!token(f.values[0], staged.access, &staged.access_bytes) ||
        ((f.seen & 2) && !token(f.values[1], staged.refresh, &staged.refresh_bytes))) return E::malformed;
    staged.expires_seconds = static_cast<u32>(expires);
    *out = staged;
    return E::none;
}
} // namespace maicord
