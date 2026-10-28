// Pure loopback CAPTCHA page protocol, launcher shape and login presentation.
#include "maicord/protocol/captcha_bridge.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/browser_sign_in_launcher.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord::captcha_bridge_test {
using Check = void (*)(bool, const char*) noexcept;
constexpr Utf8View kState = utf8_literal("0123456789abcdef0123456789abcdef");
constexpr Utf8View kNonce = utf8_literal("fedcba9876543210fedcba9876543210");
constexpr u16 kPort = 50123;

struct Wire {
    u8 data[26 * 1024];
    u32 bytes;
    void add(Utf8View value) noexcept { copy_bytes(data + bytes, value.data, value.bytes); bytes += value.bytes; }
    void number(u32 value) noexcept {
        char digits[10]; u32 count = 0;
        do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
        while (count) data[bytes++] = static_cast<u8>(digits[--count]);
    }
};
Wire wire{};
CaptchaBridgeParseResult parse(Utf8View state = kState) noexcept {
    return captcha_bridge_parse_request({wire.data, wire.bytes}, kPort, state);
}
void get(Utf8View extra = {}, Utf8View target = utf8_literal("/c/0123456789abcdef0123456789abcdef")) noexcept {
    wire.bytes = 0;
    wire.add(utf8_literal("GET ")); wire.add(target);
    wire.add(utf8_literal(" HTTP/1.1\r\nHost: 127.0.0.1:50123\r\nSec-Fetch-Site: none\r\nSec-Fetch-Dest: document\r\n"));
    wire.add(extra); wire.add(utf8_literal("\r\n"));
}
void post(Utf8View body, Utf8View extra = {}, Utf8View origin = utf8_literal("Origin: http://127.0.0.1:50123\r\n"),
          Utf8View target = utf8_literal("/s/0123456789abcdef0123456789abcdef")) noexcept {
    wire.bytes = 0;
    wire.add(utf8_literal("POST ")); wire.add(target);
    wire.add(utf8_literal(" HTTP/1.1\r\nHost: 127.0.0.1:50123\r\n")); wire.add(origin);
    wire.add(utf8_literal("Sec-Fetch-Site: same-origin\r\nContent-Type: application/json\r\n"));
    wire.add(extra); wire.add(utf8_literal("Content-Length: ")); wire.number(body.bytes);
    wire.add(utf8_literal("\r\n\r\n")); wire.add(body);
}
bool contains(ConstSlice<u8> haystack, Utf8View needle) noexcept {
    if (needle.bytes > haystack.count) return false;
    for (u32 i = 0; i + needle.bytes <= haystack.count; ++i)
        if (bytes_equal(haystack.data + i, needle.data, needle.bytes)) return true;
    return false;
}
constexpr Utf8View kToken = utf8_literal("{\"token\":\"P1_eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.synthetic-solution_01\"}");

