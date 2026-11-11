#include "../../../support/test_main_shared.hpp"

#include "../../../support/secure_text_store_access.hpp"
#include "../../../support/layout_validation.hpp"
#include "maicord/app/account/login/mfa_policy.hpp"
#include "maicord/app/account/login/user_auth_coordinator.hpp"
#include "maicord/app/account/login/remote_auth_client.hpp"
#include "maicord/app/account/login/remote_auth_ticket_exchange.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/platform/desktop/webauthn.hpp"
#include "maicord/ui/account/login_presentation.hpp"
#include "maicord/ui/account/login_text_presentation.hpp"
#include "maicord/sys/crypto.hpp"
#include "maicord/sys/tsf.hpp"

namespace maicord {
namespace {

u32 write_fixture_json(Utf8View prefix, Utf8View value, Utf8View suffix, Slice<char> output) noexcept {
    const u64 required64 = static_cast<u64>(prefix.bytes) + value.bytes + suffix.bytes;
    if (required64 > output.count || required64 > 0xFFFFFFFFull || !output.data) {
        return 0;
    }
    u32 offset = 0;
    for (u32 index = 0; index < prefix.bytes; ++index) {
        output[offset++] = prefix.data[index];
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        output[offset++] = value.data[index];
    }
    for (u32 index = 0; index < suffix.bytes; ++index) {
        output[offset++] = suffix.data[index];
    }
    return offset;
}

bool remote_auth_encrypt_fixture(void* key, ConstSlice<u8> plaintext, Slice<u8> ciphertext,
                                 u32* ciphertext_bytes) noexcept {
    if (!key || !plaintext.data || plaintext.count == 0 || !ciphertext.data ||
        ciphertext.count < kRemoteAuthRsaBytes || !ciphertext_bytes) {
        return false;
    }
    BCRYPT_OAEP_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM, nullptr, 0};
    ULONG written = 0;
    const NTSTATUS status = BCryptEncrypt(
        static_cast<BCRYPT_KEY_HANDLE>(key), const_cast<PUCHAR>(plaintext.data), plaintext.count, &padding,
        nullptr, 0, ciphertext.data, ciphertext.count, &written, BCRYPT_PAD_OAEP);
    *ciphertext_bytes = written;
    return status >= 0 && written == kRemoteAuthRsaBytes;
}

} // namespace

void test_login_scene() noexcept {
    LoginScene scene{};
    expect(login_scene_layout(&scene, 1280.0f, 800.0f), "login_scene_wide_layout");
    UiRect card = login_scene_rectangle(scene, LoginElement::card);
    UiRect login_field = login_scene_rectangle(scene, LoginElement::login_field);
    UiRect password_field = login_scene_rectangle(scene, LoginElement::password_field);
    UiRect submit = login_scene_rectangle(scene, LoginElement::submit);
    UiRect qr = login_scene_rectangle(scene, LoginElement::qr);
    expect(scene.qr_visible && card.left == 248.0f &&
               card.top == (800.0f - kDefaultLoginLayoutMetrics.card_height) * 0.5f &&
               card.right == 1032.0f &&
               card.bottom == (800.0f + kDefaultLoginLayoutMetrics.card_height) * 0.5f,
           "login_scene_wide_card");
    const UiRect recovery = login_scene_rectangle(scene, LoginElement::forgot_password);
    expect(login_scene_hit_test(scene, recovery.left + 1, recovery.top + 1) == LoginControl::forgot_password &&
               login_scene_focus(&scene, LoginControl::forgot_password),
           "login_recovery_is_a_native_hit_and_focus_target");
    login_scene_focus(&scene, LoginControl::none);
    const u32 layout_rebuilds = scene.layout_rebuilds;
    expect(login_scene_move_focus(&scene, false) == LoginControl::login &&
               login_scene_move_focus(&scene, false) == LoginControl::password &&
               login_scene_move_focus(&scene, false) == LoginControl::forgot_password &&
               login_scene_move_focus(&scene, false) == LoginControl::submit &&
               login_scene_layout(&scene, 1280.0f, 800.0f) && scene.layout_rebuilds == layout_rebuilds,
           "login_scene_widget_focus_and_invalidation_cache");
    login_scene_focus(&scene, LoginControl::none);
    expect(login_scene_hit_test(scene, login_field.left + 1.0f, login_field.top + 1.0f) ==
                   LoginControl::login &&
               login_scene_hit_test(scene, password_field.left + 1.0f, password_field.top + 1.0f) ==
                   LoginControl::password &&
               login_scene_hit_test(scene, submit.left + 1.0f, submit.top + 1.0f) == LoginControl::submit &&
               login_scene_hit_test(scene, qr.left + 1.0f, qr.top + 1.0f) == LoginControl::qr,
           "login_scene_hit_targets");
    UiLayoutDiagnostic diagnostics[32]{};
    expect(ui_layout_engine_validate(scene.widgets.layout, Slice<UiLayoutDiagnostic>{diagnostics, 32}).clean(),
           "login_scene_wide_layout_validation");
    const u32 wide_rebuilds = scene.layout_rebuilds;
    const u32 wide_layout_passes = scene.layout_passes;
    const u16 wide_generation = scene.widgets.generation;
    expect(login_scene_layout(&scene, 1200.0f, 800.0f) && scene.layout_rebuilds == wide_rebuilds &&
               scene.layout_passes == wide_layout_passes + 1 &&
               scene.widgets.generation == wide_generation && login_scene_layout(&scene, 1280.0f, 800.0f) &&
               scene.layout_rebuilds == wide_rebuilds && scene.layout_passes == wide_layout_passes + 2,
           "login_scene_same_variant_rearrange_without_tree_rebuild");
    scene.mfa_capabilities = static_cast<u8>(login_mfa_capability_totp | login_mfa_capability_sms |
                                             login_mfa_capability_backup | login_mfa_capability_webauthn);
    scene.selected_mfa_capability = login_mfa_capability_sms;
    expect(login_scene_layout(&scene, 1280.0f, 800.0f), "login_scene_mfa_layout");
    const UiRect mfa_totp = login_scene_rectangle(scene, LoginElement::mfa_totp);
    const UiRect mfa_sms = login_scene_rectangle(scene, LoginElement::mfa_sms);
    const UiRect mfa_backup = login_scene_rectangle(scene, LoginElement::mfa_backup);
    const UiRect mfa_webauthn = login_scene_rectangle(scene, LoginElement::mfa_webauthn);
    expect(scene.selected_mfa_capability == login_mfa_capability_sms && mfa_webauthn.left < mfa_totp.left &&
               mfa_totp.left < mfa_backup.left && mfa_backup.left < mfa_sms.left &&
               login_scene_hit_test(scene, mfa_totp.left + 1.0f, mfa_totp.top + 1.0f) ==
                   LoginControl::mfa_totp &&
               login_scene_hit_test(scene, mfa_sms.left + 1.0f, mfa_sms.top + 1.0f) ==
                   LoginControl::mfa_sms &&
               login_scene_hit_test(scene, mfa_backup.left + 1.0f, mfa_backup.top + 1.0f) ==
                   LoginControl::mfa_backup &&
               login_scene_hit_test(scene, mfa_webauthn.left + 1.0f, mfa_webauthn.top + 1.0f) ==
                   LoginControl::mfa_webauthn &&
               !login_scene_element(scene, LoginElement::forgot_password),
           "login_scene_mfa_method_picker");
    scene.mfa_capabilities = login_mfa_capability_totp;
    expect(login_scene_layout(&scene, 1280.0f, 800.0f) &&
               scene.selected_mfa_capability == login_mfa_capability_none &&
               !login_scene_focus(&scene, LoginControl::mfa_sms) &&
               login_scene_focus(&scene, LoginControl::mfa_totp),
           "login_scene_mfa_capability_filter");
    scene.mfa_capabilities = 0xFF;
    scene.selected_mfa_capability = static_cast<u8>(login_mfa_capability_totp | login_mfa_capability_sms);
    expect(login_scene_layout(&scene, 1280.0f, 800.0f) &&
               scene.mfa_capabilities == login_mfa_capability_all &&
               scene.selected_mfa_capability == login_mfa_capability_none,
           "login_scene_rejects_ambiguous_mfa_selection");
    scene.mfa_capabilities = login_mfa_capability_none;
    scene.selected_mfa_capability = login_mfa_capability_none;
    expect(login_scene_focus(&scene, LoginControl::password) && scene.focus == LoginControl::password &&
               login_scene_layout(&scene, 640.0f, 480.0f) && !scene.qr_visible &&
               scene.focus == LoginControl::password &&
               login_scene_rectangle(scene, LoginElement::card).left == 80.0f &&
               login_scene_rectangle(scene, LoginElement::card).right == 560.0f &&
               login_scene_hit_test(scene, 1.0f, 1.0f) == LoginControl::none &&
               !login_scene_focus(&scene, LoginControl::qr),
           "login_scene_compact_focus_and_bounds");
    expect(login_scene_layout(&scene, 485.0f, 480.0f) && scene.full_viewport &&
               login_scene_rectangle(scene, LoginElement::card).left == 0.0f &&
               login_scene_rectangle(scene, LoginElement::card).right == 485.0f &&
               login_scene_layout(&scene, 829.0f, 720.0f) && !scene.qr_visible &&
               login_scene_layout(&scene, 830.0f, 720.0f) && scene.qr_visible,
           "login_scene_responsive_breakpoints");
    expect(login_scene_focus(&scene, LoginControl::password), "login_presentation_password_focus");
    scene.focus_visible = true;
    scene.action_disabled = true;
    scene.submit_disabled = true;
    password_field = login_scene_rectangle(scene, LoginElement::password_field);
    const UiRect selection{password_field.left + 4.0f, password_field.top + 4.0f,
                           password_field.left + 24.0f, password_field.top + 18.0f};
    PaintStream login_paint{};
    const LoginPresentationInput presentation{ConstSlice<UiRect>{&selection, 1},
                                              ConstSlice<LoginCompositionGeometry>{},
                                              UiRect{password_field.left + 26.0f, password_field.top + 4.0f,
                                                     password_field.left + 27.0f,
                                                     password_field.top + 20.0f},
                                              nullptr,
                                              true,
                                              true};
    expect(login_presentation_build(scene, presentation, kDefaultTheme, 830.0f, 720.0f, 96, &login_paint) &&
               login_paint.rejected == 0 && login_paint.count == 8 &&
               login_paint.quads[3].border.red == kDefaultTheme.accent.red &&
               login_paint.quads[6].fill.red == kDefaultTheme.interactive.red,
           "login_presentation_renderer_neutral_white_qr_pending_stream");
    scene.focus_visible = false;
    expect(login_presentation_build(scene, presentation, kDefaultTheme, 830.0f, 720.0f, 96, &login_paint) &&
        login_paint.quads[3].border.red == kDefaultTheme.tokens.border.strong.red &&
            scene.focus == LoginControl::password,
        "login pointer focus preserves editing without blue outline");
    expect(!login_presentation_build(scene, presentation, kDefaultTheme, 830.0f, 720.0f, 0, &login_paint),
           "login_presentation_reject_invalid_dpi");
    UiTextCommand login_text_commands[kMaximumLoginTextCommands]{};
    UiTextCommandStream login_text_stream{};
    const auto has_text = [&](LoginElement element, Utf8View text) noexcept {
        for (u32 index = 0; index < login_text_stream.count; ++index) {
            const UiTextCommand& command = login_text_stream.storage[index];
            if (command.owner.manager == &scene.widgets &&
                command.owner.handle.slot == scene.elements[static_cast<u32>(element)].slot &&
                utf8_equal(command.text, text)) {
                return true;
            }
        }
        return false;
    };
    const LoginView login_text_view{utf8_literal("account@example.com"),
                                    utf8_literal("********"),
                                    {},
                                    0,
                                    0,
                                    false,
                                    LoginControl::password,
                                    LoginStatus::submitting,
                                    login_mfa_capability_none,
                                    login_mfa_capability_none,
                                    0,
                                    false,
                                    nullptr,
                                    false,
                                    false,
                                    false,
                                    true};
    scene.recovery_disabled = login_view_recovery_disabled(login_text_view);
    expect(ui_text_command_stream_initialize(
               &login_text_stream, Slice<UiTextCommand>{login_text_commands, kMaximumLoginTextCommands}) &&
               login_text_presentation_build(scene, login_text_view, kDefaultTheme, &login_text_stream) &&
               login_text_stream.count == 10,
           "login_text_presentation_builds_bounded_semantic_stream");
    const UiRect login_outer = login_scene_rectangle(scene, LoginElement::login_field);
    const UiRect login_content = login_scene_content_rectangle(scene, LoginElement::login_field);
    bool found_login_value = false;
    bool found_submit_action = false;
    for (u32 index = 0; index < login_text_stream.count; ++index) {
        const UiTextCommand& command = login_text_stream.storage[index];
        if (command.owner.manager == &scene.widgets &&
            command.owner.handle.slot == scene.elements[static_cast<u32>(LoginElement::login_field)].slot &&
            command.owner.handle.generation ==
                scene.elements[static_cast<u32>(LoginElement::login_field)].generation) {
            found_login_value = utf8_equal(command.text, utf8_literal("account@example.com")) &&
                                command.rectangle.left == login_content.left &&
                                command.rectangle.top == login_content.top &&
                                command.horizontal_alignment == UiTextAlignment::start &&
                                command.vertical_alignment == UiTextAlignment::center;
        }
        if (command.owner.manager == &scene.widgets &&
            command.owner.handle.slot == scene.elements[static_cast<u32>(LoginElement::submit)].slot) {
            found_submit_action = utf8_equal(command.text, utf8_literal("Signing in…")) &&
                                  command.horizontal_alignment == UiTextAlignment::center &&
                                  command.vertical_alignment == UiTextAlignment::center;
        }
    }
    expect(found_login_value && found_submit_action && login_content.left == login_outer.left + 12.0f &&
               login_content.top == login_outer.top + 8.0f &&
               login_content.right == login_outer.right - 12.0f &&
               login_content.bottom == login_outer.bottom - 8.0f,
           "login_text_commands_use_widget_owned_content_geometry");
    UiTextCommand undersized_commands[kMaximumLoginTextCommands - 1]{};
    UiTextCommandStream undersized_stream{};
    expect(
        ui_text_command_stream_initialize(
            &undersized_stream, Slice<UiTextCommand>{undersized_commands, kMaximumLoginTextCommands - 1}) &&
            !login_text_presentation_build(scene, login_text_view, kDefaultTheme, &undersized_stream) &&
            undersized_stream.rejected == 1,
        "login_text_presentation_rejects_undersized_output_atomically");
    scene.action_disabled = false;
    scene.submit_disabled = false;
    LoginView remote_text_view = login_text_view;
    remote_text_view.status = LoginStatus::remote_mobile_pending;
    remote_text_view.remote_user = utf8_literal("mobile-user");
    scene.recovery_disabled = login_view_recovery_disabled(remote_text_view);
    login_text_stream.count = 0;
    expect(login_text_presentation_build(scene, remote_text_view, kDefaultTheme, &login_text_stream),
           "login_text_presentation_remote_status_build");
    const UiRect status_message_rectangle =
        login_scene_content_rectangle(scene, LoginElement::status_message);
    const UiRect status_account_rectangle =
        login_scene_content_rectangle(scene, LoginElement::status_account);
    expect(has_text(LoginElement::status_message, utf8_literal("Confirm on your phone to log in as:")) &&
               has_text(LoginElement::status_account, utf8_literal("mobile-user")) &&
               status_message_rectangle.bottom <= status_account_rectangle.top,
           "login_status_lines_are_distinct_layout_owned_widgets");

    LoginView backup_acknowledgement_view = login_text_view;
    backup_acknowledgement_view.status = LoginStatus::mfa_backup_acknowledgement;
    backup_acknowledgement_view.focus = LoginControl::submit;
    backup_acknowledgement_view.mfa_capabilities = login_mfa_capability_backup;
    backup_acknowledgement_view.selected_mfa_capability = login_mfa_capability_backup;
    backup_acknowledgement_view.backup_acknowledgement_seconds = 10;
    scene.focus = backup_acknowledgement_view.focus;
    scene.mfa_capabilities = backup_acknowledgement_view.mfa_capabilities;
    scene.selected_mfa_capability = backup_acknowledgement_view.selected_mfa_capability;
    scene.credential_fields_hidden = true;
    scene.action_disabled = login_view_action_disabled(backup_acknowledgement_view);
    scene.recovery_disabled = login_view_recovery_disabled(backup_acknowledgement_view);
    scene.submit_disabled = login_view_submit_disabled(backup_acknowledgement_view);
    login_text_stream.count = 0;
    expect(login_scene_layout(&scene, 830.0f, 720.0f) && scene.focus == LoginControl::none,
           "login_backup_countdown_clears_disabled_submit_focus");
    backup_acknowledgement_view.focus = scene.focus;
    expect(login_text_presentation_build(scene, backup_acknowledgement_view, kDefaultTheme,
                                             &login_text_stream),
           "login_text_presentation_backup_acknowledgement_build");
    const UiRect backup_acknowledgement_submit = login_scene_rectangle(scene, LoginElement::submit);
    expect(has_text(LoginElement::status_message, utf8_literal("Backup codes can only be used once.")) &&
               has_text(LoginElement::submit, utf8_literal("Continue in 10")) &&
               !login_scene_element(scene, LoginElement::password_label) &&
               !login_scene_element(scene, LoginElement::password_field) &&
               login_view_action_disabled(backup_acknowledgement_view) &&
               login_scene_hit_test(scene, backup_acknowledgement_submit.left + 1.0f,
                                    backup_acknowledgement_submit.top + 1.0f) == LoginControl::none,
           "login_backup_acknowledgement_hides_code_form_and_disables_continue");
    backup_acknowledgement_view.backup_acknowledgement_seconds = 0;
    scene.action_disabled = login_view_action_disabled(backup_acknowledgement_view);
    scene.recovery_disabled = login_view_recovery_disabled(backup_acknowledgement_view);
    scene.submit_disabled = login_view_submit_disabled(backup_acknowledgement_view);
    login_text_stream.count = 0;
    const u32 backup_layout_passes = scene.layout_passes;
    expect(login_scene_layout(&scene, 830.0f, 720.0f) && scene.layout_passes == backup_layout_passes &&
               login_text_presentation_build(scene, backup_acknowledgement_view, kDefaultTheme,
                                         &login_text_stream),
           "login_text_presentation_backup_acknowledgement_ready_build");
    expect(has_text(LoginElement::submit, utf8_literal("Continue")) &&
               !login_view_action_disabled(backup_acknowledgement_view) &&
               login_scene_hit_test(scene, backup_acknowledgement_submit.left + 1.0f,
                                    backup_acknowledgement_submit.top + 1.0f) == LoginControl::submit,
           "login_backup_acknowledgement_enables_explicit_continue_at_zero");

    LoginView local_mfa_error_view = login_text_view;
    local_mfa_error_view.status = LoginStatus::mfa_input_invalid;
    local_mfa_error_view.focus = LoginControl::password;
    local_mfa_error_view.mfa_capabilities = login_mfa_capability_backup;
    local_mfa_error_view.selected_mfa_capability = login_mfa_capability_backup;
    scene.focus = local_mfa_error_view.focus;
    scene.mfa_capabilities = local_mfa_error_view.mfa_capabilities;
    scene.selected_mfa_capability = local_mfa_error_view.selected_mfa_capability;
    scene.credential_fields_hidden = false;
    scene.action_disabled = login_view_action_disabled(local_mfa_error_view);
    scene.recovery_disabled = login_view_recovery_disabled(local_mfa_error_view);
    scene.submit_disabled = login_view_submit_disabled(local_mfa_error_view);
    login_text_stream.count = 0;
    expect(
        login_scene_layout(&scene, 830.0f, 720.0f) &&
            login_text_presentation_build(scene, local_mfa_error_view, kDefaultTheme, &login_text_stream),
        "login_text_presentation_local_mfa_error_build");
    expect(has_text(LoginElement::status_message,
                    utf8_literal("Enter a valid backup code (8-11 characters).")) &&
               has_text(LoginElement::submit, utf8_literal("Verify")) &&
               !login_view_action_disabled(local_mfa_error_view) &&
               login_view_submit_disabled(local_mfa_error_view),
           "login_local_mfa_error_remains_retryable_in_challenge");
    const UiRect local_mfa_field = login_scene_rectangle(scene, LoginElement::password_field);
    const UiRect local_mfa_submit = login_scene_rectangle(scene, LoginElement::submit);
    expect(login_scene_hit_test(scene, local_mfa_field.left + 1.0f, local_mfa_field.top + 1.0f) ==
                   LoginControl::password &&
               login_scene_hit_test(scene, local_mfa_submit.left + 1.0f, local_mfa_submit.top + 1.0f) ==
                   LoginControl::none,
           "login_invalid_mfa_disables_only_submit_not_editing");
    LoginView ready_mfa_view = local_mfa_error_view;
    ready_mfa_view.status = LoginStatus::mfa_required;
    ready_mfa_view.mfa_code_ready = true;
    login_scene_apply_view(&scene, ready_mfa_view);
    const u32 mfa_layout_passes = scene.layout_passes;
    expect(login_scene_layout(&scene, 830.0f, 720.0f) && scene.layout_passes == mfa_layout_passes &&
               login_view_valid(ready_mfa_view) && !login_view_submit_disabled(ready_mfa_view) &&
               login_scene_hit_test(scene, local_mfa_submit.left + 1.0f, local_mfa_submit.top + 1.0f) ==
                   LoginControl::submit,
           "login_valid_mfa_enables_submit_without_relayout");

    LoginView rejected_mfa_view = login_text_view;
    rejected_mfa_view.status = LoginStatus::mfa_code_rejected;
    rejected_mfa_view.focus = LoginControl::password;
    rejected_mfa_view.mfa_capabilities = login_mfa_capability_all;
    rejected_mfa_view.selected_mfa_capability = login_mfa_capability_totp;
    rejected_mfa_view.mfa_code_ready = false;
    scene.focus = rejected_mfa_view.focus;
    scene.mfa_capabilities = rejected_mfa_view.mfa_capabilities;
    scene.selected_mfa_capability = rejected_mfa_view.selected_mfa_capability;
    scene.credential_fields_hidden = false;
    scene.action_disabled = login_view_action_disabled(rejected_mfa_view);
    scene.recovery_disabled = login_view_recovery_disabled(rejected_mfa_view);
    scene.submit_disabled = login_view_submit_disabled(rejected_mfa_view);
    login_text_stream.count = 0;
    expect(login_scene_layout(&scene, 830.0f, 720.0f) &&
               login_text_presentation_build(scene, rejected_mfa_view, kDefaultTheme, &login_text_stream),
           "login_text_presentation_rejected_mfa_build");
    expect(has_text(LoginElement::status_message, utf8_literal("That code didn't work. Try again.")) &&
               has_text(LoginElement::submit, utf8_literal("Verify")) &&
               !login_view_action_disabled(rejected_mfa_view) &&
               login_view_submit_disabled(rejected_mfa_view) &&
               rejected_mfa_view.mfa_capabilities == login_mfa_capability_all,
           "login_rejected_mfa_retains_methods_and_requires_replacement_code");
    LoginView ready_retry_mfa_view = rejected_mfa_view;
    ready_retry_mfa_view.password_mask = utf8_literal("******");
    ready_retry_mfa_view.mfa_code_ready = true;
    expect(login_view_valid(ready_retry_mfa_view) && !login_view_submit_disabled(ready_retry_mfa_view),
           "login_rejected_mfa_valid_six_digit_replacement_enables_verify");
    LoginView rejected_password_view = login_text_view;
    rejected_password_view.status = LoginStatus::rejected;
    rejected_password_view.mfa_capabilities = login_mfa_capability_none;
    rejected_password_view.selected_mfa_capability = login_mfa_capability_none;
    rejected_password_view.mfa_code_ready = false;
    expect(login_view_valid(rejected_password_view) && !login_view_action_disabled(rejected_password_view) &&
               !login_view_submit_disabled(rejected_password_view),
           "login_rejected_password_preserves_ordinary_retry_fallback");
    // A refusal with a suspended-user token names the suspension instead of the generic failure.
    rejected_password_view.account_suspended = true;
    scene.focus = rejected_password_view.focus;
    scene.mfa_capabilities = rejected_password_view.mfa_capabilities;
    scene.selected_mfa_capability = rejected_password_view.selected_mfa_capability;
    scene.action_disabled = login_view_action_disabled(rejected_password_view);
    scene.recovery_disabled = login_view_recovery_disabled(rejected_password_view);
    scene.submit_disabled = login_view_submit_disabled(rejected_password_view);
    login_text_stream.count = 0;
    expect(login_scene_layout(&scene, 830.0f, 720.0f) &&
               login_text_presentation_build(scene, rejected_password_view, kDefaultTheme, &login_text_stream),
           "login_text_presentation_suspended_build");
    expect(has_text(LoginElement::status_message,
                    utf8_literal("Discord has suspended this account. See its standing at discord.com.")),
           "login_suspended_account_is_named");
    rejected_password_view.account_suspended = false;

    LoginView captcha_text_view = login_text_view;
    captcha_text_view.status = LoginStatus::captcha_required;
    captcha_text_view.focus = LoginControl::back;
    scene.focus = captcha_text_view.focus;
    scene.back_only = login_requires_back(captcha_text_view.status, captcha_text_view.remote_status);
    scene.action_disabled = login_view_action_disabled(captcha_text_view);
    scene.recovery_disabled = login_view_recovery_disabled(captcha_text_view);
    scene.submit_disabled = login_view_submit_disabled(captcha_text_view);
    scene.captcha_action = login_view_captcha_action(captcha_text_view);
    expect(login_scene_layout(&scene, 830.0f, 720.0f), "login_captcha_back_layout");
    login_text_stream.count = 0;
    expect(login_text_presentation_build(scene, captcha_text_view, kDefaultTheme, &login_text_stream),
           "login_text_presentation_captcha_status_build");
    const UiRect captcha_submit = login_scene_rectangle(scene, LoginElement::submit);
    const UiRect captcha_login = login_scene_rectangle(scene, LoginElement::login_field);
    const UiRect captcha_back = login_scene_rectangle(scene, LoginElement::forgot_password);
    expect(has_text(LoginElement::submit, utf8_literal("Verify in browser")) &&
               has_text(LoginElement::status_message,
                        utf8_literal("Complete a quick check in your browser to continue.")) &&
               login_view_action_disabled(captcha_text_view) &&
               login_scene_hit_test(scene, captcha_submit.left + 1.0f, captcha_submit.top + 1.0f) ==
                   LoginControl::submit &&
               login_scene_hit_test(scene, captcha_login.left + 1.0f, captcha_login.top + 1.0f) ==
                   LoginControl::none,
           "login_captcha_state_offers_browser_verification_without_credentials");
    expect(has_text(LoginElement::forgot_password, utf8_literal("Back to sign in")) &&
               login_scene_hit_test(scene, captcha_back.left + 1, captcha_back.top + 1) == LoginControl::back &&
               login_scene_move_focus(&scene, false) == LoginControl::submit &&
               login_scene_move_focus(&scene, false) == LoginControl::back &&
               login_control_action(LoginControl::back, captcha_text_view.status, captcha_text_view.remote_status) == LoginAction::back &&
               login_control_action(LoginControl::submit, captcha_text_view.status, captcha_text_view.remote_status) == LoginAction::captcha_verify &&
               login_control_action(LoginControl::qr, captcha_text_view.status, captcha_text_view.remote_status) == LoginAction::none,
           "login_password_captcha_offers_verify_and_back_only");
    QrCode captcha_qr{};
    captcha_qr.size = kQrCodeSize;
    captcha_text_view.status = LoginStatus::idle;
    captcha_text_view.remote_status = LoginRemoteStatus::captcha_required;
    captcha_text_view.password_mask = {};
    captcha_text_view.mfa_capabilities = login_mfa_capability_sms;
    captcha_text_view.selected_mfa_capability = login_mfa_capability_sms;
    captcha_text_view.qr_code = &captcha_qr;
    captcha_text_view.focus = LoginControl::back;
    scene.focus = captcha_text_view.focus;
    scene.back_only = login_requires_back(captcha_text_view.status, captcha_text_view.remote_status);
    scene.action_disabled = login_view_action_disabled(captcha_text_view);
    scene.recovery_disabled = login_view_recovery_disabled(captcha_text_view);
    scene.submit_disabled = login_view_submit_disabled(captcha_text_view);
    scene.captcha_action = login_view_captcha_action(captcha_text_view);
    expect(login_scene_layout(&scene, 830.0f, 720.0f), "login_remote_captcha_back_layout");
    login_text_stream.count = 0;
    expect(login_text_presentation_build(scene, captcha_text_view, kDefaultTheme, &login_text_stream),
           "login_text_presentation_remote_captcha_build");
    expect(has_text(LoginElement::submit, utf8_literal("Verify in browser")) &&
               login_view_action_disabled(captcha_text_view) &&
               login_scene_hit_test(scene, captcha_submit.left + 1.0f, captcha_submit.top + 1.0f) ==
                   LoginControl::submit &&
               login_scene_hit_test(scene, captcha_login.left + 1.0f, captcha_login.top + 1.0f) ==
                   LoginControl::none &&
               login_scene_hit_test(scene, captcha_back.left + 1, captcha_back.top + 1) == LoginControl::back &&
               login_control_action(LoginControl::back, LoginStatus::idle, LoginRemoteStatus::captcha_required) == LoginAction::back,
           "login_remote_captcha_retained_mfa_metadata_cannot_hide_back");
    bool mfa_text_absent = true;
    for (u32 index = 0; index < login_text_stream.count; ++index) {
        const auto& command = login_text_stream.storage[index];
        mfa_text_absent = mfa_text_absent && !utf8_equal(command.text, utf8_literal("SMS")) &&
            !utf8_equal(command.text, utf8_literal("SMS code")) &&
            !utf8_equal(command.text, utf8_literal("TOTP")) &&
            !utf8_equal(command.text, utf8_literal("BACKUP")) &&
            !utf8_equal(command.text, utf8_literal("KEY"));
    }
    expect(has_text(LoginElement::password_field, utf8_literal("Password")) && mfa_text_absent,
           "login_remote_captcha_ignores_retained_mfa_placeholder_and_method_text");
    expect(login_control_action(LoginControl::forgot_password, LoginStatus::idle, LoginRemoteStatus::idle) == LoginAction::recovery &&
               login_control_action(LoginControl::forgot_password, LoginStatus::mfa_required, LoginRemoteStatus::idle) == LoginAction::none &&
               login_control_action(LoginControl::submit, LoginStatus::cancelling, LoginRemoteStatus::idle) == LoginAction::none &&
               login_control_action(LoginControl::back, LoginStatus::cancelling, LoginRemoteStatus::idle) == LoginAction::back &&
               login_control_action(LoginControl::qr, LoginStatus::idle, LoginRemoteStatus::qr_ready) == LoginAction::remote_auth &&
               login_control_action(LoginControl::mfa_totp, LoginStatus::mfa_required, LoginRemoteStatus::idle) == LoginAction::mfa &&
               login_control_action(LoginControl::submit, LoginStatus::profile_save_failed, LoginRemoteStatus::idle) == LoginAction::submit,
           "login_semantic_dispatch_admits_recovery_blocks_cancelled_results_preserves_continuations");
    LoginView profile_retry_view = login_text_view;
    profile_retry_view.status = LoginStatus::profile_save_failed;
    scene.back_only = false;
    scene.captcha_action = false;
    scene.focus = profile_retry_view.focus;
    scene.action_disabled = login_view_action_disabled(profile_retry_view);
    scene.recovery_disabled = login_view_recovery_disabled(profile_retry_view);
    scene.submit_disabled = login_view_submit_disabled(profile_retry_view);
    expect(login_scene_layout(&scene, 830.0f, 720.0f) &&
               login_scene_hit_test(scene, captcha_back.left + 1, captcha_back.top + 1) == LoginControl::none &&
               login_scene_focus(&scene, LoginControl::login),
           "login_leaving_blocked_state_restores_credentials_and_removes_back");
    scene.focus = profile_retry_view.focus;
    login_text_stream.count = 0;
    expect(login_text_presentation_build(scene, profile_retry_view, kDefaultTheme, &login_text_stream),
           "login_text_presentation_profile_retry_build");
    expect(has_text(LoginElement::submit, utf8_literal("Try again")) &&
               has_text(LoginElement::status_message, utf8_literal("Couldn't save your sign-in.")) &&
               has_text(LoginElement::status_account,
                        utf8_literal("Try again, or close MaiCord to sign out.")) &&
               !login_view_action_disabled(profile_retry_view) &&
               status_message_rectangle.bottom <= status_account_rectangle.top &&
               login_scene_hit_test(scene, captcha_submit.left + 1.0f, captcha_submit.top + 1.0f) ==
                   LoginControl::submit,
           "login_profile_commit_failure_has_explicit_retry_action");
    LoginLayoutMetrics custom_metrics = kDefaultLoginLayoutMetrics;
    custom_metrics.revision = 2;
    custom_metrics.compact_card_width = 460.0f;
    expect(login_scene_layout_with_metrics(&scene, 640.0f, 480.0f, custom_metrics) &&
               scene.layout_metrics_revision == 2 &&
               login_scene_rectangle(scene, LoginElement::card).left == 90.0f &&
               login_scene_layout(&scene, 640.0f, 480.0f) &&
               scene.layout_metrics_revision == kDefaultLoginLayoutMetrics.revision &&
               login_scene_rectangle(scene, LoginElement::card).left == 80.0f,
           "login_scene_revisioned_layout_metrics");
    expect(login_scene_layout(&scene, 240.0f, 320.0f), "login_scene_extreme_narrow_layout");
    const UiLayoutValidationResult narrow_validation =
        ui_layout_engine_validate(scene.widgets.layout, Slice<UiLayoutDiagnostic>{diagnostics, 32});
    bool narrow_overflow = false;
    for (u32 index = 0; index < narrow_validation.count; ++index) {
        narrow_overflow = narrow_overflow ||
                          diagnostics[index].kind == UiLayoutDiagnosticKind::child_overflow ||
                          diagnostics[index].kind == UiLayoutDiagnosticKind::text_overflow;
    }
    expect(narrow_overflow, "login_scene_reports_unresolved_extreme_narrow_overflow");
    expect(!login_scene_layout(nullptr, 1.0f, 1.0f) && !login_scene_layout(&scene, 0.0f, 1.0f),
           "login_scene_reject_invalid_extent");
    const u32 widget_capacity = scene.widgets.storage.count;
    scene.widgets.storage.count = 1;
    expect(!login_scene_layout(&scene, 1280.0f, 800.0f) && !scene.widgets.count &&
               !scene.layout_metrics_revision &&
               !ui_widget_manager_resolve(scene.widgets,
                   scene.elements[static_cast<u32>(LoginElement::root)]),
           "login_scene_failed_rebuild_discards_partial_tree_and_cached_signature");
    scene.widgets.storage.count = widget_capacity;
    expect(login_scene_layout(&scene, 240.0f, 320.0f) && scene.widgets.count > 1 &&
               login_scene_element(scene, LoginElement::submit),
           "login_scene_failed_rebuild_can_restore_prior_layout");
    // The accessibility tree builds its heading and status from the login state without any credential text. The
    // scene's flags come from the full view, so a view that differs only in what the flags never read (the typed text,
    // the reveal, the ready flags) must still produce the text. Used to yield no text while a password was revealed,
    // a code was ready, a cooldown ran or a phone code was complete.
    {
        LoginScene bare_scene{};
        UiTextCommand bare_commands[kMaximumLoginTextCommands]{};
        UiTextCommandStream bare_text{};
        const auto bare_has = [&](LoginElement element, Utf8View text) noexcept {
            for (u32 index = 0; index < bare_text.count; ++index)
                if (bare_text.storage[index].owner.handle.slot ==
                        bare_scene.elements[static_cast<u32>(element)].slot &&
                    utf8_equal(bare_text.storage[index].text, text))
                    return true;
            return false;
        };
        const auto bare_build = [&](const LoginView& full) noexcept {
            LoginView bare = full;
            bare.login = {};
            bare.password_mask = {};
            bare.password_revealed = bare.mfa_code_ready = bare.phone_code_ready = false;
            login_scene_apply_view(&bare_scene, full);
            const bool laid_out = login_scene_layout(&bare_scene, 830.0f, 720.0f);
            bare.focus = bare_scene.focus;
            return laid_out && login_view_valid(full) && login_view_valid(bare) &&
                   ui_text_command_stream_initialize(&bare_text,
                                                     Slice<UiTextCommand>{bare_commands, kMaximumLoginTextCommands}) &&
                   login_text_presentation_build(bare_scene, bare, kDefaultTheme, &bare_text);
        };
        LoginView revealed = login_text_view;
        revealed.status = LoginStatus::idle;
        revealed.focus = LoginControl::password;
        revealed.password_mask = utf8_literal("hunter2");
        revealed.password_revealed = true;
        expect(bare_build(revealed) && bare_scene.password_revealed &&
                   bare_has(LoginElement::heading, utf8_literal("Log in to MaiCord")) &&
                   bare_has(LoginElement::submit, utf8_literal("Log in")),
               "login_accessible_text_while_password_revealed");
        LoginView ready = login_text_view;
        ready.status = LoginStatus::mfa_required;
        ready.focus = LoginControl::password;
        ready.mfa_capabilities = ready.selected_mfa_capability = login_mfa_capability_totp;
        ready.password_mask = utf8_literal("123456");
        ready.mfa_code_ready = true;
        expect(bare_build(ready) && !bare_scene.submit_disabled &&
                   bare_has(LoginElement::status_message, utf8_literal("Choose a verification method.")) &&
                   bare_has(LoginElement::submit, utf8_literal("Verify")),
               "login_accessible_text_with_mfa_code_ready");
        LoginView limited = login_text_view;
        limited.status = LoginStatus::idle;
        limited.focus = LoginControl::login;
        limited.auth_rate_limited = true;
        expect(bare_build(limited) && bare_scene.action_disabled &&
                   bare_has(LoginElement::status_message, utf8_literal("Too many attempts. Wait before retrying.")) &&
                   bare_has(LoginElement::submit, utf8_literal("Please wait")),
               "login_accessible_text_during_rate_limit_cooldown");
        LoginView phone = login_text_view;
        phone.status = LoginStatus::recovery_phone_required;
        phone.focus = LoginControl::phone_code;
        phone.login = {};
        phone.password_mask = utf8_literal("123456");
        phone.phone_code_ready = true;
        expect(bare_build(phone) && !bare_scene.submit_disabled &&
                   bare_has(LoginElement::heading, utf8_literal("Phone recovery")) &&
                   bare_has(LoginElement::status_message, utf8_literal("Enter the 6-digit code.")) &&
                   bare_has(LoginElement::submit, utf8_literal("Verify")),
               "login_accessible_text_with_phone_code_ready");
        login_scene_release(&bare_scene);
    }
    login_scene_release(&scene);
    expect(!scene.storage.arena.base && !scene.widgets.storage.data, "login_scene_owned_storage_release");
}

