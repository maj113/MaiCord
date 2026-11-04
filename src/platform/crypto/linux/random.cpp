#include "maicord/platform/crypto/random.hpp"

#include "maicord/sys/linux/calls.hpp"

namespace maicord {

// getrandom(2) with flags 0: blocks only before the kernel's pool is first seeded, never after. It is a system
// call, so the RNG does not depend on libcrypto being installed (nonces and jitter work without it).
SecureRandomResult secure_random_fill(Slice<u8> output) noexcept {
    if (!output.data || output.count == 0) {
        return SecureRandomResult{0, SecureRandomError::invalid_input};
    }
    u32 done = 0;
    while (done != output.count) {
        const long got = syscall3(kSysGetrandom, reinterpret_cast<long>(output.data + done),
                                  static_cast<long>(output.count - done), 0);
        if (got == -kErrorInterrupted) {
            continue;
        }
        if (syscall_failed(got) || got == 0) {
            return SecureRandomResult{syscall_failed(got) ? static_cast<i32>(got) : -kErrorAgain,
                                      SecureRandomError::system_failure};
        }
        done += static_cast<u32>(got);
    }
    return SecureRandomResult{0, SecureRandomError::none};
}

SecureRandomResult secure_random_u32(u32* output) noexcept {
    return output ? secure_random_fill(Slice<u8>{reinterpret_cast<u8*>(output), sizeof(*output)})
                  : SecureRandomResult{0, SecureRandomError::invalid_input};
}

} // namespace maicord
