#include "maicord/platform/crypto/random.hpp"

#include "maicord/sys/crypto.hpp"

namespace maicord {

SecureRandomResult secure_random_fill(Slice<u8> output) noexcept {
    if (!output.data || output.count == 0) {
        return SecureRandomResult{0, SecureRandomError::invalid_input};
    }
    const NTSTATUS status =
        BCryptGenRandom(nullptr, output.data, output.count, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0 ? SecureRandomResult{status, SecureRandomError::none}
                       : SecureRandomResult{status, SecureRandomError::system_failure};
}

} // namespace maicord
