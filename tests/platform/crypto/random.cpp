#include "maicord/platform/crypto/random.hpp"
#include "maicord/core/memory.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

// How many distinct byte values the bytes hold.
u32 distinct(const u8* data, u32 count) noexcept {
    bool seen[256]{};
    u32 total = 0;
    for (u32 i = 0; i != count; ++i)
        if (!seen[data[i]]) {
            seen[data[i]] = true;
            ++total;
        }
    return total;
}

} // namespace

void test_secure_random(Check check) noexcept {
    u8 first[32], second[32];
    check(secure_random_fill({first, 32}).ok() && secure_random_fill({second, 32}).ok() && !bytes_equal(first, second, 32),
          "two 32-byte random fills differ");
    check(distinct(first, 32) > 8, "a random fill is not a constant run");
    check(secure_random_fill({nullptr, 16}).error == SecureRandomError::invalid_input &&
              secure_random_fill({first, 0}).error == SecureRandomError::invalid_input,
          "random fill rejects a null or empty destination");
    // Larger than any single getrandom guarantee: the fill loops until every byte is written.
    static u8 large[1 << 20];
    memset(large, 0, sizeof(large));
    check(secure_random_fill({large, sizeof(large)}).ok() && distinct(large, sizeof(large)) == 256 &&
              distinct(large + sizeof(large) - 4096, 4096) > 200,
          "a 1 MiB random fill reaches its last byte");
    // An exact-size destination at the end of a page does not write past it.
    u8 edge[300];
    memset(edge, 0xa5, sizeof(edge));
    check(secure_random_fill({edge + 1, 257}).ok() && edge[0] == 0xa5 && test_filled(edge + 258, 42, 0xa5),
          "random fill writes exactly the requested bytes");
    u32 a = 0, b = 0;
    check(secure_random_u32(&a).ok() && secure_random_u32(&b).ok() && (a != b || a != 0),
          "random u32 values come from the generator");
    check(secure_random_u32(nullptr).error == SecureRandomError::invalid_input, "random u32 rejects a null destination");
}

} // namespace maicord
