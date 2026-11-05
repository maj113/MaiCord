// WebAuthn sign-in on Linux: no authenticator API is bound (Windows Hello and the inbox webauthn.dll are the Windows side,
// webauthn.cpp; libfido2 was evaluated and left unbound, see AGENTS.md). kWebAuthnAvailable is false here, so the sign-in
// says that security keys are not supported before it starts a ceremony; this answer is for a caller that asks anyway.
#include "maicord/platform/desktop/webauthn.hpp"

namespace maicord {

WindowsWebAuthnResult windows_webauthn_authenticate(Window, const WebAuthnMfaRequestOptions&,
                                                    SecureBuffer*) noexcept {
    return {0, 0, WindowsWebAuthnError::unavailable};
}

} // namespace maicord
