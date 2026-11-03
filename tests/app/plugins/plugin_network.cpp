#include "maicord/app/plugins/consent.hpp"
#include "maicord/app/plugins/http.hpp"
#include "maicord/app/plugins/pipeline.hpp"
#include "maicord/app/plugins/ui.hpp"
#include "maicord/app/plugins/settings_page.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "plugins/binding_internal.hpp"
#include "fake_transport.hpp"
#include "../../support/test_output.hpp"
#include "plugin_fixture.hpp"

#include "maicord/core/win32_kernel.hpp"

// The HTTP broker, completions, gestures, typed and secret settings and the setting and keybind hooks,
// against the network fixture DLL (tests/fixtures/plugin_runtime_fixture.c, FIXTURE_NET) and a scripted
// transport in place of the network.
namespace maicord::plugin_network_tests {
using namespace plugin_runtime_detail;
using namespace plugin_fixture;

u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL plugin_network: ");
    test_write_line(label);
}

constexpr u64 kOwner = 0x4D4352544E455421ull;

// The layouts of FIXTURE_NET.
struct NetState {
    u32 sends, completions, setting_changes, keybinds;
    u32 last_kind, last_error, last_status, last_body_bytes, last_flags, body_sum;
    u32 last_setting, last_keybind, chained_flags, modal_result, modal_toggle, modal_select;
    u64 last_tag;
    i32 submit_status, chained_status, gesture_status;
    u32 url_bytes;
    char url[128];
};
struct NetSettings {
    mc_str secret;
    u32 colour, key;
    u64 channel;
    mc_ids users;
    mc_str label;
};
enum : u32 { kSecret, kColour, kKey, kChannel, kUsers, kLabel };

bool same(mc_str text, const char* expected) noexcept {
    return text.bytes == length(expected) && bytes_equal(text.data, expected, text.bytes);
}
// The file's bytes, or zero.
u32 read_file(const wchar_t* path, u8* output, u32 capacity) noexcept {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD read = 0;
    if (!ReadFile(file, output, capacity, &read, nullptr)) read = 0;
    CloseHandle(file);
    return read;
}
bool contains(const u8* bytes, u32 count, const char* needle) noexcept {
    const u32 n = length(needle);
    for (u32 i = 0; i + n <= count; ++i)
        if (bytes_equal(bytes + i, needle, n)) return true;
    return false;
}

// ---- The scripted network -----------------------------------------------------------------

struct Network {
    plugin_fake::Server api, cdn, other, google, deepl;
    u32 requests;
    // The last request the plugin made.
    char host[64];
    char target[128];
    bool post, user_agent, cookie, x_key, content_type, read_error_body, deepl_authorization;
    u32 header_count, body_bytes;
    u8 body[16];
    // Every host contacted, in order (the last 16).
    char contacted[16][64];
    u32 contacted_count;
};
Network g_network;

bool header_is(const HttpsHeader& header, const char* name) noexcept {
    return ascii_equal_ignore_case(header.name, {name, length(name)});
}
UpdateTransportError exchange(void* context, const UpdateHttpRequest& request, const HttpsExchangeOptions& options,
                              UpdateHttpResponse* response) noexcept {
    auto* network = static_cast<Network*>(context);
    ++network->requests;
    const u32 host = request.host_name.bytes < sizeof(network->host) ? request.host_name.bytes : 0;
    clear_bytes(network->host, sizeof(network->host));
    copy_bytes(network->host, request.host_name.data, host);
    char* slot = network->contacted[network->contacted_count++ % 16];
    clear_bytes(slot, 64);
    copy_bytes(slot, request.host_name.data, host);
    clear_bytes(network->target, sizeof(network->target));
    copy_bytes(network->target, request.target.data,
               request.target.count < sizeof(network->target) ? request.target.count : 0);
    network->post = options.body.count != 0;
    network->body_bytes = options.body.count;
    copy_bytes(network->body, options.body.data, options.body.count < sizeof(network->body) ? options.body.count : 0);
    network->header_count = options.headers.count;
    network->user_agent = network->cookie = network->x_key = network->content_type = network->deepl_authorization = false;
    for (u32 i = 0; i < options.headers.count; ++i) {
        const HttpsHeader& header = options.headers[i];
        if (header_is(header, "User-Agent"))
            network->user_agent = ascii_equal_ignore_case(header.value, utf8_literal("MaiCord-Plugin"));
        network->cookie = network->cookie || header_is(header, "Cookie");
        if (header_is(header, "Authorization"))
            network->deepl_authorization = ascii_equal_ignore_case(header.value, utf8_literal("DeepL-Auth-Key abc:fx"));
        network->x_key = network->x_key || header_is(header, "X-Key");
        network->content_type = network->content_type || header_is(header, "Content-Type");
    }
    network->read_error_body = options.read_error_body;
    plugin_fake::Server* servers[]{&network->api, &network->cdn, &network->other, &network->google, &network->deepl};
    for (plugin_fake::Server* server : servers) {
        const Utf8View origin{server->origin, c_string_length(server->origin)};
        if (utf8_equal(origin, request.host_name)) return plugin_fake::get(server, request, response);
    }
    *response = {};
    response->status = 404;
    return UpdateTransportError::none;
}

u8 g_big[2000];
void script() noexcept {
    for (u8& b : g_big) b = 'a';
    clear_bytes(&g_network, sizeof(g_network));
    plugin_fake::reset(g_network.api, "api.example.com");
    plugin_fake::reset(g_network.cdn, "img.cdn.example.net");
    plugin_fake::reset(g_network.other, "other.example.org");
    plugin_fake::reset(g_network.google, "translate.googleapis.com");
    plugin_fake::reset(g_network.deepl, "api-free.deepl.com");
    using plugin_fake::serve;
    constexpr UpdateHost provider = UpdateHost::provider;
    plugin_fake::Server& api = g_network.api;
    serve(api, provider, "/x", 200, "hello", 5);
    serve(api, provider, "/post", 200, "ok", 2);
    serve(api, provider, "/big", 200, g_big, sizeof(g_big));
    serve(api, provider, "/err", 500);
    serve(api, provider, "/r1", 302, nullptr, 0, "https://img.cdn.example.net/ok");
    serve(api, provider, "/r-evil", 302, nullptr, 0, "https://evil.example.com/x");
    serve(api, provider, "/r-other", 302, nullptr, 0, "https://other.example.org/x");
    serve(api, provider, "/r-discord", 302, nullptr, 0, "https://cdn.discordapp.com/x");
    serve(api, provider, "/r-http", 302, nullptr, 0, "http://api.example.com/x");
    serve(api, provider, "/r-relative", 302, nullptr, 0, "/x");
    serve(api, provider, "/r-userinfo", 302, nullptr, 0, "https://user@img.cdn.example.net/ok");
    serve(api, provider, "/loop", 302, nullptr, 0, "/loop");
    serve(api, provider, "/post-redirect", 307, nullptr, 0, "/post");
    serve(g_network.cdn, provider, "/ok", 200, "cdn!", 4);
    static const char translated[] = "[[[\"Hallo \",\"Hello \",null,null,10],[\"Welt\",\"world\",null,null,10]],null,\"en\"]";
    static const char deepl[] = "{\"translations\":[{\"detected_source_language\":\"EN\",\"text\":\"Hallo Welt\"}]}";
    serve(g_network.deepl, provider, "/v2/translate", 200, deepl, sizeof(deepl) - 1);
    serve(g_network.google, provider, "/translate_a/single?client=gtx&sl=auto&dt=t&tl=en", 200, translated, sizeof(translated) - 1);
}