void test_remote_auth_protocol() noexcept {
    char encoded[512]{};
    u8 decoded[320]{};
    Base64Result base64 = base64_encode(byte_literal("Man"), Slice<char>{encoded, sizeof(encoded)});
    expect(base64.ok() && base64.bytes == 4 && encoded[0] == 'T' && encoded[1] == 'W' &&
               encoded[2] == 'F' && encoded[3] == 'u',
           "base64_encode_rfc_vector");
    base64 = base64_decode(Utf8View{encoded, base64.bytes}, Slice<u8>{decoded, sizeof(decoded)});
    expect(base64.ok() && base64.bytes == 3 && decoded[0] == 'M' && decoded[1] == 'a' && decoded[2] == 'n',
           "base64_decode_rfc_vector");
    expect(base64_encode(byte_literal("M"), Slice<char>{encoded, sizeof(encoded)}).bytes == 4 &&
               encoded[0] == 'T' && encoded[1] == 'Q' && encoded[2] == '=' && encoded[3] == '=' &&
               base64_decode(utf8_literal("TR=="), Slice<u8>{decoded, sizeof(decoded)}).error ==
                   Base64Error::non_canonical &&
               base64_decode(utf8_literal("TQ="), Slice<u8>{decoded, sizeof(decoded)}).error ==
                   Base64Error::invalid_length,
           "base64_canonical_padding_contract");

    u32 structurals[128]{};
    char text[512]{};
    RemoteAuthPacket packet{};
    RemoteAuthDecodeResult remote = remote_auth_decode_server_packet(
        utf8_literal(R"({"op":"hello","timeout_ms":120000,"heartbeat_interval":41250})"),
        Slice<u32>{structurals, 128}, 8, Slice<char>{text, sizeof(text)}, &packet);
    expect(remote.ok() && packet.kind == RemoteAuthPacketKind::hello && packet.timeout_ms == 120000 &&
               packet.heartbeat_interval_ms == 41250 && packet.text_bytes == 0,
           "remote_auth_decode_hello");

    remote = remote_auth_decode_server_packet(
        utf8_literal(
            R"({"fingerprint":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","op":"pending_remote_init"})"),
        Slice<u32>{structurals, 128}, 8, Slice<char>{text, sizeof(text)}, &packet);
    expect(remote.ok() && packet.kind == RemoteAuthPacketKind::pending_remote_init &&
               packet.text_bytes == 43 && text[0] == 'A' && text[42] == 'A',
           "remote_auth_decode_fingerprint");

    u8 rsa_block[kRemoteAuthRsaBytes]{};
    Base64Result rsa_encoded = base64_encode(ConstSlice<u8>{rsa_block, kRemoteAuthRsaBytes},
                                             Slice<char>{encoded, sizeof(encoded)});
    char nonce_packet[512]{};
    constexpr Utf8View nonce_prefix = utf8_literal("{\"op\":\"nonce_proof\",\"encrypted_nonce\":\"");
    constexpr Utf8View packet_suffix = utf8_literal("\"}");
    u32 nonce_bytes = 0;
    for (u32 index = 0; index < nonce_prefix.bytes; ++index) {
        nonce_packet[nonce_bytes++] = nonce_prefix.data[index];
    }
    for (u32 index = 0; index < rsa_encoded.bytes; ++index) {
        nonce_packet[nonce_bytes++] = encoded[index];
    }
    for (u32 index = 0; index < packet_suffix.bytes; ++index) {
        nonce_packet[nonce_bytes++] = packet_suffix.data[index];
    }
    remote =
        remote_auth_decode_server_packet(Utf8View{nonce_packet, nonce_bytes}, Slice<u32>{structurals, 128},
                                         8, Slice<char>{text, sizeof(text)}, &packet);
    expect(remote.ok() && packet.kind == RemoteAuthPacketKind::nonce_proof &&
               packet.text_bytes == rsa_encoded.bytes,
           "remote_auth_decode_encrypted_nonce");

    char wire[512]{};
    RemoteAuthWriteResult written = remote_auth_write_nonce_proof(
        utf8_literal("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), Slice<char>{wire, sizeof(wire)});
    expect(written.ok() && written.bytes == 74 && wire[0] == '{' && wire[written.bytes - 1] == '}',
           "remote_auth_write_v2_nonce_proof");
    written = remote_auth_write_qr_url(utf8_literal("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"),
                                       Slice<char>{wire, sizeof(wire)});
    expect(written.ok() && written.bytes == 66 && wire[0] == 'h' && wire[written.bytes - 1] == 'A',
           "remote_auth_write_qr_url");
    written = remote_auth_write_heartbeat(Slice<char>{wire, sizeof(wire)});
    expect(written.ok() && written.bytes == 18 && wire[7] == 'h' &&
               remote_auth_write_ticket_request(utf8_literal("gateway-ticket"),
                                                Slice<char>{wire, sizeof(wire)})
                   .ok(),
           "remote_auth_write_heartbeat_and_ticket");

    constexpr Utf8View preview_payload =
        utf8_literal("400000000000000042:0:0123456789abcdef0123456789abcdef:doodles");
    RemoteAuthUserPreview preview{};
    RemoteAuthUserPreviewResult preview_result = remote_auth_parse_user_preview(preview_payload, &preview);
    expect(preview_result.ok() && preview.user_id == 400000000000000042ull &&
               preview.discriminator.bytes == 1 && preview.discriminator.data[0] == '0' &&
               preview.avatar.bytes == 32 && preview.username.bytes == 7 &&
               preview.username.data[0] == 'd' && preview.username.data[6] == 's',
           "remote_auth_user_preview_parse");
    const RemoteAuthUserPreview retained = preview;
    preview_result =
        remote_auth_parse_user_preview(utf8_literal("18446744073709551616:0:0:user"), &preview);
    expect(preview_result.error == RemoteAuthUserPreviewError::invalid_user_id &&
               preview.user_id == retained.user_id && preview.username.data == retained.username.data &&
               remote_auth_parse_user_preview(utf8_literal("123:12:0:user"), &preview).error ==
                   RemoteAuthUserPreviewError::invalid_discriminator &&
               remote_auth_parse_user_preview(utf8_literal("123:0:short:user"), &preview).error ==
                   RemoteAuthUserPreviewError::invalid_avatar &&
               remote_auth_parse_user_preview(utf8_literal("123:0:0:user:extra"), &preview).error ==
                   RemoteAuthUserPreviewError::invalid_username,
           "remote_auth_user_preview_rejects_invalid_atomic");

    packet.kind = RemoteAuthPacketKind::cancel;
    text[0] = 'Q';
    remote = remote_auth_decode_server_packet(utf8_literal(R"({"op":"cancel","op":"heartbeat_ack"})"),
                                              Slice<u32>{structurals, 128}, 8,
                                              Slice<char>{text, sizeof(text)}, &packet);
    expect(remote.error == RemoteAuthError::duplicate_field &&
               packet.kind == RemoteAuthPacketKind::cancel && text[0] == 'Q',
           "remote_auth_decode_failure_is_atomic");
}

void test_remote_auth_crypto() noexcept {
    RemoteAuthCrypto crypto{};
    const RemoteAuthCryptoResult initialized = remote_auth_crypto_initialize(&crypto);
    expect(initialized.ok(), "remote_auth_crypto_initialize");
    if (!initialized.ok()) {
        return;
    }

    u8 spki[kRemoteAuthPublicSpkiMaximumBytes]{};
    const RemoteAuthCryptoResult size_query = remote_auth_crypto_export_spki(crypto, Slice<u8>{nullptr, 0});
    const RemoteAuthCryptoResult exported =
        remote_auth_crypto_export_spki(crypto, Slice<u8>{spki, sizeof(spki)});
    expect(size_query.error == RemoteAuthCryptoError::output_too_small &&
               size_query.bytes == exported.bytes && exported.ok() && exported.bytes >= 292 &&
               exported.bytes <= 294 && spki[0] == 0x30 && spki[1] == 0x82,
           "remote_auth_crypto_spki_der");

    remote_auth_crypto_release(&crypto);
    expect(!crypto.rsa_algorithm && !crypto.rsa_key && !crypto.sha256_algorithm,
           "remote_auth_crypto_release");
}

void test_remote_auth_handshake() noexcept {
    RemoteAuthHandshake handshake{};
    RemoteAuthHandshakeResult handled = remote_auth_handshake_initialize(&handshake);
    expect(handled.ok() && handshake.phase == RemoteAuthHandshakePhase::awaiting_hello,
           "remote_auth_handshake_initialize");
    if (!handled.ok()) {
        return;
    }

    // The first heartbeat lands uniformly within one interval; half of it here.
    handshake.first_heartbeat_fraction = 0x80000000u;
    u32 structurals[128]{};
    char command[kRemoteAuthMaximumCommandBytes]{};
    char data[512]{};
    RemoteAuthHandshakeEvent event{};
    handled = remote_auth_handshake_handle(
        &handshake, utf8_literal(R"({"op":"hello","timeout_ms":120000,"heartbeat_interval":1000})"),
        Slice<u32>{structurals, 128}, 8, 1000, Slice<char>{command, sizeof(command)},
        Slice<char>{data, sizeof(data)}, &event);
    expect(handled.ok() && handshake.phase == RemoteAuthHandshakePhase::awaiting_nonce &&
               event.kind == RemoteAuthHandshakeEventKind::send_command && event.command_bytes > 400 &&
               event.next_wake_at_ms == 1500,
           "remote_auth_handshake_hello_init");

    handled = remote_auth_handshake_tick(&handshake, 1499, Slice<char>{command, sizeof(command)}, &event);
    expect(handled.ok() && event.kind == RemoteAuthHandshakeEventKind::none &&
               event.next_wake_at_ms == 1500,
           "remote_auth_handshake_wait_deadline");
    handled = remote_auth_handshake_tick(&handshake, 1500, Slice<char>{command, sizeof(command)}, &event);
    expect(handled.ok() && event.kind == RemoteAuthHandshakeEventKind::send_command &&
               handshake.heartbeat_unacknowledged,
           "remote_auth_handshake_heartbeat");
    handled = remote_auth_handshake_handle(
        &handshake, utf8_literal(R"({"op":"heartbeat_ack"})"), Slice<u32>{structurals, 128}, 8, 2001,
        Slice<char>{command, sizeof(command)}, Slice<char>{data, sizeof(data)}, &event);
    expect(handled.ok() && !handshake.heartbeat_unacknowledged, "remote_auth_handshake_heartbeat_ack");

    const u8 nonce[32] = {31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16,
                          15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1,  0};
    u8 ciphertext[kRemoteAuthRsaBytes]{};
    u32 ciphertext_bytes = 0;
    char encoded[512]{};
    bool encrypted = remote_auth_encrypt_fixture(
        handshake.crypto.rsa_key, ConstSlice<u8>{nonce, static_cast<u32>(sizeof(nonce))},
        Slice<u8>{ciphertext, static_cast<u32>(sizeof(ciphertext))}, &ciphertext_bytes);
    Base64Result encoded_result =
        base64_encode(ConstSlice<u8>{ciphertext, ciphertext_bytes}, Slice<char>{encoded, sizeof(encoded)});
    char packet[512]{};
    u32 packet_bytes = write_fixture_json(utf8_literal("{\"op\":\"nonce_proof\",\"encrypted_nonce\":\""),
                                          Utf8View{encoded, encoded_result.bytes}, utf8_literal("\"}"),
                                          Slice<char>{packet, sizeof(packet)});
    handled = remote_auth_handshake_handle(
        &handshake, Utf8View{packet, packet_bytes}, Slice<u32>{structurals, 128}, 8, 2002,
        Slice<char>{command, sizeof(command)}, Slice<char>{data, sizeof(data)}, &event);
    expect(encrypted && encoded_result.ok() && handled.ok() &&
               handshake.phase == RemoteAuthHandshakePhase::awaiting_fingerprint &&
               event.kind == RemoteAuthHandshakeEventKind::send_command && event.command_bytes == 74,
           "remote_auth_handshake_nonce_proof");

    packet_bytes = write_fixture_json(utf8_literal("{\"op\":\"pending_remote_init\",\"fingerprint\":\""),
                                      Utf8View{handshake.fingerprint, kRemoteAuthFingerprintEncodedBytes},
                                      utf8_literal("\"}"), Slice<char>{packet, sizeof(packet)});
    handled = remote_auth_handshake_handle(
        &handshake, Utf8View{packet, packet_bytes}, Slice<u32>{structurals, 128}, 8, 2003,
        Slice<char>{command, sizeof(command)}, Slice<char>{data, sizeof(data)}, &event);
    expect(handled.ok() && handshake.phase == RemoteAuthHandshakePhase::awaiting_mobile &&
               event.kind == RemoteAuthHandshakeEventKind::qr_ready &&
               event.data_bytes == kRemoteAuthFingerprintEncodedBytes,
           "remote_auth_handshake_verified_qr");

    constexpr Utf8View user_payload =
        utf8_literal("123456789:0:0123456789abcdef0123456789abcdef:Native User");
    encrypted = remote_auth_encrypt_fixture(
        handshake.crypto.rsa_key,
        ConstSlice<u8>{reinterpret_cast<const u8*>(user_payload.data), user_payload.bytes},
        Slice<u8>{ciphertext, static_cast<u32>(sizeof(ciphertext))}, &ciphertext_bytes);
    encoded_result =
        base64_encode(ConstSlice<u8>{ciphertext, ciphertext_bytes}, Slice<char>{encoded, sizeof(encoded)});
    packet_bytes = write_fixture_json(
        utf8_literal("{\"op\":\"pending_ticket\",\"encrypted_user_payload\":\""),
        Utf8View{encoded, encoded_result.bytes}, utf8_literal("\"}"), Slice<char>{packet, sizeof(packet)});
    handled = remote_auth_handshake_handle(
        &handshake, Utf8View{packet, packet_bytes}, Slice<u32>{structurals, 128}, 8, 2004,
        Slice<char>{command, sizeof(command)}, Slice<char>{data, sizeof(data)}, &event);
    expect(
        encrypted && handled.ok() && event.kind == RemoteAuthHandshakeEventKind::user_pending &&
            event.data_bytes == user_payload.bytes &&
            bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(data), event.data_bytes},
                        ConstSlice<u8>{reinterpret_cast<const u8*>(user_payload.data), user_payload.bytes}),
        "remote_auth_handshake_user_preview");

    handled = remote_auth_handshake_handle(
        &handshake, utf8_literal(R"({"op":"pending_login","ticket":"one-time-ticket"})"),
        Slice<u32>{structurals, 128}, 8, 2005, Slice<char>{command, sizeof(command)},
        Slice<char>{data, sizeof(data)}, &event);
    expect(handled.ok() && handshake.phase == RemoteAuthHandshakePhase::ticket_ready &&
               event.kind == RemoteAuthHandshakeEventKind::ticket_ready && event.data_bytes == 15,
           "remote_auth_handshake_ticket_ready");

    constexpr Utf8View token_value = utf8_literal("fixture.user.token");
    encrypted = remote_auth_encrypt_fixture(
        handshake.crypto.rsa_key,
        ConstSlice<u8>{reinterpret_cast<const u8*>(token_value.data), token_value.bytes},
        Slice<u8>{ciphertext, static_cast<u32>(sizeof(ciphertext))}, &ciphertext_bytes);
    encoded_result =
        base64_encode(ConstSlice<u8>{ciphertext, ciphertext_bytes}, Slice<char>{encoded, sizeof(encoded)});
    packet_bytes =
        write_fixture_json(utf8_literal("{\"encrypted_token\":\""), Utf8View{encoded, encoded_result.bytes},
                           utf8_literal("\"}"), Slice<char>{packet, sizeof(packet)});
    SecureBuffer token{};
    const SecureBufferResult token_storage = secure_buffer_initialize(&token, 256);
    handled = token_storage.ok()
                  ? remote_auth_handshake_finish_ticket(&handshake, Utf8View{packet, packet_bytes},
                                                        Slice<u32>{structurals, 128}, 8, &token)
                  : RemoteAuthHandshakeResult{};
    expect(
        encrypted && token_storage.ok() && handled.ok() &&
            handled.stage == RemoteAuthHandshakeStage::ticket_complete &&
            handled.protocol_error == RemoteAuthError::none &&
            handled.crypto_error == RemoteAuthCryptoError::none &&
            handled.base64_error == Base64Error::none &&
            handshake.phase == RemoteAuthHandshakePhase::authenticated &&
            token.bytes == token_value.bytes &&
            bytes_equal(secure_buffer_view(token),
                        ConstSlice<u8>{reinterpret_cast<const u8*>(token_value.data), token_value.bytes}),
        "remote_auth_handshake_locked_token_finish");
    secure_buffer_release(&token);

    // The decrypt failure needs a real key, so it reuses the authenticated handshake.
    SecureBuffer corrupt_token_storage{};
    const SecureBufferResult corrupt_storage = secure_buffer_initialize(&corrupt_token_storage, 256);
    u8 corrupt_ciphertext[kRemoteAuthRsaBytes]{};
    char corrupt_encoded[512]{};
    const Base64Result corrupt_encoding =
        base64_encode(ConstSlice<u8>{corrupt_ciphertext, sizeof(corrupt_ciphertext)},
                      Slice<char>{corrupt_encoded, sizeof(corrupt_encoded)});
    packet_bytes = write_fixture_json(utf8_literal("{\"encrypted_token\":\""),
                                      Utf8View{corrupt_encoded, corrupt_encoding.bytes},
                                      utf8_literal("\"}"), Slice<char>{packet, sizeof(packet)});
    if (handled.ok() && corrupt_storage.ok() && corrupt_encoding.ok() && packet_bytes != 0) {
        secure_buffer_assign(&corrupt_token_storage, byte_literal("stale-fixture-credential"));
        handshake.phase = RemoteAuthHandshakePhase::ticket_ready;
        handled =
            remote_auth_handshake_finish_ticket(&handshake, Utf8View{packet, packet_bytes},
                                                Slice<u32>{structurals, 128}, 8, &corrupt_token_storage);
    }
    expect(handled.error == RemoteAuthHandshakeError::crypto &&
               handled.stage == RemoteAuthHandshakeStage::ticket_decrypt &&
               handled.protocol_error == RemoteAuthError::none &&
               handled.crypto_error == RemoteAuthCryptoError::platform_failure &&
               handled.base64_error == Base64Error::none &&
               handshake.phase == RemoteAuthHandshakePhase::failed && corrupt_token_storage.bytes == 0,
           "remote_auth_handshake_ticket_decrypt_diagnostic");
    secure_buffer_release(&corrupt_token_storage);
    remote_auth_handshake_release(&handshake);
    expect(handshake.phase == RemoteAuthHandshakePhase::empty && !handshake.crypto.rsa_key,
           "remote_auth_handshake_release");

    // Ticket parsing fails before any key use, so no RSA key is generated here.
    RemoteAuthHandshake missing_token{};
    SecureBuffer missing_token_storage{};
    const SecureBufferResult missing_storage = secure_buffer_initialize(&missing_token_storage, 256);
    if (missing_storage.ok()) {
        secure_buffer_assign(&missing_token_storage, byte_literal("stale-fixture-credential"));
        missing_token.phase = RemoteAuthHandshakePhase::ticket_ready;
        handled = remote_auth_handshake_finish_ticket(
            &missing_token, utf8_literal("{}"), Slice<u32>{structurals, 128}, 8, &missing_token_storage);
    }
    bool missing_storage_erased = missing_token_storage.data != nullptr;
    for (u32 index = 0; missing_storage_erased && index < missing_token_storage.capacity; ++index) {
        missing_storage_erased = missing_token_storage.data[index] == 0;
    }
    expect(handled.error == RemoteAuthHandshakeError::protocol &&
               handled.stage == RemoteAuthHandshakeStage::ticket_parse &&
               handled.protocol_error == RemoteAuthError::missing_field &&
               handled.crypto_error == RemoteAuthCryptoError::none &&
               handled.base64_error == Base64Error::none &&
               missing_token.phase == RemoteAuthHandshakePhase::failed && missing_token_storage.bytes == 0 &&
               missing_storage_erased,
           "remote_auth_handshake_ticket_shape_diagnostic");
    secure_buffer_release(&missing_token_storage);

    RemoteAuthHandshake expiring{};
    handled = remote_auth_handshake_initialize(&expiring);
    if (handled.ok()) {
        handled = remote_auth_handshake_handle(
            &expiring, utf8_literal(R"({"op":"hello","timeout_ms":10,"heartbeat_interval":1000})"),
            Slice<u32>{structurals, 128}, 8, 5'000, Slice<char>{command, sizeof(command)},
            Slice<char>{data, sizeof(data)}, &event);
    }
    if (handled.ok()) {
        handled =
            remote_auth_handshake_tick(&expiring, 5'010, Slice<char>{command, sizeof(command)}, &event);
    }
    expect(handled.error == RemoteAuthHandshakeError::session_timeout &&
               expiring.phase == RemoteAuthHandshakePhase::failed,
           "remote_auth_handshake_session_expiry");
    remote_auth_handshake_release(&expiring);

    // Server cancellation never touches the key, so the waiting phase is set directly.
    RemoteAuthHandshake cancelling{};
    cancelling.phase = RemoteAuthHandshakePhase::awaiting_mobile;
    handled = remote_auth_handshake_handle(
        &cancelling, utf8_literal(R"({"op":"cancel"})"), Slice<u32>{structurals, 128}, 8, 6'000,
        Slice<char>{command, sizeof(command)}, Slice<char>{data, sizeof(data)}, &event);
    expect(handled.ok() && cancelling.phase == RemoteAuthHandshakePhase::cancelled &&
               event.kind == RemoteAuthHandshakeEventKind::cancelled,
           "remote_auth_handshake_server_cancel");
}

