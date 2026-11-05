#pragma once
#include "maicord/platform/crypto/secure_buffer.hpp"
#include "maicord/platform/window/window.hpp"

namespace maicord {
// Accepts only the receiver's fixed sign-in URL shape in caller-owned locked
// storage. The URL and UTF-16 copy are transient and never logged.
bool browser_sign_in_launch(Window window, const SecureBuffer* url) noexcept;
// Accepts only http://127.0.0.1:<port>/c/<32 lowercase hex> from the CAPTCHA
// bridge, with the same transient locked handling.
bool captcha_bridge_url_valid(const SecureBuffer* url) noexcept;
bool captcha_bridge_launch(Window window, const SecureBuffer* url) noexcept;
} // namespace maicord
