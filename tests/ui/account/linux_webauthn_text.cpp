#include "maicord/ui/account/login_text_presentation.hpp"
#include "../../support/test_output.hpp"

// What the sign-in screen says about a security key on Linux, where the application asks the key itself (libfido2): the touch
// prompt, the PIN field, and the words of each way it can fail. The wording names the system's real state, never Windows.

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool contains(const char* text, u32 bytes, const char* needle) noexcept {
    u32 length = 0;
    while (needle[length])
        ++length;
    for (u32 i = 0; i + length <= bytes; ++i) {
        u32 j = 0;
        while (j < length && text[i + j] == needle[j])
            ++j;
        if (j == length)
            return true;
    }
    return false;
}

// Builds the text commands of the sign-in screen in `status` with every second factor on offer, key selected. The screen is the
// one the application's view builds: the security key's state as the view says it, and (the PIN being a code) the code method
// of a ceremony that asks for it.
bool build(LoginStatus status, LoginSecurityKey key, UiTextCommand* commands, UiTextCommandStream* text,
           bool reset = false) noexcept {
    LoginScene scene{};
    LoginView view{};
    view.status = status;
    view.visible = true;
    view.reset_mode = reset;
    view.security_key = key;
    view.mfa_capabilities = login_mfa_capability_all;
    view.selected_mfa_capability = login_mfa_capability_webauthn;
    view.mfa_code_ready = key == LoginSecurityKey::pin || key == LoginSecurityKey::pin_wrong;
    view.focus = LoginControl::submit;
    scene.mfa_capabilities = view.mfa_capabilities;
    scene.selected_mfa_capability = view.selected_mfa_capability;
    scene.focus = view.focus;
    scene.phone_mode = login_phone_mode(view.status);
    scene.reset_mode = view.reset_mode;
    scene.recovery_mode = login_recovery_mode(view.status);
    scene.back_only = login_requires_back(view.status, view.remote_status);
    scene.phone_resend_disabled = scene.phone_mode && !login_phone_editable(view.status);
    scene.credential_fields_hidden = login_credential_fields_hidden(view.status, view.remote_status, view.reset_mode);
    scene.action_disabled = login_view_action_disabled(view);
    scene.submit_disabled = login_view_submit_disabled(view);
    scene.recovery_disabled = login_view_recovery_disabled(view);
    if (!ui_text_command_stream_initialize(text, {commands, kMaximumLoginTextCommands}) ||
        !login_scene_layout(&scene, 560, 600))
        return false;
    // The layout settles the focus and the capabilities; the view the screen is built from says the same.
    view.focus = scene.focus;
    view.mfa_capabilities = scene.mfa_capabilities;
    view.selected_mfa_capability = scene.selected_mfa_capability;
    return login_text_presentation_build(scene, view, kDefaultTheme, text);
}

bool some_command_says(const UiTextCommand* commands, const UiTextCommandStream& text, const char* needle) noexcept {
    for (u32 i = 0; i < text.count; ++i)
        if (contains(commands[i].text.data, commands[i].text.bytes, needle))
            return true;
    return false;
}

// The screen in `status` with the security key in `key`: lays out, and says `words`; none of it names Windows.
void check_screen(LoginStatus status, LoginSecurityKey key, const char* words, const char* label) noexcept {
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream text{};
    const bool built = build(status, key, commands, &text);
    check(built, label);
    check(built && some_command_says(commands, text, words) && !some_command_says(commands, text, "Windows"), words);
}

void test_text() noexcept {
    check_screen(LoginStatus::submitting, LoginSecurityKey::touch, "Touch your security key.", "the touch prompt lays out");
    check_screen(LoginStatus::webauthn_failed, LoginSecurityKey::no_key, "No security key found. Plug it in and try again.",
                 "no key lays out");
    check_screen(LoginStatus::webauthn_failed, LoginSecurityKey::cannot_open, "Check the udev rules for FIDO keys.",
                 "keys that cannot be opened lay out");
    check_screen(LoginStatus::webauthn_failed, LoginSecurityKey::not_registered, "isn't registered for your account.",
                 "a key without the credential lays out");
    check_screen(LoginStatus::webauthn_failed, LoginSecurityKey::pin_blocked, "PIN is blocked.", "a blocked PIN lays out");
    check_screen(LoginStatus::webauthn_failed, LoginSecurityKey::pin_not_set, "the security key has none.",
                 "a key with no PIN lays out");
    check_screen(LoginStatus::webauthn_failed, LoginSecurityKey::timed_out, "wasn't touched in time.",
                 "a touch that never came lays out");
    check_screen(LoginStatus::webauthn_failed, LoginSecurityKey::unavailable, "need libfido2", "no libfido2 lays out");
    check_screen(LoginStatus::webauthn_failed, LoginSecurityKey::failed, "Security-key verification failed. Try again.",
                 "a failure lays out");
    check_screen(LoginStatus::webauthn_failed, LoginSecurityKey::none, "Security-key verification failed. Try again.",
                 "a failure with no detail says the general one");
    check_screen(LoginStatus::webauthn_required, LoginSecurityKey::none, "Security key", "the waiting state lays out");

    // The PIN is typed in the code field: its label, hint and prompt say so.
    check_screen(LoginStatus::mfa_required, LoginSecurityKey::pin, "Enter your security key's PIN.", "the PIN prompt lays out");
    check_screen(LoginStatus::mfa_required, LoginSecurityKey::pin, "SECURITY KEY PIN", "its field is labelled");
    check_screen(LoginStatus::mfa_required, LoginSecurityKey::pin, "Security key PIN", "its field has a hint");
    check_screen(LoginStatus::mfa_required, LoginSecurityKey::pin_wrong, "That PIN didn't work. Try again.",
                 "a refused PIN says so");

    // A password reset waits for the key with the same prompt.
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream text{};
    check(build(LoginStatus::reset_mfa_pending, LoginSecurityKey::touch, commands, &text, true) &&
              some_command_says(commands, text, "Touch your security key."),
          "the touch prompt shows in a password reset too");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_webauthn_text_entry) {
    using namespace maicord;
    test_text();
    test_exit(g_failures, "linux_webauthn_text");
}