void test_remote_auth_client_contract() noexcept {
    RemoteAuthClient client{};
    char output[8]{};
    for (u32 index = 0; index < static_cast<u32>(sizeof(output)); ++index) {
        output[index] = 'Q';
    }
    u32 qr_bytes = ~0u;
    u32 user_bytes = ~0u;
    SecureBuffer credential{};
    const RemoteAuthClientResult status = remote_auth_client_status(&client);
    const RemoteAuthClientResult cancelled = remote_auth_client_cancel(&client);
    const SecureBufferResult taken = remote_auth_client_take_credential(&client, &credential);
    const SecureBufferResult captcha =
        remote_auth_client_copy_captcha_secret(&client, CaptchaChallengeSecretKind::service, &credential);
    const RemoteAuthClientResult captcha_submitted =
        remote_auth_client_submit_captcha(&client, &credential);
    bool unchanged = true;
    for (u32 index = 0; index < static_cast<u32>(sizeof(output)); ++index) {
        unchanged = unchanged && output[index] == 'Q';
    }
    expect(status.ok() && status.phase == RemoteAuthClientPhase::empty &&
               !remote_auth_client_copy_qr_url(
                   client, Slice<char>{output, static_cast<u32>(sizeof(output))}, &qr_bytes) &&
               qr_bytes == 0 &&
               !remote_auth_client_copy_user(client, Slice<char>{output, static_cast<u32>(sizeof(output))},
                                             &user_bytes) &&
               user_bytes == 0 && unchanged && cancelled.error == RemoteAuthClientError::invalid_input &&
               taken.error == SecureBufferError::invalid_input &&
               captcha.error == SecureBufferError::invalid_input &&
               captcha_submitted.error == RemoteAuthClientError::invalid_input,
           "remote_auth_client_empty_contract");
}

