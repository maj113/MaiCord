#pragma once
namespace maicord {
// Offline native GPU fixture. Logout/form/login/menu controllers are real;
// server transport and surrounding lifecycle coordination are injected.
// Stop/join and canonical fixture-file cleanup use real first-party helpers.
bool account_logout_ui_probe() noexcept;
} // namespace maicord