// ---- Helpers ---------------------------------------------------------------------------------

void send(const char* text) noexcept {
    const PluginDraft draft{1, 2, 0, 0, {text, length(text)}};
    plugin_dispatch_draft(PluginHook::pre_send, draft);
}
// Delivers until `expected` completions arrived (or gives up).
u32 pump(PluginRuntime* runtime, u32 expected) noexcept {
    u32 delivered = 0;
    HANDLE notify = plugin_http_notification(runtime);
    for (u32 spin = 0; delivered < expected && spin < 300; ++spin) {
        delivered += plugin_http_deliver(runtime);
        if (delivered < expected && notify) WaitForSingleObject(notify, 50);
    }
    return delivered;
}
// One request through the plugin and back; the state shows the result.
const NetState& ask(PluginRuntime* runtime, const PluginBinding* binding, const char* text) noexcept {
    const auto& state = *static_cast<const NetState*>(binding->ctx.state);
    send(text);
    if (state.submit_status == MC_OK) pump(runtime, 1);
    return state;
}

constexpr u32 kApproved = 3;
PluginBinding* enable(PluginRuntime* runtime, u32 hosts = kApproved) noexcept {
    const PluginEnableResult enabled =
        plugin_runtime_enable(runtime, {L"net.dll", length(L"net.dll")}, MC_PERM_KNOWN, hosts);
    check(enabled.ok(), "the network plugin is enabled");
    return enabled.ok() ? enabled.binding : nullptr;
}

// ---- Tests ----------------------------------------------------------------------------------

void test_hosts() noexcept {
    const auto valid = [](const char* pattern) { return plugin_network_pattern_valid({pattern, length(pattern)}); };
    const auto denied = [](const char* host) { return plugin_network_host_denied({host, length(host)}); };
    const auto matches = [](const char* pattern, const char* host) {
        return plugin_network_pattern_matches({pattern, length(pattern)}, {host, length(host)});
    };
    check(valid("api.example.com") && valid("*.example.com") && valid("a-b.example.co.uk") && valid("notdiscord.com"),
          "ordinary DNS names and wildcards are valid hosts");
    check(!valid("") && !valid("*.com") && !valid("com") && !valid("localhost") && !valid("1.2.3.4") &&
              !valid("Example.com") && !valid("a.example.com:8443") && !valid("user@example.com") && !valid("a b.com") &&
              !valid("*.") && !valid("*example.com") && !valid("a..com"),
          "single labels, addresses, uppercase, ports and user info are not hosts");
    check(!valid("discord.com") && !valid("*.discord.com") && !valid("cdn.discordapp.com") && !valid("*.discordapp.com") &&
              !valid("gateway.discord.gg") && !valid("*.discord.media") && !valid("discordapp.net") &&
              !valid("a.b.discord.com") && !valid("*.discord.gift"),
          "Discord's domains and their subdomains cannot be declared");
    check(denied("discord.com") && denied("canary.discord.com") && denied("cdn.discordapp.com") && denied("x.discord.gg") &&
              denied("discord.media") && !denied("notdiscord.com") && !denied("discord.com.example.org") &&
              !denied("example.com"),
          "the deny list matches whole labels");
    check(matches("api.example.com", "api.example.com") && !matches("api.example.com", "x.api.example.com") &&
              matches("*.example.com", "a.example.com") && matches("*.example.com", "a.b.example.com") &&
              !matches("*.example.com", "example.com") && !matches("*.example.com", "badexample.com") &&
              !matches("*.example.com", ".example.com"),
          "wildcards match subdomains, never the apex or a longer name");
    const mc_str hosts[]{MC_S("a.example.com"), MC_S("b.example.com")}, repeated[]{MC_S("a.example.com"), MC_S("a.example.com")},
        discord[]{MC_S("discord.com")};
    mc_str many[MC_NETWORK_HOSTS_MAX + 1];
    for (mc_str& host : many) host = hosts[0];
    check(plugin_network_hosts_valid(hosts, 2, true) && plugin_network_hosts_valid(nullptr, 0, false) &&
              !plugin_network_hosts_valid(hosts, 2, false) && !plugin_network_hosts_valid(repeated, 2, true) &&
              !plugin_network_hosts_valid(discord, 1, true) && !plugin_network_hosts_valid(many, MC_NETWORK_HOSTS_MAX + 1, true),
          "a host list needs valid distinct hosts and a completion hook");
}

void test_secret_seal() noexcept {
    const char text[] = "s3cret-key";
    u8 sealed[2048], plain[2048];
    u32 sealed_bytes = 0, plain_bytes = 0;
    const mc_str key = MC_S("secret"), other = MC_S("another");
    const bool sealed_ok = plugin_secret_seal(kOwner, key, {reinterpret_cast<const u8*>(text), 10}, {sealed, sizeof(sealed)},
                                              &sealed_bytes);
    check(sealed_ok && sealed_bytes > 10 && !contains(sealed, sealed_bytes, "s3cret"), "a secret is sealed with DPAPI");
    check(plugin_secret_unseal(kOwner, key, {sealed, sealed_bytes}, {plain, sizeof(plain)}, &plain_bytes) &&
              plain_bytes == 10 && bytes_equal(plain, text, 10),
          "a sealed secret round-trips");
    check(!plugin_secret_unseal(kOwner + 1, key, {sealed, sealed_bytes}, {plain, sizeof(plain)}, &plain_bytes) &&
              !plugin_secret_unseal(kOwner, other, {sealed, sealed_bytes}, {plain, sizeof(plain)}, &plain_bytes),
          "another plugin or setting cannot open it");
    sealed[sealed_bytes / 2] ^= 0x55;
    check(!plugin_secret_unseal(kOwner, key, {sealed, sealed_bytes}, {plain, sizeof(plain)}, &plain_bytes),
          "a damaged blob is refused");
    check(!plugin_secret_seal(kOwner, key, {reinterpret_cast<const u8*>(text), 10}, {sealed, 8}, &sealed_bytes),
          "a short output buffer fails");
}

