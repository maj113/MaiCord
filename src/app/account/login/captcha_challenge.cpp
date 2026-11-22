#include "maicord/app/account/login/captcha_challenge.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"

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

} // namespace

CaptchaProvider captcha_provider_from_service(const ConstSlice<u8>& service) noexcept {
    // The service names and their providers, in the same order.
    static constexpr char kNames[] = "hcaptcha\0recaptcha\0recaptcha_enterprise\0recaptcha-enterprise\0turnstile\0";
    static constexpr CaptchaProvider kProviders[] = {CaptchaProvider::hcaptcha, CaptchaProvider::recaptcha,
                                                     CaptchaProvider::recaptcha_enterprise,
                                                     CaptchaProvider::recaptcha_enterprise, CaptchaProvider::turnstile};
    const Utf8View view{reinterpret_cast<const char*>(service.data), service.count};
    u32 index = 0;
    for (NameList list{kNames}; list; list.next(), ++index) {
        if (ascii_equal_ignore_case(view, list.name())) {
            return kProviders[index];
        }
    }
    return CaptchaProvider::unknown;
}

namespace {

void clear_locked(CaptchaChallenge* challenge) noexcept {
    secure_buffer_clear(&challenge->secrets);
    reset_value<CaptchaChallengeSnapshot>(&challenge->snapshot);
}

void secret_range(CaptchaChallengeSecretKind kind, const CaptchaChallengeSnapshot& snapshot, u32* offset,
                  u32* bytes) noexcept {
    // Indexed by CaptchaChallengeSecretKind.
    static constexpr struct {
        u32 offset;
        u32 CaptchaChallengeSnapshot::*bytes;
    } kRanges[] = {{kServiceOffset, &CaptchaChallengeSnapshot::service_bytes},
                   {kSitekeyOffset, &CaptchaChallengeSnapshot::sitekey_bytes},
                   {kRqdataOffset, &CaptchaChallengeSnapshot::rqdata_bytes},
                   {kRqtokenOffset, &CaptchaChallengeSnapshot::rqtoken_bytes},
                   {kSessionOffset, &CaptchaChallengeSnapshot::session_id_bytes},
                   {kUserFlowOffset, &CaptchaChallengeSnapshot::user_flow_bytes}};
    static_assert(static_cast<u32>(CaptchaChallengeSecretKind::user_flow) + 1 == sizeof(kRanges) / sizeof(kRanges[0]));
    const u32 index = static_cast<u32>(kind);
    *offset = kRanges[index].offset;
    *bytes = snapshot.*kRanges[index].bytes;
}

} // namespace

CaptchaChallengeResult captcha_challenge_initialize(CaptchaChallenge* challenge) noexcept {
    // Both callers give a challenge without secrets (a reset owner, or after testing it).
    reset_value<CaptchaChallenge>(challenge);
    const SecureBufferResult initialized =
        secure_buffer_initialize(&challenge->secrets, kCaptchaChallengeSecretBytes);
    if (!initialized.ok()) {
        return CaptchaChallengeResult{initialized.native_error, CaptchaChallengeError::memory,
                                      UserAuthDecodeError::none};
    }
    return CaptchaChallengeResult{};
}

void captcha_challenge_clear(CaptchaChallenge* challenge) noexcept {
    if (!challenge->secrets.data) {
        return;
    }
    AcquireSRWLockExclusive(challenge->lock.native());
    clear_locked(challenge);
    ReleaseSRWLockExclusive(challenge->lock.native());
}

void captcha_challenge_release(CaptchaChallenge* challenge) noexcept {
    secure_buffer_release(&challenge->secrets);
    reset_value<CaptchaChallenge>(challenge);
}

CaptchaChallengeResult captcha_challenge_decode(CaptchaChallenge* challenge, u16 status_code,
                                                const Utf8View& body, const Slice<u32>& structural_scratch,
                                                u32 maximum_depth, UserAuthResponse* response) noexcept {
    // Both owners initialize the challenge before they decode into it (secure_buffer_initialize locks or fails).
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
        reset_value<UserAuthResponse>(response);
    }
    ReleaseSRWLockExclusive(challenge->lock.native());
    return result;
}

CaptchaChallengeSnapshot captcha_challenge_snapshot(const CaptchaChallenge* challenge) noexcept {
    AcquireSRWLockShared(challenge->lock.native());
    const CaptchaChallengeSnapshot snapshot = challenge->snapshot;
    ReleaseSRWLockShared(challenge->lock.native());
    return snapshot;
}

SecureBufferResult captcha_challenge_copy_secret(const CaptchaChallenge* challenge,
                                                 CaptchaChallengeSecretKind kind,
                                                 SecureBuffer* output) noexcept {
    AcquireSRWLockShared(challenge->lock.native());
    u32 offset = 0;
    u32 bytes = 0;
    SecureBufferResult result{};
    if (challenge->snapshot.active)
        secret_range(kind, challenge->snapshot, &offset, &bytes);
    if (bytes == 0) {
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
    AcquireSRWLockShared(challenge->lock.native());
    const CaptchaChallengeSnapshot snapshot = challenge->snapshot;
    if (!snapshot.active) {
        ReleaseSRWLockShared(challenge->lock.native());
        return SecureBufferResult{0, ERROR_NOT_FOUND, SecureBufferError::invalid_input};
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
    AcquireSRWLockShared(challenge->lock.native());
    const CaptchaChallengeSnapshot snapshot = challenge->snapshot;
    SecureBuffer rqtoken{const_cast<u8*>(secret(challenge, kRqtokenOffset)), kCaptchaChallengeRqtokenBytes,
                         snapshot.rqtoken_bytes, true};
    SecureBuffer session{const_cast<u8*>(secret(challenge, kSessionOffset)), kCaptchaChallengeSessionBytes,
                         snapshot.session_id_bytes, true};
    const bool visited = snapshot.active && visitor(context, rqtoken.bytes != 0 ? &rqtoken : nullptr,
                                                    session.bytes != 0 ? &session : nullptr);
    reset_value<SecureBuffer>(&rqtoken);
    reset_value<SecureBuffer>(&session);
    ReleaseSRWLockShared(challenge->lock.native());
    return visited;
}

} // namespace maicord
