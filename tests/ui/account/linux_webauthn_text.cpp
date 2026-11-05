#include "maicord/ui/account/login_presentation.hpp"
#include "maicord/ui/account/login_text_presentation.hpp"
#include "../../support/test_output.hpp"

// What the sign-in screen says about a security key on Linux, where no authenticator API is bound (the Windows build
// says it through the Windows security prompt): the wording names the system's real state, never Windows, and the failed
// state says that keys are not supported instead of "failed or cancelled".

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

// Builds the text commands of the sign-in screen in `status` with every second factor on offer, key selected.
bool build(LoginStatus status, UiTextCommand* commands, UiTextCommandStream* text) noexcept {
    LoginScene scene{};
    LoginView view{};
    view.status = status;
    view.visible = true;
    view.mfa_capabilities = login_mfa_capability_all;
    view.selected_mfa_capability = login_mfa_capability_webauthn;
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

void test_text() noexcept {
    UiTextCommand commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream text{};
    check(build(LoginStatus::webauthn_failed, commands, &text), "the failed state of a security key lays out");
    check(some_command_says(commands, text, "Security keys aren't supported on Linux yet. Use another method."),
          "it says that security keys are not supported on Linux, and what to do");
    check(!some_command_says(commands, text, "cancelled") && !some_command_says(commands, text, "Windows"),
          "it does not call that a failure or a cancel, and names no Windows prompt");
    check(some_command_says(commands, text, "Security key"), "the field names the security key");

    reset_value(&text);
    check(build(LoginStatus::webauthn_required, commands, &text), "the waiting state of a security key lays out");
    check(!some_command_says(commands, text, "Windows"), "the waiting state names no Windows prompt");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_webauthn_text_entry) {
    using namespace maicord;
    test_text();
    test_exit(g_failures, "linux_webauthn_text");
}