void test_requests(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    const auto& state = *static_cast<const NetState*>(binding->ctx.state);
    check((binding->permissions & MC_PERM_NETWORK) && binding->approved_hosts == kApproved,
          "declared hosts imply the network permission and carry the approved set");

    const NetState& got = ask(runtime, binding, "get https://api.example.com/x");
    check(got.submit_status == MC_OK && got.completions == 1 && got.last_kind == MC_COMPLETION_HTTP && got.last_error == MC_HTTP_OK &&
              got.last_status == 200 && got.last_body_bytes == 5 && got.body_sum == 'h' + 'e' + 'l' + 'l' + 'o' && got.last_tag == 1,
          "a GET completes with its status, body and tag");
    check(got.last_flags == MC_CTX_GESTURE, "sending is a gesture, so a request made while sending completes as one");
    check(!g_network.post && g_network.user_agent && !g_network.cookie && !g_network.x_key && g_network.read_error_body,
          "no cookie, no plugin headers, an identifying User-Agent, error bodies read");
    check(g_network.header_count == 1 && same({g_network.host, length(g_network.host), 0}, "api.example.com"),
          "one header, the declared host");
    check(length(g_network.target) == 2 && g_network.target[0] == '/', "the target is the path");

    const NetState& posted = ask(runtime, binding, "post https://api.example.com/post");
    check(posted.last_status == 200 && posted.last_tag == 2 && g_network.post && g_network.body_bytes == 7 &&
              bytes_equal(g_network.body, "payload", 7) && g_network.x_key && g_network.content_type && g_network.user_agent &&
              g_network.header_count == 3,
          "a POST sends its body and the plugin's headers");

    const NetState& missing = ask(runtime, binding, "get https://api.example.com/missing");
    check(missing.last_error == MC_HTTP_OK && missing.last_status == 404 && missing.last_body_bytes == 0, "404 is a response");
    const NetState& broken = ask(runtime, binding, "get https://api.example.com/err");
    check(broken.last_error == MC_HTTP_OK && broken.last_status == 500, "500 is a response");
    const NetState& large = ask(runtime, binding, "get https://api.example.com/big");
    check(large.last_status == 200 && large.last_body_bytes == 2000 && large.body_sum == 2000u * 'a',
          "a response up to the limit arrives whole");
    const NetState& small = ask(runtime, binding, "small https://api.example.com/big");
    check(small.last_error == MC_HTTP_E_TOO_LARGE && small.last_status == 0 && small.last_body_bytes == 0 && small.last_tag == 3,
          "a response past the plugin's limit fails as too large");
    ask(runtime, binding, "get https://sub.cdn.example.net/x");
    check(state.last_error == MC_HTTP_OK && state.last_status == 404 && same({g_network.host, length(g_network.host), 0}, "sub.cdn.example.net"),
          "a wildcard host is reached by name");
}

void test_refusals(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    const auto& state = *static_cast<const NetState*>(binding->ctx.state);
    (void)runtime;
    struct Case {
        const char* command;
        i32 status;
        const char* label;
    };
    static constexpr Case cases[]{
        {"get https://cdn.discordapp.com/x", MC_E_DENIED, "a Discord host is refused"},
        {"get https://discord.com/api/v9/users/@me", MC_E_DENIED, "Discord's API is refused"},
        {"get https://example.com/", MC_E_DENIED, "an undeclared host is refused"},
        {"get https://other.example.org/", MC_E_DENIED, "a declared host the user did not approve is refused"},
        {"get https://cdn.example.net/", MC_E_DENIED, "a wildcard does not cover its apex"},
        {"get http://api.example.com/x", MC_E_INVALID, "plain HTTP is refused"},
        {"get https://user@api.example.com/x", MC_E_INVALID, "user info is refused"},
        {"get https://api.example.com:8443/x", MC_E_INVALID, "a port is refused"},
        {"get https://api.example.com/x#frag", MC_E_INVALID, "a fragment is refused"},
        {"get https://api.example.com/a b", MC_E_INVALID, "a space in the path is refused"},
        {"get ftp://api.example.com/x", MC_E_INVALID, "another scheme is refused"},
        {"get https://", MC_E_INVALID, "an empty host is refused"},
        {"get https://127.0.0.1/x", MC_E_INVALID, "an address is refused"},
    };
    const u32 requests = g_network.requests, completions = state.completions;
    for (const Case& test : cases) {
        send(test.command);
        check(state.submit_status == test.status, test.label);
    }
    check(g_network.requests == requests && state.completions == completions, "a refused request never reaches the network");
}

