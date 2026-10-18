#include "maicord/app/captcha_challenge.hpp"
#include "maicord/core/byte_range.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

inline constexpr u32 kServiceOffset = 0;
inline constexpr u32 kSitekeyOffset = kServiceOffset + kCaptchaChallengeServiceBytes;
inline constexpr u32 kRqdataOffset = kSitekeyOffset + kCaptchaChallengeSitekeyBytes;
inline constexpr u32 kRqtokenOffset = kRqdataOffset + kCaptchaChallengeRqdataBytes;
inline constexpr u32 kSessionOffset = kRqtokenOffset + kCaptchaChallengeRqtokenBytes;
inline constexpr u32 kUserFlowOffset = kSessionOffset + kCaptchaChallengeSessionBytes;
static_assert(kUserFlowOffset + kCaptchaChallengeUserFlowBytes == kCaptchaChallengeSecretBytes);

u8* secret(CaptchaChallenge* challenge, u32 offset) noexcept {
    return challenge->secrets.data + offset;
}

const u8* secret(const CaptchaChallenge* challenge, u32 offset) noexcept {
    return challenge->secrets.data + offset;
}

Slice<char> storage(CaptchaChallenge* challenge, u32 offset, u32 capacity) noexcept {
    return Slice<char>{reinterpret_cast<char*>(secret(challenge, offset)), capacity};
}