void protocol(Check check) noexcept {
    get();
    auto parsed = parse();
    check(parsed.ok() && parsed.request.kind == CaptchaBridgeRequestKind::page,
          "bridge parser: exact top-level GET for the state is admitted");
    wire.bytes -= 2;
    check(parse().state == CaptchaBridgeParseState::need_input, "bridge parser: unterminated head waits for input");
    post(kToken);
    parsed = parse();
    check(parsed.ok() && parsed.request.kind == CaptchaBridgeRequestKind::submit &&
              utf8_equal(parsed.request.token, utf8_literal("P1_eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9.synthetic-solution_01")),
          "bridge parser: same-origin JSON POST yields the borrowed token");
    post(kToken);
    wire.bytes -= 3;
    check(parse().state == CaptchaBridgeParseState::need_input, "bridge parser: partial body waits for Content-Length");
    post(kToken); wire.add(utf8_literal("GET"));
    check(parse().error == CaptchaBridgeProtocolError::framing, "bridge parser: pipelined trailing bytes are rejected");

    struct Case { Utf8View extra; Utf8View origin; CaptchaBridgeProtocolError error; const char* label; };
    const Case posts[]{
        {{}, {}, CaptchaBridgeProtocolError::origin, "bridge parser: POST without Origin is rejected"},
        {{}, utf8_literal("Origin: http://localhost:50123\r\n"), CaptchaBridgeProtocolError::origin, "bridge parser: foreign Origin is rejected"},
        {{}, utf8_literal("Origin: null\r\n"), CaptchaBridgeProtocolError::origin, "bridge parser: opaque Origin is rejected"},
        {utf8_literal("Origin: http://127.0.0.1:50123\r\n"), utf8_literal("Origin: http://127.0.0.1:50123\r\n"),
         CaptchaBridgeProtocolError::framing, "bridge parser: duplicate Origin is rejected"},
        {utf8_literal("Sec-Fetch-Site: cross-site\r\n"), utf8_literal("Origin: http://127.0.0.1:50123\r\n"),
         CaptchaBridgeProtocolError::framing, "bridge parser: duplicate Sec-Fetch-Site is rejected"},
        {utf8_literal("Transfer-Encoding: chunked\r\n"), utf8_literal("Origin: http://127.0.0.1:50123\r\n"),
         CaptchaBridgeProtocolError::framing, "bridge parser: Transfer-Encoding is rejected"},
        {utf8_literal("Expect: 100-continue\r\n"), utf8_literal("Origin: http://127.0.0.1:50123\r\n"),
         CaptchaBridgeProtocolError::framing, "bridge parser: Expect is rejected"},
        {utf8_literal("Cookie: a=b\r\n"), utf8_literal("Origin: http://127.0.0.1:50123\r\n"),
         CaptchaBridgeProtocolError::framing, "bridge parser: Cookie is rejected"},
        {utf8_literal("Authorization: Basic eA==\r\n"), utf8_literal("Origin: http://127.0.0.1:50123\r\n"),
         CaptchaBridgeProtocolError::framing, "bridge parser: Authorization is rejected"},
        {utf8_literal("Content-Length: 5\r\n"), utf8_literal("Origin: http://127.0.0.1:50123\r\n"),
         CaptchaBridgeProtocolError::framing, "bridge parser: duplicate Content-Length is rejected"},
    };
    for (const auto& item : posts) {
        post(kToken, item.extra, item.origin);
        const auto result = parse();
        check(!result.ok() && result.error == item.error, item.label);
    }
    wire.bytes = 0;
    wire.add(utf8_literal("POST /s/0123456789abcdef0123456789abcdef HTTP/1.1\r\nHost: 127.0.0.1:50123\r\n"
                          "Origin: http://127.0.0.1:50123\r\nSec-Fetch-Site: cross-site\r\nContent-Type: application/json\r\n"
                          "Content-Length: 2\r\n\r\n{}"));
    check(parse().error == CaptchaBridgeProtocolError::fetch_metadata, "bridge parser: cross-site fetch metadata is rejected");
    wire.bytes = 0;
    wire.add(utf8_literal("POST /s/0123456789abcdef0123456789abcdef HTTP/1.1\r\nHost: 127.0.0.1:50124\r\n"
                          "Origin: http://127.0.0.1:50123\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}"));
    check(parse().error == CaptchaBridgeProtocolError::host, "bridge parser: Host must name this listener exactly");
    wire.bytes = 0;
    wire.add(utf8_literal("POST /s/0123456789abcdef0123456789abcdef HTTP/1.1\r\nHost: 127.0.0.1:50123\r\n"
                          "Origin: http://127.0.0.1:50123\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\n{}"));
    check(parse().error == CaptchaBridgeProtocolError::framing, "bridge parser: only a JSON body is accepted");
    wire.bytes = 0;
    wire.add(utf8_literal("POST /s/0123456789abcdef0123456789abcdef HTTP/1.1\r\nHost: 127.0.0.1:50123\r\n"
                          "Origin: http://127.0.0.1:50123\r\nContent-Type: application/json\r\nContent-Length: 8193\r\n\r\n"));
    check(parse().error == CaptchaBridgeProtocolError::capacity, "bridge parser: bodies over 8 KiB are rejected");
    get(utf8_literal("Host: 127.0.0.1:50123\r\n"));
    check(parse().error == CaptchaBridgeProtocolError::framing, "bridge parser: duplicate Host is rejected");
    get(utf8_literal("X-Long: "));
    wire.bytes -= 2;
    while (wire.bytes < kCaptchaBridgeMaximumHeaderBytes) wire.data[wire.bytes++] = 'a';
    check(parse().error == CaptchaBridgeProtocolError::capacity, "bridge parser: headers over 16 KiB are rejected");
    wire.bytes = 0;
    wire.add(utf8_literal("GET /c/0123456789abcdef0123456789abcdef HTTP/1.1\nHost: 127.0.0.1:50123\r\n\r\n"));
    check(parse().error == CaptchaBridgeProtocolError::http, "bridge parser: bare LF framing is rejected");
    wire.bytes = 0;
    wire.add(utf8_literal("OPTIONS /s/0123456789abcdef0123456789abcdef HTTP/1.1\r\nHost: 127.0.0.1:50123\r\n\r\n"));
    check(parse().error == CaptchaBridgeProtocolError::route, "bridge parser: no CORS preflight handler exists");
    get({}, utf8_literal("/c/0123456789abcdef0123456789abcdef?x=1"));
    check(parse().error == CaptchaBridgeProtocolError::route, "bridge parser: query strings are not routes");
    get({}, utf8_literal("/s/0123456789abcdef0123456789abcdef"));
    check(parse().error == CaptchaBridgeProtocolError::route, "bridge parser: GET cannot reach the submit route");
    get({}, utf8_literal("/c/0123456789ABCDEF0123456789abcdef"));
    check(parse().error == CaptchaBridgeProtocolError::route, "bridge parser: state shape is lowercase hex only");
    get({}, utf8_literal("/c/0123456789abcdef0123456789abcdee"));
    check(parse().error == CaptchaBridgeProtocolError::state, "bridge parser: a different state is refused");
    get(utf8_literal("Origin: https://example.com\r\n"));
    check(parse().error == CaptchaBridgeProtocolError::origin, "bridge parser: cross-origin page fetch is refused");
    wire.bytes = 0;
    wire.add(utf8_literal("GET /c/0123456789abcdef0123456789abcdef HTTP/1.1\r\nHost: 127.0.0.1:50123\r\nSec-Fetch-Dest: iframe\r\n\r\n"));
    check(parse().error == CaptchaBridgeProtocolError::fetch_metadata, "bridge parser: framed or subresource loads are refused");
    wire.bytes = 0;
    wire.add(utf8_literal("GET /c/0123456789abcdef0123456789abcdef HTTP/1.1\r\nHost: 127.0.0.1:50123\r\nSec-Fetch-Site: cross-site\r\n\r\n"));
    check(parse().error == CaptchaBridgeProtocolError::fetch_metadata, "bridge parser: cross-site navigation is refused");

    const Utf8View bodies[]{utf8_literal("{\"token\":\"short\"}"), utf8_literal("{\"token\":\"has<angle>bracket-0123456789\"}"),
                            utf8_literal("{\"token\":\"esc\\u0061ped-0123456789abcdef\"}"),
                            utf8_literal("{\"token\":\"0123456789abcdef0123\",\"extra\":1}"),
                            utf8_literal("{\"token\":12345678901234567890}"), utf8_literal("[]")};
    for (const auto body : bodies) {
        post(body);
        const auto result = parse();
        check(!result.ok() && (result.error == CaptchaBridgeProtocolError::json || result.error == CaptchaBridgeProtocolError::token),
              "bridge parser: token charset, length, escapes and shape are enforced after the state matched");
    }
    static char long_token[kCaptchaBridgeMaximumTokenBytes + 32];
    u32 cursor = 0;
    for (const char c : "{\"token\":\"") if (c) long_token[cursor++] = c;
    for (u32 i = 0; i <= kCaptchaBridgeMaximumTokenBytes; ++i) long_token[cursor++] = 'a';
    long_token[cursor++] = '"'; long_token[cursor++] = '}';
    post({long_token, cursor});
    check(parse().error == CaptchaBridgeProtocolError::token, "bridge parser: tokens over 4096 bytes are refused");
    check(captcha_bridge_token_valid(utf8_literal("10000000-aaaa-bbbb-cccc-000000000001")) &&
              !captcha_bridge_token_valid(utf8_literal("0123456789abcdef 0123")),
          "bridge parser: provider test-key responses fit the token alphabet");
}

