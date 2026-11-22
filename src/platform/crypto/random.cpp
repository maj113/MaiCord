#include "maicord/platform/crypto/random.hpp"

#include "maicord/sys/crypto.hpp"

namespace maicord {

// Every caller fills a nonempty local array, field or secure buffer.
SecureRandomResult secure_random_fill(Slice<u8> output) noexcept {
    const NTSTATUS status =
        BCryptGenRandom(nullptr, output.data, output.count, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0 ? SecureRandomResult{status, SecureRandomError::none}
                       : SecureRandomResult{status, SecureRandomError::system_failure};
}

} // namespace maicord