// Direct submissions: what the fixture's fixed requests cannot reach.
void test_limits(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    (void)runtime;
    char url[MC_HTTP_URL_MAX + 64] = "https://api.example.com/";
    mc_http_request request{MC_HTTP_GET, 0, MC_S("https://api.example.com/x"), nullptr, 0, 0, nullptr, 0, 0};
    u8 body[MC_HTTP_BODY_MAX + 1] = {};
    check(plugin_http_submit(binding, request, 9) == MC_OK, "a plain request queues");
    pump(runtime, 1);

    mc_http_request post = request;
    post.method = MC_HTTP_POST;
    post.body = body;
    post.body_bytes = MC_HTTP_BODY_MAX + 1;
    check(plugin_http_submit(binding, post, 9) == MC_E_CAPACITY, "a POST body past the limit is refused");
    post.body_bytes = 0;
    check(plugin_http_submit(binding, post, 9) == MC_E_INVALID, "a POST needs a body");
    mc_http_request get_with_body = request;
    get_with_body.body = body;
    get_with_body.body_bytes = 3;
    check(plugin_http_submit(binding, get_with_body, 9) == MC_E_INVALID, "a GET cannot carry a body");
    mc_http_request bad = request;
    bad.method = 7;
    check(plugin_http_submit(binding, bad, 9) == MC_E_INVALID, "an unknown method is refused");
    bad = request;
    bad.max_response_bytes = MC_HTTP_RESPONSE_MAX + 1;
    check(plugin_http_submit(binding, bad, 9) == MC_E_INVALID, "a response limit past the maximum is refused");
    bad = request;
    bad.flags = 1;
    check(plugin_http_submit(binding, bad, 9) == MC_E_INVALID, "unknown flags are refused");
    for (u32 i = 24; i < sizeof(url) - 1; ++i) url[i] = 'a';
    bad = request;
    bad.url = {url, MC_HTTP_URL_MAX + 1, 0};
    check(plugin_http_submit(binding, bad, 9) == MC_E_INVALID, "a URL past the limit is refused");

    // Headers.
    mc_http_header headers[MC_HTTP_HEADERS_MAX + 1];
    for (mc_http_header& header : headers) header = {MC_S("X-A"), MC_S("v")};
    const char* names[]{"X-B", "X-C", "X-D", "X-E", "X-F", "X-G", "X-H", "X-I"};
    for (u32 i = 0; i < MC_HTTP_HEADERS_MAX + 1; ++i) headers[i].name = {names[i], 3, 0};
    mc_http_request with_headers = request;
    with_headers.headers = headers;
    with_headers.header_count = MC_HTTP_HEADERS_MAX;
    check(plugin_http_submit(binding, with_headers, 9) == MC_OK, "eight headers are accepted");
    pump(runtime, 1);
    with_headers.header_count = MC_HTTP_HEADERS_MAX + 1;
    check(plugin_http_submit(binding, with_headers, 9) == MC_E_INVALID, "nine headers are refused");
    const char* forbidden[]{"Cookie", "Host", "Content-Length", "Transfer-Encoding", "Connection", "Accept-Encoding",
                            "User-Agent", "Set-Cookie", "cookie", "X Bad", "X:Bad", ""};
    bool all_refused = true;
    for (const char* name : forbidden) {
        mc_http_header one{{name, length(name), 0}, MC_S("v")};
        with_headers.headers = &one;
        with_headers.header_count = 1;
        all_refused = all_refused && plugin_http_submit(binding, with_headers, 9) == MC_E_INVALID;
    }
    check(all_refused, "credential and framing headers are refused");
    mc_http_header duplicate[]{{MC_S("X-A"), MC_S("1")}, {MC_S("x-a"), MC_S("2")}};
    with_headers.headers = duplicate;
    with_headers.header_count = 2;
    check(plugin_http_submit(binding, with_headers, 9) == MC_E_INVALID, "a repeated header is refused");
    mc_http_header control{MC_S("X-A"), {"a\nb", 3, 0}};
    with_headers.headers = &control;
    with_headers.header_count = 1;
    check(plugin_http_submit(binding, with_headers, 9) == MC_E_INVALID, "a header value with a line break is refused");
}

void test_redirects(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    const auto& state = *static_cast<const NetState*>(binding->ctx.state);
    ask(runtime, binding, "get https://api.example.com/r1");
    check(state.last_error == MC_HTTP_OK && state.last_status == 200 && state.body_sum == 'c' + 'd' + 'n' + '!' &&
              same({g_network.host, length(g_network.host), 0}, "img.cdn.example.net"),
          "a redirect to another approved host is followed");
    ask(runtime, binding, "get https://api.example.com/r-relative");
    check(state.last_status == 200 && state.last_body_bytes == 5, "a relative redirect stays on the host");
    struct Case {
        const char* command;
        const char* label;
    };
    static constexpr Case refused[]{
        {"get https://api.example.com/r-evil", "a redirect to an undeclared host is refused"},
        {"get https://api.example.com/r-other", "a redirect to a host the user did not approve is refused"},
        {"get https://api.example.com/r-discord", "a redirect to Discord is refused"},
        {"get https://api.example.com/r-http", "a redirect to plain HTTP is refused"},
        {"get https://api.example.com/r-userinfo", "a redirect carrying user info is refused"},
        {"get https://api.example.com/loop", "a redirect loop ends after the limit"},
        {"post https://api.example.com/post-redirect", "a POST is never redirected"},
    };
    for (const Case& test : refused) {
        g_network.contacted_count = 0;
        clear_bytes(g_network.contacted, sizeof(g_network.contacted));
        ask(runtime, binding, test.command);
        check(state.last_error == MC_HTTP_E_REDIRECT && state.last_status == 0 && state.last_body_bytes == 0, test.label);
    }
    // The loop followed its three redirects: four requests, all to the one host.
    g_network.contacted_count = 0;
    ask(runtime, binding, "get https://api.example.com/loop");
    check(g_network.contacted_count == 4, "a redirect chain is cut after MC_HTTP_REDIRECTS_MAX hops");
    // Headers meant for the first host do not follow a redirect to another.
    ask(runtime, binding, "get https://api.example.com/r1");
    check(g_network.header_count == 1 && !g_network.x_key, "a redirect to another host drops the plugin's headers");
    // Nothing reached the refused hosts.
    bool reached_refused = false;
    for (u32 i = 0; i < 16; ++i) {
        const char* host = g_network.contacted[i];
        reached_refused = reached_refused || same({host, length(host), 0}, "evil.example.com") ||
                          same({host, length(host), 0}, "other.example.org") || same({host, length(host), 0}, "cdn.discordapp.com");
    }
    check(!reached_refused, "refused redirect targets are never contacted");
}

void test_queue_limits(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    const auto& state = *static_cast<const NetState*>(binding->ctx.state);
    for (u32 i = 0; i < MC_HTTP_PENDING_MAX; ++i) {
        send("get https://api.example.com/x");
        check(state.submit_status == MC_OK, "requests queue up to the pending limit");
    }
    send("get https://api.example.com/x");
    check(state.submit_status == MC_E_BUSY, "a request past the pending limit is refused");
    check(pump(runtime, MC_HTTP_PENDING_MAX) == MC_HTTP_PENDING_MAX, "all queued requests complete");
    send("get https://api.example.com/x");
    check(state.submit_status == MC_OK, "delivered requests free their place");
    pump(runtime, 1);
}

