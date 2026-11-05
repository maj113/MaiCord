// WebAuthn sign-in on Linux: no platform authenticator API is bound (Windows Hello and the inbox webauthn.dll are the
// Windows side, webauthn.cpp). The caller falls back to the other second factors when the ceremony is unavailable.
#include "maicord/platform/desktop/webauthn.hpp"

namespace maicord {

WindowsWebAuthnResult windows_webauthn_authenticate(Window, const WebAuthnMfaRequestOptions&,
                                                    SecureBuffer*) noexcept {
    return {0, 0, WindowsWebAuthnError::unavailable};
}

} // namespace maicord