void test_remote_auth_ticket_exchange_contract() noexcept {
    RemoteAuthTicketExchange exchange{};
    RemoteAuthHandshake handshake{};
    SecureBuffer output{};
    SecureBuffer solution{};
    const RemoteAuthTicketExchangeResult initialized = remote_auth_ticket_exchange_initialize(&exchange);
    const SecureBufferResult output_initialized =
        secure_buffer_initialize(&output, kRemoteAuthTicketExchangeCredentialBytes);
    const SecureBufferResult solution_initialized =
        secure_buffer_initialize(&solution, kRemoteAuthTicketExchangeSolutionBytes);
    if (solution_initialized.ok()) {
        solution.data[0] = 'x';
        solution.bytes = 1;
    }
    const RemoteAuthTicketExchangeResult invalid_run = remote_auth_ticket_exchange_run(
        &exchange, &handshake, utf8_literal("ticket"), nullptr, nullptr, nullptr, nullptr);
    const SecureBufferResult copied = remote_auth_ticket_exchange_copy_captcha_secret(
        &exchange, CaptchaChallengeSecretKind::service, &output);
    const SecureBufferResult submitted =
        remote_auth_ticket_exchange_submit_captcha(&exchange, &solution, nullptr);
    const SecureBufferResult credential = remote_auth_ticket_exchange_take_credential(&exchange, &output);
    const RemoteAuthTicketExchangeResult stopped = remote_auth_ticket_exchange_stop(&exchange, 1'000);
    expect(initialized.ok() && output_initialized.ok() && solution_initialized.ok() &&
               invalid_run.error == RemoteAuthTicketExchangeError::invalid_input &&
               copied.error == SecureBufferError::invalid_input &&
               submitted.error == SecureBufferError::invalid_input &&
               credential.error == SecureBufferError::invalid_input && stopped.ok(),
           "remote_auth_ticket_exchange_owned_empty_contract");
    remote_auth_ticket_exchange_release(&exchange);
    secure_buffer_release(&solution);
    secure_buffer_release(&output);
}

void test_qr_code() noexcept {
    const ConstSlice<u8> payload =
        byte_literal("https://discord.com/ra/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    QrCode code{};
    const bool encoded = qr_code_encode_m(payload, 5, &code);
    u8 unpacked[kQrCodeSize * kQrCodeSize]{};
    for (u32 y = 0; y < kQrCodeSize; ++y) {
        for (u32 x = 0; x < kQrCodeSize; ++x) {
            unpacked[y * kQrCodeSize + x] = qr_code_module(code, x, y) ? 1 : 0;
        }
    }
    expect(encoded && code.size == kQrCodeSize && code.mask < 8 &&
               qr_code_module(code, 0, 0) && !qr_code_module(code, 1, 1) && qr_code_module(code, 3, 3) &&
               qr_code_module(code, 8, kQrCodeSize - 8),
           "qr_v5_m_function_patterns");
    expect(hash64_bytes(kHash64Seed, unpacked, sizeof(unpacked)) == 0x707756D752311B69ull,
           "qr_reference_matrix_hash");

    QrCodeRect rectangles[512]{};
    const QrCodeRectResult query = qr_code_compact_rects(code, Slice<QrCodeRect>{nullptr, 0});
    const QrCodeRectResult compacted = qr_code_compact_rects(code, Slice<QrCodeRect>{rectangles, 512});
    bool coverage = compacted.ok() && compacted.count == query.count;
    for (u32 y = 0; coverage && y < kQrCodeSize; ++y) {
        for (u32 x = 0; coverage && x < kQrCodeSize; ++x) {
            bool covered = false;
            for (u32 index = 0; index < compacted.count; ++index) {
                covered = covered || (x >= rectangles[index].left && x < rectangles[index].right &&
                                      y >= rectangles[index].top && y < rectangles[index].bottom);
            }
            coverage = covered == qr_code_module(code, x, y);
        }
    }
    expect(query.output_too_small && query.count > 0 && query.count <= 384 && coverage,
           "qr_compact_rect_exact_coverage");

    u8 oversized[kQrCodeMaximumPayloadBytes + 1]{};
    QrCode untouched{};
    untouched.size = 99;
    expect(
        !qr_code_encode_m(ConstSlice<u8>{oversized, static_cast<u32>(sizeof(oversized))}, 1, &untouched) &&
            untouched.size == 99 && !qr_code_module(code, kQrCodeSize, 0),
        "qr_capacity_failure_is_atomic");

    const ConstSlice<u8> changed_payload =
        byte_literal("https://discord.com/ra/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB");
    QrCode changed{};
    const bool changed_ok = qr_code_encode_m(changed_payload, 5, &changed);
    expect(changed_ok &&
               hash64_bytes(kHash64Seed, code.modules, sizeof(code.modules)) !=
                   hash64_bytes(kHash64Seed, changed.modules, sizeof(changed.modules)),
           "qr_payload_changes_symbol");

    // Larger versions against a reference encoder's matrices (segno, level M, byte mode, its mask forced to the one this
    // encoder's penalty rules choose: the borders count as light for the finder-like runs, so another encoder may pick
    // another mask; segno also pads a stream that already ends on a codeword boundary with a stray zero codeword,
    // which is patched out), packed as QrCode keeps them: version 6 (four blocks) and version 10 (version
    // information, a 3 x 3 alignment grid, 16-bit count, uneven blocks).
    const auto matches = [](const QrCodeLarge& encoded, u32 size, u32 mask, const char* hex) noexcept {
        const auto nibble = [](char c) noexcept { return static_cast<u8>(c <= '9' ? c - '0' : c - 'a' + 10); };
        bool same = encoded.size == size && encoded.mask == mask;
        for (u32 i = 0; same && i < (size * size + 7) / 8; ++i)
            same = encoded.modules[i] == static_cast<u8>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
        return same;
    };
    constexpr char kV6[] =
        "7fc84538fc837636710976d16476d0edd25a02a6dbd59c8c5a374821739be05f5555557f00267f7900550823ef915e75272280e01159c8b446"
        "2d31d351a4dda633fa0217d4672628de279ed1c981e2199f48c8cf08118f45ba8a7b8bffeb89c4f9677d4b7763009f9e13f35d41382aea3430"
        "cec8cc731821b3ffc6ea3fa228de0365c4e60029f15e08051fa1d9b75790af37ef7f1a05436d0669f66a1dc16793727d7d01656d80009f016e"
        "f57fe2ff4191c5570e72b3558fd05557baf3ad4beb660e7f57315904ee20e59999577f09b9bbb701";
    constexpr char kV10[] =
        "7ff44d432be6fc83a4d1689b6d0976fd24321cd8d3ed4ad914f723a4db1555de87c8483768f984380e8be05f55555555557f800b80a3f4fd00"
        "7d3228ffd42a7d74131ff7635cc7446937239ede87b385229f3f02816bdc495586850c72739d1449c072281361902ad7e61f1cac77ad9c6935"
        "694431c6bae0410c9721e3707b734227a63c9c8251ab3879931d4cfbcf87078b26e5e63001e5e75df463ca7cd57b67d3abb24d02609d13cb2a"
        "dbf388a41c860446af3adb030a27eb7766ffc563ffcaf8092876ef8f931557522dcf500de275b228d5240e7dc09ffff7dd27cbc9c0ea23007b"
        "44edff0d7e2a2b6f9c970a371d8a1d7f2c9f716d2abe5c6348c89905d3964c238f2d12a5652c3d30864624f22f33b4a9b8ba4a46dcd88134bf"
        "f3efec5c70480a591806c2742c8628b770f537bd842b79985728b1ac7e9f556eab0396b9a6e1ba21b31375398fd25d3c94b64f559243c77ef5"
        "c0a745fd906e5f0032568f417ee2fc89615f7fb9d70a0a712d3e1388d315f1d6c7c1f0b3eb09d36be2a04b574c8710f7e68920ae4dba9c5a3d"
        "7fcd5ddafe84d700";
    const ConstSlice<u8> otpauth = byte_literal(
        "otpauth://totp/Discord:someone.with.a.long.name?secret=JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP&issuer=Discord");
    u8 long_payload[208];
    for (u32 i = 0; i < 190; ++i)
        long_payload[i] = static_cast<u8>(32 + i % 95);
    for (u32 i = 0; i < 18; ++i)
        long_payload[190 + i] = static_cast<u8>('a' + i);
    QrCodeLarge six{}, ten{};
    QrCode small{};
    expect(qr_code_encode_m(otpauth, 1, &six) && matches(six, 41, 0, kV6) &&
               qr_code_encode_m(ConstSlice<u8>{long_payload, 208}, 1, &ten) && matches(ten, 57, 2, kV10) &&
               !qr_code_encode_m(otpauth, 1, &small),
           "qr_versions_6_and_10_match_the_reference_and_a_small_symbol_caps_its_version");
    QrCodeRect large[1024]{};
    expect(qr_code_compact_rects(ten, Slice<QrCodeRect>{large, 1024}).ok(),
           "qr_version_10_compacts_within_the_paint_buffer");
}

namespace {

struct SecureTextStoreTestContext {
    u32 changed;
    u32 geometry_queries;
};

bool secure_text_store_test_geometry(void* opaque, SecureTextStoreGeometry* geometry) noexcept {
    auto* context = static_cast<SecureTextStoreTestContext*>(opaque);
    if (!context || !geometry) {
        return false;
    }
    ++context->geometry_queries;
    geometry->text = {120, 240, 520, 284};
    geometry->window = {100, 200, 900, 700};
    return true;
}

void secure_text_store_test_changed(void* opaque) noexcept {
    auto* context = static_cast<SecureTextStoreTestContext*>(opaque);
    if (context) {
        ++context->changed;
    }
}

bool secure_text_store_test_guid_equal(REFGUID left, REFGUID right) noexcept {
    if (left.Data1 != right.Data1 || left.Data2 != right.Data2 || left.Data3 != right.Data3) {
        return false;
    }
    for (u32 index = 0; index < 8; ++index) {
        if (left.Data4[index] != right.Data4[index]) {
            return false;
        }
    }
    return true;
}

struct SecureTextStoreTestSink final : ITextStoreACPSink {
    LONG references = 1;
    ITextStoreACP* store = nullptr;
    u32 lock_grants = 0;
    u32 text_changes = 0;
    u32 selection_changes = 0;
    u32 layout_changes = 0;
    DWORD first_lock = 0;
    DWORD second_lock = 0;
    HRESULT no_write_under_read = E_FAIL;
    HRESULT nested_sync_outer = E_FAIL;
    HRESULT nested_sync_session = E_FAIL;
    HRESULT nested_upgrade_outer = E_FAIL;
    HRESULT nested_upgrade_session = E_FAIL;
    HRESULT read_text = E_FAIL;
    HRESULT read_extent = E_FAIL;
    HRESULT inserted = E_FAIL;
    const WCHAR* insert_text = nullptr;
    ULONG insert_count = 0;
    TS_TEXTCHANGE last_external_change{};
    WCHAR read_units[8]{};
    ULONG read_count = 0;
    LONG read_next = 0;
    bool edit_in_callback = false, editing = false;
    u32 reentrant_edits = 0;
    RECT read_rectangle{};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** output) override {
        if (!output) {
            return E_POINTER;
        }
        *output = nullptr;
        if (secure_text_store_test_guid_equal(riid, __uuidof(IUnknown)) ||
            secure_text_store_test_guid_equal(riid, __uuidof(ITextStoreACPSink))) {
            *output = static_cast<ITextStoreACPSink*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return static_cast<ULONG>(InterlockedDecrement(&references));
    }

    HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE* change) override {
        ++text_changes;
        if (edit_in_callback && !editing) {
            // A text service edits the document while it is being told about a change.
            editing = true;
            HRESULT reentrant_session = E_FAIL;
            store->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &reentrant_session);
            editing = false;
        }
        if (change) {
            last_external_change = *change;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnSelectionChange() override {
        ++selection_changes;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override {
        ++layout_changes;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD flags) override {
        if (editing) {
            const WCHAR marker[] = {L'#'};
            TS_TEXTCHANGE change{};
            reentrant_edits += SUCCEEDED(store->InsertTextAtSelection(TS_IAS_NOQUERY, marker, 1, nullptr, nullptr, &change));
            return S_OK;
        }
        ++lock_grants;
        if (lock_grants == 1) {
            first_lock = flags;
            ULONG run_count = 0;
            TS_RUNINFO run{};
            read_text = store->GetText(0, -1, read_units, 8, &read_count, &run, 1, &run_count, &read_next);
            TS_SELECTION_ACP invalid_write{};
            no_write_under_read = store->SetSelection(1, &invalid_write);
            nested_sync_outer = store->RequestLock(TS_LF_SYNC | TS_LF_READWRITE, &nested_sync_session);
            nested_upgrade_outer = store->RequestLock(TS_LF_READWRITE, &nested_upgrade_session);
            BOOL clipped = TRUE;
            read_extent = store->GetTextExt(1, 0, read_next, &read_rectangle, &clipped);
        } else if (lock_grants == 2) {
            second_lock = flags;
            const WCHAR suffix[] = {L'!'};
            TS_TEXTCHANGE change{};
            inserted = store->InsertTextAtSelection(TS_IAS_NOQUERY, insert_text ? insert_text : suffix,
                insert_text ? insert_count : 1, nullptr, nullptr, &change);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override {
        return S_OK;
    }
};

} // namespace

void test_secure_text_store() noexcept {
    SecureEdit edit{};
    SecureTextStore store{};
    SecureTextStoreTestContext context{};
    const char initial[] = {'A', static_cast<char>(0xF0u), static_cast<char>(0x9Fu),
                            static_cast<char>(0x98u), static_cast<char>(0x80u)};
    expect(secure_edit_initialize(&edit, 32, false) &&
               secure_edit_assign(&edit, Utf8View{initial, static_cast<u32>(sizeof(initial))}) ==
                   SecureEditResult::changed &&
               secure_text_store_initialize(
                   &store, SecureTextStoreConfig{reinterpret_cast<HWND>(1), secure_text_store_test_geometry,
                                                 secure_text_store_test_changed, &context}) &&
               secure_text_store_bind(&store, &edit, SecureTextInputScope::login_name),
           "secure_text_store_core_initialize_and_bind");

    auto* text_store = static_cast<ITextStoreACP*>(secure_text_store_test_interface(&store));
    LONG unlocked_end = -1;
    expect(text_store && text_store->GetEndACP(&unlocked_end) == TS_E_NOLOCK,
           "secure_text_store_requires_document_lock");

    ITfInputScope* input_scope = nullptr;
    InputScope* scopes = nullptr;
    UINT scope_count = 0;
    const HRESULT scope_query =
        text_store
            ? text_store->QueryInterface(__uuidof(ITfInputScope), reinterpret_cast<void**>(&input_scope))
            : E_FAIL;
    const HRESULT scope_read = input_scope ? input_scope->GetInputScopes(&scopes, &scope_count) : E_FAIL;
    expect(SUCCEEDED(scope_query) && SUCCEEDED(scope_read) && scope_count == 1 && scopes &&
               scopes[0] == IS_LOGINNAME,
           "secure_text_store_itf_input_scope_login_name");
    if (scopes) {
        HMODULE ole32 = LoadLibraryExW(L"ole32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        using TaskFreeFunction = void(WINAPI*)(void* memory);
        const auto task_free =
            ole32 ? reinterpret_cast<TaskFreeFunction>(GetProcAddress(ole32, "CoTaskMemFree")) : nullptr;
        if (task_free) {
            task_free(scopes);
        }
        if (ole32) {
            FreeLibrary(ole32);
        }
    }
    com_release(input_scope);

    SecureEdit password_edit{};
    expect(secure_edit_initialize(&password_edit, 32, true) &&
               !secure_text_store_bind(&store, &password_edit, SecureTextInputScope::text),
           "secure_text_store_rejects_password_documents");
    secure_edit_release(&password_edit);

    SecureTextStoreTestSink sink{};
    sink.store = text_store;
    const HRESULT advised = text_store->AdviseSink(
        __uuidof(ITextStoreACPSink), &sink, TS_AS_TEXT_CHANGE | TS_AS_SEL_CHANGE | TS_AS_LAYOUT_CHANGE);
    HRESULT session = E_FAIL;
    const HRESULT lock_request = text_store->RequestLock(TS_LF_SYNC | TS_LF_READ, &session);
    expect(SUCCEEDED(advised) && SUCCEEDED(lock_request) && session == S_OK && sink.lock_grants == 2 &&
               sink.first_lock == (TS_LF_SYNC | TS_LF_READ) && sink.second_lock == TS_LF_READWRITE &&
               sink.nested_sync_outer == S_OK && sink.nested_sync_session == TS_E_SYNCHRONOUS &&
               sink.nested_upgrade_outer == S_OK && sink.nested_upgrade_session == TS_S_ASYNC,
           "secure_text_store_lock_upgrade_protocol");
    expect(sink.read_text == S_OK && sink.read_count == 3 && sink.read_next == 3 &&
               sink.read_units[0] == L'A' && sink.read_units[1] == 0xD83Du &&
               sink.read_units[2] == 0xDE00u && sink.no_write_under_read == TS_E_NOLOCK &&
               sink.read_extent == S_OK && sink.read_rectangle.left == 120 &&
               sink.read_rectangle.bottom == 284 && context.geometry_queries == 1,
           "secure_text_store_locked_utf16_and_geometry_read");
    u32 length_after_insert = 0;
    expect(sink.inserted == S_OK &&
               secure_edit_utf16_length(edit, &length_after_insert) == SecureEditResult::unchanged &&
               length_after_insert == 4 && context.changed == 1 && sink.text_changes == 0 &&
               sink.selection_changes == 0 && sink.layout_changes == 1,
           "secure_text_store_tsf_mutation_not_echoed_to_sink");

    expect(secure_edit_insert_scalar(&edit, '?') == SecureEditResult::changed,
           "secure_text_store_external_mutation_fixture");
    secure_text_store_synchronize(&store);
    expect(sink.text_changes == 1 && sink.selection_changes == 1 && sink.layout_changes == 2 &&
               sink.last_external_change.acpStart == 0 && sink.last_external_change.acpOldEnd == 4 &&
               sink.last_external_change.acpNewEnd == 5,
           "secure_text_store_external_change_notifications");
    secure_text_store_notify_layout(&store);
    expect(sink.layout_changes == 3, "secure_text_store_explicit_layout_notification");
    // A text service edit made inside OnTextChange is observed by the store, so the
    // next synchronization has nothing left to announce. Layout is announced once for
    // the text change and once when the nested write lock ends.
    sink.edit_in_callback = true;
    expect(secure_edit_insert_scalar(&edit, '?') == SecureEditResult::changed, "secure_text_store_reentrant_fixture");
    secure_text_store_synchronize(&store);
    expect(sink.reentrant_edits == 1 && sink.layout_changes == 5, "secure_text_store_edit_inside_text_change_applies");
    const u32 text_after_reentry = sink.text_changes, selection_after_reentry = sink.selection_changes;
    secure_text_store_synchronize(&store);
    expect(sink.text_changes == text_after_reentry && sink.selection_changes == selection_after_reentry &&
               sink.layout_changes == 5,
           "secure_text_store_edit_inside_text_change_is_not_announced_again");
    sink.edit_in_callback = false;
    expect(text_store->UnadviseSink(&sink) == S_OK && sink.references == 1,
           "secure_text_store_sink_identity_unadvise");

    SecureEdit multiline{};
    expect(secure_edit_initialize(&multiline, 32, false, {5, true}) &&
        secure_edit_assign(&multiline, utf8_literal("A\r\n\xF0\x9F\x98\x80")) == SecureEditResult::changed &&
        secure_text_store_bind(&store, &multiline, SecureTextInputScope::text),
        "secure_text_store_bind_multiline_field");
    SecureTextStoreTestSink multiline_sink{};
    multiline_sink.store = text_store;
    const WCHAR newline[] = {L'\n'};
    multiline_sink.insert_text = newline;
    multiline_sink.insert_count = 1;
    const u32 before_multiline = context.changed;
    expect(text_store->AdviseSink(__uuidof(ITextStoreACPSink), &multiline_sink,
        TS_AS_TEXT_CHANGE | TS_AS_SEL_CHANGE | TS_AS_LAYOUT_CHANGE) == S_OK &&
        text_store->RequestLock(TS_LF_SYNC | TS_LF_READ, &session) == S_OK && session == S_OK &&
        multiline_sink.read_count == 5 && multiline_sink.read_units[1] == L'\r' &&
        multiline_sink.read_units[2] == L'\n' && multiline_sink.read_units[3] == 0xD83D &&
        multiline_sink.read_units[4] == 0xDE00 && multiline_sink.inserted == S_OK &&
        context.changed == before_multiline + 1 && multiline_sink.text_changes == 0 &&
        multiline.storage.bytes == 8 && multiline.storage.data[7] == '\n',
        "secure_text_store_multiline_acp_insert_preserves_offsets");
    expect(text_store->UnadviseSink(&multiline_sink) == S_OK,
        "secure_text_store_multiline_unadvise");
    SecureTextStoreTestSink limited_sink{};
    limited_sink.store = text_store;
    const u32 limited_revision = multiline.revision;
    expect(text_store->AdviseSink(__uuidof(ITextStoreACPSink), &limited_sink,
        TS_AS_TEXT_CHANGE | TS_AS_SEL_CHANGE | TS_AS_LAYOUT_CHANGE) == S_OK &&
        text_store->RequestLock(TS_LF_SYNC | TS_LF_READ, &session) == S_OK && session == S_OK &&
        limited_sink.inserted == E_FAIL && multiline.revision == limited_revision &&
        multiline.storage.bytes == 8 && context.changed == before_multiline + 1 &&
        limited_sink.text_changes == 0 && limited_sink.layout_changes == 0,
        "secure_text_store_character_limit_rejects_without_notifications");
    expect(text_store->UnadviseSink(&limited_sink) == S_OK, "secure_text_store_limited_unadvise");
    secure_text_store_unbind(&store);
    secure_edit_release(&multiline);

    secure_text_store_release(&store);
    secure_edit_release(&edit);
    expect(!store.implementation && !edit.storage.data, "secure_text_store_release_owns_no_secret_copy");
}

void test_secure_edit() noexcept {
    SecureEdit edit{};
    expect(secure_edit_initialize(&edit, 32, false) && edit.storage.locked && !edit.password,
           "secure_edit_locked_initialize");
    const char initial[] = {'a', static_cast<char>(0xC3u), static_cast<char>(0xA9u)};
    expect(secure_edit_assign(&edit, Utf8View{initial, sizeof(initial)}) == SecureEditResult::changed &&
               edit.caret == 3 && edit.anchor == 3 && edit.revision == 1,
           "secure_edit_assign_utf8");
    expect(secure_edit_insert_utf16(&edit, 0xD83Du) == SecureEditResult::pending_surrogate &&
               secure_edit_insert_utf16(&edit, 0xDE00u) == SecureEditResult::changed &&
               edit.storage.bytes == 7 && edit.caret == 7 &&
               utf8_validate(Utf8View{reinterpret_cast<const char*>(edit.storage.data), edit.storage.bytes})
                       .codepoints == 3,
           "secure_edit_utf16_surrogate_pair");
    expect(secure_edit_move(&edit, SecureEditMove::previous, false) == SecureEditResult::changed &&
               edit.caret == 3 && secure_edit_backspace(&edit) == SecureEditResult::changed &&
               edit.storage.bytes == 5 && edit.caret == 1,
           "secure_edit_unicode_boundary_backspace");
    expect(secure_edit_move(&edit, SecureEditMove::end, false) == SecureEditResult::changed &&
               secure_edit_move(&edit, SecureEditMove::previous, true) == SecureEditResult::changed &&
               edit.anchor != edit.caret &&
               secure_edit_insert_scalar(&edit, 'x') == SecureEditResult::changed &&
               edit.storage.bytes == 2 && edit.storage.data[0] == 'a' && edit.storage.data[1] == 'x',
           "secure_edit_selection_replace");
    expect(secure_edit_select_all(&edit) == SecureEditResult::changed &&
               secure_edit_delete_forward(&edit) == SecureEditResult::changed && edit.storage.bytes == 0 &&
               edit.storage.data[0] == 0,
           "secure_edit_delete_and_zero_tail");
    expect(secure_edit_insert_utf8(&edit, utf8_literal("native.user")) == SecureEditResult::changed &&
               edit.storage.bytes == 11 && edit.caret == 11 &&
               secure_edit_move(&edit, SecureEditMove::home, false) == SecureEditResult::changed &&
               secure_edit_move(&edit, SecureEditMove::next, true) == SecureEditResult::changed &&
               secure_edit_insert_utf8(&edit, utf8_literal("N")) == SecureEditResult::changed &&
               secure_edit_view(edit)[0] == 'N' && edit.storage.bytes == 11,
           "secure_edit_utf8_paste_replaces_selection");
    const u32 clear_revision = edit.revision;
    expect(secure_edit_clear(&edit) == SecureEditResult::changed && edit.storage.bytes == 0 &&
               edit.anchor == 0 && edit.caret == 0 && edit.pending_high_surrogate == 0 &&
               edit.revision == clear_revision + 1 &&
               secure_edit_clear(&edit) == SecureEditResult::unchanged,
           "secure_edit_clear_resets_text_and_selection");
    expect(secure_edit_insert_utf8(&edit, utf8_literal("Native.user")) == SecureEditResult::changed &&
               secure_edit_move(&edit, SecureEditMove::home, false) == SecureEditResult::changed &&
               secure_edit_move(&edit, SecureEditMove::next, false) == SecureEditResult::changed,
           "secure_edit_repopulate_after_clear");
    const u32 paste_revision = edit.revision;
    expect(secure_edit_insert_utf8(&edit, utf8_literal("line\nfeed")) == SecureEditResult::invalid_input &&
               edit.revision == paste_revision && edit.storage.bytes == 11,
           "secure_edit_utf8_paste_rejects_control_atomic");
    char edit_window[16]{};
    u32 edit_window_bytes = 0;
    SecureEditRenderWindow edit_visual{};
    expect(secure_edit_move(&edit, SecureEditMove::end, false) == SecureEditResult::changed &&
               secure_edit_move(&edit, SecureEditMove::previous, true) == SecureEditResult::changed &&
               secure_edit_move(&edit, SecureEditMove::previous, true) == SecureEditResult::changed &&
               secure_edit_render_window(edit, 4,
                                         Slice<char>{edit_window, static_cast<u32>(sizeof(edit_window))},
                                         &edit_window_bytes, &edit_visual) == SecureEditResult::unchanged &&
               edit_window_bytes == 4 && edit_window[0] == 'u' && edit_window[1] == 's' &&
               edit_window[3] == 'r' && edit_visual.first_codepoint == 7 && edit_visual.anchor == 4 &&
               edit_visual.caret == 2,
           "secure_edit_render_window_keeps_selection_visible");
    expect(secure_edit_set_codepoint(&edit, 1, false) == SecureEditResult::changed && edit.anchor == 1 &&
               edit.caret == 1 && secure_edit_set_codepoint(&edit, 4, true) == SecureEditResult::changed &&
               edit.anchor == 1 && edit.caret == 4 &&
               secure_edit_set_codepoint(&edit, 99, false) == SecureEditResult::invalid_input &&
               edit.anchor == 1 && edit.caret == 4,
           "secure_edit_absolute_codepoint_selection_atomic");
    const char invalid[] = {static_cast<char>(0xC0u), static_cast<char>(0xAFu)};
    expect(secure_edit_assign(&edit, Utf8View{invalid, sizeof(invalid)}) == SecureEditResult::invalid_utf8,
           "secure_edit_reject_invalid_utf8");
    secure_edit_release(&edit);
    expect(!edit.storage.data && edit.storage.capacity == 0, "secure_edit_release_zeroes_owner");

    SecureEdit password{};
    expect(secure_edit_initialize(&password, 16, true), "secure_edit_password_initialize");
    const char secret[] = {'p', static_cast<char>(0xF0u), static_cast<char>(0x9Fu),
                           static_cast<char>(0x98u), static_cast<char>(0x80u)};
    char masked[12]{};
    u32 masked_bytes = 0;
    SecureEditRenderWindow masked_window{};
    expect(secure_edit_assign(&password, Utf8View{secret, sizeof(secret)}) == SecureEditResult::changed &&
               secure_edit_render_window(password, 16, Slice<char>{masked, sizeof(masked)}, &masked_bytes,
                                         &masked_window) == SecureEditResult::unchanged &&
               masked_bytes == 6 && static_cast<u8>(masked[0]) == 0xE2u &&
               static_cast<u8>(masked[3]) == 0xE2u && masked[0] != 'p',
           "secure_edit_password_mask_only");
    secure_edit_release(&password);

    SecureEdit bounded{};
    expect(secure_edit_initialize(&bounded, 4, false) &&
               secure_edit_assign(&bounded, utf8_literal("1234")) == SecureEditResult::changed &&
               secure_edit_insert_scalar(&bounded, '5') == SecureEditResult::capacity &&
               bounded.storage.bytes == 4,
           "secure_edit_capacity_is_transactional");
    secure_edit_release(&bounded);

    SecureEdit acp{};
    const char acp_text[] = {'A',
                             static_cast<char>(0xF0u),
                             static_cast<char>(0x9Fu),
                             static_cast<char>(0x98u),
                             static_cast<char>(0x80u),
                             static_cast<char>(0xC3u),
                             static_cast<char>(0xA9u)};
    u32 acp_units = 0;
    u16 copied[8]{};
    u32 copied_units = 0;
    u32 copied_next = 0;
    SecureEditUtf16Selection acp_selection{};
    expect(
        secure_edit_initialize(&acp, 32, false) &&
            secure_edit_assign(&acp, Utf8View{acp_text, static_cast<u32>(sizeof(acp_text))}) ==
                SecureEditResult::changed &&
            secure_edit_utf16_length(acp, &acp_units) == SecureEditResult::unchanged && acp_units == 4 &&
            secure_edit_copy_utf16(acp, 0, acp_units,
                                   Slice<u16>{copied, static_cast<u32>(sizeof(copied) / sizeof(copied[0]))},
                                   &copied_units, &copied_next) == SecureEditResult::unchanged &&
            copied_units == 4 && copied_next == 4 && copied[0] == 'A' && copied[1] == 0xD83Du &&
            copied[2] == 0xDE00u && copied[3] == 0x00E9u,
        "secure_edit_tsf_acp_length_and_copy");
    expect(secure_edit_set_utf16_selection(&acp, SecureEditUtf16Selection{3, 1}) ==
                   SecureEditResult::changed &&
               secure_edit_utf16_selection(acp, &acp_selection) == SecureEditResult::unchanged &&
               acp_selection.anchor == 3 && acp_selection.caret == 1,
           "secure_edit_tsf_acp_selection_roundtrip");
    const u32 acp_anchor = acp.anchor;
    const u32 acp_caret = acp.caret;
    expect(secure_edit_set_utf16_selection(&acp, SecureEditUtf16Selection{2, 2}) ==
                   SecureEditResult::invalid_utf16 &&
               acp.anchor == acp_anchor && acp.caret == acp_caret,
           "secure_edit_tsf_rejects_split_surrogate_selection");
    u16 short_copy[2]{};
    expect(secure_edit_copy_utf16(acp, 0, 4, Slice<u16>{short_copy, 2}, &copied_units, &copied_next) ==
                   SecureEditResult::unchanged &&
               copied_units == 1 && copied_next == 1 && short_copy[0] == 'A',
           "secure_edit_tsf_copy_preserves_scalar_boundary");
    const u16 replacement[] = {0x65E5u};
    SecureEditUtf16Change acp_change{};
    expect(secure_edit_replace_utf16(
               &acp, 1, 3,
               ConstSlice<u16>{replacement, static_cast<u32>(sizeof(replacement) / sizeof(replacement[0]))},
               &acp_change) == SecureEditResult::changed &&
               acp_change.start == 1 && acp_change.old_end == 3 && acp_change.new_end == 2 &&
               acp.storage.bytes == 6 && acp.storage.data[0] == 'A' && acp.storage.data[1] == 0xE6u &&
               acp.storage.data[2] == 0x97u && acp.storage.data[3] == 0xA5u &&
               acp.storage.data[4] == 0xC3u && acp.storage.data[5] == 0xA9u,
           "secure_edit_tsf_transactional_utf16_replace");
    const u16 invalid_surrogate[] = {0xD800u};
    const u32 acp_revision = acp.revision;
    const SecureEditUtf16Change unchanged_change{7, 8, 9};
    acp_change = unchanged_change;
    expect(secure_edit_replace_utf16(
               &acp, 1, 2,
               ConstSlice<u16>{invalid_surrogate,
                               static_cast<u32>(sizeof(invalid_surrogate) / sizeof(invalid_surrogate[0]))},
               &acp_change) == SecureEditResult::invalid_utf16 &&
               acp.revision == acp_revision && acp_change.start == unchanged_change.start &&
               acp_change.old_end == unchanged_change.old_end &&
               acp_change.new_end == unchanged_change.new_end,
           "secure_edit_tsf_invalid_utf16_is_atomic");
    secure_edit_release(&acp);
}

void test_secure_edit_policy() noexcept {
    SecureEdit edit{};
    expect(!secure_edit_initialize(&edit, 32, true, {0, true}) && !edit.storage.data,
        "secure_edit_multiline_password_rejected");
    expect(secure_edit_initialize(&edit, 32, false, {5, true}), "secure_edit_multiline_initialize");
    const auto text = [&edit]() noexcept {
        return Utf8View{reinterpret_cast<const char*>(edit.storage.data), edit.storage.bytes};
    };
    expect(secure_edit_assign(&edit, utf8_literal("A\r\n\xF0\x9F\x98\x80")) == SecureEditResult::changed &&
        secure_edit_insert_scalar(&edit, '\n') == SecureEditResult::changed &&
        utf8_validate(text()).codepoints == 5 && edit.storage.bytes == 8,
        "secure_edit_multiline_scalar_limit_counts_unicode_not_bytes_or_acp");
    const u32 full_revision = edit.revision;
    expect(secure_edit_insert_scalar(&edit, 'X') == SecureEditResult::capacity &&
        secure_edit_insert_utf8(&edit, utf8_literal("XY")) == SecureEditResult::capacity &&
        secure_edit_assign(&edit, utf8_literal("123456")) == SecureEditResult::capacity &&
        edit.revision == full_revision && edit.caret == 8 && edit.anchor == 8 && edit.storage.bytes == 8,
        "secure_edit_multiline_all_utf8_limit_failures_atomic");
    SecureEditUtf16Change change{77, 88, 99};
    const u16 pair[] = {0xD83D, 0xDE00};
    expect(secure_edit_replace_utf16(&edit, 6, 6, {pair, 2}, &change) == SecureEditResult::capacity &&
        change.start == 77 && change.old_end == 88 && change.new_end == 99 &&
        edit.revision == full_revision && edit.caret == 8,
        "secure_edit_multiline_tsf_limit_failure_atomic");
    expect(secure_edit_set_utf16_selection(&edit, {5, 3}) == SecureEditResult::changed &&
        secure_edit_insert_utf8(&edit, utf8_literal("Z")) == SecureEditResult::changed &&
        utf8_equal(text(), utf8_literal("A\r\nZ\n")),
        "secure_edit_multiline_replacement_credits_selected_scalars");
    expect(secure_edit_replace_utf16(&edit, 3, 4, {pair, 2}, &change) == SecureEditResult::changed &&
        change.start == 3 && change.old_end == 4 && change.new_end == 5 &&
        utf8_equal(text(), utf8_literal("A\r\n\xF0\x9F\x98\x80\n")),
        "secure_edit_multiline_tsf_replacement_at_scalar_limit");
    u16 copied[8]{};
    u32 written = 0, next = 0;
    expect(secure_edit_copy_utf16(edit, 0, 6, {copied, 8}, &written, &next) == SecureEditResult::unchanged &&
        written == 6 && next == 6 && copied[1] == '\r' && copied[2] == '\n' &&
        copied[3] == 0xD83D && copied[4] == 0xDE00 && copied[5] == '\n',
        "secure_edit_multiline_copy_roundtrip_crlf_and_non_bmp");
    const u16 invalid[] = {'\n', 0};
    const u32 valid_revision = edit.revision;
    expect(secure_edit_replace_utf16(&edit, 0, 6, {invalid, 2}, &change) == SecureEditResult::invalid_utf16 &&
        secure_edit_assign(&edit, utf8_literal("line\tbreak")) == SecureEditResult::invalid_input &&
        secure_edit_insert_utf8(&edit, utf8_literal("\n\x7F")) == SecureEditResult::invalid_input &&
        edit.revision == valid_revision && edit.storage.bytes == 8,
        "secure_edit_multiline_other_controls_still_rejected");
    secure_edit_release(&edit);
    expect(secure_edit_initialize(&edit, 32, false, {0, true}) &&
        secure_edit_assign(&edit, utf8_literal("ab\r\n\xC3\xA9x\nyz")) == SecureEditResult::changed,
        "secure_edit_line_navigation_fixture");
    expect(secure_edit_move(&edit, SecureEditMove::line_home, false) == SecureEditResult::changed &&
        edit.anchor == 8 && edit.caret == 8 &&
        secure_edit_move(&edit, SecureEditMove::line_end, true) == SecureEditResult::changed &&
        edit.anchor == 8 && edit.caret == 10,
        "secure_edit_line_navigation_selects_last_line");
    expect(secure_edit_set_codepoint(&edit, 5, false) == SecureEditResult::changed &&
        secure_edit_move(&edit, SecureEditMove::line_home, true) == SecureEditResult::changed &&
        edit.caret == 4 && edit.anchor == 6 &&
        secure_edit_move(&edit, SecureEditMove::line_end, false) == SecureEditResult::changed &&
        edit.caret == 7 && edit.anchor == 7,
        "secure_edit_line_navigation_unicode_and_selection_collapse");
    expect(secure_edit_set_utf16_selection(&edit, {3, 3}) == SecureEditResult::changed &&
        secure_edit_move(&edit, SecureEditMove::line_end, false) == SecureEditResult::changed && edit.caret == 2 &&
        secure_edit_move(&edit, SecureEditMove::line_home, false) == SecureEditResult::changed && edit.caret == 0,
        "secure_edit_line_navigation_inside_crlf");
    expect(secure_edit_assign(&edit, utf8_literal("a\n")) == SecureEditResult::changed &&
        secure_edit_move(&edit, SecureEditMove::line_home, false) == SecureEditResult::unchanged &&
        secure_edit_move(&edit, SecureEditMove::line_end, false) == SecureEditResult::unchanged && edit.caret == 2,
        "secure_edit_empty_final_line_navigation");
    secure_edit_release(&edit);
    expect(secure_edit_initialize(&edit, 4, false, {99, true}) &&
        secure_edit_assign(&edit, utf8_literal("a\r\nb")) == SecureEditResult::changed &&
        secure_edit_insert_scalar(&edit, '\n') == SecureEditResult::capacity && edit.storage.bytes == 4,
        "secure_edit_multiline_byte_limit_remains_independent");
    secure_edit_release(&edit);
    expect(secure_edit_initialize(&edit, 32, false) &&
        secure_edit_assign(&edit, utf8_literal("safe")) == SecureEditResult::changed &&
        secure_edit_assign(&edit, utf8_literal("not\nsafe")) == SecureEditResult::invalid_input &&
        utf8_equal(text(), utf8_literal("safe")),
        "secure_edit_single_line_assignment_cannot_bypass_control_policy");
    secure_edit_release(&edit);
    expect(secure_edit_initialize(&edit, 64, false) &&
        secure_edit_assign(&edit, utf8_literal("one caf\xC3\xA9 two")) == SecureEditResult::changed &&
        secure_edit_select_word(&edit, 7) == SecureEditResult::changed && edit.anchor == 4 && edit.caret == 9 &&
        secure_edit_select_word(&edit, 8) == SecureEditResult::invalid_input && edit.anchor == 4 && edit.caret == 9,
        "secure_edit_shared_word_selection_unicode_and_boundary_validation");
    expect(secure_edit_move(&edit, SecureEditMove::home, false) == SecureEditResult::changed && edit.caret == 0 &&
        secure_edit_select_word(&edit, 0) == SecureEditResult::changed &&
        secure_edit_move(&edit, SecureEditMove::end, false) == SecureEditResult::changed && edit.caret == edit.storage.bytes,
        "secure_edit_document_navigation_goes_to_boundary_with_selection");
    secure_edit_release(&edit);
}

void test_login_mfa_policy() noexcept {
    UserAuthMfaType type = UserAuthMfaType::sms;
    expect(
        login_mfa_first_supported(login_mfa_capability_all, &type) && type == UserAuthMfaType::webauthn &&
            login_mfa_first_supported(
                static_cast<u8>(user_auth_mfa_capability_totp | user_auth_mfa_capability_sms), &type) &&
            type == UserAuthMfaType::totp &&
            login_mfa_first_supported(
                static_cast<u8>(user_auth_mfa_capability_backup | user_auth_mfa_capability_sms), &type) &&
            type == UserAuthMfaType::backup &&
            login_mfa_first_supported(user_auth_mfa_capability_sms, &type) &&
            type == UserAuthMfaType::sms && !login_mfa_first_supported(0, &type),
        "login_mfa_installed_client_method_order");
    expect(login_mfa_invalid_error(kLoginMfaInvalidCode) && !login_mfa_invalid_error(60003) &&
               !login_mfa_invalid_error(0),
           "login_mfa_invalid_error_is_exact");
    expect(login_mfa_backup_acknowledgement_seconds(500, 10'500) == 10 &&
               login_mfa_backup_acknowledgement_seconds(1'500, 10'500) == 9 &&
               login_mfa_backup_acknowledgement_seconds(10'499, 10'500) == 1 &&
               login_mfa_backup_acknowledgement_seconds(10'500, 10'500) == 0 &&
               login_mfa_backup_acknowledgement_seconds(500, 0) == 0,
           "login_mfa_backup_acknowledgement_countdown_is_bounded");

    SecureBuffer output{};
    const SecureBufferResult initialized = secure_buffer_initialize(&output, kLoginMfaNormalizedCodeBytes);
    constexpr u8 totp[] = {'1', '2', '3', '4', '5', '6'};
    constexpr u8 sms_bad[] = {'1', '2', '3', 'x', '5', '6'};
    constexpr u8 backup[] = {'A', 'B', 'C', 'D', '-', 'E', 'F', 'G', 'H'};
    const LoginMfaCodeResult totp_result =
        initialized.ok()
            ? login_mfa_prepare_code(UserAuthMfaType::totp,
                                     ConstSlice<u8>{totp, static_cast<u32>(sizeof(totp))}, &output)
            : LoginMfaCodeResult{};
    const bool totp_equal =
        totp_result.ok() &&
        bytes_equal(secure_buffer_view(output), ConstSlice<u8>{totp, static_cast<u32>(sizeof(totp))});
    const LoginMfaCodeResult backup_result = login_mfa_prepare_code(
        UserAuthMfaType::backup, ConstSlice<u8>{backup, static_cast<u32>(sizeof(backup))}, &output);
    constexpr u8 normalized_backup[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'};
    const bool backup_equal =
        backup_result.ok() && backup_result.bytes == sizeof(normalized_backup) &&
        bytes_equal(secure_buffer_view(output),
                    ConstSlice<u8>{normalized_backup, static_cast<u32>(sizeof(normalized_backup))});
    expect(initialized.ok() && output.locked && totp_equal && backup_equal,
           "login_mfa_locked_exact_normalization");

    constexpr u8 marker[] = {'k', 'e', 'e', 'p'};
    const SecureBufferResult marked =
        secure_buffer_assign(&output, ConstSlice<u8>{marker, static_cast<u32>(sizeof(marker))});
    const LoginMfaCodeResult invalid = login_mfa_prepare_code(
        UserAuthMfaType::sms, ConstSlice<u8>{sms_bad, static_cast<u32>(sizeof(sms_bad))}, &output);
    expect(marked.ok() && invalid.error == LoginMfaCodeError::invalid_character &&
               bytes_equal(secure_buffer_view(output),
                           ConstSlice<u8>{marker, static_cast<u32>(sizeof(marker))}) &&
               login_mfa_validate_code(UserAuthMfaType::totp, ConstSlice<u8>{totp, 5}).error ==
                   LoginMfaCodeError::invalid_length &&
               login_mfa_validate_code(UserAuthMfaType::webauthn, ConstSlice<u8>{totp, 6}).error ==
                   LoginMfaCodeError::invalid_type,
           "login_mfa_invalid_input_is_atomic");
    secure_buffer_release(&output);
}

void test_user_auth_protocol() noexcept {
    u8 output[2048]{};
    const UserAuthPasswordRequest password{utf8_literal("mai\"\n@example.test"), utf8_literal("p\\\tword"), false};
    const ConstSlice<u8> expected_password = byte_literal(
        R"({"login":"mai\"\n@example.test","password":"p\\\tword","undelete":false,"login_source":null,"gift_code_sku_id":null})");
    UserAuthWriteResult written =
        user_auth_write_password_request(password, Slice<u8>{output, expected_password.count});
    expect(written.ok() && written.bytes == expected_password.count &&
               bytes_equal(ConstSlice<u8>{output, written.bytes}, expected_password) &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(kUserAuthLoginTarget.data),
                                          kUserAuthLoginTarget.bytes},
                           byte_literal("/api/v9/auth/login")),
           "user_auth_password_wire_contract");

    u8 guarded[2048]{};
    guard_fill(guarded, sizeof(guarded));
    written = user_auth_write_password_request(password, Slice<u8>{guarded, expected_password.count - 1});
    bool unchanged = written.error == UserAuthWriteError::output_too_small &&
                     guard_intact(guarded, sizeof(guarded));
    expect(unchanged, "user_auth_password_capacity_is_atomic");

    char oversized_login[kUserAuthMaximumLoginBytes + 1]{};
    for (u32 index = 0; index < static_cast<u32>(sizeof(oversized_login)); ++index) {
        oversized_login[index] = 'a';
    }
    UserAuthPasswordRequest oversized = password;
    oversized.login = Utf8View{oversized_login, static_cast<u32>(sizeof(oversized_login))};
    const char invalid_utf8[] = {static_cast<char>(0xC0), static_cast<char>(0xAF)};
    UserAuthPasswordRequest invalid = password;
    invalid.password = Utf8View{invalid_utf8, static_cast<u32>(sizeof(invalid_utf8))};
    expect(
        user_auth_write_password_request(oversized, Slice<u8>{output, static_cast<u32>(sizeof(output))})
                    .error == UserAuthWriteError::value_too_large &&
            user_auth_write_password_request(invalid, Slice<u8>{output, static_cast<u32>(sizeof(output))})
                    .error == UserAuthWriteError::invalid_utf8,
        "user_auth_password_input_bounds");

    const UserAuthMfaRequest mfa{utf8_literal("012 345"), utf8_literal("ticket\"value"),
                                 utf8_literal("instance-7"),
                                 UserAuthMfaType::totp};
    const ConstSlice<u8> expected_mfa = byte_literal(
        R"({"code":"012 345","ticket":"ticket\"value","login_source":null,"gift_code_sku_id":null,"login_instance_id":"instance-7"})");
    written = user_auth_write_mfa_request(mfa, Slice<u8>{output, static_cast<u32>(sizeof(output))});
    expect(written.ok() && written.bytes == expected_mfa.count &&
               bytes_equal(ConstSlice<u8>{output, written.bytes}, expected_mfa) &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(
                                              user_auth_mfa_target(UserAuthMfaType::totp).data),
                                          user_auth_mfa_target(UserAuthMfaType::totp).bytes},
                           byte_literal("/api/v9/auth/mfa/totp")) &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(
                                              user_auth_mfa_target(UserAuthMfaType::sms).data),
                                          user_auth_mfa_target(UserAuthMfaType::sms).bytes},
                           byte_literal("/api/v9/auth/mfa/sms")) &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(
                                              user_auth_mfa_target(UserAuthMfaType::backup).data),
                                          user_auth_mfa_target(UserAuthMfaType::backup).bytes},
                           byte_literal("/api/v9/auth/mfa/backup")) &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(
                                              user_auth_mfa_target(UserAuthMfaType::webauthn).data),
                                          user_auth_mfa_target(UserAuthMfaType::webauthn).bytes},
                           byte_literal("/api/v9/auth/mfa/webauthn")) &&
               user_auth_mfa_data_maximum(UserAuthMfaType::totp) == kUserAuthMaximumCodeBytes &&
               user_auth_mfa_data_maximum(UserAuthMfaType::webauthn) ==
                   kUserAuthMaximumWebauthnCredentialBytes,
           "user_auth_mfa_wire_contract");

    char webauthn_credential[kUserAuthMaximumCodeBytes + 1]{};
    for (u32 index = 0; index < static_cast<u32>(sizeof(webauthn_credential)); ++index) {
        webauthn_credential[index] = 'A';
    }
    UserAuthMfaRequest webauthn = mfa;
    webauthn.code = Utf8View{webauthn_credential, static_cast<u32>(sizeof(webauthn_credential))};
    webauthn.type = UserAuthMfaType::webauthn;
    u8 webauthn_output[1024]{};
    const UserAuthWriteResult webauthn_written = user_auth_write_mfa_request(
        webauthn, Slice<u8>{webauthn_output, static_cast<u32>(sizeof(webauthn_output))});
    webauthn.type = UserAuthMfaType::totp;
    expect(webauthn_written.ok() && webauthn_written.bytes > sizeof(webauthn_credential) &&
               webauthn_output[0] == '{' && webauthn_output[webauthn_written.bytes - 1] == '}' &&
               user_auth_write_mfa_request(
                   webauthn, Slice<u8>{webauthn_output, static_cast<u32>(sizeof(webauthn_output))})
                       .error == UserAuthWriteError::value_too_large &&
               user_auth_mfa_data_maximum(static_cast<UserAuthMfaType>(0xFF)) == 0,
           "user_auth_webauthn_credential_bound");

    const ConstSlice<u8> expected_sms_send = byte_literal(R"({"ticket":"ticket\"value"})");
    written = user_auth_write_mfa_sms_send_request(UserAuthMfaSmsSendRequest{mfa.ticket},
                                                   Slice<u8>{output, static_cast<u32>(sizeof(output))});
    expect(written.ok() && written.bytes == expected_sms_send.count &&
               bytes_equal(ConstSlice<u8>{output, written.bytes}, expected_sms_send) &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(kUserAuthMfaSmsSendTarget.data),
                                          kUserAuthMfaSmsSendTarget.bytes},
                           byte_literal("/api/v9/auth/mfa/sms/send")),
           "user_auth_mfa_sms_send_wire_contract");

    const ConstSlice<u8> expected_captcha_headers =
        byte_literal("X-Captcha-Key: solution\r\nX-Captcha-Rqtoken: request-token\r\nX-Captcha-Session-Id: "
                     "session-7\r\n");
    written = user_auth_write_captcha_headers(UserAuthCaptchaHeaders{utf8_literal("solution"),
                                                                     utf8_literal("request-token"),
                                                                     utf8_literal("session-7")},
                                              Slice<u8>{output, static_cast<u32>(sizeof(output))});
    guard_fill(guarded, sizeof(guarded));
    const UserAuthWriteResult small_headers = user_auth_write_captcha_headers(
        UserAuthCaptchaHeaders{utf8_literal("solution"), utf8_literal("request-token"),
                               utf8_literal("session-7")},
        Slice<u8>{guarded, expected_captcha_headers.count - 1});
    unchanged = small_headers.error == UserAuthWriteError::output_too_small &&
                guard_intact(guarded, sizeof(guarded));
    expect(written.ok() && written.bytes == expected_captcha_headers.count &&
               bytes_equal(ConstSlice<u8>{output, written.bytes}, expected_captcha_headers) && unchanged &&
               user_auth_write_captcha_headers(
                   UserAuthCaptchaHeaders{utf8_literal("bad\r\nheader"), Utf8View{}, Utf8View{}},
                   Slice<u8>{output, static_cast<u32>(sizeof(output))})
                       .error == UserAuthWriteError::invalid_input,
           "user_auth_captcha_headers_atomic_and_injection_safe");

    const ConstSlice<u8> expected_auth_head =
        byte_literal("POST /api/v9/auth/login HTTP/1.1\r\n"
                     "Host: discord.com\r\n"
                     "Connection: keep-alive\r\n"
                     "Content-Length: 123\r\n"
                     "X-Captcha-Key: solution\r\n"
                     "X-Captcha-Rqtoken: request-token\r\n"
                     "X-Captcha-Session-Id: session-7\r\n"
                     "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                     "Content-Type: application/json\r\n"
                     "Accept: */*\r\n"
                     "Accept-Encoding: gzip, deflate\r\n\r\n");
    const ConstSlice<u8> expected_remote_auth_head =
        byte_literal("POST /api/v9/users/@me/remote-auth/login HTTP/1.1\r\n"
                     "Host: discord.com\r\n"
                     "Connection: keep-alive\r\n"
                     "Content-Length: 32\r\n"
                     "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                     "Content-Type: application/json\r\n"
                     "Accept: */*\r\n"
                     "Accept-Encoding: gzip, deflate\r\n\r\n");
    const ConstSlice<u8> expected_remote_auth_captcha_head =
        byte_literal("POST /api/v9/users/@me/remote-auth/login HTTP/1.1\r\n"
                     "Host: discord.com\r\n"
                     "Connection: keep-alive\r\n"
                     "Content-Length: 32\r\n"
                     "X-Captcha-Key: solution\r\n"
                     "X-Captcha-Rqtoken: request-token\r\n"
                     "X-Captcha-Session-Id: session-7\r\n"
                     "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                     "Content-Type: application/json\r\n"
                     "Accept: */*\r\n"
                     "Accept-Encoding: gzip, deflate\r\n\r\n");
    written = user_auth_write_request_head(UserAuthOperation::password, 123,
                                           UserAuthCaptchaHeaders{utf8_literal("solution"),
                                                                  utf8_literal("request-token"),
                                                                  utf8_literal("session-7")},
                                           Slice<u8>{output, static_cast<u32>(sizeof(output))});
    guard_fill(guarded, sizeof(guarded));
    const UserAuthWriteResult small_head = user_auth_write_request_head(
        UserAuthOperation::password, 123,
        UserAuthCaptchaHeaders{utf8_literal("solution"), Utf8View{}, Utf8View{}}, Slice<u8>{guarded, 32});
    unchanged = small_head.error == UserAuthWriteError::output_too_small &&
                guard_intact(guarded, sizeof(guarded));
    expect(
        written.ok() && written.bytes == expected_auth_head.count &&
            bytes_equal(ConstSlice<u8>{output, written.bytes}, expected_auth_head) && unchanged &&
            user_auth_write_request_head(UserAuthOperation::mfa_sms_send, 2, UserAuthCaptchaHeaders{},
                                         Slice<u8>{output, static_cast<u32>(sizeof(output))})
                .ok() &&
            ((written = user_auth_write_request_head(UserAuthOperation::remote_auth_ticket, 32,
                                                     UserAuthCaptchaHeaders{},
                                                     Slice<u8>{output, static_cast<u32>(sizeof(output))}))
                 .ok() &&
             written.bytes == expected_remote_auth_head.count &&
             bytes_equal(ConstSlice<u8>{output, written.bytes}, expected_remote_auth_head)) &&
            ((written = user_auth_write_request_head(UserAuthOperation::remote_auth_ticket, 32,
                                                     UserAuthCaptchaHeaders{utf8_literal("solution"),
                                                                            utf8_literal("request-token"),
                                                                            utf8_literal("session-7")},
                                                     Slice<u8>{output, static_cast<u32>(sizeof(output))}))
                 .ok() &&
             written.bytes == expected_remote_auth_captcha_head.count &&
             bytes_equal(ConstSlice<u8>{output, written.bytes}, expected_remote_auth_captcha_head)) &&
            user_auth_write_request_head(static_cast<UserAuthOperation>(0xFF), 1, UserAuthCaptchaHeaders{},
                                         Slice<u8>{output, static_cast<u32>(sizeof(output))})
                    .error == UserAuthWriteError::invalid_input,
        "user_auth_complete_request_head_contract");

    const auto header_count = [](ConstSlice<u8> head, ConstSlice<u8> field) noexcept {
        u32 count = 0;
        for (u32 index = 0; index + field.count <= head.count; ++index)
            count += maicord::bytes_equal(head.data + index, field.data, field.count) ? 1u : 0u;
        return count;
    };
    const RestUserRequestHeaders metadata{byte_literal("e30="), utf8_literal("en-GB"),
                                          utf8_literal("Europe/Ljubljana"), utf8_literal("sl,en-GB;q=0.9")};
    bool metadata_operations = true;
    for (u32 index = 0; index <= static_cast<u32>(UserAuthOperation::browser_handoff_exchange); ++index) {
        const auto operation = static_cast<UserAuthOperation>(index);
        const auto captcha = operation == UserAuthOperation::browser_handoff_exchange
            ? UserAuthCaptchaHeaders{} : UserAuthCaptchaHeaders{utf8_literal("solution"),
                utf8_literal("request-token"), utf8_literal("session-7")};
        const auto head = user_auth_write_request_head(operation, 32, captcha,
            {output, sizeof(output)}, &metadata);
        const auto target = user_auth_target(operation);
        const ConstSlice<u8> bytes{output, head.bytes};
        metadata_operations = metadata_operations && head.ok() && head.bytes >= target.bytes + 5 &&
            maicord::bytes_equal(output, "POST ", 5) &&
            maicord::bytes_equal(output + 5, target.data, target.bytes) &&
            header_count(bytes, byte_literal("\r\nX-Super-Properties: e30=\r\nAccept-Language: sl,en-GB;q=0.9\r\n"
                                             "X-Discord-Locale: en-GB\r\n")) == 1 &&
            header_count(bytes, byte_literal("\r\nX-Debug-Options: bugReporterEnabled\r\n")) == 1 &&
            header_count(bytes, byte_literal("\r\nAccept-Encoding: gzip, deflate\r\n")) == 1 &&
            header_count(bytes, byte_literal("\r\nsec-ch-ua-platform: \"Windows\"\r\n")) == 1 &&
            header_count(bytes, byte_literal("\r\nSec-Fetch-Site: same-origin\r\n")) == 1 &&
            header_count(bytes, byte_literal("\r\nX-Discord-Timezone: Europe/Ljubljana\r\n")) == 1 &&
            header_count(bytes, byte_literal("\r\nAuthorization:")) == 0 &&
            header_count(bytes, byte_literal("\r\nOrigin: https://discord.com\r\n")) == 1 &&
            header_count(bytes, byte_literal("\r\nReferer: https://discord.com/login\r\n")) == 1 &&
            header_count(bytes, byte_literal("\r\nX-Fingerprint:")) == 0 &&
            header_count(bytes, byte_literal("\r\nX-Captcha-Key: solution\r\n")) ==
                (captcha.key.bytes ? 1u : 0u) && head.bytes >= 4 &&
            maicord::bytes_equal(output + head.bytes - 4, "\r\n\r\n", 4);
    }
    expect(metadata_operations, "user_auth_all_operations_attach_metadata_and_login_page_without_authorization");

    const auto metadata_head = user_auth_write_request_head(UserAuthOperation::password, 32,
        {}, {output, sizeof(output)}, &metadata);
    bool metadata_atomic = metadata_head.ok();
    // One size preflight guards every write, so each short capacity takes the same branch:
    // sample empty, half and one-byte-short outputs.
    u8 auth_guarded[2048]{};
    const u32 short_capacities[]{0, metadata_head.bytes / 2, metadata_head.bytes - 1};
    for (const u32 capacity : short_capacities) {
        guard_fill(auth_guarded, sizeof(auth_guarded));
        metadata_atomic = metadata_atomic &&
            user_auth_write_request_head(UserAuthOperation::password, 32, {}, {auth_guarded, capacity},
                &metadata).error == UserAuthWriteError::output_too_small;
        metadata_atomic = metadata_atomic && guard_intact(auth_guarded, sizeof(auth_guarded));
    }
    const RestUserRequestHeaders invalid_metadata[]{
        {byte_literal("not-base64"), metadata.locale, metadata.timezone},
        {metadata.super_properties, utf8_literal("en-US\r\nInjected: value"), metadata.timezone},
        {metadata.super_properties, metadata.locale, utf8_literal("UTC\r\nInjected: value")},
        {metadata.super_properties, {}, metadata.timezone},
        {{nullptr, 4}, metadata.locale, metadata.timezone},
    };
    for (const auto& invalid_headers : invalid_metadata) {
        guard_fill(auth_guarded, sizeof(auth_guarded));
        metadata_atomic = metadata_atomic &&
            user_auth_write_request_head(UserAuthOperation::password, 32, {},
                {auth_guarded, sizeof(auth_guarded)}, &invalid_headers).error == UserAuthWriteError::invalid_input;
        metadata_atomic = metadata_atomic && guard_intact(auth_guarded, sizeof(auth_guarded));
    }
    expect(metadata_atomic, "user_auth_metadata_capacity_and_injection_rejection_is_atomic");
    RestUserRequestHeaders no_timezone = metadata;
    no_timezone.timezone = {};
    const auto optional_zone = user_auth_write_request_head(UserAuthOperation::recovery, 32, {},
        {output, sizeof(output)}, &no_timezone);
    expect(optional_zone.ok() &&
        header_count({output, optional_zone.bytes}, byte_literal("X-Discord-Timezone:")) == 0 &&
        header_count({output, optional_zone.bytes}, byte_literal("X-Super-Properties:")) == 1,
        "user_auth_metadata_omits_unavailable_timezone");

    // The login page's own requests: the experiments GET that mints the
    // fingerprint (no fingerprint yet, the page named in its context), then
    // every later signed-out request with X-Fingerprint and the page's cookies.
    const auto cookie = byte_literal("__dcfduid=a; _cfuvid=b");
    const auto experiments = user_auth_write_request_head(UserAuthOperation::experiments, 0, {},
        {output, sizeof(output)}, &metadata, cookie);
    expect(experiments.ok() &&
               bytes_equal(ConstSlice<u8>{output, experiments.bytes},
                   byte_literal("GET /api/v9/experiments?with_guild_experiments=true HTTP/1.1\r\n"
                                "Host: discord.com\r\n"
                                "Connection: keep-alive\r\n"
                                "sec-ch-ua: \"Not?A_Brand\";v=\"24\", \"Chromium\";v=\"152\"\r\n"
                                "sec-ch-ua-mobile: ?0\r\n"
                                "sec-ch-ua-platform: \"Windows\"\r\n"
                                "X-Super-Properties: e30=\r\n"
                                "Accept-Language: sl,en-GB;q=0.9\r\n"
                                "X-Discord-Locale: en-GB\r\n"
                                "X-Discord-Timezone: Europe/Ljubljana\r\n"
                                "X-Debug-Options: bugReporterEnabled\r\n"
                                "X-Context-Properties: eyJsb2NhdGlvbiI6IkxvZ2luIn0=\r\n"
                                "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                                "Accept: */*\r\n"
                                "Sec-Fetch-Site: same-origin\r\n"
                                "Sec-Fetch-Mode: cors\r\n"
                                "Sec-Fetch-Dest: empty\r\n"
                                "Referer: https://discord.com/login\r\n"
                                "Accept-Encoding: gzip, deflate\r\n"
                                "Cookie: __dcfduid=a; _cfuvid=b\r\n\r\n")),
           "user_auth_experiments_request_head_wire");
    RestUserRequestHeaders fingerprinted = metadata;
    fingerprinted.fingerprint = utf8_literal("1436551184213917696.Jz4qD1N8fRk0aSoW3ZpVr0v7hOM");
    const auto location = user_auth_write_request_head(UserAuthOperation::location_metadata, 0, {},
        {output, sizeof(output)}, &fingerprinted, cookie);
    const ConstSlice<u8> location_bytes{output, location.bytes};
    expect(location.ok() && maicord::bytes_equal(output, "GET /api/v9/auth/location-metadata HTTP/1.1\r\n", 45) &&
               header_count(location_bytes, byte_literal("\r\nX-Super-Properties: e30=\r\nX-Fingerprint: "
                                                         "1436551184213917696.Jz4qD1N8fRk0aSoW3ZpVr0v7hOM\r\n"
                                                         "Accept-Language: ")) == 1 &&
               header_count(location_bytes, byte_literal("Content-")) == 0 &&
               header_count(location_bytes, byte_literal("\r\nOrigin:")) == 0 &&
               header_count(location_bytes, byte_literal("X-Context-Properties")) == 0 &&
               header_count(location_bytes, byte_literal("\r\nCookie: __dcfduid=a; _cfuvid=b\r\n\r\n")) == 1,
           "user_auth_location_metadata_carries_fingerprint_and_cookies");
    const auto fingerprinted_login = user_auth_write_request_head(UserAuthOperation::password, 32,
        UserAuthCaptchaHeaders{utf8_literal("solution"), {}, {}}, {output, sizeof(output)}, &fingerprinted, cookie);
    const ConstSlice<u8> login_bytes{output, fingerprinted_login.bytes};
    expect(fingerprinted_login.ok() &&
               header_count(login_bytes, byte_literal("\r\nX-Fingerprint: 1436551184213917696.")) == 1 &&
               header_count(login_bytes, byte_literal("\r\nX-Captcha-Key: solution\r\n")) == 1 &&
               header_count(login_bytes, byte_literal("\r\nCookie: __dcfduid=a; _cfuvid=b\r\n\r\n")) == 1 &&
               !user_auth_write_request_head(UserAuthOperation::experiments, 0,
                    UserAuthCaptchaHeaders{utf8_literal("solution"), {}, {}}, {output, sizeof(output)}, &metadata)
                    .ok() &&
               !user_auth_write_request_head(UserAuthOperation::experiments, 2, {}, {output, sizeof(output)},
                    &metadata).ok() &&
               !user_auth_write_request_head(UserAuthOperation::password, 0, {}, {output, sizeof(output)},
                    &metadata).ok(),
           "user_auth_login_carries_fingerprint_captcha_headers_and_cookies");
    char fingerprint[kRestUserFingerprintMaximumBytes]{};
    u32 fingerprint_scratch[64]{};
    const auto decode_fingerprint = [&](const char* body) noexcept {
        return user_auth_decode_fingerprint({body, c_string_length(body)}, {fingerprint_scratch, 64},
                                            {fingerprint, sizeof(fingerprint)});
    };
    const u32 fingerprint_bytes = decode_fingerprint(
        R"({"fingerprint":"1436551184213917696.Jz4qD1N8fRk0aSoW3ZpVr0v7hOM","assignments":[[1,2,0,-1,0]],"guild_experiments":[]})");
    expect(fingerprint_bytes == 47 && maicord::bytes_equal(fingerprint, "1436551184213917696.Jz4qD1N8fRk0aSoW3ZpVr0v7hOM", 47) &&
               decode_fingerprint(R"({"assignments":[]})") == 0 &&
               decode_fingerprint(R"({"fingerprint":"a\u0041"})") == 0 &&
               decode_fingerprint(R"({"fingerprint":12})") == 0 &&
               decode_fingerprint(R"({"fingerprint":"a b"})") == 0 &&
               decode_fingerprint(R"(["fingerprint"])") == 0,
           "user_auth_experiments_fingerprint_decode");

    u32 structurals[256]{};
    char token[128]{};
    char ticket[128]{};
    char instance[64]{};
    char webauthn_challenge[256]{};
    char captcha_service[32]{};
    char captcha_sitekey[64]{};
    char captcha_rqdata[64]{};
    char captcha_rqtoken[64]{};
    char captcha_session[64]{};
    char captcha_flow[32]{};
    char suspended[128]{};
    const UserAuthResponseStorage storage{
        Slice<char>{token, static_cast<u32>(sizeof(token))},
        Slice<char>{ticket, static_cast<u32>(sizeof(ticket))},
        Slice<char>{instance, static_cast<u32>(sizeof(instance))},
        Slice<char>{webauthn_challenge, static_cast<u32>(sizeof(webauthn_challenge))},
        Slice<char>{captcha_service, static_cast<u32>(sizeof(captcha_service))},
        Slice<char>{captcha_sitekey, static_cast<u32>(sizeof(captcha_sitekey))},
        Slice<char>{captcha_rqdata, static_cast<u32>(sizeof(captcha_rqdata))},
        Slice<char>{captcha_rqtoken, static_cast<u32>(sizeof(captcha_rqtoken))},
        Slice<char>{captcha_session, static_cast<u32>(sizeof(captcha_session))},
        Slice<char>{captcha_flow, static_cast<u32>(sizeof(captcha_flow))},
        Slice<char>{suspended, static_cast<u32>(sizeof(suspended))}};

    UserAuthResponse response{};
    const Utf8View success =
        utf8_literal(R"({"token":"user.secret","user_id":"123456789","required_actions":[]})");
    UserAuthDecodeResult decoded = user_auth_decode_response(
        200, success,
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        storage, &response);
    expect(decoded.ok() && response.kind == UserAuthResponseKind::authenticated && response.has_user_id &&
               response.user_id == 123456789ull &&
               response.token_bytes == byte_literal("user.secret").count &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(token), response.token_bytes},
                           byte_literal("user.secret")),
           "user_auth_decode_authenticated");

    const Utf8View mfa_response = utf8_literal(
        R"({"mfa":true,"sms":true,"webauthn":"{\"publicKey\":{\"challenge\":\"AQIDBA\",\"timeout\":60000,\"rpId\":\"discord.com\",\"allowCredentials\":[],\"userVerification\":\"preferred\"}}","ticket":"mfa-ticket","backup":true,"user_id":"42","totp":true,"login_instance_id":"login-instance"})");
    decoded = user_auth_decode_response(
        200, mfa_response,
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        storage, &response);
    constexpr u8 all_capabilities = user_auth_mfa_capability_totp | user_auth_mfa_capability_sms |
                                    user_auth_mfa_capability_backup | user_auth_mfa_capability_webauthn;
    expect(
        decoded.ok() && response.kind == UserAuthResponseKind::mfa_required &&
            response.mfa_capabilities == all_capabilities &&
            response.ticket_bytes == byte_literal("mfa-ticket").count &&
            response.login_instance_id_bytes == byte_literal("login-instance").count &&
            response.webauthn_challenge_bytes ==
                byte_literal(
                    R"({"publicKey":{"challenge":"AQIDBA","timeout":60000,"rpId":"discord.com","allowCredentials":[],"userVerification":"preferred"}})")
                    .count &&
            bytes_equal(
                ConstSlice<u8>{reinterpret_cast<const u8*>(webauthn_challenge),
                               response.webauthn_challenge_bytes},
                byte_literal(
                    R"({"publicKey":{"challenge":"AQIDBA","timeout":60000,"rpId":"discord.com","allowCredentials":[],"userVerification":"preferred"}})")) &&
            bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(ticket), response.ticket_bytes},
                        byte_literal("mfa-ticket")),
        "user_auth_decode_mfa_capabilities");

    const Utf8View nullable_mfa = utf8_literal(
        R"({"mfa":true,"ticket":"nullable-ticket","sms":null,"backup":null,"totp":null,"webauthn":null,"login_instance_id":null})");
    decoded = user_auth_decode_response(
        200, nullable_mfa,
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        storage, &response);
    expect(decoded.ok() && response.kind == UserAuthResponseKind::mfa_required &&
               response.mfa_capabilities == user_auth_mfa_capability_none &&
               response.webauthn_challenge_bytes == 0 && response.login_instance_id_bytes == 0,
           "user_auth_decode_nullable_optional_capabilities");

    const Utf8View captcha = utf8_literal(
        R"({"captcha_key":["captcha-required"],"captcha_service":"hcaptcha","captcha_sitekey":"site-key","captcha_rqdata":"rq\ndata","captcha_rqtoken":"rq-token","captcha_session_id":"session-id","should_serve_invisible":true,"user_flow":"login"})");
    decoded = user_auth_decode_response(
        400, captcha,
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        storage, &response);
    expect(decoded.ok() && response.kind == UserAuthResponseKind::captcha_required &&
               response.captcha_invisible &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(captcha_rqdata),
                                          response.captcha_rqdata_bytes},
                           byte_literal("rq\ndata")) &&
               bytes_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(captcha_sitekey),
                                          response.captcha_sitekey_bytes},
                           byte_literal("site-key")),
           "user_auth_decode_captcha_contract");

    constexpr auto standard_sitekey = utf8_literal("6Lef5iQTAAAAAKeIvIY-DeexoO3gj7ryl9rLMEnn");
    constexpr auto enterprise_sitekey = utf8_literal("6LeYqFcqAAAAAD6iZesmNgVulsO4PkpBdr6NVG6M");
    const struct {
        Utf8View body, sitekey;
        UserAuthResponseKind kind = UserAuthResponseKind::captcha_required;
    } sitekeys[]{
        {utf8_literal(
             R"({"captcha_key":["required"],"captcha_service":"ReCAPTCHA","captcha_sitekey":null})"),
         standard_sitekey},
        {utf8_literal(
             R"({"captcha_key":["required"],"captcha_service":"recaptcha_enterprise","captcha_sitekey":null})"),
         enterprise_sitekey},
        {utf8_literal(
             R"({"captcha_key":["required"],"captcha_service":"recaptcha_enterprise","captcha_sitekey":""})"),
         {}},
        {utf8_literal(
             R"({"captcha_key":["required"],"captcha_service":"recaptcha_enterprise","captcha_sitekey":"custom-enterprise-key"})"),
         utf8_literal("custom-enterprise-key")},
        {utf8_literal(
             R"({"captcha_key":["required"],"captcha_service":"recaptcha_enterprise","captcha_sitekey":"bad\"key"})"),
         utf8_literal("bad\"key")},
        {utf8_literal(
             R"({"captcha_key":["required"],"captcha_service":"recaptcha\u002denterprise","captcha_sitekey":null})"),
         enterprise_sitekey},
        {utf8_literal(R"({"code":50001,"captcha_service":"recaptcha_enterprise","captcha_sitekey":null})"),
         {},
         UserAuthResponseKind::rejected}};
    for (const auto& item : sitekeys) {
        decoded = user_auth_decode_response(400, item.body, {structurals, 256}, 16, storage, &response);
        expect(
            decoded.ok() && response.kind == item.kind &&
                response.captcha_sitekey_bytes == item.sitekey.bytes &&
                (!item.sitekey.bytes ||
                 maicord::bytes_equal(captcha_sitekey, item.sitekey.data, item.sitekey.bytes)),
            "user_auth_captcha_defaults_only_nullable_known_provider_keys_and_preserves_explicit_values");
    }
    const Utf8View invalid_sitekeys[]{
        utf8_literal(
            R"({"captcha_key":["required"],"captcha_service":"recaptcha_enterprise","captcha_sitekey":17})"),
        utf8_literal(
            R"({"captcha_key":["required"],"captcha_service":"recaptcha_enterprise","captcha_sitekey":null,"captcha_sitekey":"custom"})")};
    for (u32 i = 0; i < 2; ++i) {
        decoded =
            user_auth_decode_response(400, invalid_sitekeys[i], {structurals, 256}, 16, storage, &response);
        expect(decoded.error ==
                       (i ? UserAuthDecodeError::duplicate_field : UserAuthDecodeError::invalid_field) &&
                   response.kind == UserAuthResponseKind::none && !response.captcha_sitekey_bytes,
               "user_auth_captcha_default_does_not_hide_wrong_types_or_duplicate_keys");
    }
    {
        char fallback_output[128]{};
        UserAuthResponseStorage bounded{};
        bounded.captcha_service = {fallback_output, 24};
        bounded.captcha_sitekey = {fallback_output + 24, enterprise_sitekey.bytes};
        bounded.captcha_rqdata = {fallback_output + 64, 16};
        bounded.captcha_rqtoken = {fallback_output + 80, 16};
        bounded.captcha_session_id = {fallback_output + 96, 16};
        bounded.captcha_user_flow = {fallback_output + 112, 16};
        constexpr auto complete_challenge = utf8_literal(
            R"({"captcha_key":["required"],"captcha_service":"recaptcha_enterprise","captcha_sitekey":null,"captcha_rqdata":"data","captcha_rqtoken":"rqtoken","captcha_session_id":"session","user_flow":"login"})");
        for (u32 i = 0; i < 3; ++i) {
            memset(fallback_output, 'Q', sizeof(fallback_output));
            auto short_storage = bounded;
            if (i == 0)
                --short_storage.captcha_sitekey.count;
            else if (i == 1)
                short_storage.captcha_sitekey.data = nullptr;
            else
                short_storage.captcha_rqdata.count = 1;
            response.kind = UserAuthResponseKind::rejected;
            response.captcha_service_bytes = 19;
            response.captcha_sitekey_bytes = 17;
            response.captcha_rqtoken_bytes = 23;
            decoded = user_auth_decode_response(400, complete_challenge, {structurals, 256}, 16,
                                                short_storage, &response);
            bool fallback_untouched = true;
            for (const char value : fallback_output)
                fallback_untouched = fallback_untouched && value == 'Q';
            expect(decoded.error == UserAuthDecodeError::output_too_small && fallback_untouched &&
                       response.kind == UserAuthResponseKind::none && !response.captcha_service_bytes &&
                       !response.captcha_sitekey_bytes && !response.captcha_rqtoken_bytes,
                   "user_auth_captcha_default_and_later_field_capacity_failures_are_atomic");
        }
    }

    UserAuthResponse classified{};
    decoded = user_auth_classify_response(
        400, captcha,
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        &classified);
    expect(decoded.ok() && classified.kind == UserAuthResponseKind::captcha_required &&
               classified.captcha_invisible && classified.token_bytes == 0 &&
               classified.captcha_rqdata_bytes == 0 && classified.captcha_rqtoken_bytes == 0,
           "user_auth_classify_captcha_without_secret_copy");

    const Utf8View rejected =
        utf8_literal(R"({"code":20013,"message":"disabled","suspended_user_token":"suspended-secret"})");
    decoded = user_auth_decode_response(
        403, rejected,
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        storage, &response);
    expect(decoded.ok() && response.kind == UserAuthResponseKind::rejected && response.has_error_code &&
               response.error_code == 20013 && response.suspended &&
               response.suspended_user_token_bytes == byte_literal("suspended-secret").count,
           "user_auth_decode_rejected_metadata");
    decoded = user_auth_classify_response(
        403, rejected,
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        &classified);
    expect(decoded.ok() && classified.kind == UserAuthResponseKind::rejected && classified.has_error_code &&
               classified.error_code == 20013 && classified.suspended &&
               classified.suspended_user_token_bytes == 0,
           "user_auth_classify_rejected_metadata_without_secret_copy");

    char tiny_token[4] = {'Q', 'Q', 'Q', 'Q'};
    UserAuthResponseStorage tiny_storage = storage;
    tiny_storage.token = Slice<char>{tiny_token, static_cast<u32>(sizeof(tiny_token))};
    UserAuthResponse untouched_response{};
    untouched_response.kind = UserAuthResponseKind::rejected;
    decoded = user_auth_decode_response(
        200, success,
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        tiny_storage, &untouched_response);
    expect(decoded.error == UserAuthDecodeError::output_too_small && tiny_token[0] == 'Q' &&
               tiny_token[1] == 'Q' && tiny_token[2] == 'Q' && tiny_token[3] == 'Q' &&
               untouched_response.kind == UserAuthResponseKind::none,
           "user_auth_decode_capacity_is_atomic");

    expect(user_auth_decode_response(
               200, utf8_literal(R"({"token":"a","token":"b"})"),
               Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
               storage, &response)
                       .error == UserAuthDecodeError::duplicate_field &&
               user_auth_decode_response(
                   200, utf8_literal(R"({"mfa":true})"),
                   Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))},
                   16, storage, &response)
                       .error == UserAuthDecodeError::missing_required_field &&
               user_auth_decode_response(
                   200, utf8_literal(R"({"mfa":"yes"})"),
                   Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))},
                   16, storage, &response)
                       .error == UserAuthDecodeError::invalid_field &&
               user_auth_decode_response(
                   200, utf8_literal(R"({"unknown":{"nested":{}}})"),
                   Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))},
                   1, storage, &response)
                       .error == UserAuthDecodeError::invalid_json,
           "user_auth_decode_negative_contracts");

    UserAuthState state{};
    user_auth_state_initialize(&state);
    UserAuthTransitionResult transition = user_auth_state_begin_password(&state);
    const u64 password_generation = transition.generation;
    UserAuthResponse captcha_result{};
    captcha_result.kind = UserAuthResponseKind::captcha_required;
    UserAuthResponse mfa_result{};
    mfa_result.kind = UserAuthResponseKind::mfa_required;
    UserAuthResponse authenticated_result{};
    authenticated_result.kind = UserAuthResponseKind::authenticated;
    expect(transition.ok() && state.phase == UserAuthPhase::password_pending &&
               user_auth_state_apply(&state, password_generation, captcha_result).ok() &&
               state.phase == UserAuthPhase::captcha_required,
           "user_auth_state_password_captcha");
    transition = user_auth_state_resume_captcha(&state);
    expect(transition.ok() && state.phase == UserAuthPhase::password_pending &&
               user_auth_state_apply(&state, password_generation, mfa_result).error ==
                   UserAuthTransitionError::stale_generation &&
               user_auth_state_apply(&state, transition.generation, mfa_result).ok() &&
               state.phase == UserAuthPhase::mfa_required,
           "user_auth_state_stale_generation");
    transition = user_auth_state_begin_mfa(&state, UserAuthMfaType::webauthn);
    expect(transition.ok() && state.phase == UserAuthPhase::mfa_pending &&
               state.mfa_type == UserAuthMfaType::webauthn &&
               user_auth_state_apply(&state, transition.generation, mfa_result).error ==
                   UserAuthTransitionError::invalid_response &&
               state.phase == UserAuthPhase::mfa_pending &&
               user_auth_state_apply(&state, transition.generation, captcha_result).ok() &&
               user_auth_state_resume_captcha(&state).ok() &&
               user_auth_state_apply(&state, state.generation, authenticated_result).ok() &&
               state.phase == UserAuthPhase::authenticated,
           "user_auth_state_mfa_captcha_success");
    const u64 authenticated_generation = state.generation;
    user_auth_state_cancel(&state);
    expect(state.phase == UserAuthPhase::idle && state.generation != authenticated_generation &&
               user_auth_state_begin_password(&state).ok(),
           "user_auth_state_cancel_invalidates_work");
}

void test_webauthn_protocol() noexcept {
    u8 binary[16]{};
    char encoded[32]{};
    Base64UrlResult base64 =
        base64url_decode(utf8_literal("SGVsbG8="), Slice<u8>{binary, static_cast<u32>(sizeof(binary))});
    expect(base64.ok() && base64.bytes == 5 && binary[0] == 'H' && binary[4] == 'o',
           "base64url_padded_decode");

    const u8 url_fixture[] = {0xFB, 0xFF, 0x00};
    base64 = base64url_encode(ConstSlice<u8>{url_fixture, static_cast<u32>(sizeof(url_fixture))},
                              Slice<char>{encoded, static_cast<u32>(sizeof(encoded))});
    expect(base64.ok() && base64.bytes == 4 && encoded[0] == '-' && encoded[1] == '_' &&
               encoded[2] == '8' && encoded[3] == 'A',
           "base64url_canonical_encode");

    guard_fill(binary, sizeof(binary));
    const Base64UrlResult invalid_alphabet =
        base64url_decode(utf8_literal("+/8A"), Slice<u8>{binary, static_cast<u32>(sizeof(binary))});
    const Base64UrlResult invalid_padding =
        base64url_decode(utf8_literal("SG=VsbG8"), Slice<u8>{binary, static_cast<u32>(sizeof(binary))});
    const Base64UrlResult noncanonical =
        base64url_decode(utf8_literal("AB"), Slice<u8>{binary, static_cast<u32>(sizeof(binary))});
    const Base64UrlResult short_output = base64url_decode(utf8_literal("SGVsbG8"), Slice<u8>{binary, 4});
    const bool atomic = invalid_alphabet.error == Base64UrlError::invalid_character &&
                        invalid_padding.error == Base64UrlError::invalid_padding &&
                        noncanonical.error == Base64UrlError::non_canonical &&
                        short_output.error == Base64UrlError::output_too_small && short_output.bytes == 5 &&
                        guard_intact(binary, sizeof(binary));
    expect(atomic, "base64url_rejects_noncanonical_atomically");

    constexpr Utf8View request_json = utf8_literal(
        R"({"publicKey":{"challenge":"AQIDBA","timeout":60000,"rpId":"discord.com","allowCredentials":[{"type":"public-key","id":"-_8A","transports":["usb","internal","hybrid"]}],"userVerification":"required","extensions":{"uvm":true}}})");
    u32 structurals[2048]{};
    WebAuthnMfaRequestOptions request{};
    WebAuthnMfaResult decoded = webauthn_mfa_decode_request(
        request_json,
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        &request);
    expect(decoded.ok() && request.challenge_bytes == 4 && request.challenge[0] == 1 &&
               request.challenge[3] == 4 && request.timeout_ms == 60'000 &&
               request.user_verification == WebAuthnUserVerification::required &&
               request.credential_count == 1 && request.credential_id_bytes == 3 &&
               request.credential_ids[0] == 0xFB && request.credential_ids[1] == 0xFF &&
               request.credentials[0].transports ==
                   (webauthn_transport_usb | webauthn_transport_internal | webauthn_transport_hybrid),
           "webauthn_mfa_request_decode");

    u8 client_data[kWebAuthnMfaMaximumClientDataBytes]{};
    WebAuthnMfaResult written = webauthn_mfa_write_client_data(
        request, Slice<u8>{client_data, static_cast<u32>(sizeof(client_data))});
    const ConstSlice<u8> expected_client_data = byte_literal(
        R"({"type":"webauthn.get","challenge":"AQIDBA","origin":"https://discord.com","crossOrigin":false})");
    expect(written.ok() && written.bytes == expected_client_data.count &&
               bytes_equal(ConstSlice<u8>{client_data, written.bytes}, expected_client_data),
           "webauthn_collected_client_data_contract");

    const u8 authenticator_data[] = {1, 2, 3};
    const u8 signature[] = {4, 5};
    const u8 empty_json[] = {'{', '}'};
    u8 assertion[1024]{};
    written = webauthn_mfa_write_assertion(
        WebAuthnMfaAssertionView{
            ConstSlice<u8>{url_fixture, static_cast<u32>(sizeof(url_fixture))},
            ConstSlice<u8>{empty_json, static_cast<u32>(sizeof(empty_json))},
            ConstSlice<u8>{authenticator_data, static_cast<u32>(sizeof(authenticator_data))},
            ConstSlice<u8>{signature, static_cast<u32>(sizeof(signature))}, ConstSlice<u8>{nullptr, 0},
            WebAuthnAuthenticatorAttachment::platform},
        Slice<u8>{assertion, static_cast<u32>(sizeof(assertion))});
    const ConstSlice<u8> expected_assertion = byte_literal(
        R"({"id":"-_8A","rawId":"-_8A","response":{"authenticatorData":"AQID","clientDataJSON":"e30","signature":"BAU","userHandle":null},"type":"public-key","clientExtensionResults":{},"authenticatorAttachment":"platform"})");
    expect(written.ok() && written.bytes == expected_assertion.count &&
               bytes_equal(ConstSlice<u8>{assertion, written.bytes}, expected_assertion),
           "webauthn_public_key_credential_json_contract");

    u8 short_assertion[32];
    guard_fill(short_assertion, sizeof(short_assertion));
    written = webauthn_mfa_write_assertion(
        WebAuthnMfaAssertionView{
            ConstSlice<u8>{url_fixture, static_cast<u32>(sizeof(url_fixture))},
            ConstSlice<u8>{empty_json, static_cast<u32>(sizeof(empty_json))},
            ConstSlice<u8>{authenticator_data, static_cast<u32>(sizeof(authenticator_data))},
            ConstSlice<u8>{signature, static_cast<u32>(sizeof(signature))}, ConstSlice<u8>{nullptr, 0},
            WebAuthnAuthenticatorAttachment::platform},
        Slice<u8>{short_assertion, static_cast<u32>(sizeof(short_assertion))});
    const bool assertion_atomic = written.error == WebAuthnMfaError::output_too_small &&
                                  written.bytes == expected_assertion.count &&
                                  guard_intact(short_assertion, sizeof(short_assertion));
    expect(assertion_atomic, "webauthn_assertion_capacity_is_atomic");

    WebAuthnMfaRequestOptions untouched{};
    untouched.rp_id[0] = 'X';
    decoded = webauthn_mfa_decode_request(
        utf8_literal(R"({"publicKey":{"challenge":"AQIDBA","rpId":"example.test"}})"),
        Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16,
        &untouched);
    expect(decoded.error == WebAuthnMfaError::relying_party_mismatch && untouched.rp_id[0] == 'X' &&
               windows_webauthn_authenticate(nullptr, request, nullptr).error ==
                   WindowsWebAuthnError::invalid_input,
           "webauthn_domain_binding_and_platform_guard");
}

void test_captcha_challenge_owner() noexcept {
    CaptchaChallenge challenge{};
    const CaptchaChallengeResult initialized = captcha_challenge_initialize(&challenge);
    u32 structurals[256]{};
    UserAuthResponse response{};
    const CaptchaChallengeResult decoded =
        initialized.ok()
            ? captcha_challenge_decode(
                  &challenge, 400,
                  utf8_literal(
                      R"({"captcha_key":["captcha-required"],"captcha_service":"hcaptcha","captcha_sitekey":"site-key","captcha_rqdata":"rq-data","captcha_rqtoken":"rq-token","captcha_session_id":"session-id","should_serve_invisible":true,"user_flow":"login"})"),
                  Slice<u32>{structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))},
                  16, &response)
            : CaptchaChallengeResult{initialized.native_error, initialized.error,
                                     UserAuthDecodeError::none};
    const CaptchaChallengeSnapshot snapshot = captcha_challenge_snapshot(&challenge);

    SecureBuffer service{};
    SecureBuffer rqtoken{};
    SecureBuffer session{};
    const SecureBufferResult service_initialized = secure_buffer_initialize(&service, 64);
    const SecureBufferResult token_initialized =
        secure_buffer_initialize(&rqtoken, kCaptchaChallengeRqtokenBytes);
    const SecureBufferResult session_initialized =
        secure_buffer_initialize(&session, kCaptchaChallengeSessionBytes);
    const SecureBufferResult service_copied =
        service_initialized.ok()
            ? captcha_challenge_copy_secret(&challenge, CaptchaChallengeSecretKind::service, &service)
            : service_initialized;
    const SecureBufferResult replay_copied =
        token_initialized.ok() && session_initialized.ok()
            ? captcha_challenge_copy_replay_headers(&challenge, &rqtoken, &session)
            : SecureBufferResult{0, ERROR_NOT_ENOUGH_MEMORY, SecureBufferError::allocation_failed};
    expect(initialized.ok() && decoded.ok() && response.kind == UserAuthResponseKind::captcha_required &&
               snapshot.active && snapshot.provider == CaptchaProvider::hcaptcha && snapshot.invisible &&
               service_copied.ok() && bytes_equal(secure_buffer_view(service), byte_literal("hcaptcha")) &&
               replay_copied.ok() && bytes_equal(secure_buffer_view(rqtoken), byte_literal("rq-token")) &&
               bytes_equal(secure_buffer_view(session), byte_literal("session-id")),
           "captcha_challenge_locked_owner_and_replay_snapshot");

    // Both outputs are caller-owned projections, so reject aliases before
    // either secure_buffer_assign can erase a replay value or its source.
    const SecureBufferResult same_output =
        captcha_challenge_copy_replay_headers(&challenge, &rqtoken, &rqtoken);
    SecureBuffer overlapping_session{rqtoken.data ? rqtoken.data + 1 : nullptr,
                                     rqtoken.capacity != 0 ? rqtoken.capacity - 1 : 0, 0, true};
    const SecureBufferResult overlapping_output =
        captcha_challenge_copy_replay_headers(&challenge, &rqtoken, &overlapping_session);
    SecureBuffer challenge_projection{challenge.secrets.data, challenge.secrets.capacity, 0, true};
    const SecureBufferResult owned_output =
        captcha_challenge_copy_replay_headers(&challenge, &rqtoken, &challenge_projection);
    const SecureBufferResult owned_secret =
        captcha_challenge_copy_secret(&challenge, CaptchaChallengeSecretKind::service,
                                      &challenge_projection);
    expect(replay_copied.ok() && same_output.error == SecureBufferError::invalid_input &&
               overlapping_output.error == SecureBufferError::invalid_input &&
               owned_output.error == SecureBufferError::invalid_input &&
               owned_secret.error == SecureBufferError::invalid_input &&
               overlapping_session.bytes == 0 && challenge_projection.bytes == 0 &&
               bytes_equal(secure_buffer_view(rqtoken), byte_literal("rq-token")) &&
               bytes_equal(secure_buffer_view(session), byte_literal("session-id")) &&
               captcha_challenge_snapshot(&challenge).active,
           "captcha_challenge_replay_alias_rejected_without_mutation");
    SecureBuffer short_session{session.data, 1, 0, true};
    const SecureBufferResult insufficient_output =
        captcha_challenge_copy_replay_headers(&challenge, &rqtoken, &short_session);
    expect(replay_copied.ok() && insufficient_output.error == SecureBufferError::output_too_small &&
               insufficient_output.bytes == 10 && short_session.bytes == 0 &&
               bytes_equal(secure_buffer_view(rqtoken), byte_literal("rq-token")) &&
               bytes_equal(secure_buffer_view(session), byte_literal("session-id")),
           "captcha_challenge_replay_capacity_failure_is_atomic");

    char dense_members[320]{};
    u32 dense_member_bytes = 0;
    for (u32 index = 0; index < 100; ++index) {
        if (index != 0) {
            dense_members[dense_member_bytes++] = ',';
        }
        dense_members[dense_member_bytes++] = '[';
        dense_members[dense_member_bytes++] = ']';
    }
    char dense_body[507]{};
    const u32 dense_prefix_bytes = write_fixture_json(
        utf8_literal(
            R"({"captcha_key":["captcha-required"],"captcha_service":"hcaptcha","captcha_sitekey":"site-key","captcha_rqdata":"rq-data","captcha_rqtoken":"rq-token","padding":[)"),
        Utf8View{dense_members, dense_member_bytes}, utf8_literal(R"(],"filler":")"),
        Slice<char>{dense_body, static_cast<u32>(sizeof(dense_body))});
    bool dense_built = dense_prefix_bytes != 0 && dense_prefix_bytes + 2 <= sizeof(dense_body);
    for (u32 index = dense_prefix_bytes; dense_built && index + 2 < sizeof(dense_body); ++index) {
        dense_body[index] = 'x';
    }
    if (dense_built) {
        dense_body[sizeof(dense_body) - 2] = '"';
        dense_body[sizeof(dense_body) - 1] = '}';
    }
    u32 small_structurals[256]{};
    u32 ticket_structurals[kRemoteAuthTicketExchangeStructuralCount]{};
    const JsonScanResult small_scan =
        dense_built
            ? json_scan_structurals_avx1(
                  Utf8View{dense_body, static_cast<u32>(sizeof(dense_body))},
                  Slice<u32>{small_structurals,
                             static_cast<u32>(sizeof(small_structurals) / sizeof(small_structurals[0]))})
            : JsonScanResult{};
    const CaptchaChallengeResult dense_decoded =
        dense_built
            ? captcha_challenge_decode(
                  &challenge, 400, Utf8View{dense_body, static_cast<u32>(sizeof(dense_body))},
                  Slice<u32>{ticket_structurals,
                             static_cast<u32>(sizeof(ticket_structurals) / sizeof(ticket_structurals[0]))},
                  16, &response)
            : CaptchaChallengeResult{ERROR_INVALID_DATA, CaptchaChallengeError::decode,
                                     UserAuthDecodeError::invalid_json};
    const CaptchaChallengeSnapshot dense_snapshot = captcha_challenge_snapshot(&challenge);
    expect(dense_built && small_scan.error == JsonScanError::output_overflow && dense_decoded.ok() &&
               response.kind == UserAuthResponseKind::captcha_required && dense_snapshot.active &&
               dense_snapshot.provider == CaptchaProvider::hcaptcha && !dense_snapshot.invisible,
           "captcha_challenge_observed_507_byte_structural_bound");

    const Utf8View provider_bodies[]{
        utf8_literal(
            R"({"captcha_key":["required"],"captcha_service":"HCAPTCHA","captcha_sitekey":null,"should_serve_invisible":true})"),
        utf8_literal(
            R"({"captcha_key":["required"],"captcha_service":"recaptcha","should_serve_invisible":false})"),
        utf8_literal(R"({"captcha_key":["required"],"captcha_service":"ReCAPTCHA_ENTERPRISE"})"),
        utf8_literal(
            R"({"captcha_key":["required"],"captcha_service":"recaptcha-enterprise","captcha_sitekey":null,"should_serve_invisible":null})"),
        utf8_literal(
            R"({"captcha_key":["required"],"captcha_service":"Turnstile","captcha_sitekey":null,"should_serve_invisible":true})"),
        utf8_literal(
            R"({"captcha_key":["required"],"captcha_service":"future-provider","captcha_sitekey":null,"should_serve_invisible":true})"),
        utf8_literal(R"({"captcha_key":["required"]})")};
    const CaptchaProvider providers[]{CaptchaProvider::hcaptcha,
                                      CaptchaProvider::recaptcha,
                                      CaptchaProvider::recaptcha_enterprise,
                                      CaptchaProvider::recaptcha_enterprise,
                                      CaptchaProvider::turnstile,
                                      CaptchaProvider::unknown,
                                      CaptchaProvider::unknown};
    const Utf8View sitekeys[]{{},
                              utf8_literal("6Lef5iQTAAAAAKeIvIY-DeexoO3gj7ryl9rLMEnn"),
                              utf8_literal("6LeYqFcqAAAAAD6iZesmNgVulsO4PkpBdr6NVG6M"),
                              utf8_literal("6LeYqFcqAAAAAD6iZesmNgVulsO4PkpBdr6NVG6M"),
                              {},
                              {},
                              {}};
    for (u32 index = 0; index < 7; ++index) {
        const auto replaced = captcha_challenge_decode(
            &challenge, 400, provider_bodies[index],
            {structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16, &response);
        const auto current = captcha_challenge_snapshot(&challenge);
        const auto copied =
            captcha_challenge_copy_secret(&challenge, CaptchaChallengeSecretKind::sitekey, &service);
        const bool exact_key =
            sitekeys[index].bytes
                ? copied.ok() && service.bytes == sitekeys[index].bytes &&
                      maicord::bytes_equal(service.data, sitekeys[index].data, sitekeys[index].bytes)
                : copied.error == SecureBufferError::invalid_input;
        expect(replaced.ok() && current.active && current.provider == providers[index] &&
                   current.invisible == (index == 0 || index == 4 || index == 5) &&
                   current.sitekey_bytes == sitekeys[index].bytes && exact_key && !current.rqtoken_bytes &&
                   !current.session_id_bytes,
               "captcha_challenge_replacement_copies_only_known_provider_defaults_without_stale_metadata");
    }
    const auto invalid_replacement = captcha_challenge_decode(&challenge, 400,
        utf8_literal(R"({"captcha_key":["required"],"captcha_service":"hcaptcha","should_serve_invisible":true,"should_serve_invisible":false})"),
        {structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16, &response);
    const auto invalid_snapshot = captcha_challenge_snapshot(&challenge);
    expect(!invalid_replacement.ok() && !invalid_snapshot.active && !invalid_snapshot.invisible &&
               invalid_snapshot.provider == CaptchaProvider::unknown && !invalid_snapshot.service_bytes,
           "captcha_challenge_malformed_replacement_clears_provider_hint_and_secret_metadata");
    captcha_challenge_decode(&challenge, 400, provider_bodies[0],
        {structurals, static_cast<u32>(sizeof(structurals) / sizeof(structurals[0]))}, 16, &response);
    captcha_challenge_clear(&challenge);
    const CaptchaChallengeSnapshot cleared = captcha_challenge_snapshot(&challenge);
    expect(!cleared.active && !cleared.invisible && cleared.provider == CaptchaProvider::unknown && !cleared.service_bytes &&
               captcha_challenge_copy_secret(&challenge, CaptchaChallengeSecretKind::service, &service)
                       .error == SecureBufferError::invalid_input,
           "captcha_challenge_clear_removes_secrets");
    secure_buffer_release(&session);
    secure_buffer_release(&rqtoken);
    secure_buffer_release(&service);
    captcha_challenge_release(&challenge);
}

void test_user_auth_coordinator_contract() noexcept {
    UserAuthCoordinator coordinator{};
    const UserAuthCoordinatorResult started = user_auth_coordinator_start(&coordinator);
    expect(started.ok() && started.phase == UserAuthCoordinatorPhase::idle && coordinator.runtime &&
               coordinator.transport.worker.thread &&
               user_auth_coordinator_notification_handle(&coordinator),
           "user_auth_coordinator_locked_owner_start");
    if (!coordinator.runtime || !coordinator.transport.worker.thread) {
        user_auth_coordinator_stop(&coordinator, 5'000);
        return;
    }

    u8 pageable_login_bytes[]{'u', 's', 'e', 'r'};
    u8 pageable_password_bytes[]{'s', 'e', 'c', 'r', 'e', 't'};
    const SecureBuffer pageable_login{pageable_login_bytes, static_cast<u32>(sizeof(pageable_login_bytes)),
                                      static_cast<u32>(sizeof(pageable_login_bytes)), false};
    const SecureBuffer pageable_password{pageable_password_bytes,
                                         static_cast<u32>(sizeof(pageable_password_bytes)),
                                         static_cast<u32>(sizeof(pageable_password_bytes)), false};
    expect(user_auth_coordinator_submit_password(
               &coordinator, UserAuthPasswordInput{&pageable_login, &pageable_password, nullptr, false})
                   .error == UserAuthCoordinatorError::invalid_input,
           "user_auth_coordinator_rejects_pageable_credentials");

    SecureBuffer login{};
    SecureBuffer password{};
    const bool locked_inputs =
        secure_buffer_initialize(&login, 64).ok() &&
        secure_buffer_assign(
            &login, ConstSlice<u8>{pageable_login_bytes, static_cast<u32>(sizeof(pageable_login_bytes))})
            .ok() &&
        secure_buffer_initialize(&password, 64).ok() &&
        secure_buffer_assign(&password, ConstSlice<u8>{pageable_password_bytes,
                                                       static_cast<u32>(sizeof(pageable_password_bytes))})
            .ok();
    expect(locked_inputs, "user_auth_coordinator_locked_input_fixture");
    if (locked_inputs) {
        const UserAuthCoordinatorResult wrong_captcha = user_auth_coordinator_submit_password(
            &coordinator, UserAuthPasswordInput{&login, &password, &password, false});
        const UserAuthCoordinatorResult mfa_without_ticket = user_auth_coordinator_submit_mfa(
            &coordinator, UserAuthMfaInput{&password, nullptr, UserAuthMfaType::totp});
        expect(wrong_captcha.error == UserAuthCoordinatorError::state &&
                   mfa_without_ticket.error == UserAuthCoordinatorError::invalid_input &&
                   user_auth_coordinator_submit_sms_send(&coordinator).error ==
                       UserAuthCoordinatorError::state,
               "user_auth_coordinator_state_guards_without_network");
    }

    UserAuthCoordinatorEvent event{};
    AccountProfile profile{};
    const SecureBufferResult no_secret = user_auth_coordinator_copy_secret(
        &coordinator, UserAuthCoordinatorSecretKind::captcha_sitekey, &login);
    const UserAuthCoordinatorResult pumped = user_auth_coordinator_pump(&coordinator, &event);
    expect(no_secret.native_error == ERROR_NOT_FOUND &&
               no_secret.error == SecureBufferError::invalid_input && pumped.ok() &&
               pumped.phase == UserAuthCoordinatorPhase::idle &&
               event.kind == UserAuthCoordinatorEventKind::none &&
               user_auth_coordinator_take_profile(&coordinator, 1, &profile, 5'000).error ==
                   UserAuthCoordinatorError::invalid_input &&
               !profile.credential.data,
           "user_auth_coordinator_secret_free_idle_contract");

    secure_buffer_release(&password);
    secure_buffer_release(&login);
    const UserAuthCoordinatorResult stopped = user_auth_coordinator_stop(&coordinator, 5'000);
    expect(stopped.ok() && stopped.phase == UserAuthCoordinatorPhase::stopped && !coordinator.runtime &&
               !coordinator.transport.worker.thread &&
               user_auth_coordinator_notification_handle(&coordinator) == nullptr,
           "user_auth_coordinator_idle_stop_erases_owner");
}

} // namespace maicord