void test_rate_limit(PluginRuntime* runtime) noexcept {
    PluginBinding* binding = enable(runtime);
    if (!binding) return;
    const auto& state = *static_cast<const NetState*>(binding->ctx.state);
    bool all_accepted = true;
    for (u32 i = 0; i < MC_HTTP_REQUESTS_PER_MINUTE; ++i) {
        ask(runtime, binding, "get https://api.example.com/x");
        all_accepted = all_accepted && state.submit_status == MC_OK;
    }
    check(all_accepted && state.completions == MC_HTTP_REQUESTS_PER_MINUTE, "a plugin may make 30 requests a minute");
    send("get https://api.example.com/x");
    check(state.submit_status == MC_E_RATE_LIMITED, "the 31st request in the window is refused");
    check(plugin_runtime_disable(runtime, binding), "the rate-limit plugin is disabled");
}

void test_gestures(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    const auto& state = *static_cast<const NetState*>(binding->ctx.state);
    send("url https://api.example.com/x");
    // A modal the user accepted is a gesture; the request it starts carries it into its completion.
    PluginCompletion modal{};
    modal.kind = PluginCompletionKind::modal;
    modal.tag = 50;
    modal.gesture = true;
    modal.accepted = 1;
    modal.toggle = 1;
    modal.select_index = 2;
    modal.text = utf8_literal("hi");
    check(plugin_dispatch_completion(kOwner, modal) && state.modal_result == 1 && state.modal_toggle == 1 &&
              state.modal_select == 2 && state.gesture_status == MC_OK && state.last_flags == MC_CTX_GESTURE,
          "an accepted modal calls the completion hook as a gesture and may start a request");
    check(pump(runtime, 1) == 1 && state.last_tag == 100 && state.last_status == 200 && (state.last_flags & MC_CTX_GESTURE),
          "the request's completion inherits the gesture");
    check(state.chained_status == MC_OK && pump(runtime, 1) == 1 && state.last_tag == 101 && (state.chained_flags & MC_CTX_GESTURE),
          "a request started inside the inherited gesture keeps it");
    // Without a gesture there is none to inherit.
    // A request made outside any gesture (a timer, an event, this test) completes without one.
    const mc_http_request request{MC_HTTP_GET, 0, MC_S("https://api.example.com/x"), nullptr, 0, 0, nullptr, 0, 0};
    check(plugin_http_submit(binding, request, 5) == MC_OK && pump(runtime, 1) == 1 && state.last_tag == 5,
          "a request made outside a gesture completes");
    check(state.last_flags == 0, "and carries no gesture");
    // The window ends: a stale deadline gives no gesture, a live one does.
    PluginCompletion http{};
    http.kind = PluginCompletionKind::http;
    http.tag = 7;
    http.inherited_gesture_until_ms = 1;
    check(plugin_dispatch_completion(kOwner, http) && state.last_tag == 7 && state.last_flags == 0,
          "an expired gesture window is not inherited");
    http.inherited_gesture_until_ms = clock_monotonic_milliseconds() + 60'000;
    check(plugin_dispatch_completion(kOwner, http) && (state.last_flags & MC_CTX_GESTURE), "a live window is");
    PluginCompletion cancelled{};
    cancelled.kind = PluginCompletionKind::banner;
    cancelled.tag = 1;
    cancelled.which = 2;
    check(plugin_dispatch_completion(kOwner, cancelled) && state.last_kind == MC_COMPLETION_BANNER && state.last_flags == 0,
          "a banner dismissal is not a gesture");
    check(!plugin_dispatch_completion(kOwner + 5, cancelled), "a plugin that is not enabled receives nothing");
}

void test_revocation(PluginRuntime* runtime, PluginBinding* binding) noexcept {
    const auto& state = *static_cast<const NetState*>(binding->ctx.state);
    check(plugin_binding_set_host(binding, 0, false) && !plugin_binding_set_host(binding, 3, true), "a host can be revoked");
    send("get https://api.example.com/x");
    check(state.submit_status == MC_E_DENIED, "a revoked host is refused");
    ask(runtime, binding, "get https://sub.cdn.example.net/x");
    check(state.submit_status == MC_OK && state.last_status == 404, "other approved hosts still work");
    check(plugin_binding_set_host(binding, 0, true), "and approved again");
    ask(runtime, binding, "get https://api.example.com/x");
    check(state.submit_status == MC_OK && state.last_status == 200, "an approved host works again");
}

void test_disable_discards(PluginRuntime* runtime) noexcept {
    PluginBinding* binding = enable(runtime);
    if (!binding) return;
    send("get https://api.example.com/x");
    check(plugin_runtime_disable(runtime, binding), "a plugin with a request in flight is disabled");
    binding = enable(runtime);
    if (!binding) return;
    const auto& state = *static_cast<const NetState*>(binding->ctx.state);
    Sleep(50);
    plugin_http_deliver(runtime);
    check(state.completions == 0, "a result for a disabled plugin never reaches its next enable");
    for (u32 i = 0; i < kPluginMaximumBindings; ++i) ask(runtime, binding, "get https://api.example.com/x");
    check(state.submit_status == MC_OK || state.submit_status == MC_E_RATE_LIMITED, "its broker slots were released");
    check(plugin_runtime_disable(runtime, binding), "the plugin is disabled again");
}

void test_consent(PluginBinding* binding) noexcept {
    PluginImage image{};
    image.descriptor = binding->image.descriptor;
    image.info = binding->image.info;
    image.permissions = binding->permissions;
    Utf8View names[MC_NETWORK_HOSTS_MAX];
    const PluginConsentRequest built = plugin_consent_request(image, names);
    check(built.hosts.count == 3 && utf8_equal(built.hosts[1], utf8_literal("*.cdn.example.net")) && built.name.bytes != 0,
          "a request is built from a loaded plugin");
}

// The page's editors: one per kind of text row, keyed by the setting.
struct Editors {
    UiSettingsEditor editor;
    u32 asked, masked, multiline;
};
const UiSettingsEditor* editor_for(void* context, u32, bool multiline, bool masked) noexcept {
    auto* editors = static_cast<Editors*>(context);
    ++editors->asked;
    editors->masked += masked;
    editors->multiline += multiline;
    return &editors->editor;
}
const UiSettingsControl* row(const PluginSettingsPage& page, u64 id) noexcept {
    for (u32 i = 0; i < page.count; ++i)
        if (page.ids[i] == id) return &page.controls[i];
    return nullptr;
}