void page(Check check) noexcept {
    static u8 output[kCaptchaBridgeMaximumResponseBytes];
    struct ProviderCase { CaptchaBridgeProvider provider; Utf8View script; Utf8View absent; };
    const ProviderCase providers[]{
        {CaptchaBridgeProvider::hcaptcha, utf8_literal("https://js.hcaptcha.com/1/api.js"), utf8_literal("google")},
        {CaptchaBridgeProvider::recaptcha, utf8_literal("https://www.google.com/recaptcha/api.js"), utf8_literal("hcaptcha")},
        {CaptchaBridgeProvider::recaptcha_enterprise, utf8_literal("https://www.google.com/recaptcha/enterprise.js"), utf8_literal("cloudflare")},
        {CaptchaBridgeProvider::turnstile, utf8_literal("https://challenges.cloudflare.com/turnstile/v0/api.js"), utf8_literal("gstatic")},
    };
    for (const auto& provider : providers) {
        const auto written = captcha_bridge_write_page({utf8_literal("10000000-ffff-ffff-ffff-000000000001"),
            utf8_literal("rq+data/with=base64_-."), kState, kNonce, kPort, provider.provider, false}, {output, sizeof(output)});
        const ConstSlice<u8> response{output, written.bytes};
        u32 head = 0;
        while (head + 3 < written.bytes && !bytes_equal(output + head, "\r\n\r\n", 4)) ++head;
        const ConstSlice<u8> header{output, head};
        check(written.ok() && contains(header, utf8_literal("default-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")) &&
                  contains(header, utf8_literal("script-src 'nonce-fedcba9876543210fedcba9876543210' ")) &&
                  contains(header, utf8_literal("connect-src 'self' ")) && !contains(header, provider.absent) &&
                  contains(header, utf8_literal("Cache-Control: no-store")) && contains(header, utf8_literal("Referrer-Policy: no-referrer")) &&
                  contains(header, utf8_literal("X-Content-Type-Options: nosniff")) && !contains(header, utf8_literal("Access-Control")) &&
                  contains(response, provider.script) && contains(response, utf8_literal("nonce=\"fedcba9876543210fedcba9876543210\"")) &&
                  contains(response, utf8_literal("\"s\":\"/s/0123456789abcdef0123456789abcdef\"")) &&
                  !contains(response, utf8_literal("Discord")),
              "bridge page: provider-scoped nonce CSP, no-store headers, same-origin submit route and no foreign branding");
        u32 declared = 0, offset = 0;
        for (; offset + 16 < head; ++offset) if (bytes_equal(output + offset, "Content-Length: ", 16)) break;
        for (offset += 16; offset < head && output[offset] >= '0' && output[offset] <= '9'; ++offset) declared = declared * 10 + output[offset] - '0';
        check(declared == written.bytes - head - 4, "bridge page: Content-Length frames the complete document");
    }
    CaptchaBridgePage enterprise{utf8_literal("enterprise_sitekey"),          {},   kState, kNonce, kPort,
                                 CaptchaBridgeProvider::recaptcha_enterprise, false};
    auto enterprise_written = captcha_bridge_write_page(enterprise, {output, sizeof(output)});
    ConstSlice<u8> enterprise_response{output, enterprise_written.bytes};
    check(
        enterprise_written.ok() && contains(enterprise_response, utf8_literal("enterprise.js?render=")) &&
            contains(enterprise_response, utf8_literal("encodeURIComponent(c.k)")) &&
            contains(enterprise_response, utf8_literal("l.nonce=document.currentScript.nonce")) &&
            contains(enterprise_response, utf8_literal("l.async=true;l.defer=true")) &&
            contains(enterprise_response, utf8_literal("a.ready(function()")) &&
            contains(enterprise_response, utf8_literal("execute(c.k,c.a==null?undefined:{action:c.a})")) &&
            contains(enterprise_response, utf8_literal(".then(done,failed)")) &&
            contains(enterprise_response, utf8_literal("l.onerror=failed")) &&
            contains(enterprise_response, utf8_literal("Checking your browser&#8230;")) &&
            !contains(enterprise_response, utf8_literal("\"a\":")) &&
            !contains(enterprise_response, utf8_literal("a.render(")) &&
            !contains(enterprise_response, utf8_literal("execute(id)")) &&
            !contains(enterprise_response, utf8_literal("https://js.hcaptcha.com")) &&
            !contains(enterprise_response, utf8_literal("https://challenges.cloudflare.com")),
        "bridge enterprise: score loader executes the sitekey with no invented action or widget path");
    enterprise.user_flow = utf8_literal("flow/\"<>&'\\\n\xe2\x80\xa8\xe2\x80\xa9");
    enterprise_written = captcha_bridge_write_page(enterprise, {output, sizeof(output)});
    enterprise_response = {output, enterprise_written.bytes};
    check(enterprise_written.ok() &&
              contains(
                  enterprise_response,
                  utf8_literal(
                      "\"a\":\"flow/\\u0022\\u003c\\u003e\\u0026\\u0027\\u005c\\u000a\\u2028\\u2029\"")) &&
              !contains(enterprise_response, enterprise.user_flow),
          "bridge enterprise: server action is JSON escaped without executable markup or line separators");
    char oversized_flow[129];
    for (char& c : oversized_flow)
        c = 'a';
    const char malformed_flow[]{static_cast<char>(0xc0), static_cast<char>(0xaf)};
    const Utf8View invalid_flows[]{
        {nullptr, 1}, {oversized_flow, sizeof(oversized_flow)}, {malformed_flow, sizeof(malformed_flow)}};
    for (const Utf8View flow : invalid_flows) {
        enterprise.user_flow = flow;
        check(captcha_bridge_write_page(enterprise, {output, sizeof(output)}).error ==
                  CaptchaBridgeProtocolError::invalid_input,
              "bridge enterprise: malformed, oversized and invalid action views are rejected");
    }
    check(!captcha_bridge_write_page({utf8_literal("bad\"key"), {}, kState, kNonce, kPort, CaptchaBridgeProvider::hcaptcha, false},
                                     {output, sizeof(output)}).ok() &&
              !captcha_bridge_write_page({utf8_literal("key"), utf8_literal("<script>"), kState, kNonce, kPort, CaptchaBridgeProvider::hcaptcha, false},
                                         {output, sizeof(output)}).ok() &&
              !captcha_bridge_write_page({utf8_literal("key"), {}, kState, kNonce, kPort, CaptchaBridgeProvider::unknown, false},
                                         {output, sizeof(output)}).ok() &&
              captcha_bridge_write_page({utf8_literal("key"), {}, kState, kNonce, kPort, CaptchaBridgeProvider::hcaptcha, true},
                                        {output, 512}).error == CaptchaBridgeProtocolError::capacity,
          "bridge page: sitekey/rqdata alphabets, provider allowlist and capacity are enforced");
    u8 url[kCaptchaBridgeMaximumUrlBytes];
    const auto written = captcha_bridge_write_url(kPort, kState, {url, sizeof(url)});
    check(written.ok() && utf8_equal({reinterpret_cast<const char*>(url), written.bytes},
              utf8_literal("http://127.0.0.1:50123/c/0123456789abcdef0123456789abcdef")),
          "bridge url: only the state is carried in the loopback URL");
    SecureBuffer locked{};
    if (secure_buffer_initialize(&locked, 128).ok()) {
        secure_buffer_assign(&locked, {url, written.bytes});
        const bool accepted = captcha_bridge_url_valid(&locked);
        const char* rejected[]{"https://127.0.0.1:50123/c/0123456789abcdef0123456789abcdef",
                               "http://127.0.0.1:80/c/0123456789abcdef0123456789abcdef",
                               "http://127.0.0.1:50123/c/0123456789abcdef0123456789abcdeg",
                               "http://localhost:50123/c/0123456789abcdef0123456789abcdef",
                               "http://127.0.0.1:050123/c/0123456789abcdef0123456789abcdef",
                               "http://127.0.0.1:50123/c/0123456789abcdef0123456789abcdef0"};
        bool refused = true;
        for (const char* candidate : rejected) {
            secure_buffer_assign(&locked, {reinterpret_cast<const u8*>(candidate), c_string_length(candidate)});
            refused = refused && !captcha_bridge_url_valid(&locked);
        }
        locked.locked = false;
        secure_buffer_assign(&locked, {url, written.bytes});
        check(accepted && refused && !captcha_bridge_url_valid(&locked),
              "bridge launcher: only the exact locked loopback page shape can reach the shell");
        locked.locked = true;
        secure_buffer_release(&locked);
    }
}

