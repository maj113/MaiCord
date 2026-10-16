#include "../src/app/application_guild_invite.cpp"

namespace maicord::app_detail {
// The owner fixture exercises no browser, modal or navigation admission.
bool application_link_open(Utf8View, bool) noexcept {
    return false;
}
bool select_content_guild(u64) noexcept {
    return false;
}
bool application_text_form_open(const ApplicationTextFormRequest&) noexcept {
    return false;
}
void application_text_form_close(bool, bool) noexcept {
}
namespace {
u32 retry_calls, retry_captures;
GuildInviteOperation retry_operation;
ApplicationActionCaptchaPhase retry_phase;
RestClientSubmitResult WINAPI submit_retry(void*, const GuildInviteRequest* request, u64) noexcept {
    ++retry_calls;
    retry_operation = request->operation;
    return {};
}
} // namespace

bool application_action_captcha_capture(ApplicationActionCaptchaKind, u64, const RestClientResponse&,
                                        Utf8View) noexcept {
    ++retry_captures;
    retry_phase = ApplicationActionCaptchaPhase::captured;
    return true;
}
ApplicationActionCaptchaPhase application_action_captcha_phase(ApplicationActionCaptchaKind, u64) noexcept {
    return retry_phase;
}
Utf8View application_action_captcha_status(ApplicationActionCaptchaPhase) noexcept {
    return utf8_literal("Verification required.");
}
bool application_action_captcha_verify(ApplicationActionCaptchaKind, u64) noexcept {
    return false;
}
void application_action_captcha_cancel() noexcept {
    retry_phase = ApplicationActionCaptchaPhase::none;
}

void test_invite_retry(void (*check)(bool, const char*) noexcept) noexcept {
    const auto old_instance = g_app.account.instance;
    const auto old_submit = g_app.account.api.guild_invite;
    g_app.account.instance = &retry_calls;
    g_app.account.api.guild_invite = submit_retry;
    for (u32 accept = 0; accept < 2; ++accept) {
        for (u32 kind = 0; kind < 7; ++kind) {
            g_invite = {};
            g_invite.scope = g_app.plugin_account_scope_serial;
            g_invite.code_bytes = 3;
            copy_bytes(g_invite.code, "abc", 3);
            g_invite.invite.guild_id = 80;
            g_invite.invite.channel_id = 81;
            g_invite.state = accept ? InviteState::accept_queued : InviteState::resolve_queued;
            retry_calls = retry_captures = 0;
            retry_phase = ApplicationActionCaptchaPhase::none;
            check(application_guild_invite_submit(101, 100) && retry_calls == 1 &&
                      retry_operation ==
                          (accept ? GuildInviteOperation::accept : GuildInviteOperation::resolve),
                  "invite admits one request with its original operation");
            RestClientResponse response{};
            response.request_id = 101;
            response.status_code = static_cast<u16>(kind == 5 ? (accept ? 403 : 401) : 429);
            response.rate_limited = kind != 1;
            response.retry_at_ms = kind == 2 ? 0 : 8123;
            response.rejection.captcha = kind == 0;
            response.rejection.has_code = kind == 4;
            response.rejection.code = kind == 4 ? 20026 : 0;
            response.error = kind == 6 ? RestClientError::receive : RestClientError::none;
            const auto body =
                kind == 0 ? utf8_literal(R"({"captcha_key":["captcha-required"]})") : Utf8View{};
            check(application_guild_invite_response(response, body), "invite owned response consumed");
            if (kind == 3) {
                check(g_invite.retry_at_ms == 8123 && !application_guild_invite_pending(8122) &&
                          !application_guild_invite_submit(102, 8122) && retry_calls == 1 &&
                          application_guild_invite_pending(8123),
                      "invite genuine 429 preserves exact deadline with no early request");
                check(application_guild_invite_submit(102, 8123) && retry_calls == 2 &&
                          retry_operation ==
                              (accept ? GuildInviteOperation::accept : GuildInviteOperation::resolve),
                      "invite genuine 429 retries the same operation at the server deadline");
                response.request_id = 102;
                response.status_code = 403;
                response.rate_limited = false;
                response.retry_at_ms = 0;
                application_guild_invite_response(response, {});
            } else if (kind == 0) {
                check(g_invite.state == InviteState::captcha && retry_captures == accept,
                      "invite challenge 429 enters verification and captures only an accept challenge");
            } else {
                check(g_invite.state == InviteState::failed && !retry_captures,
                      "invite unknown cooldown, action-required, auth or network error is terminal");
            }
            const u32 expected_calls = kind == 3 ? 2u : 1u;
            check(!application_guild_invite_pending(9000) && !application_guild_invite_submit(103, 9000) &&
                      !g_invite.request_id && retry_calls == expected_calls,
                  "invite terminal outcome cannot loop resolve or accept");
            const auto state = g_invite.state;
            response.request_id = 101;
            application_guild_invite_response(response, {});
            check(g_invite.state == state && retry_calls == expected_calls,
                  "invite drained receipt cannot arm another request");
        }
    }
    application_guild_invite_release();
    g_app.account.instance = old_instance;
    g_app.account.api.guild_invite = old_submit;
}
} // namespace maicord::app_detail