void test_page(PluginBinding* binding) noexcept {
    static PluginSettingsPage page;
    static Editors editors;
    clear_bytes(&page, sizeof(page));
    clear_bytes(&editors, sizeof(editors));
    const PluginSettingsHost host{&editors, editor_for};
    const auto* settings = static_cast<const NetSettings*>(binding->ctx.settings);
    const auto* state = static_cast<const NetState*>(binding->ctx.state);
    plugin_settings_project(*binding, host, &page);
    check(page.count == 10 && page.content.revision == 1 && editors.asked == 6 && editors.masked == 1 && editors.multiline == 1,
          "every setting and host is a row; secrets are masked");
    const UiSettingsControl* secret = row(page, kSecret + 1);
    const UiSettingsControl* label = row(page, kLabel + 1);
    check(secret && secret->kind == UiSettingsControlKind::edit && secret->editor && same({secret->label.data, secret->label.bytes, 0}, "API key") &&
              row(page, kUsers + 1)->kind == UiSettingsControlKind::multiline && label &&
              row(page, kPluginSettingsNetworkHeader)->kind == UiSettingsControlKind::category,
          "rows follow the schema");
    const UiSettingsControl* host0 = row(page, kPluginSettingsHostBase);
    check(host0 && host0->kind == UiSettingsControlKind::toggle && host0->checked && row(page, kPluginSettingsHostBase + 1)->checked &&
              !row(page, kPluginSettingsHostBase + 2)->checked &&
              same({host0->label.data, host0->label.bytes, 0}, "api.example.com"),
          "each declared host is a switch showing its approval");

    // Switching hosts changes what the plugin may reach.
    UiSettingsActivation activation{};
    activation.target = {UiSettingsTargetKind::control, {kOwner, 1}, kPluginSettingsHostBase + 2};
    activation.kind = UiSettingsControlKind::toggle;
    activation.checked = true;
    check(plugin_settings_activate(binding, &page, activation) == PluginSettingsOutcome::changed && binding->approved_hosts == 7,
          "a host switch approves it");
    activation.target.control_id = kPluginSettingsHostBase;
    activation.checked = false;
    check(plugin_settings_activate(binding, &page, activation) == PluginSettingsOutcome::changed && binding->approved_hosts == 6, "and revokes");
    plugin_binding_set_host(binding, 0, true);
    plugin_binding_set_host(binding, 2, false);
    activation.target.control_id = 55;
    check(plugin_settings_activate(binding, &page, activation) == PluginSettingsOutcome::none, "an unknown row does nothing");

    // Typed values.
    const u32 changes = state->setting_changes;
    check(plugin_settings_commit(binding, &page, kColour + 1, utf8_literal(" #00ff10 ")) == PluginSettingsOutcome::changed &&
              settings->colour == 0x00FF10 && state->setting_changes == changes + 1 && state->last_setting == kColour,
          "a typed colour is a setting change");
    check(plugin_settings_commit(binding, &page, kColour + 1, utf8_literal("green")) == PluginSettingsOutcome::refused &&
              page.rejected == kColour + 1 && settings->colour == 0x00FF10 && state->setting_changes == changes + 1,
          "a refused value changes nothing and marks the row");
    plugin_settings_project(*binding, host, &page);
    check(page.count == 11 && page.controls[kColour + 1].kind == UiSettingsControlKind::error, "an error row follows it");
    check(plugin_settings_commit(binding, &page, kColour + 1, utf8_literal("#336699")) == PluginSettingsOutcome::changed && !page.rejected,
          "a good value clears the mark");
    check(plugin_settings_commit(binding, &page, kKey + 1, utf8_literal("ctrl+shift+k")) == PluginSettingsOutcome::changed &&
              settings->key == MC_KEY('K', MC_KEY_CTRL | MC_KEY_SHIFT) &&
              plugin_settings_commit(binding, &page, kKey + 1, utf8_literal("Ctrl+Shift")) == PluginSettingsOutcome::refused &&
              plugin_settings_commit(binding, &page, kKey + 1, utf8_literal("F5")) == PluginSettingsOutcome::changed &&
              settings->key == 0x74 && plugin_settings_commit(binding, &page, kKey + 1, utf8_literal("")) == PluginSettingsOutcome::changed &&
              settings->key == 0,
          "keys are typed as Ctrl+Shift+K, F-keys by number, or cleared");
    check(plugin_settings_commit(binding, &page, kChannel + 1, utf8_literal("42")) == PluginSettingsOutcome::changed && settings->channel == 42 &&
              plugin_settings_commit(binding, &page, kChannel + 1, utf8_literal("4x")) == PluginSettingsOutcome::refused,
          "a channel is its id");
    check(plugin_settings_commit(binding, &page, kUsers + 1, utf8_literal("1\n2, 3")) == PluginSettingsOutcome::changed &&
              settings->users.count == 3 && settings->users.items[2] == 3 &&
              plugin_settings_commit(binding, &page, kUsers + 1, utf8_literal("1\nx")) == PluginSettingsOutcome::refused &&
              plugin_settings_commit(binding, &page, kUsers + 1, utf8_literal("0")) == PluginSettingsOutcome::refused,
          "users are ids on separate lines");
    check(plugin_settings_commit(binding, &page, kSecret + 1, utf8_literal("hunter2")) == PluginSettingsOutcome::changed &&
              same(settings->secret, "hunter2") && plugin_settings_commit(binding, &page, 1000 + 7, utf8_literal("x")) == PluginSettingsOutcome::none,
          "a typed secret is set");

    // What an editor shows.
    char text[64];
    const auto shown = [&](u32 setting, const char* expected) noexcept {
        const u32 bytes = plugin_setting_format(*binding, setting, {text, sizeof(text)});
        return bytes == length(expected) && bytes_equal(text, expected, bytes);
    };
    plugin_settings_commit(binding, &page, kKey + 1, utf8_literal("Ctrl+Alt+Delete"));
    check(shown(kColour, "#336699") && shown(kKey, "Ctrl+Alt+Delete") && shown(kChannel, "42") && shown(kUsers, "1\n2\n3") &&
              shown(kSecret, "hunter2") && shown(kLabel, "hi"),
          "values are shown as they are typed");
    plugin_settings_commit(binding, &page, kKey + 1, utf8_literal(""));
}

