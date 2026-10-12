#pragma once

#include "maicord/platform/secure_buffer.hpp"
#include "maicord/protocol/user_auth.hpp"

#include "maicord/core/win32_records.hpp"

namespace maicord {

inline constexpr u32 kCaptchaChallengeServiceBytes = 128;
inline constexpr u32 kCaptchaChallengeSitekeyBytes = 256;
inline constexpr u32 kCaptchaChallengeRqdataBytes = 4 * 1024;
inline constexpr u32 kCaptchaChallengeRqtokenBytes = kUserAuthMaximumCaptchaTokenBytes;
inline constexpr u32 kCaptchaChallengeSessionBytes = kUserAuthMaximumCaptchaSessionBytes;
inline constexpr u32 kCaptchaChallengeUserFlowBytes = 128;
inline constexpr u32 kCaptchaChallengeSecretBytes =
    kCaptchaChallengeServiceBytes + kCaptchaChallengeSitekeyBytes + kCaptchaChallengeRqdataBytes +
    kCaptchaChallengeRqtokenBytes + kCaptchaChallengeSessionBytes + kCaptchaChallengeUserFlowBytes;

enum class CaptchaProvider : u8 {
    unknown,
    hcaptcha,
    recaptcha,
    recaptcha_enterprise,
    turnstile,
};

// Classifies a borrowed service value without copying or retaining its bytes.
CaptchaProvider captcha_provider_from_service(ConstSlice<u8> service) noexcept;

enum class CaptchaChallengeSecretKind : u8 {
    service,
    sitekey,
    rqdata,
    rqtoken,
    session_id,
    user_flow,
};

enum class CaptchaChallengeError : u8 {
    none,
    invalid_input,
    memory,
    decode,
    not_challenge,
};

struct CaptchaChallengeResult {
    u32 native_error;
    CaptchaChallengeError error;
    UserAuthDecodeError decode_error;

    constexpr bool ok() const noexcept {
        return error == CaptchaChallengeError::none;
    }
};

struct CaptchaChallengeSnapshot {
    u32 service_bytes;
    u32 sitekey_bytes;
    u32 rqdata_bytes;
    u32 rqtoken_bytes;
    u32 session_id_bytes;
    u32 user_flow_bytes;
    CaptchaProvider provider;
    bool invisible;
    bool active;
};

// Owns one server challenge in a single VirtualLock-backed allocation. The
// response body, solution, and replay headers never enter this metadata
// snapshot and must cross boundaries through initialized SecureBuffers.
struct CaptchaChallenge {
    SrwLock lock;
    SecureBuffer secrets;
    CaptchaChallengeSnapshot snapshot;
};

CaptchaChallengeResult captcha_challenge_initialize(CaptchaChallenge* challenge) noexcept;
void captcha_challenge_clear(CaptchaChallenge* challenge) noexcept;
void captcha_challenge_release(CaptchaChallenge* challenge) noexcept;

// Decodes only the bounded public CAPTCHA schema directly into locked memory.
// A non-CAPTCHA response or any decode failure leaves the owner empty.
CaptchaChallengeResult captcha_challenge_decode(CaptchaChallenge* challenge, u16 status_code, Utf8View body,
                                                Slice<u32> structural_scratch, u32 maximum_depth,
                                                UserAuthResponse* response) noexcept;
CaptchaChallengeSnapshot captcha_challenge_snapshot(const CaptchaChallenge* challenge) noexcept;
// The locked destination must be disjoint from the challenge owner.
SecureBufferResult captcha_challenge_copy_secret(const CaptchaChallenge* challenge,
                                                 CaptchaChallengeSecretKind kind,
                                                 SecureBuffer* output) noexcept;

// Copies the optional replay values as one atomic snapshot. Both destinations
// must already be locked and disjoint from each other and the challenge owner;
// either both are updated or neither is touched.
SecureBufferResult captcha_challenge_copy_replay_headers(const CaptchaChallenge* challenge,
                                                         SecureBuffer* rqtoken,
                                                         SecureBuffer* session_id) noexcept;

using CaptchaChallengeReplayVisitor = bool (*)(void* context, const SecureBuffer* rqtoken,
                                               const SecureBuffer* session_id) noexcept;
// Borrows locked projections only for the synchronous visitor call. The
// visitor must copy them before returning and must never retain the pointers.
bool captcha_challenge_visit_replay_headers(const CaptchaChallenge* challenge,
                                            CaptchaChallengeReplayVisitor visitor, void* context) noexcept;

} // namespace maicord
