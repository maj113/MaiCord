#include "maicord/app/frame_rate.hpp"

namespace maicord {

void test_frame_rate(void (*check)(bool, const char*) noexcept) noexcept {
    FrameRateCounter counter{};
    check(!frame_rate_update(counter, 9'000'000) && counter.next_update_us == 0,
          "frame rate idle does not schedule work");
    for (u32 i = 0; i < 180; ++i) {
        frame_rate_presented(counter, 1'000'000 + static_cast<u64>(i) * 500'000 / 180);
    }
    check(!frame_rate_update(counter, 1'499'999), "frame rate title update is throttled");
    check(frame_rate_update(counter, 1'500'000) && counter.fps == 360,
          "frame rate measures 360 actual presents per second");
    for (u32 i = 0; i < 15; ++i) frame_rate_presented(counter, 1'500'000 + i * 30'000);
    check(frame_rate_update(counter, 2'000'000) && counter.fps == 30,
          "frame rate is not display refresh or worker frequency");
    check(frame_rate_update(counter, 2'500'000) && counter.fps == 0 && counter.next_update_us == 0,
          "frame rate settles to zero and cancels idle wakeups");
    frame_rate_presented(counter, 20'000'000);
    check(frame_rate_update(counter, 21'000'000) && counter.fps == 1,
          "frame rate delayed sample uses actual elapsed time");
    check(frame_rate_update(counter, 21'500'000) && counter.fps == 0,
          "frame rate no presents are counted for unchanged paints");
}

} // namespace maicord