// The digits after `key` in the command index JSON.
u64 index_number(const char* json, u32 bytes, const char* key) noexcept {
    const u32 n = length(key);
    for (u32 i = 0; i + n < bytes; ++i) {
        if (!bytes_equal(json + i, key, n)) continue;
        u32 at = i + n;
        while (at < bytes && (json[at] < '0' || json[at] > '9')) ++at;
        u64 value = 0;
        for (; at < bytes && json[at] >= '0' && json[at] <= '9'; ++at) value = value * 10 + static_cast<u64>(json[at] - '0');
        return value;
    }
    return 0;
}
Utf8View g_inserted;
char g_inserted_text[128];
mc_status capture_insert(Utf8View text) noexcept {
    g_inserted = {g_inserted_text, text.bytes < sizeof(g_inserted_text) ? text.bytes : 0};
    copy_bytes(g_inserted_text, text.data, g_inserted.bytes);
    return MC_OK;
}

// The Translate sample end to end: /tr -> POST to the translation host -> completion -> composer insert,
// which needs the gesture the command began with.
void test_translate(PluginRuntime* runtime, const Root& root) noexcept {
    if (!install(root, L"translate.dll", L"translate.dll")) {
        check(false, "the translate sample is built");
        return;
    }
    const PluginEnableResult enabled = plugin_runtime_enable(runtime, {L"translate.dll", length(L"translate.dll")}, MC_PERM_KNOWN, 1);
    check(enabled.ok(), "the translate sample is enabled with its first host approved");
    if (!enabled.ok()) return;
    PluginPipelineSink sink{};
    sink.composer_insert = capture_insert;
    plugin_pipeline_set_sink(&sink);
    static char json[4096];
    const u32 bytes = plugin_command_index_json({json, sizeof(json)});
    const u64 application = index_number(json, bytes, "\"application_id\":");
    const u64 command = index_number(json, bytes, "\"application_commands\":[{\"id\":");
    static u32 scratch[256];
    const Utf8View options = utf8_literal("[{\"name\":\"text\",\"type\":3,\"value\":\"Hello world\"}]");
    g_inserted = {};
    const PluginCommandResult ran = plugin_command_run(application, command, 1, 2, options, {scratch, 256});
    check(application && command && ran.outcome == PluginCommandOutcome::none, "/tr runs and waits for the network");
    check(pump(runtime, 1) == 1, "the translation completes");
    check(g_network.post && same({g_network.host, length(g_network.host), 0}, "translate.googleapis.com") && g_network.content_type &&
              g_network.body_bytes > 2 && g_network.body[0] == 'q' && g_network.body[1] == '=',
          "the request went to the translation host as a form POST");
    check(same({g_inserted.data, g_inserted.bytes, 0}, "Hallo Welt"),
          "the translation reaches the composer through the command's inherited gesture");
    // DeepL with a free-plan key: its own host, the key as an Authorization header, JSON back.
    const u32 service = 1;
    plugin_binding_set_host(enabled.binding, 1, true);
    check(plugin_binding_set_setting(enabled.binding, 2, &service, 4) == MC_OK &&
              plugin_binding_set_setting(enabled.binding, 3, "abc:fx", 6) == MC_OK,
          "the DeepL service and key are set");
    g_inserted = {};
    g_network.deepl_authorization = false;
    check(plugin_command_run(application, command, 1, 2, options, {scratch, 256}).outcome == PluginCommandOutcome::none &&
              pump(runtime, 1) == 1,
          "/tr asks DeepL");
    check(g_network.deepl_authorization && same({g_network.host, length(g_network.host), 0}, "api-free.deepl.com") &&
              same({g_inserted.data, g_inserted.bytes, 0}, "Hallo Welt"),
          "the secret key travels as the plugin's Authorization header to the free-plan host");
    plugin_pipeline_set_sink(nullptr);
    check(plugin_runtime_disable(runtime, enabled.binding), "the translate sample is disabled");
}

