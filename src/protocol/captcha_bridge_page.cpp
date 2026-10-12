#include "maicord/protocol/captcha_bridge.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/http1.hpp"

#include <Windows.h>

namespace maicord {
namespace {

bool folded(Utf8View value, const char* literal) noexcept {
    return http_ascii_equal_case_insensitive({reinterpret_cast<const u8*>(value.data), value.bytes}, literal);
}
bool port_valid(u16 port) noexcept { return port >= 1024; }
bool alnum(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool lower_hex(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

// Appends when room remains and always counts, so one routine both measures
// and writes. Overflow is reported once at the end.
struct Writer {
    u8* data;
    u32 capacity;
    u32 bytes;
    bool overflow;
    void raw(const void* source, u32 count) noexcept {
        if (data && !overflow) {
            if (count <= capacity - bytes) copy_bytes(data + bytes, source, count);
            else overflow = true;
        }
        bytes += count;
    }
    void text(Utf8View value) noexcept { raw(value.data, value.bytes); }
    template <usize Count> void literal(const char (&value)[Count]) noexcept { raw(value, static_cast<u32>(Count - 1)); }
    void decimal(u32 value) noexcept {
        char digits[10];
        raw(digits, decimal_write(digits, value));
    }
    void json(Utf8View value) noexcept {
        constexpr char hex[] = "0123456789abcdef";
        raw("\"", 1);
        for (u32 i = 0; i < value.bytes; ++i) {
            const u8 c = static_cast<u8>(value.data[i]);
            const bool separator = c == 0xE2 && i + 2 < value.bytes && static_cast<u8>(value.data[i + 1]) == 0x80 &&
                (static_cast<u8>(value.data[i + 2]) == 0xA8 || static_cast<u8>(value.data[i + 2]) == 0xA9);
            if (separator) {
                const char escaped[6]{'\\', 'u', '2', '0', '2', static_cast<u8>(value.data[i + 2]) == 0xA8 ? '8' : '9'};
                raw(escaped, 6); i += 2;
            } else if (c < 0x20 || c == '"' || c == '\\' || c == '<' || c == '>' || c == '&' || c == '\'' || c == 0x7f) {
                const char escaped[6]{'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
                raw(escaped, 6);
            } else {
                raw(&value.data[i], 1);
            }
        }
        raw("\"", 1);
    }
};

CaptchaBridgeParseResult reject(CaptchaBridgeProtocolError error) noexcept { return {{}, error, CaptchaBridgeParseState::rejected}; }

struct TokenBody {
    Utf8View token;
    u32 fields;
};
bool token_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto* body = static_cast<TokenBody*>(opaque);
    JsonString token{};
    if (body->fields++ || !json_string_equals_ascii(name, utf8_literal("token")) || name.has_escapes ||
        !json_value_as_string(value, &token) || token.has_escapes)
        return false;
    body->token = token.encoded;
    return true;
}

struct ProviderSources {
    const char* scripts;
    const char* frames;
    const char* loader;
    char code;
};
ProviderSources provider_sources(CaptchaBridgeProvider provider) noexcept {
    switch (provider) {
    case CaptchaBridgeProvider::hcaptcha:
        return {"https://hcaptcha.com https://*.hcaptcha.com", "https://hcaptcha.com https://*.hcaptcha.com",
                "https://js.hcaptcha.com/1/api.js?render=explicit&amp;onload=mcReady&amp;recaptchacompat=off", 'h'};
    case CaptchaBridgeProvider::recaptcha:
        return {"https://www.google.com/recaptcha/ https://www.gstatic.com/recaptcha/",
                "https://www.google.com/recaptcha/ https://recaptcha.google.com/recaptcha/",
                "https://www.google.com/recaptcha/api.js?render=explicit&amp;onload=mcReady", 'r'};
    case CaptchaBridgeProvider::recaptcha_enterprise:
        return {"https://www.google.com/recaptcha/ https://www.gstatic.com/recaptcha/",
                "https://www.google.com/recaptcha/ https://recaptcha.google.com/recaptcha/",
                "https://www.google.com/recaptcha/enterprise.js?render=explicit&amp;onload=mcReady", 'e'};
    case CaptchaBridgeProvider::turnstile:
        return {"https://challenges.cloudflare.com", "https://challenges.cloudflare.com",
                "https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit&amp;onload=mcReady", 't'};
    default:
        return {};
    }
}
Utf8View cstring(const char* value) noexcept {
    u32 bytes = 0; while (value[bytes]) ++bytes;
    return {value, bytes};
}

constexpr char kPageHead[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta name=\"referrer\" content=\"no-referrer\"><title>MaiCord verification</title><style>"
    ":root{color-scheme:light dark;--bg:#f3f4f6;--card:#fff;--ink:#1c1e22;--muted:#5d6470;--line:#dfe2e7}"
    "@media (prefers-color-scheme:dark){:root{--bg:#141619;--card:#1e2126;--ink:#e9ebef;--muted:#9ba2ad;--line:#2d3138}}"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
    "padding:16px;background:var(--bg);color:var(--ink);font:15px/1.5 system-ui,-apple-system,\"Segoe UI\",sans-serif}"
    "main{width:100%;max-width:400px;padding:28px 24px;border:1px solid var(--line);border-radius:12px;"
    "background:var(--card);text-align:center}h1{margin:0 0 6px;font-size:20px;font-weight:600}"
    "p{margin:0;color:var(--muted)}#w{display:flex;justify-content:center;min-height:78px;margin:20px 0 0}"
    ".done p{color:var(--ink);font-weight:600}.done #w{display:none}"
    "</style></head><body><main><h1>Verify it&#39;s you</h1>"
    "<p id=\"s\">Complete the check below to continue in MaiCord.</p><div id=\"w\"></div>"
    "<noscript><p>Turn on JavaScript to complete the check.</p></noscript></main>"
    "<script type=\"application/json\" id=\"c\">";
// The widget callback posts only the solution token to the same origin.
constexpr char kPageScript[] =
    "(function(){var c=JSON.parse(document.getElementById(\"c\").textContent),"
    "s=document.getElementById(\"s\"),w=document.getElementById(\"w\"),sent=false;"
    "function say(t,d){s.textContent=t;if(d)document.body.className=\"done\"}"
    "function api(){var g=window.grecaptcha;return c.p==\"h\"?window.hcaptcha:c.p==\"t\"?window.turnstile:"
    "c.p==\"e\"?g&&g.enterprise:g}"
    "function done(t){if(sent||typeof t!=\"string\")return;sent=true;say(\"Sending the result to MaiCord\\u2026\");"
    "fetch(c.s,{method:\"POST\",headers:{\"Content-Type\":\"application/json\"},body:JSON.stringify({token:t}),"
    "credentials:\"omit\",cache:\"no-store\",redirect:\"error\",referrerPolicy:\"no-referrer\"})"
    ".then(function(r){say(r.ok?\"Done. You can return to MaiCord.\":"
    "\"This check expired. Return to MaiCord and try again.\",r.ok)})"
    ".catch(function(){say(\"Couldn\\u2019t reach MaiCord. Return to MaiCord and try again.\")})}"
    "function expired(){if(!sent)say(\"The check expired. Complete it again.\")}"
    "function failed(){if(!sent)say(\"The check couldn\\u2019t load. Return to MaiCord and try again.\")}"
    "window.mcReady=function(){var a=api();if(!a){failed();return}"
    "var o={sitekey:c.k,callback:done,\"expired-callback\":expired,\"error-callback\":failed,"
    "theme:matchMedia(\"(prefers-color-scheme: dark)\").matches?\"dark\":\"light\"};"
    "if(c.i&&c.p!=\"t\")o.size=\"invisible\";"
    "try{var id=a.render(w,o);if(c.p==\"h\"&&c.r)a.setData(id,{rqdata:c.r});"
    "if(c.i&&c.p!=\"t\")a.execute(id)}catch(e){failed()}}})();";

void write_csp(Writer& out, const ProviderSources& sources, Utf8View nonce) noexcept {
    out.literal("default-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'; "
                "object-src 'none'; script-src 'nonce-");
    out.text(nonce); out.literal("' "); out.text(cstring(sources.scripts));
    out.literal("; frame-src "); out.text(cstring(sources.frames));
    out.literal("; connect-src 'self' "); out.text(cstring(sources.scripts));
    out.literal("; img-src "); out.text(cstring(sources.scripts));
    // Widgets set inline styles on their own containers; no untrusted markup exists.
    out.literal("; style-src 'unsafe-inline' "); out.text(cstring(sources.scripts));
}
void write_body(Writer& out, const CaptchaBridgePage& page, const ProviderSources& sources) noexcept {
    out.literal(kPageHead);
    const char code[2]{sources.code, 0};
    out.literal("{\"p\":"); out.json({code, 1});
    out.literal(",\"k\":"); out.json(page.sitekey);
    out.literal(",\"r\":"); out.json(page.rqdata);
    if (page.invisible) out.literal(",\"i\":true");
    else out.literal(",\"i\":false");
    out.literal(",\"s\":\"/s/"); out.text(page.state); out.literal("\"}</script><script nonce=\"");
    out.text(page.nonce); out.literal("\">"); out.literal(kPageScript);
    out.literal("</script><script nonce=\""); out.text(page.nonce); out.literal("\" src=\"");
    out.text(cstring(sources.loader)); out.literal("\" async defer></script></body></html>");
}

} // namespace

bool captcha_bridge_hex_valid(Utf8View value, u32 bytes) noexcept {
    if (!value.data || value.bytes != bytes) return false;
    for (u32 i = 0; i < bytes; ++i) if (!lower_hex(value.data[i])) return false;
    return true;
}
bool captcha_bridge_sitekey_valid(Utf8View sitekey) noexcept {
    if (!sitekey.data || !sitekey.bytes || sitekey.bytes > kCaptchaBridgeMaximumSitekeyBytes) return false;
    for (u32 i = 0; i < sitekey.bytes; ++i) {
        const char c = sitekey.data[i];
        if (!alnum(c) && c != '-' && c != '_') return false;
    }
    return true;
}
bool captcha_bridge_rqdata_valid(Utf8View rqdata) noexcept {
    if ((!rqdata.data && rqdata.bytes) || rqdata.bytes > kCaptchaBridgeMaximumRqdataBytes) return false;
    for (u32 i = 0; i < rqdata.bytes; ++i) {
        const char c = rqdata.data[i];
        if (!alnum(c) && c != '-' && c != '_' && c != '+' && c != '/' && c != '=' && c != '.') return false;
    }
    return true;
}
bool captcha_bridge_token_valid(Utf8View token) noexcept {
    if (!token.data || token.bytes < kCaptchaBridgeMinimumTokenBytes || token.bytes > kCaptchaBridgeMaximumTokenBytes)
        return false;
    for (u32 i = 0; i < token.bytes; ++i) {
        const char c = token.data[i];
        if (!alnum(c) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
}

CaptchaBridgeParseResult captcha_bridge_parse_request(ConstSlice<u8> input, u16 port, Utf8View state) noexcept {
    if (!port_valid(port) || !input.data || !byte_range_valid(input) || !captcha_bridge_hex_valid(state, kCaptchaBridgeStateBytes))
        return reject(CaptchaBridgeProtocolError::invalid_input);
    if (input.count > kCaptchaBridgeMaximumHeaderBytes + kCaptchaBridgeMaximumBodyBytes)
        return reject(CaptchaBridgeProtocolError::capacity);
    u32 head_bytes = 0;
    for (u32 i = 0; i < input.count; ++i) {
        if (input[i] == '\n' && (!i || input[i - 1] != '\r')) return reject(CaptchaBridgeProtocolError::http);
        if (i + 3 < input.count && input[i] == '\r' && input[i + 1] == '\n' && input[i + 2] == '\r' && input[i + 3] == '\n') {
            head_bytes = i + 4; break;
        }
    }
    if (!head_bytes)
        return input.count >= kCaptchaBridgeMaximumHeaderBytes ? reject(CaptchaBridgeProtocolError::capacity) : CaptchaBridgeParseResult{};
    if (head_bytes > kCaptchaBridgeMaximumHeaderBytes) return reject(CaptchaBridgeProtocolError::capacity);
    const char* text = reinterpret_cast<const char*>(input.data);
    u32 line_end = 0;
    while (line_end + 1 < head_bytes && !(text[line_end] == '\r' && text[line_end + 1] == '\n')) ++line_end;
    u32 method_end = 0; while (method_end < line_end && text[method_end] != ' ') ++method_end;
    u32 target_end = method_end + 1; while (target_end < line_end && text[target_end] != ' ') ++target_end;
    if (!method_end || method_end >= line_end || target_end <= method_end + 1 || target_end >= line_end ||
        !utf8_equal({text + target_end + 1, line_end - target_end - 1}, utf8_literal("HTTP/1.1")))
        return reject(CaptchaBridgeProtocolError::http);
    const Utf8View method{text, method_end}, target{text + method_end + 1, target_end - method_end - 1};
    const bool get = utf8_equal(method, utf8_literal("GET")), post = utf8_equal(method, utf8_literal("POST"));
    if (!get && !post) return reject(CaptchaBridgeProtocolError::route);
    // Exact route shape; the secret comparison happens after header validation.
    if (target.bytes != 3 + kCaptchaBridgeStateBytes || target.data[0] != '/' || target.data[1] != (get ? 'c' : 's') ||
        target.data[2] != '/' || !captcha_bridge_hex_valid({target.data + 3, kCaptchaBridgeStateBytes}, kCaptchaBridgeStateBytes))
        return reject(CaptchaBridgeProtocolError::route);
    Utf8View host{}, origin{}, content_type{}, fetch_site{}, fetch_dest{};
    u32 length = 0, fields = 0, seen = 0;
    u32 cursor = line_end + 2;
    while (cursor < head_bytes - 2) {
        const u32 begin = cursor;
        while (cursor + 1 < head_bytes && !(text[cursor] == '\r' && text[cursor + 1] == '\n')) ++cursor;
        const u32 end = cursor; cursor += 2;
        if (++fields > 64 || begin == end || text[begin] == ' ' || text[begin] == '\t') return reject(CaptchaBridgeProtocolError::http);
        u32 colon = begin; while (colon < end && text[colon] != ':') ++colon;
        if (colon == begin || colon == end) return reject(CaptchaBridgeProtocolError::http);
        for (u32 i = begin; i < colon; ++i)
            if (!alnum(text[i]) && text[i] != '-') return reject(CaptchaBridgeProtocolError::http);
        for (u32 i = colon + 1; i < end; ++i)
            if ((static_cast<u8>(text[i]) < 0x20 && text[i] != '\t') || text[i] == 0x7f) return reject(CaptchaBridgeProtocolError::http);
        const Utf8View name{text + begin, colon - begin}, value = http_trim_ows({text + colon + 1, end - colon - 1});
        if (folded(name, "transfer-encoding") || folded(name, "expect") || folded(name, "cookie") ||
            folded(name, "authorization") || folded(name, "access-control-request-method"))
            return reject(CaptchaBridgeProtocolError::framing);
        const u32 bit = folded(name, "host") ? 1u : folded(name, "origin") ? 2u : folded(name, "content-length") ? 4u :
            folded(name, "content-type") ? 8u : folded(name, "sec-fetch-site") ? 16u : folded(name, "sec-fetch-dest") ? 32u : 0u;
        if (bit & seen) return reject(CaptchaBridgeProtocolError::framing);
        seen |= bit;
        if (bit == 1) host = value;
        else if (bit == 2) origin = value;
        else if (bit == 8) content_type = value;
        else if (bit == 16) fetch_site = value;
        else if (bit == 32) fetch_dest = value;
        else if (bit == 4) {
            if (!value.bytes) return reject(CaptchaBridgeProtocolError::framing);
            for (u32 i = 0; i < value.bytes; ++i) {
                const char c = value.data[i];
                if (c < '0' || c > '9' || length > kCaptchaBridgeMaximumBodyBytes) return reject(CaptchaBridgeProtocolError::framing);
                length = length * 10 + static_cast<u32>(c - '0');
            }
            if (length > kCaptchaBridgeMaximumBodyBytes) return reject(CaptchaBridgeProtocolError::capacity);
        }
    }
    u8 expected[32]; u32 expected_bytes = 0;
    {
        Writer out{expected, sizeof(expected), 0, false};
        out.literal("http://127.0.0.1:"); out.decimal(port);
        expected_bytes = out.bytes;
    }
    const Utf8View own_origin{reinterpret_cast<const char*>(expected), expected_bytes};
    const Utf8View own_host{own_origin.data + 7, own_origin.bytes - 7};
    if (!utf8_equal(host, own_host)) return reject(CaptchaBridgeProtocolError::host);
    if (get) {
        // A top-level navigation from the shell; never a subresource or frame.
        if ((seen & 2) && !utf8_equal(origin, own_origin)) return reject(CaptchaBridgeProtocolError::origin);
        if ((seen & 16) && !utf8_equal(fetch_site, utf8_literal("none")) && !utf8_equal(fetch_site, utf8_literal("same-origin")))
            return reject(CaptchaBridgeProtocolError::fetch_metadata);
        if ((seen & 32) && !utf8_equal(fetch_dest, utf8_literal("document"))) return reject(CaptchaBridgeProtocolError::fetch_metadata);
        if (length) return reject(CaptchaBridgeProtocolError::framing);
    } else {
        if (!(seen & 2) || !utf8_equal(origin, own_origin)) return reject(CaptchaBridgeProtocolError::origin);
        if ((seen & 16) && !utf8_equal(fetch_site, utf8_literal("same-origin"))) return reject(CaptchaBridgeProtocolError::fetch_metadata);
        if ((seen & 32) && !utf8_equal(fetch_dest, utf8_literal("empty"))) return reject(CaptchaBridgeProtocolError::fetch_metadata);
        if (!(seen & 4) || !length ||
            !(folded(content_type, "application/json") || folded(content_type, "application/json; charset=utf-8") ||
              folded(content_type, "application/json;charset=utf-8")))
            return reject(CaptchaBridgeProtocolError::framing);
    }
    u8 difference = 0;
    for (u32 i = 0; i < kCaptchaBridgeStateBytes; ++i)
        difference |= static_cast<u8>(target.data[3 + i] ^ state.data[i]);
    if (difference) return reject(CaptchaBridgeProtocolError::state);
    const u32 total = head_bytes + length;
    if (input.count < total) return {};
    if (input.count != total) return reject(CaptchaBridgeProtocolError::framing);
    CaptchaBridgeRequest request{};
    request.consumed_bytes = total;
    request.kind = get ? CaptchaBridgeRequestKind::page : CaptchaBridgeRequestKind::submit;
    if (get) return {request, CaptchaBridgeProtocolError::none, CaptchaBridgeParseState::complete};
    const Utf8View body{text + head_bytes, length};
    u32 structurals[32];
    const auto scan = json_scan_structurals_avx1(body, {structurals, 32});
    TokenBody token{};
    const bool parsed = scan.ok() &&
        json_visit_root_object(body, {structurals, scan.count}, 2, token_member, &token).ok() && token.fields == 1;
    SecureZeroMemory(structurals, sizeof(structurals));
    if (!parsed) return reject(CaptchaBridgeProtocolError::json);
    if (!captcha_bridge_token_valid(token.token)) return reject(CaptchaBridgeProtocolError::token);
    request.token = token.token;
    return {request, CaptchaBridgeProtocolError::none, CaptchaBridgeParseState::complete};
}

CaptchaBridgeWriteResult captcha_bridge_write_hex(ConstSlice<u8> random, Slice<u8> output) noexcept {
    if (!random.data || random.count != 16 || !output.data || !byte_range_valid(random) ||
        !byte_range_valid({output.data, output.count}) || byte_ranges_overlap(random, {output.data, output.count}))
        return {0, CaptchaBridgeProtocolError::invalid_input};
    if (output.count < 32) return {32, CaptchaBridgeProtocolError::capacity};
    constexpr char digits[] = "0123456789abcdef";
    for (u32 i = 0; i < 16; ++i) {
        output[i * 2] = static_cast<u8>(digits[random[i] >> 4]);
        output[i * 2 + 1] = static_cast<u8>(digits[random[i] & 15]);
    }
    return {32, CaptchaBridgeProtocolError::none};
}

CaptchaBridgeWriteResult captcha_bridge_write_url(u16 port, Utf8View state, Slice<u8> output) noexcept {
    if (!port_valid(port) || !captcha_bridge_hex_valid(state, kCaptchaBridgeStateBytes) || !output.data ||
        !byte_range_valid({output.data, output.count}) ||
        byte_ranges_overlap({output.data, output.count}, {reinterpret_cast<const u8*>(state.data), state.bytes}))
        return {0, CaptchaBridgeProtocolError::invalid_input};
    Writer out{output.data, output.count, 0, false};
    out.literal("http://127.0.0.1:"); out.decimal(port); out.literal("/c/"); out.text(state);
    if (out.overflow) { SecureZeroMemory(output.data, output.count); return {out.bytes, CaptchaBridgeProtocolError::capacity}; }
    return {out.bytes, CaptchaBridgeProtocolError::none};
}

CaptchaBridgeWriteResult captcha_bridge_write_page(const CaptchaBridgePage& page, Slice<u8> output) noexcept {
    const ProviderSources sources = provider_sources(page.provider);
    if (!sources.code || !port_valid(page.port) || !captcha_bridge_sitekey_valid(page.sitekey) ||
        !captcha_bridge_rqdata_valid(page.rqdata) || !captcha_bridge_hex_valid(page.state, kCaptchaBridgeStateBytes) ||
        !captcha_bridge_hex_valid(page.nonce, kCaptchaBridgeNonceBytes) || !output.data ||
        !byte_range_valid({output.data, output.count}))
        return {0, CaptchaBridgeProtocolError::invalid_input};
    const ConstSlice<u8> destination{output.data, output.count};
    const Utf8View sources_in[]{page.sitekey, page.rqdata, page.state, page.nonce};
    for (const Utf8View source : sources_in)
        if (byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(source.data), source.bytes}))
            return {0, CaptchaBridgeProtocolError::invalid_input};
    Writer measure{nullptr, 0, 0, false};
    write_body(measure, page, sources);
    Writer out{output.data, output.count, 0, false};
    out.literal("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Security-Policy: ");
    write_csp(out, sources, page.nonce);
    out.literal("\r\nCache-Control: no-store\r\nPragma: no-cache\r\nReferrer-Policy: no-referrer\r\n"
                "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nContent-Length: ");
    out.decimal(measure.bytes);
    out.literal("\r\nConnection: close\r\n\r\n");
    write_body(out, page, sources);
    if (out.overflow) { SecureZeroMemory(output.data, output.count); return {out.bytes, CaptchaBridgeProtocolError::capacity}; }
    return {out.bytes, CaptchaBridgeProtocolError::none};
}

CaptchaBridgeWriteResult captcha_bridge_write_response(CaptchaBridgeResponse response, Slice<u8> output) noexcept {
    if (!output.data || !byte_range_valid({output.data, output.count})) return {0, CaptchaBridgeProtocolError::invalid_input};
    Writer out{output.data, output.count, 0, false};
    if (response == CaptchaBridgeResponse::accepted) {
        out.literal("HTTP/1.1 204 No Content\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n"
                    "X-Content-Type-Options: nosniff\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    } else if (response == CaptchaBridgeResponse::gone) {
        constexpr char body[] = "This verification link has already been used or has expired. Return to MaiCord.";
        out.literal("HTTP/1.1 410 Gone\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\n"
                    "Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n"
                    "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\nContent-Length: ");
        out.decimal(sizeof(body) - 1); out.literal("\r\nConnection: close\r\n\r\n"); out.literal(body);
    } else {
        return {0, CaptchaBridgeProtocolError::invalid_input};
    }
    return out.overflow ? CaptchaBridgeWriteResult{out.bytes, CaptchaBridgeProtocolError::capacity}
                        : CaptchaBridgeWriteResult{out.bytes, CaptchaBridgeProtocolError::none};
}

} // namespace maicord
