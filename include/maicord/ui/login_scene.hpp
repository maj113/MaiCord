#pragma once

#include "maicord/ui/widget_storage.hpp"

namespace maicord {

inline constexpr u32 kMaximumLoginWidgets = 25;

enum class LoginControl : u8 {
    none,
    login,
    password,
    submit,
    forgot_password,
    qr,
    mfa_totp,
    mfa_sms,
    mfa_backup,
    mfa_webauthn,
    back,
    phone_code,
    phone_resend,
    browser_sign_in,
};

enum LoginMfaCapability : u8 {
    login_mfa_capability_none = 0,
    login_mfa_capability_totp = 1u << 0,
    login_mfa_capability_sms = 1u << 1,
    login_mfa_capability_backup = 1u << 2,
    login_mfa_capability_webauthn = 1u << 3,
    login_mfa_capability_all = login_mfa_capability_totp | login_mfa_capability_sms |
                               login_mfa_capability_backup | login_mfa_capability_webauthn,
};

enum class LoginElement : u8 {
    root,
    card,
    content_row,
    form,
    heading,
    subtitle,
    login_label,
    login_field,
    password_label,
    password_field,
    auxiliary,
    forgot_password,
    mfa_totp,
    mfa_sms,
    mfa_backup,
    mfa_webauthn,
    submit,
    status,
    status_message,
    status_account,
    qr_column,
    qr,
    qr_title,
    qr_subtitle,
    browser_sign_in,
    count,
};

struct LoginMfaPresentationMethod {
    u8 capability;
    LoginControl control;
    LoginElement element;
};

inline constexpr u32 kLoginMfaPresentationMethodCount = 4;
inline constexpr LoginMfaPresentationMethod kLoginMfaPresentationMethods[kLoginMfaPresentationMethodCount] =
    {
        {login_mfa_capability_webauthn, LoginControl::mfa_webauthn, LoginElement::mfa_webauthn},
        {login_mfa_capability_totp, LoginControl::mfa_totp, LoginElement::mfa_totp},
        {login_mfa_capability_backup, LoginControl::mfa_backup, LoginElement::mfa_backup},
        {login_mfa_capability_sms, LoginControl::mfa_sms, LoginElement::mfa_sms},
};

struct LoginLayoutMetrics {
    u32 revision;
    float full_viewport_max_width;
    float qr_min_width;
    float intermediate_max_width;
    float short_viewport_height;
    float wide_card_width;
    float compact_card_width;
    float card_height;
    UiInsets outer_margin;
    UiInsets card_padding;
    UiInsets full_viewport_padding;
    float qr_gap;
    float qr_column_width;
    float qr_extent;
    float input_height;
    float submit_height;
};

inline constexpr LoginLayoutMetrics kDefaultLoginLayoutMetrics{
    1,
    485.0f,
    830.0f,
    528.0f,
    456.0f,
    784.0f,
    480.0f,
    412.8f,
    UiInsets{24.0f, 24.0f, 24.0f, 24.0f},
    UiInsets{32.0f, 32.0f, 32.0f, 32.0f},
    UiInsets{16.0f, 20.0f, 16.0f, 20.0f},
    64.0f,
    240.0f,
    176.0f,
    44.0f,
    39.6f,
};

struct LoginScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiWidgetHandle elements[static_cast<u32>(LoginElement::count)];
    float layout_width;
    float layout_height;
    u32 layout_rebuilds;
    u32 layout_passes;
    u32 layout_metrics_revision;
    LoginControl focus;
    u8 mfa_capabilities;
    u8 selected_mfa_capability;
    u8 layout_mfa_capabilities;
    u8 layout_variant;
    bool credential_fields_hidden;
    bool layout_credential_fields_hidden;
    bool qr_visible;
    bool full_viewport;
    bool action_disabled;
    bool submit_disabled;
    bool focus_visible;
    bool restoring_session;
    bool back_only;
    bool recovery_mode;
    bool layout_recovery_mode;
    bool layout_recovery_result;
    bool recovery_disabled;
    bool reset_mode;
    bool layout_reset_mode;
    bool phone_mode;
    bool layout_phone_mode;
    bool phone_resend_disabled;
    bool browser_mode;
    bool browser_available;
    bool layout_browser_mode;
    bool layout_browser_available;
    bool captcha_action;            // Verify in browser stays enabled beside Back
    bool password_reveal_available; // eye toggle inside the password field
    bool password_revealed;
    // Saved accounts (text only): adding another account, a way back to a
    // saved account (Esc), and the restore view while switching.
    bool add_account;
    bool account_return;
    bool switching_accounts;
    bool auth_retry_resume;
};

inline constexpr u32 kLoginPhoneCodeDigits = 6;
UiRect login_phone_cell_rectangle(UiRect bounds, u32 cell) noexcept;
UiRect login_phone_caret_rectangle(UiRect bounds, u32 position) noexcept;
UiRect login_phone_range_rectangle(UiRect bounds, u32 first, u32 last) noexcept;
bool login_phone_point_position(UiRect bounds, u32 digits, float x, float y,
    bool round_nearest, u32* position, bool* inside) noexcept;

bool login_scene_layout(LoginScene* scene, float width, float height) noexcept;
void login_scene_release(LoginScene* scene) noexcept;
bool login_scene_layout_with_metrics(LoginScene* scene, float width, float height,
                                     const LoginLayoutMetrics& metrics) noexcept;
LoginControl login_scene_hit_test(const LoginScene& scene, float x, float y) noexcept;
// Show/hide-password toggle box; empty unless the scene offers the toggle.
UiRect login_password_reveal_rectangle(const LoginScene& scene) noexcept;
bool login_password_reveal_hit_test(const LoginScene& scene, float x, float y) noexcept;
bool login_scene_focus(LoginScene* scene, LoginControl control) noexcept;
LoginControl login_scene_move_focus(LoginScene* scene, bool reverse) noexcept;
const UiLayoutNode* login_scene_element(const LoginScene& scene, LoginElement element) noexcept;
UiRect login_scene_rectangle(const LoginScene& scene, LoginElement element) noexcept;
UiRect login_scene_content_rectangle(const LoginScene& scene, LoginElement element) noexcept;

} // namespace maicord