bool ascii_equal_folded(ConstSlice<u8> value, const char* expected, u32 expected_bytes) noexcept {
    if (!value.data || !expected || value.count != expected_bytes) {
        return false;
    }
    for (u32 index = 0; index < expected_bytes; ++index) {
        u8 left = value[index];
        u8 right = static_cast<u8>(expected[index]);
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<u8>(left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<u8>(right + ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

} // namespace

CaptchaProvider captcha_provider_from_service(ConstSlice<u8> service) noexcept {
    if (ascii_equal_folded(service, "hcaptcha", 8)) {
        return CaptchaProvider::hcaptcha;
    }
    if (ascii_equal_folded(service, "recaptcha", 9)) {
        return CaptchaProvider::recaptcha;
    }
    if (ascii_equal_folded(service, "recaptcha_enterprise", 20) ||
        ascii_equal_folded(service, "recaptcha-enterprise", 20)) {
        return CaptchaProvider::recaptcha_enterprise;
    }
    if (ascii_equal_folded(service, "turnstile", 9)) {
        return CaptchaProvider::turnstile;
    }
    return CaptchaProvider::unknown;
}

namespace {

void clear_locked(CaptchaChallenge* challenge) noexcept {
    secure_buffer_clear(&challenge->secrets);
    challenge->snapshot = CaptchaChallengeSnapshot{};
}

bool external_output(const CaptchaChallenge* challenge, const SecureBuffer* output) noexcept {
    if (!output || !output->data || !output->locked || output->capacity == 0 ||
        output->bytes > output->capacity) {
        return false;
    }
    const ConstSlice<u8> destination{output->data, output->capacity};
    return byte_range_valid(destination) &&
           !byte_ranges_overlap(destination, {challenge->secrets.data, challenge->secrets.capacity}) &&
           !byte_ranges_overlap(destination, {reinterpret_cast<const u8*>(challenge), sizeof(*challenge)});
}

bool secret_range(CaptchaChallengeSecretKind kind, const CaptchaChallengeSnapshot& snapshot, u32* offset,
                  u32* bytes) noexcept {
    switch (kind) {
    case CaptchaChallengeSecretKind::service:
        *offset = kServiceOffset;
        *bytes = snapshot.service_bytes;
        return true;
    case CaptchaChallengeSecretKind::sitekey:
        *offset = kSitekeyOffset;
        *bytes = snapshot.sitekey_bytes;
        return true;
    case CaptchaChallengeSecretKind::rqdata:
        *offset = kRqdataOffset;
        *bytes = snapshot.rqdata_bytes;
        return true;
    case CaptchaChallengeSecretKind::rqtoken:
        *offset = kRqtokenOffset;
        *bytes = snapshot.rqtoken_bytes;
        return true;
    case CaptchaChallengeSecretKind::session_id:
        *offset = kSessionOffset;
        *bytes = snapshot.session_id_bytes;
        return true;
    case CaptchaChallengeSecretKind::user_flow:
        *offset = kUserFlowOffset;
        *bytes = snapshot.user_flow_bytes;
        return true;
    default:
        return false;
    }
}

} // namespace

CaptchaChallengeResult captcha_challenge_initialize(CaptchaChallenge* challenge) noexcept {
    if (!challenge || challenge->secrets.data) {
        return CaptchaChallengeResult{ERROR_INVALID_PARAMETER, CaptchaChallengeError::invalid_input,
                                      UserAuthDecodeError::none};
    }
    *challenge = CaptchaChallenge{};
    const SecureBufferResult initialized =
        secure_buffer_initialize(&challenge->secrets, kCaptchaChallengeSecretBytes);
    if (!initialized.ok()) {
        return CaptchaChallengeResult{initialized.native_error, CaptchaChallengeError::memory,
                                      UserAuthDecodeError::none};
    }
    return CaptchaChallengeResult{};
}

void captcha_challenge_clear(CaptchaChallenge* challenge) noexcept {
    if (!challenge || !challenge->secrets.data) {
        return;
    }
    AcquireSRWLockExclusive(challenge->lock.native());
    clear_locked(challenge);
    ReleaseSRWLockExclusive(challenge->lock.native());
}

void captcha_challenge_release(CaptchaChallenge* challenge) noexcept {
    if (!challenge) {
        return;
    }
    secure_buffer_release(&challenge->secrets);
    *challenge = CaptchaChallenge{};
}

CaptchaChallengeResult captcha_challenge_decode(CaptchaChallenge* challenge, u16 status_code, Utf8View body,
                                                Slice<u32> structural_scratch, u32 maximum_depth,
                                                UserAuthResponse* response) noexcept {
    if (!challenge || !challenge->secrets.data || !challenge->secrets.locked || !response) {
        return CaptchaChallengeResult{ERROR_INVALID_PARAMETER, CaptchaChallengeError::invalid_input,
                                      UserAuthDecodeError::none};
    }
    AcquireSRWLockExclusive(challenge->lock.native());
    clear_locked(challenge);
    UserAuthResponse decoded{};
    const UserAuthResponseStorage response_storage{
        {},
        {},
        {},
        {},
        storage(challenge, kServiceOffset, kCaptchaChallengeServiceBytes),
        storage(challenge, kSitekeyOffset, kCaptchaChallengeSitekeyBytes),
        storage(challenge, kRqdataOffset, kCaptchaChallengeRqdataBytes),
        storage(challenge, kRqtokenOffset, kCaptchaChallengeRqtokenBytes),
        storage(challenge, kSessionOffset, kCaptchaChallengeSessionBytes),
        storage(challenge, kUserFlowOffset, kCaptchaChallengeUserFlowBytes),
        {},
    };
    const UserAuthDecodeResult parsed = user_auth_decode_response(
        status_code, body, structural_scratch, maximum_depth, response_storage, &decoded);
    CaptchaChallengeResult result{};
    if (!parsed.ok()) {
        result.error = CaptchaChallengeError::decode;
        result.decode_error = parsed.error;
        clear_locked(challenge);
    } else if (decoded.kind != UserAuthResponseKind::captcha_required) {
        result.error = CaptchaChallengeError::not_challenge;
        clear_locked(challenge);
    } else {
        CaptchaChallengeSnapshot& snapshot = challenge->snapshot;
        snapshot.service_bytes = decoded.captcha_service_bytes;
        snapshot.sitekey_bytes = decoded.captcha_sitekey_bytes;
        snapshot.rqdata_bytes = decoded.captcha_rqdata_bytes;
        snapshot.rqtoken_bytes = decoded.captcha_rqtoken_bytes;
        snapshot.session_id_bytes = decoded.captcha_session_id_bytes;
        snapshot.user_flow_bytes = decoded.captcha_user_flow_bytes;
        snapshot.provider = captcha_provider_from_service(
            ConstSlice<u8>{secret(challenge, kServiceOffset), snapshot.service_bytes});
        snapshot.invisible = decoded.captcha_invisible;
        snapshot.active = true;
        *response = decoded;
    }
    if (!result.ok()) {
        *response = UserAuthResponse{};
    }
    ReleaseSRWLockExclusive(challenge->lock.native());
    return result;
}

CaptchaChallengeSnapshot captcha_challenge_snapshot(const CaptchaChallenge* challenge) noexcept {
    if (!challenge || !challenge->secrets.data) {
        return CaptchaChallengeSnapshot{};
    }
    AcquireSRWLockShared(challenge->lock.native());
    const CaptchaChallengeSnapshot snapshot = challenge->snapshot;
    ReleaseSRWLockShared(challenge->lock.native());
    return snapshot;
}

SecureBufferResult captcha_challenge_copy_secret(const CaptchaChallenge* challenge,
                                                 CaptchaChallengeSecretKind kind,
                                                 SecureBuffer* output) noexcept {
    if (!challenge || !challenge->secrets.data || !external_output(challenge, output)) {
        return SecureBufferResult{0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    }
    AcquireSRWLockShared(challenge->lock.native());
    u32 offset = 0;
    u32 bytes = 0;
    SecureBufferResult result{};
    if (!challenge->snapshot.active || !secret_range(kind, challenge->snapshot, &offset, &bytes) ||
        bytes == 0) {
        result = SecureBufferResult{0, ERROR_NOT_FOUND, SecureBufferError::invalid_input};
    } else {
        result = secure_buffer_assign(output, ConstSlice<u8>{secret(challenge, offset), bytes});
    }
    ReleaseSRWLockShared(challenge->lock.native());
    return result;
}

SecureBufferResult captcha_challenge_copy_replay_headers(const CaptchaChallenge* challenge,
                                                         SecureBuffer* rqtoken,
                                                         SecureBuffer* session_id) noexcept {
    if (!challenge || !challenge->secrets.data || !external_output(challenge, rqtoken) ||
        !external_output(challenge, session_id) ||
        byte_ranges_overlap({rqtoken->data, rqtoken->capacity}, {session_id->data, session_id->capacity})) {
        return SecureBufferResult{0, ERROR_INVALID_PARAMETER, SecureBufferError::invalid_input};
    }
    AcquireSRWLockShared(challenge->lock.native());
    const CaptchaChallengeSnapshot snapshot = challenge->snapshot;
    if (!snapshot.active || snapshot.rqtoken_bytes > rqtoken->capacity ||
        snapshot.session_id_bytes > session_id->capacity) {
        const u32 required =
            snapshot.rqtoken_bytes > rqtoken->capacity ? snapshot.rqtoken_bytes : snapshot.session_id_bytes;
        ReleaseSRWLockShared(challenge->lock.native());
        return SecureBufferResult{required, snapshot.active ? 0u : ERROR_NOT_FOUND,
                                  snapshot.active ? SecureBufferError::output_too_small
                                                  : SecureBufferError::invalid_input};
    }
    const SecureBufferResult token = secure_buffer_assign(
        rqtoken, ConstSlice<u8>{secret(challenge, kRqtokenOffset), snapshot.rqtoken_bytes});
    const SecureBufferResult session = secure_buffer_assign(
        session_id, ConstSlice<u8>{secret(challenge, kSessionOffset), snapshot.session_id_bytes});
    ReleaseSRWLockShared(challenge->lock.native());
    return token.ok() && session.ok()
               ? SecureBufferResult{token.bytes + session.bytes, 0, SecureBufferError::none}
           : !token.ok() ? token
                         : session;
}

bool captcha_challenge_visit_replay_headers(const CaptchaChallenge* challenge,
                                            CaptchaChallengeReplayVisitor visitor, void* context) noexcept {
    if (!challenge || !challenge->secrets.data || !visitor) {
        return false;
    }
    AcquireSRWLockShared(challenge->lock.native());
    const CaptchaChallengeSnapshot snapshot = challenge->snapshot;
    SecureBuffer rqtoken{const_cast<u8*>(secret(challenge, kRqtokenOffset)), kCaptchaChallengeRqtokenBytes,
                         snapshot.rqtoken_bytes, true};
    SecureBuffer session{const_cast<u8*>(secret(challenge, kSessionOffset)), kCaptchaChallengeSessionBytes,
                         snapshot.session_id_bytes, true};
    const bool visited = snapshot.active && visitor(context, rqtoken.bytes != 0 ? &rqtoken : nullptr,
                                                    session.bytes != 0 ? &session : nullptr);
    rqtoken = SecureBuffer{};
    session = SecureBuffer{};
    ReleaseSRWLockShared(challenge->lock.native());
    return visited;
}

} // namespace maicord