void test_settings(PluginRuntime* runtime, const Root& root) noexcept {
    // Earlier tests left their values behind: start from the defaults.
    wchar_t earlier[MAX_PATH], earlier_owner[17];
    for (u32 i = 0; i < 16; ++i) earlier_owner[i] = L"0123456789abcdef"[(kOwner >> (60 - 4 * i)) & 15];
    earlier_owner[16] = 0;
    join(earlier, root.data, earlier_owner);
    remove_tree(earlier);
    PluginBinding* binding = enable(runtime);
    if (!binding) return;
    auto* state = static_cast<NetState*>(binding->ctx.state);
    const auto* settings = static_cast<const NetSettings*>(binding->ctx.settings);
    check(settings->secret.bytes == 0 && settings->colour == 0x336699 && settings->key == 0 && settings->channel == 0 &&
              settings->users.count == 0 && same(settings->label, "hi"),
          "the new setting types start at their defaults");

    // Each type validates what it is given.
    const u32 colour = 0xABCDEF, too_bright = 0x1000000, key = MC_KEY('K', MC_KEY_CTRL | MC_KEY_SHIFT);
    const u32 plain_letter = 'K', shift_letter = MC_KEY('K', MC_KEY_SHIFT), modifiers_only = MC_KEY_CTRL, unknown_bits = 0x80000 | 'K', unbound = 0;
    const u64 channel = 123456789012345ull, zero = 0, users[3] = {11, 22, 33}, with_zero[2] = {1, 0};
    check(plugin_binding_set_setting(binding, kColour, &colour, 4) == MC_OK && settings->colour == 0xABCDEF &&
              plugin_binding_set_setting(binding, kColour, &too_bright, 4) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, kColour, &colour, 3) == MC_E_INVALID,
          "a colour is 24 bits");
    check(plugin_binding_set_setting(binding, kKey, &key, 4) == MC_OK && settings->key == key &&
              plugin_binding_set_setting(binding, kKey, &modifiers_only, 4) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, kKey, &unknown_bits, 4) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, kKey, &plain_letter, 4) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, kKey, &shift_letter, 4) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, kKey, &unbound, 4) == MC_OK && settings->key == 0,
          "a keybind is a key with Ctrl or Alt (or a function key), or unbound");
    check(plugin_binding_set_setting(binding, kChannel, &channel, 8) == MC_OK && settings->channel == channel &&
              plugin_binding_set_setting(binding, kChannel, &channel, 4) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, kChannel, &zero, 8) == MC_OK && settings->channel == 0,
          "a channel is one id, or none");
    check(plugin_binding_set_setting(binding, kUsers, users, sizeof(users)) == MC_OK && settings->users.count == 3 &&
              settings->users.items[0] == 11 && settings->users.items[2] == 33 &&
              plugin_binding_set_setting(binding, kUsers, users, 12) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, kUsers, with_zero, sizeof(with_zero)) == MC_E_INVALID &&
              settings->users.count == 3,
          "a user list holds non-zero ids and keeps its value after a refusal");
    check(plugin_binding_set_setting(binding, kSecret, "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789", 72) ==
                  MC_E_CAPACITY &&
              plugin_binding_set_setting(binding, kSecret, "a\nb", 3) == MC_E_INVALID,
          "a secret is bounded, single-line text");

    // The user's change reaches the plugin's hook; the plugin's own does not.
    check(plugin_binding_change_setting(binding, kLabel, "x", 1) == MC_OK && state->setting_changes == 1 &&
              state->last_setting == kLabel && same(settings->label, "x") && state->last_flags == MC_CTX_GESTURE,
          "a change made in the settings page calls SETTING_CHANGED with a gesture");
    check(plugin_binding_change_setting(binding, kLabel, "\xff", 1) == MC_E_INVALID && state->setting_changes == 1,
          "a refused change calls nothing");
    plugin_binding_set_setting(binding, kLabel, "y", 1);
    check(state->setting_changes == 1, "a host-side set_setting is not a user change");

    // Keybinds.
    plugin_binding_set_setting(binding, kKey, &key, 4);
    check(plugin_dispatch_keybind(key) && state->keybinds == 1 && state->last_keybind == kKey &&
              state->last_flags == MC_CTX_GESTURE,
          "a bound key calls KEYBIND with a gesture");
    check(!plugin_dispatch_keybind(MC_KEY('K', MC_KEY_CTRL)) && !plugin_dispatch_keybind(0) && state->keybinds == 1,
          "other keys and the unbound value call nothing");

    // Secrets reach disk sealed and come back.
    const char secret[] = "s3cret-key";
    check(plugin_binding_set_setting(binding, kSecret, secret, 10) == MC_OK && same(settings->secret, secret),
          "a secret is set");
    plugin_binding_set_setting(binding, kColour, &colour, 4);
    plugin_binding_set_setting(binding, kChannel, &channel, 8);
    plugin_binding_set_setting(binding, kUsers, users, sizeof(users));
    plugin_binding_set_setting(binding, kLabel, "kept", 4);
    const u64 now = clock_monotonic_milliseconds();
    plugin_runtime_poll(runtime, now + kPluginFlushDelayMilliseconds + 500);
    wchar_t owner_name[17], owner_directory[MAX_PATH], settings_file[MAX_PATH];
    for (u32 i = 0; i < 16; ++i) owner_name[i] = L"0123456789abcdef"[(kOwner >> (60 - 4 * i)) & 15];
    owner_name[16] = 0;
    join(owner_directory, root.data, owner_name);
    join(settings_file, owner_directory, L"settings.bin");
    static u8 file[16384];
    const u32 bytes = read_file(settings_file, file, sizeof(file));
    check(bytes > 0 && !binding->settings_dirty && contains(file, bytes, "secret") && contains(file, bytes, "colour") &&
              !contains(file, bytes, "s3cret-key") && !contains(file, bytes, "s3cret"),
          "the settings file holds the secret only as a DPAPI blob");

    check(plugin_runtime_disable(runtime, binding), "the plugin is disabled");
    binding = enable(runtime);
    if (!binding) return;
    settings = static_cast<const NetSettings*>(binding->ctx.settings);
    check(same(settings->secret, secret) && settings->colour == 0xABCDEF && settings->channel == channel &&
              settings->users.count == 3 && settings->users.items[1] == 22 && same(settings->label, "kept"),
          "the secret and the other typed settings load again");

    // A file moved to another user or machine cannot be unsealed; the setting falls back to its default.
    check(plugin_runtime_disable(runtime, binding), "the plugin is disabled again");
    u32 damaged = bytes;
    for (u32 i = 0; i + 6 <= damaged; ++i) {
        if (!bytes_equal(file + i, "secret", 6)) continue;
        // The sealed value follows its key; flip a byte inside it.
        file[i + 6 + 40] ^= 0xFF;
        break;
    }
    HANDLE out = CreateFileW(settings_file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    if (out != INVALID_HANDLE_VALUE) {
        WriteFile(out, file, damaged, &written, nullptr);
        CloseHandle(out);
    }
    binding = enable(runtime);
    if (!binding) return;
    settings = static_cast<const NetSettings*>(binding->ctx.settings);
    // The file as a whole may be refused by its checksum; either way the secret is not recovered from damaged bytes.
    check(settings->secret.bytes == 0 || same(settings->secret, secret), "a damaged secret is never half-restored");
    check(plugin_runtime_disable(runtime, binding), "the plugin is disabled at the end");
}

void run() noexcept {
    Root root;
    if (!make_root(&root, L"mcn")) {
        check(false, "temporary directory");
        return;
    }
    test_hosts();
    test_secret_seal();
    script();
    static PluginRuntime runtime;
    const PluginLoadResult opened =
        plugin_runtime_open(&runtime, {root.plugins, length(root.plugins)}, {root.data, length(root.data)}, nullptr);
    check(opened.ok() && install(root, L"MaiCordPluginRuntimeNetFixture.dll", L"net.dll"), "the runtime and fixture open");
    if (opened.ok()) {
        check(plugin_http_use_transport(&runtime, {&g_network, exchange}), "the scripted transport is installed");
        PluginBinding* binding = enable(&runtime);
        if (binding) {
            test_requests(&runtime, binding);
            test_refusals(&runtime, binding);
            test_limits(&runtime, binding);
            test_redirects(&runtime, binding);
            test_queue_limits(&runtime, binding);
            test_gestures(&runtime, binding);
            test_revocation(&runtime, binding);
            test_consent(binding);
            test_page(binding);
            check(plugin_runtime_disable(&runtime, binding), "the first plugin is disabled");
        }
        test_rate_limit(&runtime);
        test_translate(&runtime, root);
        test_disable_discards(&runtime);
        test_settings(&runtime, root);
        check(runtime.binding_count == 0, "every test left its plugin disabled");
        check(plugin_runtime_close(&runtime).ok() && !runtime.http, "closing the runtime stops the broker");
    }
    remove_tree(root.base);
}

} // namespace maicord::plugin_network_tests

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_network_tests_entry() noexcept {
    maicord::plugin_network_tests::run();
    ExitProcess(maicord::plugin_network_tests::failures ? 1 : 0);
}
