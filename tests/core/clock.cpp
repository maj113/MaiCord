#include "../support/test_main_shared.hpp"

#include "maicord/core/time.hpp"

namespace maicord {

void test_clock_microseconds() noexcept {
    expect(clock_microseconds(Clock{10'000'000}, 0, 25'920'000'000'000ll) == 2'592'000'000'000ull &&
               clock_microseconds(Clock{10}, 4, 19) == 1'500'000,
           "clock_microseconds_long_runtime_no_product_overflow");
}

} // namespace maicord
