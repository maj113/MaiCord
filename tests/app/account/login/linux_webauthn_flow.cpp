#include "shell/state.hpp"
#include "../../../support/test_output.hpp"
#include "../../../support/window_fixture.hpp"
#include "../../../support/account_module_fake.hpp"
#include "account/login/auth.hpp"
#include "account/login/ui.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/desktop/webauthn.hpp"
#include "maicord/ui/text/secure_edit.hpp"

// The sign-in's side of a security-key ceremony on Linux (src/app/account/login/auth.cpp), over a fake ceremony that
// plays the platform: begin (the request decoded from the server's challenge, the screen that waits for the key), the key
// asking for a PIN typed into the code field (too short, then sent), a wrong PIN asked again, the answer going to the
// server as the credential, each failure shown with its words, a switch of method or a Back-like invalidation cancelling the
// ceremony and the late answer being dropped, and a system without libfido2 saying so at once. The headless window and no
// renderer: the screen is the sign-in state the renderer is given (the words themselves are in linux_webauthn_text).

namespace maicord {

// The fake ceremony (the real one is platform/desktop/linux/webauthn_ceremony.cpp, left out of the link).
namespace {

struct FakeCeremony {
    bool available;
    bool start_ok;
    u32 starts, cancels, releases, finishes, pins_sent;
    u64 token;
    WebAuthnMfaRequestOptions request;
    WebAuthnCeremonyState state;
    WebAuthnPinPrompt prompt;
    char pin[64];
    WebAuthnCeremonyError error;
};
FakeCeremony g_fake;

constexpr char kCredential[] = "FAKE-CREDENTIAL-JSON";

} // namespace

bool webauthn_platform_available() noexcept {
    return g_fake.available;
}

bool webauthn_ceremony_start(const WebAuthnMfaRequestOptions& request, const Waker&, u64 token) noexcept {
    if (!g_fake.start_ok || g_fake.state != WebAuthnCeremonyState::idle)
        return false;
    ++g_fake.starts;
    g_fake.token = token;
    copy_bytes(&g_fake.request, &request, static_cast<u32>(sizeof(request)));
    g_fake.state = WebAuthnCeremonyState::running;
    return true;
}

// Registering a key belongs to the account pages, which this test links without exercising.
bool webauthn_ceremony_start_creation(const WebAuthnMfaCreationOptions&, const Waker&, u64) noexcept {
    return false;
}

WebAuthnCeremonyState webauthn_ceremony_state() noexcept {
    return g_fake.state;
}

WebAuthnPinPrompt webauthn_ceremony_pin_prompt() noexcept {
    return g_fake.prompt;
}

bool webauthn_ceremony_submit_pin(ConstSlice<u8> pin) noexcept {
    if (g_fake.state != WebAuthnCeremonyState::need_pin || pin.count < 4 || pin.count >= sizeof(g_fake.pin))
        return false;
    copy_bytes(g_fake.pin, pin.data, pin.count);
    g_fake.pin[pin.count] = 0;
    ++g_fake.pins_sent;
    g_fake.state = WebAuthnCeremonyState::running;
    return true;
}

void webauthn_ceremony_cancel() noexcept {
    if (g_fake.state == WebAuthnCeremonyState::running || g_fake.state == WebAuthnCeremonyState::need_pin) {
        ++g_fake.cancels;
        g_fake.error = WebAuthnCeremonyError::cancelled;
        g_fake.state = WebAuthnCeremonyState::done;
    }
}

WebAuthnCeremonyResult webauthn_ceremony_finish(SecureBuffer* credential) noexcept {
    ++g_fake.finishes;
    const WebAuthnCeremonyError error = g_fake.error;
    g_fake.state = WebAuthnCeremonyState::idle;
    g_fake.error = WebAuthnCeremonyError::none;
    if (error == WebAuthnCeremonyError::none && credential)
        secure_buffer_assign(credential, {reinterpret_cast<const u8*>(kCredential), sizeof(kCredential) - 1});
    return {error, 0};
}

void webauthn_ceremony_release() noexcept {
    ++g_fake.releases;
    g_fake.state = WebAuthnCeremonyState::idle;
}

// The renderer's two sign-in entry points the application calls: there is no renderer here, and the screen is read from
// the runtime state the application builds its view from.
bool renderer_set_login_view(D3D11Renderer*, const LoginScene*, const LoginView&, const Theme&) noexcept {
    return true;
}

void renderer_set_login_caret_visible(D3D11Renderer*, bool) noexcept {}

namespace login_flow_test {
using namespace app_detail;

u32 failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&failures, condition, label);
}

