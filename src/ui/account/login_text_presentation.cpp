#include "maicord/ui/account/login_text_presentation.hpp"
#include "maicord/ui/theme/foreground_color.hpp"

#include "maicord/core/utf.hpp"
#include "maicord/ui/text/style.hpp"

namespace maicord {
namespace {

Utf8View visible_text_tail(const Utf8View& text, u32 maximum_codepoints) noexcept {
    const Utf8Validation validation = utf8_validate(text);
    if (!validation.valid || validation.codepoints <= maximum_codepoints) {
        return validation.valid ? text : Utf8View{};
    }
    u32 offset = text.bytes;
    u32 remaining = maximum_codepoints;
    while (offset != 0 && remaining != 0) {
        --offset;
        while (offset != 0 && (static_cast<u8>(text.data[offset]) & 0xC0u) == 0x80u) {
            --offset;
        }
        --remaining;
    }
    return Utf8View{text.data + offset, text.bytes - offset};
}

// Every command is one of these roles: colour, style, alignment and wrapping
// come from the table, so a call names only the element, its text and the role.
enum class Ink : u8 { primary, secondary, accent, notification, submit };

struct TextRole {
    u32 flags;
    Ink ink;
    UiTextAlignment horizontal, vertical;
    TextWrapping wrapping;
};

enum class Role : u8 {
    heading,
    root,
    browser_link,
    subtitle,
    subtitle_wrapped,
    label,
    field,
    field_hint,
    link_accent,
    link_muted,
    submit,
    submit_muted,
    status,
    status_wrapped,
    status_error,
    status_wrapped_error,
    account_name,
    account_note,
    account_error,
    resend,
    resend_muted,
    method,
    qr_title,
    qr_subtitle,
    count
};

constexpr u32 kHeadingStyle = text_style_heading_1 | text_style_bold;
constexpr u32 kLabelStyle = text_style_subtext | text_style_bold;
constexpr UiTextAlignment kStart = UiTextAlignment::start;
constexpr UiTextAlignment kCenter = UiTextAlignment::center;
constexpr TextWrapping kLine = TextWrapping::no_wrap;
constexpr TextWrapping kWrap = TextWrapping::wrap;

constexpr TextRole kRoles[]{
    {kHeadingStyle, Ink::primary, kCenter, kStart, kLine},                   // heading
    {text_style_heading_1, Ink::primary, kCenter, kCenter, kLine},           // root
    {text_style_subtext, Ink::accent, UiTextAlignment::end, kCenter, kLine}, // browser_link
    {text_style_none, Ink::secondary, kCenter, kStart, kLine},               // subtitle
    {text_style_none, Ink::secondary, kCenter, kStart, kWrap},               // subtitle_wrapped
    {kLabelStyle, Ink::secondary, kStart, kStart, kLine},                    // label
    {text_style_none, Ink::primary, kStart, kCenter, kLine},                 // field
    {text_style_none, Ink::secondary, kStart, kCenter, kLine},               // field_hint
    {text_style_subtext, Ink::accent, kStart, kCenter, kLine},               // link_accent
    {text_style_subtext, Ink::secondary, kStart, kCenter, kLine},            // link_muted
    {text_style_bold, Ink::submit, kCenter, kCenter, kLine},                 // submit
    {text_style_bold, Ink::secondary, kCenter, kCenter, kLine},              // submit_muted
    {text_style_subtext, Ink::secondary, kStart, kStart, kLine},             // status
    {text_style_subtext, Ink::secondary, kStart, kStart, kWrap},             // status_wrapped
    {text_style_subtext, Ink::notification, kStart, kStart, kLine},          // status_error
    {text_style_subtext, Ink::notification, kStart, kStart, kWrap},          // status_wrapped_error
    {text_style_bold, Ink::primary, kStart, kStart, kLine},                  // account_name
    {text_style_subtext, Ink::secondary, kStart, kStart, kLine},             // account_note
    {text_style_subtext, Ink::notification, kStart, kStart, kLine},          // account_error
    {text_style_subtext, Ink::accent, kCenter, kCenter, kLine},              // resend
    {text_style_subtext, Ink::secondary, kCenter, kCenter, kLine},           // resend_muted
    {kLabelStyle, Ink::primary, kCenter, kCenter, kLine},                    // method
    {text_style_bold, Ink::primary, kCenter, kStart, kLine},                 // qr_title
    {text_style_subtext, Ink::secondary, kCenter, kStart, kLine},            // qr_subtitle
};
static_assert(sizeof(kRoles) / sizeof(kRoles[0]) == static_cast<u32>(Role::count));

// Every sentence the screen says, in one literal; a Say is its position (0: nothing). Runs that the code indexes
// arithmetically (the Continue countdown, the security key's states, the four method names) stay contiguous and in
// the order of the enumeration that indexes them.
#define LOGIN_SAY_COMMON(X)                                                                                         \
    X(browser_heading, "Sign in with your browser")                                                                 \
    X(back_to_sign_in, "Back to sign in")                                                                           \
    X(opening_browser, "Opening your browser…")                                                                     \
    X(browser_finishing, "Finishing sign-in…")                                                                      \
    X(browser_expired, "This browser sign-in expired. Choose Back to start again.")                                 \
    X(browser_failed, "The browser sign-in could not be completed. Choose Back to try again.")                      \
    X(browser_waiting, "Finish signing in your browser, then come back here.")                                      \
    X(browser_expired_action, "Sign-in expired")                                                                    \
    X(browser_failed_action, "Sign-in failed")                                                                      \
    X(browser_waiting_action, "Waiting for your browser")                                                           \
    X(no_retry_time, "No retry time was provided. Resume when ready.")                                              \
    X(too_many_attempts, "Too many attempts. Wait before retrying.")                                                \
    X(resume_sign_in, "Resume sign-in")                                                                             \
    X(please_wait, "Please wait")                                                                                   \
    X(captcha_opening, "Opening browser…")                                                                          \
    X(captcha_again, "Open the check again")                                                                        \
    X(captcha_retry, "Try again in browser")                                                                        \
    X(captcha_verify, "Verify in browser")                                                                          \
    X(captcha_finish, "Finish the check in your browser, then come back.")                                          \
    X(captcha_not_passed, "That check didn't go through. Try again.")                                               \
    X(captcha_expired, "The browser check expired. Try again.")                                                     \
    X(captcha_not_opened, "Couldn't open the browser check. Try again.")                                            \
    X(captcha_quick, "Complete a quick check in your browser to continue.")                                         \
    X(continue_0, "Continue")                                                                                       \
    X(continue_1, "Continue in 1")                                                                                  \
    X(continue_2, "Continue in 2")                                                                                  \
    X(continue_3, "Continue in 3")                                                                                  \
    X(continue_4, "Continue in 4")                                                                                  \
    X(continue_5, "Continue in 5")                                                                                  \
    X(continue_6, "Continue in 6")                                                                                  \
    X(continue_7, "Continue in 7")                                                                                  \
    X(continue_8, "Continue in 8")                                                                                  \
    X(continue_9, "Continue in 9")                                                                                  \
    X(continue_10, "Continue in 10")                                                                                \
    X(session_expired, "Your session expired. Log in again.")                                                       \
    X(connect_failed, "Couldn't connect. Try again.")                                                               \
    X(cancel_failed, "Couldn't cancel sign-in. Try again.")                                                         \
    X(sending_instructions, "Sending instructions…")                                                                \
    X(check_instructions, "Check your email or phone for instructions.")                                            \
    X(phone_unsupported, "Phone verification isn't supported yet. Use a browser.")                                  \
    X(instructions_failed, "Couldn't send instructions. Check your details and try again.")                         \
    X(recovery_limited, "Too many recovery requests. Wait before trying again.")                                    \
    X(new_password_hint, "Enter a new password with at least 8 characters.")                                        \
    X(resetting_password, "Resetting password…")                                                                    \
    X(password_refused, "That password can't be used. Try a different one.")                                        \
    X(too_many_wait, "Too many attempts. Wait a moment and try again.")                                             \
    X(reset_failed, "Couldn't reset your password. Go back and paste your reset link again.")                       \
    X(verifying, "Verifying…")                                                                                      \
    X(sending_new_code, "Sending a new code…")                                                                      \
    X(enter_six, "Enter exactly six digits.")                                                                       \
    X(code_not_accepted, "That code didn't work. Try again or request a new one.")                                  \
    X(new_code_sent, "New code sent.")                                                                              \
    X(new_code_failed, "Couldn't send a new code. Try again.")                                                      \
    X(too_many_requests, "Too many requests. Wait a moment and try again.")                                         \
    X(request_failed, "Couldn't confirm the request. Go back and try again.")                                       \
    X(preparing_reset, "Preparing password reset…")                                                                 \
    X(enter_code, "Enter the 6-digit code.")                                                                        \
    X(qr_generating_status, "Generating QR code…")                                                                  \
    X(qr_scan_status, "Scan the QR code with Discord on your phone.")                                               \
    X(qr_confirm_status, "Confirm on your phone to log in as:")                                                     \
    X(qr_refreshing_status, "Refreshing QR code…")                                                                  \
    X(qr_reconnecting, "Reconnecting…")                                                                             \
    X(qr_cancelled, "Login cancelled on your phone.")                                                               \
    X(phone_heading, "Phone recovery")                                                                              \
    X(phone_subtitle, "Enter the six-digit SMS code to reset your password.")                                       \
    X(sms_code_label, "SMS CODE")                                                                                   \
    X(resend_code, "Resend code")                                                                                   \
    X(verify, "Verify")                                                                                             \
    X(switching_accounts, "Switching accounts…")                                                                    \
    X(connecting, "Connecting to Discord…")                                                                         \
    X(account_or_phone, "Account email or phone")                                                                   \
    X(email_phone_or_link, "Email, phone number or reset link")                                                     \
    X(sms_code, "SMS code")                                                                                         \
    X(backup_code, "Backup code")                                                                                   \
    X(authentication_code, "Authentication code")                                                                   \
    X(new_password, "New password")                                                                                 \
    X(password, "Password")                                                                                         \
    X(signing_in, "Signing in…")                                                                                    \
    X(reset_paused, "Reset paused")                                                                                 \
    X(sending_request, "Sending request…")                                                                          \
    X(request_sent, "Request sent")                                                                                 \
    X(recovery_unavailable, "Recovery unavailable")                                                                 \
    X(send_instructions, "Send instructions")                                                                       \
    X(verification_required, "Verification required")                                                               \
    X(sending_code, "Sending code…")                                                                                \
    X(try_again, "Try again")                                                                                       \
    X(continue_with_key, "Continue with security key")                                                              \
    X(reset_password, "Reset password")                                                                             \
    X(log_in, "Log in")                                                                                             \
    X(logged_out, "You've been logged out.")                                                                        \
    X(logged_out_here, "Logged out of this device.")                                                                \
    X(suspended, "Discord has suspended this account. See its standing at discord.com.")                            \
    X(login_failed, "Login failed. Check your details and try again.")                                              \
    X(code_sent, "Code sent.")                                                                                      \
    X(code_not_sent, "Couldn't send the code. Try again.")                                                          \
    X(choose_method, "Choose a verification method.")                                                               \
    X(backup_invalid, "Enter a valid backup code (8-11 characters).")                                               \
    X(totp_invalid, "Enter a 6-digit authentication code.")                                                         \
    X(code_rejected, "That code didn't work. Try again.")                                                           \
    X(backup_once, "Backup codes can only be used once.")                                                           \
    X(save_failed, "Couldn't save your sign-in.")                                                                   \
    X(use_browser_sign_in, "Use browser sign-in")                                                                   \
    X(use_browser, "Use browser")                                                                                   \
    X(reset_heading, "Reset your password")                                                                         \
    X(recovery_heading, "Password recovery")                                                                        \
    X(add_heading, "Add an account")                                                                                \
    X(login_heading, "Log in to MaiCord")                                                                           \
    X(reset_subtitle, "Choose a new password.")                                                                     \
    X(recovery_subtitle, "Get instructions to reset your password.")                                                \
    X(add_subtitle, "Press Esc to go back.")                                                                        \
    X(return_subtitle, "Press Esc to use a saved account.")                                                         \
    X(default_subtitle, "Use your Discord account.")                                                                \
    X(label_recovery, "ACCOUNT EMAIL / PHONE")                                                                      \
    X(label_login, "EMAIL, PHONE OR RESET LINK")                                                                    \
    X(label_passkey, "PASSKEY OR SECURITY KEY")                                                                     \
    X(label_code, "AUTHENTICATION CODE")                                                                            \
    X(label_new_password, "NEW PASSWORD")                                                                           \
    X(label_password, "PASSWORD")                                                                                   \
    X(forgot_password, "Forgot your password?")                                                                     \
    X(logged_out_only_here, "Logged out on this device only.")                                                      \
    X(caps_lock, "Caps Lock is on.")                                                                                \
    X(try_again_or_back, "Try again, or go back to sign in.")                                                       \
    X(try_again_or_close, "Try again, or close MaiCord to sign out.")                                               \
    X(method_key, "KEY")                                                                                            \
    X(method_totp, "TOTP")                                                                                          \
    X(method_backup, "BACKUP")                                                                                      \
    X(method_sms, "SMS")                                                                                            \
    X(qr_title, "Log in with QR code")                                                                              \
    X(qr_generating, "Generating QR code")                                                                          \
    X(qr_refreshing, "Refreshing automatically")                                                                    \
    X(qr_confirm, "Confirm the login on your phone")                                                                \
    X(qr_scan, "Scan from the Discord mobile app")
#if defined(_WIN32)
#define LOGIN_SAY_OS(X)                                                                                             \
    X(key_field, "Windows Hello or security key")                                                                   \
    X(key_prompt, "Complete verification in the Windows security prompt.")                                          \
    X(key_prompt_failed, "Security-key verification failed or was cancelled. Try again.")
#else
// What the security key of the sign-in says on Linux, where the app prompts for the touch and the PIN and names
// what went wrong: key_touch... in the order of LoginSecurityKey from `touch`.
#define LOGIN_SAY_OS(X)                                                                                             \
    X(key_field, "Security key")                                                                                    \
    X(key_prompt, "Complete verification with your security key.")                                                  \
    X(key_prompt_failed, "Security-key verification failed. Try again.")                                            \
    X(key_pin_field, "Security key PIN")                                                                            \
    X(key_pin_label, "SECURITY KEY PIN")                                                                            \
    X(key_touch, "Touch your security key.")                                                                        \
    X(key_pin, "Enter your security key's PIN.")                                                                    \
    X(key_pin_wrong, "That PIN didn't work. Try again.")                                                            \
    X(key_missing, "No security key found. Plug it in and try again.")                                              \
    X(key_cannot_open, "Can't use the security key: no access to it. Check the udev rules for FIDO keys.")          \
    X(key_not_registered, "This security key isn't registered for your account.")                                   \
    X(key_pin_blocked, "The security key's PIN is blocked. Use another method.")                                    \
    X(key_pin_not_set, "This verification needs a PIN, and the security key has none. Use another method.")        \
    X(key_timed_out, "The security key wasn't touched in time. Try again.")                                         \
    X(key_unavailable, "Security keys need libfido2, which isn't installed. Use another method.")                   \
    X(key_failed, "Security-key verification failed. Try again.")
#endif

enum class Say : u8 {
    none,
#define LOGIN_SAY_ENUM(name, text) name,
    LOGIN_SAY_COMMON(LOGIN_SAY_ENUM) LOGIN_SAY_OS(LOGIN_SAY_ENUM)
#undef LOGIN_SAY_ENUM
    count,
    // Text of the view itself: the identifier, the secret's mask and the remote account.
    login_text = count,
    password_text,
    remote_user
};
constexpr char kSay[] =
#define LOGIN_SAY_TEXT(name, text) text "\0"
    LOGIN_SAY_COMMON(LOGIN_SAY_TEXT) LOGIN_SAY_OS(LOGIN_SAY_TEXT);
#undef LOGIN_SAY_TEXT
// Where each word starts in kSay (ids from 1; the entry past the last id ends it), so a word is found without a walk.
struct WordStarts {
    u16 at[static_cast<u32>(Say::count) + 1];
    constexpr u16 operator[](u32 id) const noexcept {
        return at[id];
    }
};
consteval WordStarts word_starts() noexcept {
    WordStarts starts{};
    u32 id = 1;
    for (u32 i = 0; i + 1 < sizeof(kSay); ++i) {
        if (!kSay[i])
            starts.at[++id] = static_cast<u16>(i + 1);
    }
    if (id != static_cast<u32>(Say::count)) // not a constant expression: kSay holds another count of words
        starts.at[static_cast<u32>(Say::count) + 1] = 0;
    return starts;
}
constexpr WordStarts kWordStart = word_starts();
constexpr Say operator+(Say base, u32 offset) noexcept {
    return static_cast<Say>(static_cast<u32>(base) + offset);
}


// A set of statuses as bits.
template <typename... Statuses> consteval u64 status_set(Statuses... statuses) noexcept {
    return (... | (1ull << static_cast<u32>(statuses)));
}
using S = LoginStatus;
constexpr u64 kSignedOut = status_set(S::signed_out, S::signed_out_unconfirmed);
// A multi-factor or security-key screen: the method list shows.
constexpr u64 kMfaScreen = status_set(S::mfa_required, S::mfa_input_invalid, S::mfa_code_rejected,
                                      S::mfa_backup_acknowledgement, S::webauthn_required, S::webauthn_failed);
constexpr u64 kWebauthn = status_set(S::webauthn_required, S::webauthn_failed);
constexpr u64 kResetting = status_set(S::reset_pending, S::reset_mfa_pending);
// Statuses whose message is an error (before the screen-specific cases).
constexpr u64 kErrors = status_set(S::reset_rejected, S::reset_failed, S::reset_rate_limited, S::recovery_failed,
                                   S::recovery_phone_required, S::recovery_rate_limited);
constexpr u64 kCredentialErrors = status_set(S::session_expired, S::connection_failed, S::rejected);
constexpr u64 kMfaErrors = status_set(S::mfa_input_invalid, S::mfa_code_rejected, S::profile_save_failed,
                                      S::webauthn_failed);
constexpr u64 kPhoneErrors = status_set(S::recovery_phone_input_invalid, S::recovery_phone_code_rejected,
                                        S::recovery_phone_resend_failed, S::recovery_phone_verify_rate_limited,
                                        S::recovery_phone_resend_rate_limited, S::recovery_phone_failed);
bool in(const LoginView& view, u64 set) noexcept {
    return (set >> static_cast<u32>(view.status)) & 1;
}

// The CAPTCHA's line and action by LoginCaptchaStatus (none, ready, starting, waiting, retry, expired, failed).
constexpr Say kCaptchaStatus[]{Say::captcha_quick,   Say::captcha_quick,   Say::opening_browser, Say::captcha_finish,
                               Say::captcha_not_passed, Say::captcha_expired, Say::captcha_not_opened};
constexpr Say kCaptchaAction[]{Say::captcha_verify, Say::captcha_verify, Say::captcha_opening, Say::captcha_again,
                               Say::captcha_retry,  Say::captcha_retry,  Say::captcha_retry};
// Only a retry, an expiry or a failure is an error.
constexpr u32 kCaptchaErrors = 0x70;
// The statuses session_expired through reset_mfa_pending, the phone statuses recovery_phone_verify_pending through
// recovery_phone_token_ready and LoginRemoteStatus::connecting through cancelled, each with its own line (none:
// another case speaks).
constexpr Say kFormSay[]{Say::session_expired,    Say::connect_failed,       Say::cancel_failed,
                         Say::none,               Say::sending_instructions, Say::check_instructions,
                         Say::phone_unsupported,  Say::instructions_failed,  Say::recovery_limited,
                         Say::none,               Say::new_password_hint,    Say::resetting_password,
                         Say::password_refused,   Say::too_many_wait,        Say::reset_failed,
                         Say::none,               Say::resetting_password};
constexpr Say kPhoneSay[]{Say::verifying,         Say::sending_new_code, Say::enter_six,        Say::code_not_accepted,
                          Say::new_code_sent,     Say::new_code_failed,  Say::too_many_wait,   Say::too_many_requests,
                          Say::none,              Say::request_failed,   Say::preparing_reset};
constexpr Say kRemoteSay[]{Say::qr_generating_status, Say::qr_scan_status,   Say::qr_confirm_status,
                           Say::qr_refreshing_status, Say::none,             Say::qr_reconnecting,
                           Say::qr_cancelled};
// The QR column's subtitle by LoginRemoteStatus (idle, connecting, qr_ready, mobile_pending, refreshing,
// captcha_required, failed, cancelled).
constexpr Say kQrSubtitle[]{Say::qr_scan,       Say::qr_generating, Say::qr_scan, Say::qr_confirm,
                            Say::qr_refreshing, Say::verification_required, Say::qr_refreshing, Say::qr_refreshing};
template <u32 N> Say line(const Say (&table)[N], u32 at) noexcept {
    return at < N ? table[at] : Say::none;
}

Say cooldown_copy(const LoginView& view) noexcept {
    return view.auth_retry_time_unknown ? Say::no_retry_time : Say::too_many_attempts;
}
Say cooldown_action(const LoginScene& scene) noexcept {
    return scene.auth_retry_resume ? Say::resume_sign_in : Say::please_wait;
}

// Where each command goes: its element, in the order the commands are pushed. A slot a mode leaves without text
// pushes nothing.
enum Slot : u8 {
    slot_root,
    slot_browser_link,
    slot_heading,
    slot_subtitle,
    slot_login_label,
    slot_login_field,
    slot_password_label,
    slot_password_field,
    slot_forgot,
    slot_resend,
    slot_submit,
    slot_status,
    slot_account_remote,
    slot_account_logout,
    slot_account_caps,
    slot_account_save,
    slot_method,
    slot_qr_title = slot_method + kLoginMfaPresentationMethodCount,
    slot_qr_subtitle,
    slot_count
};
constexpr LoginElement kSlotElement[slot_count]{
    LoginElement::root,           LoginElement::browser_sign_in, LoginElement::heading,
    LoginElement::subtitle,       LoginElement::login_label,     LoginElement::login_field,
    LoginElement::password_label, LoginElement::password_field,  LoginElement::forgot_password,
    LoginElement::mfa_sms,        LoginElement::submit,          LoginElement::status_message,
    LoginElement::status_account, LoginElement::status_account,  LoginElement::status_account,
    LoginElement::status_account, LoginElement::mfa_webauthn,    LoginElement::mfa_totp,
    LoginElement::mfa_backup,     LoginElement::mfa_sms,         LoginElement::qr_title,
    LoginElement::qr_subtitle};
static_assert(kLoginMfaPresentationMethods[0].element == LoginElement::mfa_webauthn &&
              kLoginMfaPresentationMethods[1].element == LoginElement::mfa_totp &&
              kLoginMfaPresentationMethods[2].element == LoginElement::mfa_backup &&
              kLoginMfaPresentationMethods[3].element == LoginElement::mfa_sms);

struct Plan {
    Say say[slot_count];
    Role role[slot_count];
    Utf8View view[3];
    void set(Slot slot, Say text, Role as) noexcept {
        say[slot] = text;
        role[slot] = as;
    }
    Utf8View text(u32 slot) const noexcept {
        const u32 id = static_cast<u32>(say[slot]);
        if (id >= static_cast<u32>(Say::count))
            return view[id - static_cast<u32>(Say::count)];
        return id ? Utf8View{kSay + kWordStart[id], kWordStart[id + 1u] - kWordStart[id] - 1u} : Utf8View{};
    }
};

struct Writer {
    const LoginScene& scene;
    UiTextCommandStream* stream;
    const Color* inks[5];
};

// Pushes the commands of the slots that have text; false when an element is missing or the stream is full.
bool emit(const Writer& out, const Plan& plan) noexcept {
    const LoginScene& scene = out.scene;
    for (u32 slot = 0; slot < slot_count; ++slot) {
        const Utf8View text = plan.text(slot);
        if (text.bytes == 0) {
            continue;
        }
        const TextRole& style = kRoles[static_cast<u32>(plan.role[slot])];
        const LoginElement element = kSlotElement[slot];
        const UiLayoutNode* node = login_scene_element(scene, element);
        if (!node ||
            !ui_text_command_stream_push(
                out.stream, UiTextCommand{text, UiWidgetReference{&scene.widgets, scene.elements[static_cast<u32>(element)]},
                                          node->content_rectangle, node->clip_rectangle,
                                          *out.inks[static_cast<u32>(style.ink)], style.flags, style.horizontal,
                                          style.vertical, style.wrapping, TextOverflow::clip})) {
            return false;
        }
    }
    return true;
}

constexpr Role operator+(Role base, u32 offset) noexcept {
    return static_cast<Role>(static_cast<u32>(base) + offset);
}
// The status line's role: wrapped on the recovery and reset screens, in the error ink for an error.
Role status_role(bool wrapped, bool error) noexcept {
    return Role::status + (wrapped ? 1 : 0) + (error ? 2 : 0);
}

void plan_browser(Plan& plan, const LoginScene& scene, const LoginView& view) noexcept {
    const S status = view.status;
    const Say message = view.auth_rate_limited              ? cooldown_copy(view)
                        : status == S::browser_starting         ? Say::opening_browser
                        : status == S::browser_exchange_pending ? Say::browser_finishing
                        : status == S::browser_expired          ? Say::browser_expired
                        : status == S::browser_failed           ? Say::browser_failed
                                                                : Say::browser_waiting;
    plan.set(slot_heading, Say::browser_heading, Role::heading);
    plan.set(slot_forgot, Say::back_to_sign_in, Role::link_accent);
    plan.set(slot_submit,
             view.auth_rate_limited          ? cooldown_action(scene)
             : status == S::browser_expired ? Say::browser_expired_action
             : status == S::browser_failed  ? Say::browser_failed_action
                                            : Say::browser_waiting_action,
             Role::submit_muted);
    plan.set(slot_status, message, Role::status_wrapped);
}

void plan_phone(Plan& plan, const LoginScene& scene, const LoginView& view) noexcept {
    const bool captcha = view.status == S::recovery_phone_captcha_required;
    const u32 cs = static_cast<u32>(view.captcha_status);
    Say status = captcha ? kCaptchaStatus[cs]
                         : line(kPhoneSay, static_cast<u32>(view.status) - static_cast<u32>(S::recovery_phone_verify_pending));
    if (status == Say::none)
        status = Say::enter_code;
    if (view.auth_retry_time_unknown)
        status = Say::no_retry_time;
    const bool error = in(view, kPhoneErrors) || (captcha && ((kCaptchaErrors >> cs) & 1));
    plan.set(slot_heading, Say::phone_heading, Role::heading);
    plan.set(slot_subtitle, Say::phone_subtitle, Role::subtitle_wrapped);
    plan.set(slot_password_label, Say::sms_code_label, Role::label);
    plan.set(slot_forgot, Say::back_to_sign_in, Role::link_accent);
    plan.set(slot_resend, Say::resend_code, scene.phone_resend_disabled ? Role::resend_muted : Role::resend);
    plan.set(slot_submit, view.auth_rate_limited ? cooldown_action(scene) : captcha ? kCaptchaAction[cs] : Say::verify,
             Role::submit);
    plan.set(slot_status, status, status_role(true, error));
}

void plan_form(Plan& plan, const LoginScene& scene, const LoginView& view) noexcept {
    const S status = view.status;
    plan.view[0] = visible_text_tail(view.login, 48);
    plan.view[1] = visible_text_tail(view.password_mask, 48);
    plan.view[2] = visible_text_tail(view.remote_user, 32);
    const bool login = plan.view[0].bytes != 0, password = plan.view[1].bytes != 0;
    const bool blocked = scene.back_only;
    const bool signed_out = in(view, kSignedOut);
    const bool recovery = scene.recovery_mode;
    const bool reset = scene.reset_mode;
    const bool recovery_result = recovery && blocked;
    const u8 selected_mfa = blocked ? static_cast<u8>(login_mfa_capability_none) : view.selected_mfa_capability;
    const bool webauthn = !blocked && in(view, kWebauthn);
    const bool backup_acknowledgement = !blocked && status == S::mfa_backup_acknowledgement;
    const LoginRemoteStatus remote_status =
        view.remote_status != LoginRemoteStatus::idle ? view.remote_status
        : status >= S::remote_connecting && status <= S::remote_expired
            ? static_cast<LoginRemoteStatus>(static_cast<u32>(status) - static_cast<u32>(S::remote_connecting) + 1)
            : LoginRemoteStatus::idle;
    const bool mfa = !blocked && (view.mfa_capabilities != 0 || in(view, kMfaScreen));
    const bool captcha = login_captcha_mode(status, view.remote_status);
    const bool limited = view.auth_rate_limited;
    const u32 cs = static_cast<u32>(view.captcha_status);
#if !defined(_WIN32)
    const bool pin = view.security_key == LoginSecurityKey::pin || view.security_key == LoginSecurityKey::pin_wrong;
#endif
    const bool resetting_submit = reset && status == S::submitting;

    // The submit button's label, by the same precedence.
    const Say action =
        limited                            ? cooldown_action(scene)
        : captcha                          ? kCaptchaAction[cs]
        : in(view, kResetting)             ? Say::resetting_password
        : resetting_submit                 ? Say::signing_in
        : reset && blocked                 ? Say::reset_paused
        : recovery ? (status == S::recovery_pending        ? Say::sending_request
                      : status == S::recovery_sent         ? Say::request_sent
                      : recovery_result                    ? Say::recovery_unavailable
                      : status == S::recovery_rate_limited ? Say::please_wait
                                                           : Say::send_instructions)
        : blocked                          ? Say::verification_required
        : view.sms_sending                 ? Say::sending_code
        : status == S::submitting          ? Say::signing_in
        : backup_acknowledgement
            ? Say::continue_0 + (view.backup_acknowledgement_seconds <= 10 ? view.backup_acknowledgement_seconds : 0)
        : status == S::profile_save_failed ? Say::try_again
        : webauthn                         ? Say::continue_with_key
        : mfa                              ? Say::verify
        : reset                            ? Say::reset_password
                                           : Say::log_in;

    const Say form = line(kFormSay, static_cast<u32>(status) - static_cast<u32>(S::session_expired));
    const Say remote = line(kRemoteSay, static_cast<u32>(remote_status) - 1);
    // The status line: the first case that speaks, from the rate limit and the CAPTCHA down to the screen's own state.
    const Say text =
        view.auth_retry_time_unknown                                  ? Say::no_retry_time
        : limited && !reset && !recovery && !login_phone_mode(status) ? Say::too_many_attempts
        : captcha                                                     ? kCaptchaStatus[cs]
        : status == S::signed_out                                     ? Say::logged_out
        : status == S::signed_out_unconfirmed                         ? Say::logged_out_here
#if !defined(_WIN32)
        : view.security_key != LoginSecurityKey::none
            ? Say::key_touch + (static_cast<u32>(view.security_key) - static_cast<u32>(LoginSecurityKey::touch))
#endif
        : resetting_submit                                            ? Say::signing_in
        : form != Say::none                                           ? form
        : status == S::rejected ? (view.account_suspended ? Say::suspended : Say::login_failed)
        : remote != Say::none                                         ? remote
        : status == S::mfa_required
            ? (view.sms_sending                                                  ? Say::sending_code
               : selected_mfa == login_mfa_capability_sms && view.sms_sent       ? Say::code_sent
               : selected_mfa == login_mfa_capability_sms && view.sms_failed     ? Say::code_not_sent
                                                                                 : Say::choose_method)
        : status == S::mfa_input_invalid
            ? (selected_mfa == login_mfa_capability_backup ? Say::backup_invalid : Say::totp_invalid)
        : status == S::mfa_code_rejected                              ? Say::code_rejected
        : backup_acknowledgement                                      ? Say::backup_once
        : status == S::profile_save_failed                            ? Say::save_failed
        : status == S::webauthn_required                              ? Say::key_prompt
        : status == S::webauthn_failed                                ? Say::key_prompt_failed
                                                                      : Say::none;

    const bool credential_error = limited || in(view, kCredentialErrors);
    const bool remote_message = !signed_out && !credential_error && remote_status != LoginRemoteStatus::idle;
    const bool error =
        captcha ? ((kCaptchaErrors >> cs) & 1) != 0
                : in(view, kErrors) || credential_error ||
                      (remote_message ? remote_status == LoginRemoteStatus::failed
                                      : in(view, kMfaErrors) ||
                                            (status == S::mfa_required && !view.sms_sending && !view.sms_sent &&
                                             selected_mfa == login_mfa_capability_sms && view.sms_failed));
    const bool wrapped = recovery || reset;
    const bool mobile_pending = !signed_out && remote_status == LoginRemoteStatus::mobile_pending;

    if (scene.browser_available)
        plan.set(slot_browser_link, captcha ? Say::use_browser_sign_in : Say::use_browser, Role::browser_link);
    plan.set(slot_heading,
             reset               ? Say::reset_heading
             : recovery          ? Say::recovery_heading
             : scene.add_account ? Say::add_heading
                                 : Say::login_heading,
             Role::heading);
    plan.set(slot_subtitle,
             reset                  ? Say::reset_subtitle
             : recovery             ? Say::recovery_subtitle
             : scene.add_account    ? Say::add_subtitle
             : scene.account_return ? Say::return_subtitle
                                    : Say::default_subtitle,
             wrapped ? Role::subtitle_wrapped : Role::subtitle);
    if (!reset && !recovery_result) {
        plan.set(slot_login_label, recovery ? Say::label_recovery : Say::label_login, Role::label);
        plan.set(slot_login_field,
                 login ? Say::login_text : recovery ? Say::account_or_phone : Say::email_phone_or_link,
                 login ? Role::field : Role::field_hint);
    }
    if (!scene.credential_fields_hidden) {
        plan.set(slot_password_label,
                 webauthn ? Say::label_passkey
#if !defined(_WIN32)
                 : pin ? Say::key_pin_label
#endif
                 : mfa ? Say::label_code
                 : reset ? Say::label_new_password
                         : Say::label_password,
                 Role::label);
        plan.set(slot_password_field,
                 webauthn ? Say::key_field
#if !defined(_WIN32)
                 : pin ? Say::key_pin_field
#endif
                 : password ? Say::password_text
                 : selected_mfa == login_mfa_capability_sms ? Say::sms_code
                 : selected_mfa == login_mfa_capability_backup ? Say::backup_code
                 : mfa ? Say::authentication_code
                 : reset ? Say::new_password
                         : Say::password,
                 !webauthn && password ? Role::field : Role::field_hint);
    }
    if (!mfa || reset)
        plan.set(slot_forgot, reset || recovery || blocked ? Say::back_to_sign_in : Say::forgot_password,
                 reset || recovery || blocked || !scene.recovery_disabled ? Role::link_accent : Role::link_muted);
    plan.set(slot_submit, action, Role::submit);
    plan.set(slot_status, text, status_role(wrapped, error));
    if (mobile_pending)
        plan.set(slot_account_remote, Say::remote_user, Role::account_name);
    if (status == S::signed_out_unconfirmed)
        plan.set(slot_account_logout, Say::logged_out_only_here, Role::account_note);
    if (view.caps_lock && !mobile_pending && status != S::signed_out_unconfirmed && status != S::profile_save_failed)
        plan.set(slot_account_caps, Say::caps_lock, Role::account_note);
    if (status == S::profile_save_failed)
        plan.set(slot_account_save, view.reset_mode ? Say::try_again_or_back : Say::try_again_or_close,
                 Role::account_error);
    for (u32 index = 0; index < kLoginMfaPresentationMethodCount; ++index) {
        if (!blocked && (view.mfa_capabilities & kLoginMfaPresentationMethods[index].capability) != 0)
            plan.set(static_cast<Slot>(slot_method + index), Say::method_key + index, Role::method);
    }
    if (scene.qr_visible) {
        plan.set(slot_qr_title, Say::qr_title, Role::qr_title);
        plan.set(slot_qr_subtitle, kQrSubtitle[static_cast<u32>(remote_status)], Role::qr_subtitle);
    }
}

} // namespace

bool login_text_presentation_build(const LoginScene& scene, const LoginView& view, const Theme& theme,
                                   UiTextCommandStream* stream) noexcept {
    if (!stream || !stream->storage.data || stream->storage.count < kMaximumLoginTextCommands) {
        saturating_increment_if(stream, stream->rejected);
        return false;
    }
    ui_text_command_stream_reset(stream);
    if (!view.visible) {
        return true;
    }
    const Color submit_ink =
        scene.submit_disabled ? theme.text_primary : ui_surface_foreground(theme, theme.tokens.emphasis.fill);
    const Writer out{scene,
                     stream,
                     {&theme.text_primary, &theme.text_secondary, &theme.accent, &theme.notification, &submit_ink}};
    Plan plan{};

    if (scene.browser_mode) {
        plan_browser(plan, scene, view);
        return emit(out, plan);
    }
    if (scene.phone_mode) {
        plan_phone(plan, scene, view);
        if (!emit(out, plan))
            return false;
        const auto* field = login_scene_element(scene, LoginElement::password_field);
        const auto owner = scene.elements[static_cast<u32>(LoginElement::password_field)];
        if (!field)
            return false;
        for (u32 i = 0; i < view.password_mask.bytes; ++i) {
            if (view.password_mask.data[i] == ' ')
                continue;
            const UiTextCommand digit{{view.password_mask.data + i, 1},
                                      {&scene.widgets, owner},
                                      login_phone_cell_rectangle(field->content_rectangle, i),
                                      field->clip_rectangle,
                                      theme.text_primary,
                                      text_style_none,
                                      UiTextAlignment::center,
                                      UiTextAlignment::center,
                                      TextWrapping::no_wrap,
                                      TextOverflow::clip};
            if (!ui_text_command_stream_push(stream, digit))
                return false;
        }
        return true;
    }
    if (view.status == LoginStatus::restoring_session) {
        plan.set(slot_root, scene.switching_accounts ? Say::switching_accounts : Say::connecting, Role::root);
        return emit(out, plan);
    }
    plan_form(plan, scene, view);
    return emit(out, plan) && stream->count != 0;
}

} // namespace maicord