void presentation(Check check) noexcept {
    check(login_control_action(LoginControl::submit, LoginStatus::captcha_required, {}) == LoginAction::captcha_verify &&
              login_control_action(LoginControl::submit, LoginStatus::recovery_phone_captcha_required, {}) == LoginAction::captcha_verify &&
              login_control_action(LoginControl::submit, LoginStatus::reset_captcha_required, {}, true) == LoginAction::captcha_verify &&
              login_control_action(LoginControl::submit, LoginStatus::idle, LoginRemoteStatus::captcha_required) == LoginAction::captcha_verify &&
              login_control_action(LoginControl::back, LoginStatus::captcha_required, {}) == LoginAction::back &&
              login_control_action(LoginControl::browser_sign_in, LoginStatus::captcha_required, {}) == LoginAction::browser_sign_in &&
              login_control_action(LoginControl::password, LoginStatus::captcha_required, {}) == LoginAction::none &&
              login_control_action(LoginControl::submit, LoginStatus::idle, {}) == LoginAction::submit,
          "captcha UI: challenge offers verify, browser sign-in fallback and Back, never credential entry");
    LoginScene scene{};
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream text{};
    ui_text_command_stream_initialize(&text, {commands, kMaximumLoginTextCommands});
    struct CopyCase { LoginCaptchaStatus status; Utf8View action; Utf8View message; bool enabled; };
    const CopyCase cases[]{
        {LoginCaptchaStatus::ready, utf8_literal("Verify in browser"), utf8_literal("Complete a quick check in your browser to continue."), true},
        {LoginCaptchaStatus::starting, utf8_literal("Opening browser…"), utf8_literal("Opening your browser…"), false},
        {LoginCaptchaStatus::waiting, utf8_literal("Open the check again"), utf8_literal("Finish the check in your browser, then come back."), true},
        {LoginCaptchaStatus::retry, utf8_literal("Try again in browser"), utf8_literal("That check didn't go through. Try again."), true},
        {LoginCaptchaStatus::expired, utf8_literal("Try again in browser"), utf8_literal("The browser check expired. Try again."), true},
        {LoginCaptchaStatus::failed, utf8_literal("Try again in browser"), utf8_literal("Couldn't open the browser check. Try again."), true},
    };
    for (const auto& item : cases) {
        LoginView view{};
        view.visible = true;
        view.status = LoginStatus::captcha_required;
        view.login = utf8_literal("native@example.com");
        view.focus = LoginControl::back;
        view.captcha_status = item.status;
        scene.focus = view.focus;
        scene.back_only = true;
        scene.browser_available = login_browser_available(view.status, view.remote_status, false);
        scene.credential_fields_hidden = login_credential_fields_hidden(view.status, view.remote_status, false);
        scene.recovery_disabled = login_view_recovery_disabled(view);
        scene.action_disabled = login_view_action_disabled(view);
        scene.submit_disabled = login_view_submit_disabled(view);
        scene.captcha_action = login_view_captcha_action(view);
        const bool laid_out = login_scene_layout(&scene, 480, 640) && login_view_matches_scene(view, &scene);
        const auto submit = login_scene_rectangle(scene, LoginElement::submit);
        const bool hit = login_scene_hit_test(scene, (submit.left + submit.right) / 2, (submit.top + submit.bottom) / 2) ==
                         (item.enabled ? LoginControl::submit : LoginControl::none);
        bool action = false, message = false, fallback = false;
        const bool built = laid_out && login_text_presentation_build(scene, view, kDefaultTheme, &text);
        for (u32 i = 0; built && i < text.count; ++i) {
            action |= utf8_equal(commands[i].text, item.action);
            message |= utf8_equal(commands[i].text, item.message);
            fallback |= utf8_equal(commands[i].text, utf8_literal("Use browser sign-in"));
        }
        check(laid_out && hit && built && action && message && fallback && scene.captcha_action == item.enabled &&
                  login_scene_focus(&scene, LoginControl::back),
              "captcha UI: each bridge state has concise copy, the right action availability and Back");
    }
    LoginView remote{};
    remote.visible = true;
    remote.status = LoginStatus::idle;
    remote.remote_status = LoginRemoteStatus::cancelled;
    remote.focus = LoginControl::login;
    scene = {};
    scene.focus = remote.focus;
    scene.browser_available = login_browser_available(remote.status, remote.remote_status, false);
    scene.recovery_disabled = login_view_recovery_disabled(remote);
    scene.password_reveal_available = login_view_password_reveal_available(remote);
    bool cancelled = false, refreshing = false;
    const bool built = login_scene_layout(&scene, 900, 640) && login_view_matches_scene(remote, &scene) &&
                       login_text_presentation_build(scene, remote, kDefaultTheme, &text);
    for (u32 i = 0; built && i < text.count; ++i) {
        cancelled |= utf8_equal(commands[i].text, utf8_literal("Login cancelled on your phone.")) &&
                     commands[i].color.red == kDefaultTheme.text_secondary.red;
        refreshing |= utf8_equal(commands[i].text, utf8_literal("Refreshing automatically"));
        check(!utf8_equal(commands[i].text, utf8_literal("Reconnecting…")), "QR decline: no network-error copy");
    }
    check(built && cancelled && refreshing && !login_requires_back(remote.status, remote.remote_status),
          "QR decline: phone cancellation reads as a decline while a fresh code is generated");
    const auto reveal = login_password_reveal_rectangle(scene);
    const auto field = login_scene_rectangle(scene, LoginElement::password_field);
    const auto content = login_scene_content_rectangle(scene, LoginElement::password_field);
    check(scene.password_reveal_available && ui_rect_has_area(reveal) && reveal.left >= content.right &&
              reveal.right <= field.right && login_password_reveal_hit_test(scene, (reveal.left + reveal.right) / 2, (reveal.top + reveal.bottom) / 2),
          "password reveal: the toggle sits in reserved field padding and never overlaps text");
    LoginView mfa = remote;
    mfa.status = LoginStatus::mfa_required;
    mfa.mfa_capabilities = login_mfa_capability_totp;
    mfa.selected_mfa_capability = login_mfa_capability_totp;
    LoginView phone{};
    phone.status = LoginStatus::recovery_phone_required;
    LoginView captcha{};
    captcha.status = LoginStatus::captcha_required;
    LoginView revealed = remote;
    revealed.password_revealed = true;
    LoginView invalid = mfa;
    invalid.password_revealed = true;
    check(!login_view_password_reveal_available(mfa) && !login_view_password_reveal_available(phone) &&
              !login_view_password_reveal_available(captcha) && login_view_valid(revealed) && !login_view_valid(invalid),
          "password reveal: offered for passwords only, never MFA, phone codes or blocked states");
    LoginView caps = remote;
    caps.remote_status = LoginRemoteStatus::idle;
    caps.caps_lock = true;
    caps.focus = LoginControl::password;
    scene.focus = caps.focus;
    bool hint = false;
    const bool caps_built = login_scene_layout(&scene, 900, 640) && login_text_presentation_build(scene, caps, kDefaultTheme, &text);
    for (u32 i = 0; caps_built && i < text.count; ++i) hint |= utf8_equal(commands[i].text, utf8_literal("Caps Lock is on."));
    check(caps_built && hint, "caps lock: the password field shows a concise warning");
    login_scene_release(&scene);
}
} // namespace maicord::captcha_bridge_test

namespace maicord {
void test_captcha_bridge_protocol(void (*check)(bool, const char*) noexcept) noexcept {
    captcha_bridge_test::protocol(check);
    captcha_bridge_test::page(check);
    captcha_bridge_test::presentation(check);
}
} // namespace maicord
