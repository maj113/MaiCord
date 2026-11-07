// Offline fixture over the shipping REST owner: a credential swap reaches the
// next head the worker writes, and a 401 to a head written under the old
// credential does not reject the session. Part of rest_client_fixtures.cpp.
#include "../../support/rest_client_probe.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace credential_swap {
u32 failures;
void check(bool pass, const char* label) noexcept {
    test_check(&failures, pass, label);
}

ConstSlice<u8> bytes(Utf8View text) noexcept {
    return {reinterpret_cast<const u8*>(text.data), text.bytes};
}

bool contains(ConstSlice<u8> haystack, Utf8View needle) noexcept {
    for (u32 i = 0; i + needle.bytes <= haystack.count; ++i)
        if (bytes_equal(haystack.data + i, needle.data, needle.bytes))
            return true;
    return false;
}

constexpr char kTarget[] = "/api/v9/users/@me/settings";
constexpr char kBody[] = "{}";

// A JSON PATCH: the probe's receipt needs request bytes to commit.
bool submit(RestClientProbe& p, u64 request_id) noexcept {
    return rest_client_submit(&p.client, {reinterpret_cast<const u8*>(kTarget), sizeof(kTarget) - 1},
                              {reinterpret_cast<const u8*>(kBody), sizeof(kBody) - 1},
                              {0x5357415000000001ull, 0, RestMethod::patch, RestMajorParameter::none},
                              request_id, true)
        .ok();
}

ConstSlice<u8> head(RestClientProbe& p) noexcept {
    return {p.runtime->request_head, write_request_head(p.runtime)};
}

void finish(RestClientProbe& p) noexcept {
    release_completed_response(&p.client, p.runtime, p.runtime->completed.body_bytes);
}

u32 run() noexcept {
    RestClientProbe p;
    check(p.initialize(), "swap: native owner initializes without network work");
    if (!p.runtime)
        return failures;
    constexpr auto properties = utf8_literal("e30=");
    constexpr auto locale = utf8_literal("en-US");
    copy_bytes(p.runtime->user_super_properties, properties.data, properties.bytes);
    p.runtime->user_super_properties_bytes = properties.bytes;
    copy_bytes(p.runtime->user_locale, locale.data, locale.bytes);
    p.runtime->user_locale_bytes = locale.bytes;
    check(secure_buffer_initialize(&p.runtime->credential, 64).ok() &&
              secure_buffer_assign(&p.runtime->credential, bytes(utf8_literal("old.token"))).ok(),
          "swap: fixture holds the first credential");

    check(!rest_client_replace_credential(&p.client, {}) &&
              !rest_client_replace_credential(&p.client, bytes(utf8_literal("bad token"))) &&
              !rest_client_replace_credential(nullptr, bytes(utf8_literal("new.token"))),
          "swap: empty, non-token and ownerless replacements are refused");

    // (a) Submitted before the swap, written after it: the new credential.
    check(submit(p, 11) && contains(head(p), utf8_literal("Authorization: old.token\r\n")),
          "swap: a head written before the swap carries the first credential");
    check(rest_client_replace_credential(&p.client, bytes(utf8_literal("new.token"))) &&
              secure_buffer_view(p.runtime->credential).count == 9,
          "swap: the replacement is accepted");
    // (b) The head above went out under the old credential; its 401 is stale.
    check(p.receive(401, utf8_literal("{\"message\":\"401: Unauthorized\",\"code\":0}")) &&
              p.runtime->completed.status_code == 401 && !rest_client_status(&p.client).authentication_rejected,
          "swap: a 401 to a request sent with the old credential does not reject the session");
    finish(p);

    check(submit(p, 12), "swap: the session still admits requests");
    const ConstSlice<u8> written = head(p);
    check(contains(written, utf8_literal("Authorization: new.token\r\n")) &&
              !contains(written, utf8_literal("old.token")),
          "swap: the next head carries only the new credential");
    check(p.receive(401, utf8_literal("{\"message\":\"401: Unauthorized\",\"code\":0}")) &&
              rest_client_status(&p.client).authentication_rejected,
          "swap: a 401 to the new credential rejects the session");
    finish(p);
    return failures;
}
} // namespace credential_swap
} // namespace maicord
