#pragma once

#include "maicord/protocol/user_auth.hpp"
#include "maicord/ui/secure_edit.hpp"

namespace maicord::app_detail {

struct Application;

SecureEdit* application_active_login_edit() noexcept;
bool application_initialize_login_ui() noexcept;
void application_release_login_ui() noexcept;
void application_reset_login_caret_timing(bool edit_active) noexcept;
bool application_synchronize_login_view() noexcept;
bool application_refresh_login_view() noexcept;
// Settings opened before sign-in covers the form: while suspended the renderer
// shows no sign-in view and view updates wait; resuming publishes it again.
bool application_login_view_suspend(bool suspended) noexcept;
// Posts the phone-code verification once the entered code is complete and no IME composition is open.
void application_login_post_phone_verify() noexcept;

} // namespace maicord::app_detail