struct Audit {
    u32 challenge_copies, mfa_submits;
    char credential[64];
    UserAuthMfaType type;
} audit;

SecureBufferResult WINAPI copy_challenge(void*, SecureBuffer* output) noexcept {
    ++audit.challenge_copies;
    constexpr auto json = utf8_literal(
        R"({"publicKey":{"challenge":"AQIDBA","rpId":"discord.com","timeout":60000,"allowCredentials":[],"userVerification":"required"}})");
    return secure_buffer_assign(output, {reinterpret_cast<const u8*>(json.data), json.bytes});
}

AccountLoginStatus WINAPI mfa(void* instance, const SecureBuffer* credential, UserAuthMfaType type) noexcept {
    AccountLoginStatus result{};
    if (instance != &audit || !credential || !credential->locked || !credential->bytes ||
        credential->bytes >= sizeof(audit.credential)) {
        result.error = AccountLoginError::invalid_input;
        return result;
    }
    ++audit.mfa_submits;
    copy_bytes(audit.credential, credential->data, credential->bytes);
    audit.credential[credential->bytes] = 0;
    audit.type = type;
    result.phase = AccountLoginPhase::mfa_pending;
    return result;
}

bool same_text(const char* a, const char* b) noexcept {
    u32 i = 0;
    while (a[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}

LoginRuntime& login() noexcept {
    return g_app.login;
}

void seed() noexcept {
    clear_bytes(&g_fake, static_cast<u32>(sizeof(g_fake)));
    g_fake.available = g_fake.start_ok = true;
    clear_bytes(&audit, static_cast<u32>(sizeof(audit)));
    g_app.window = window_test::open();
    g_app.account = {};
    g_app.account.module = reinterpret_cast<HMODULE>(1);  // never passed to an OS loader
    g_app.account.login_instance = &audit;
    g_account_fake.login_copy_webauthn_challenge = copy_challenge;
    g_account_fake.login_submit_mfa = mfa;
    g_app.account.login_status.phase = AccountLoginPhase::mfa_required;
    login().visible = login().initialized = true;
    login().mfa_capabilities = login_mfa_capability_all;
    login().mfa_type = UserAuthMfaType::totp;
    login().status = LoginStatus::mfa_required;
    login().focus = LoginControl::password;
    login().webauthn_attempted = false;
    login().webauthn_post_epoch = 0;
    login().reset_mode = false;
    login().security_key = LoginSecurityKey::none;
    login().security_key_pin = false;
    secure_edit_clear(&login().mfa);
    login().mfa.password = false;
}

// The key is chosen and its ceremony begun: the screen that waits for it.
bool begin(u64* epoch) noexcept {
    login().mfa_type = UserAuthMfaType::totp;
    login().status = LoginStatus::mfa_required;
    if (!application_login_select_mfa(UserAuthMfaType::webauthn, false))
        return false;
    *epoch = login().webauthn_post_epoch;
    return *epoch != 0 && login().status == LoginStatus::webauthn_required && login().webauthn_attempted &&
           application_login_run_webauthn(*epoch);
}

void type_pin(const char* text) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    secure_edit_insert_utf8(&login().mfa, Utf8View{text, length});
}

void test_touch_and_success() noexcept {
    seed();
    u64 epoch = 0;
    check(begin(&epoch), "choosing the key begins its ceremony");
    check(g_fake.starts == 1 && g_fake.token == epoch && audit.challenge_copies == 1 &&
              same_text(g_fake.request.rp_id, "discord.com") && g_fake.request.challenge_bytes == 4 &&
              g_fake.request.challenge[0] == 1 && g_fake.request.challenge[3] == 4 &&
              g_fake.request.user_verification == WebAuthnUserVerification::required,
          "the request the key is asked is the server's challenge decoded: relying party, challenge, verification");
    check(login().status == LoginStatus::submitting && login().security_key == LoginSecurityKey::touch &&
              login().webauthn_attempted && !login().security_key_pin,
          "the screen waits for a touch (Back stays available)");

    // The key answers: the credential goes to the server and the screen waits for it.
    g_fake.state = WebAuthnCeremonyState::done;
    check(application_login_webauthn_progress(epoch, kWebAuthnWakeDone) && audit.mfa_submits == 1 &&
              same_text(audit.credential, "FAKE-CREDENTIAL-JSON") && audit.type == UserAuthMfaType::webauthn,
          "an answer sends the credential to the server as the key's MFA");
    check(login().status == LoginStatus::submitting && login().security_key == LoginSecurityKey::none &&
              g_fake.finishes == 1,
          "and the screen is the server's wait, no longer the key's");
}

void test_pin() noexcept {
    seed();
    u64 epoch = 0;
    begin(&epoch);
    // The key wants a PIN: the code field takes it, masked.
    g_fake.state = WebAuthnCeremonyState::need_pin;
    g_fake.prompt = {false, -1};
    check(application_login_webauthn_progress(epoch, kWebAuthnWakePin), "the key's PIN prompt reaches the screen");
    check(login().status == LoginStatus::mfa_required && login().security_key == LoginSecurityKey::pin &&
              login().security_key_pin && login().mfa.password && login().focus == LoginControl::password &&
              application_active_login_edit() == &login().mfa,
          "the code field becomes a masked PIN field, focused");

    // Too short to send; then sent.
    type_pin("12");
    check(application_login_submit_mfa() && g_fake.pins_sent == 0 && login().security_key_pin, "two characters are not a PIN");
    type_pin("34");
    check(application_login_submit_mfa() && g_fake.pins_sent == 1 && same_text(g_fake.pin, "1234"),
          "four are sent to the key");
    check(login().status == LoginStatus::submitting && login().security_key == LoginSecurityKey::touch &&
              !login().security_key_pin && !login().mfa.password && login().mfa.storage.bytes == 0,
          "the PIN is wiped from the field and the screen waits for the touch");

    // A refused PIN is asked again, saying so.
    g_fake.state = WebAuthnCeremonyState::need_pin;
    g_fake.prompt = {true, 2};
    check(application_login_webauthn_progress(epoch, kWebAuthnWakePin) && login().security_key == LoginSecurityKey::pin_wrong &&
              login().security_key_pin && login().status == LoginStatus::mfa_required,
          "a refused PIN is asked for again, and says so");

    // The ceremony ends while the PIN is asked (the request's time ran out): the failure, with the field back to a code field.
    type_pin("99");
    g_fake.state = WebAuthnCeremonyState::done;
    g_fake.error = WebAuthnCeremonyError::timed_out;
    check(application_login_webauthn_progress(epoch, kWebAuthnWakeDone) && login().status == LoginStatus::webauthn_failed &&
              login().security_key == LoginSecurityKey::timed_out && !login().security_key_pin && !login().mfa.password &&
              login().mfa.storage.bytes == 0 && !login().webauthn_attempted && audit.mfa_submits == 0,
          "the time running out on the PIN screen shows that, and leaves the code field clean");
}

void test_failures() noexcept {
    struct Case {
        WebAuthnCeremonyError error;
        LoginSecurityKey shown;
        const char* label;
    };
    const Case cases[] = {
        {WebAuthnCeremonyError::no_device, LoginSecurityKey::no_key, "no key plugged in"},
        {WebAuthnCeremonyError::cannot_open, LoginSecurityKey::cannot_open, "keys that cannot be opened"},
        {WebAuthnCeremonyError::not_registered, LoginSecurityKey::not_registered, "a key without the credential"},
        {WebAuthnCeremonyError::pin_blocked, LoginSecurityKey::pin_blocked, "a blocked PIN"},
        {WebAuthnCeremonyError::pin_not_set, LoginSecurityKey::pin_not_set, "a key with no PIN for a verification"},
        {WebAuthnCeremonyError::timed_out, LoginSecurityKey::timed_out, "a touch that never came"},
        {WebAuthnCeremonyError::failed, LoginSecurityKey::failed, "any other failure"},
    };
    for (const Case& c : cases) {
        seed();
        u64 epoch = 0;
        begin(&epoch);
        g_fake.state = WebAuthnCeremonyState::done;
        g_fake.error = c.error;
        char label[96];
        u32 length = 0;
        for (const char* p = c.label; *p && length < 60; ++p)
            label[length++] = *p;
        const char suffix[] = ": the failure is shown with its own words and the key can be tried again";
        for (u32 i = 0; i < sizeof(suffix); ++i)
            label[length++] = suffix[i];
        check(application_login_webauthn_progress(epoch, kWebAuthnWakeDone) &&
                  login().status == LoginStatus::webauthn_failed && login().security_key == c.shown &&
                  !login().webauthn_attempted && audit.mfa_submits == 0,
              label);
    }

    // The server turned the credential down.
    seed();
    u64 epoch = 0;
    begin(&epoch);
    g_account_fake.login_submit_mfa = [](void*, const SecureBuffer*, UserAuthMfaType) noexcept -> AccountLoginStatus {
        AccountLoginStatus result{};
        result.error = AccountLoginError::invalid_input;
        return result;
    };
    g_fake.state = WebAuthnCeremonyState::done;
    check(application_login_webauthn_progress(epoch, kWebAuthnWakeDone) && login().status == LoginStatus::webauthn_failed &&
              login().security_key == LoginSecurityKey::failed,
          "a credential the server refuses is a general failure");
}

void test_cancel() noexcept {
    // Choosing another method while the key is asked cancels the ceremony; its late answer changes nothing.
    seed();
    u64 epoch = 0;
    begin(&epoch);
    check(application_login_select_mfa(UserAuthMfaType::totp, false) && g_fake.cancels == 1 &&
              login().security_key == LoginSecurityKey::none && login().mfa_type == UserAuthMfaType::totp,
          "choosing another method cancels the ceremony");
    const LoginStatus shown = login().status;
    check(application_login_webauthn_progress(epoch, kWebAuthnWakeDone) && login().status == shown && g_fake.finishes == 1 &&
              audit.mfa_submits == 0 && login().security_key == LoginSecurityKey::none,
          "its late answer is joined and dropped: the screen is the other method's");

    // The same at the PIN prompt: the PIN field is a code field again.
    seed();
    begin(&epoch);
    g_fake.state = WebAuthnCeremonyState::need_pin;
    application_login_webauthn_progress(epoch, kWebAuthnWakePin);
    type_pin("12");
    check(login().security_key_pin, "the PIN is being typed");
    application_login_select_mfa(UserAuthMfaType::totp, false);
    check(!login().security_key_pin && !login().mfa.password && login().mfa.storage.bytes == 0 && g_fake.cancels == 1,
          "another method at the PIN prompt cancels it and wipes the PIN field");

    // A PIN prompt of a screen that is gone is cancelled, not shown.
    seed();
    begin(&epoch);
    login().application_auth_epoch += 5;  // another sign-in screen
    g_fake.state = WebAuthnCeremonyState::need_pin;
    application_login_webauthn_progress(epoch, kWebAuthnWakePin);
    check(g_fake.cancels == 1 && !login().security_key_pin, "a PIN prompt for a screen that is gone is cancelled");

    // Releasing the sign-in stops a ceremony in flight.
    seed();
    begin(&epoch);
    const u32 releases = g_fake.releases;
    application_login_webauthn_release();
    check(g_fake.releases == releases + 1, "releasing the sign-in releases the ceremony");
}

void test_unavailable() noexcept {
    seed();
    g_fake.available = false;
    login().mfa_type = UserAuthMfaType::totp;
    check(application_login_select_mfa(UserAuthMfaType::webauthn, false) &&
              login().status == LoginStatus::webauthn_failed && login().security_key == LoginSecurityKey::unavailable &&
              g_fake.starts == 0,
          "without libfido2 the key says so at once and no ceremony starts");

    // A ceremony that cannot start (no worker) is a failure, not a hang on the touch screen.
    seed();
    g_fake.start_ok = false;
    u64 epoch = 0;
    begin(&epoch);
    check(login().status == LoginStatus::webauthn_failed && login().security_key == LoginSecurityKey::failed &&
              !login().webauthn_attempted,
          "a ceremony that cannot start is a failure on screen");
}

} // namespace login_flow_test
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_webauthn_flow_entry) {
    using namespace maicord;
    using namespace maicord::login_flow_test;
    if (!secure_edit_initialize(&login().login, kUserAuthMaximumLoginBytes, false) ||
        !secure_edit_initialize(&login().password, kUserAuthMaximumPasswordBytes, true) ||
        !secure_edit_initialize(&login().mfa, kUserAuthMaximumCodeBytes, false))
        test_exit(1, "linux_webauthn_flow");
    test_touch_and_success();
    test_pin();
    test_failures();
    test_cancel();
    test_unavailable();
    test_exit(failures, "linux_webauthn_flow");
}
