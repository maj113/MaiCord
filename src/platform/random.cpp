#include "maicord/platform/random.hpp"

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

SecureRandomResult secure_random_u32(u32* output) noexcept {
    return output ? secure_random_fill(Slice<u8>{reinterpret_cast<u8*>(output), sizeof(*output)})
                  : SecureRandomResult{0, SecureRandomError::invalid_input};
}

} // namespace maicord
