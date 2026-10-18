#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool formats(u64 elapsed_ms, const char* expected) noexcept {
    char text[kUiVoiceElapsedBytes];
    const u32 bytes = ui_voice_elapsed_format(elapsed_ms, text, sizeof(text));
    u32 expected_bytes = 0;
    while (expected[expected_bytes])
        ++expected_bytes;
    return bytes == expected_bytes && bytes_equal(text, expected, bytes);
}
bool shows(const UiVoiceElapsedClock& clock, const char* expected) noexcept {
    u32 bytes = 0;
    while (expected[bytes])
        ++bytes;
    return clock.label_bytes == bytes && bytes_equal(clock.label, expected, bytes);
}

void format_cases(Check check) noexcept {
    check(formats(0, "0:00") && formats(999, "0:00") && formats(6'000, "0:06") && formats(83'000, "1:23"),
          "under a minute and minutes show m:ss");
    check(formats(3'599'999, "59:59") && formats(3'600'000, "1:00:00") && formats(3'723'000, "1:02:03"),
          "an hour switches to h:mm:ss");
    check(formats(36'000'000ull * 10, "100:00:00"), "hours keep growing past a day");
    char small[4];
    check(ui_voice_elapsed_format(6'000, small, sizeof(small)) == 0, "a short buffer writes nothing");
}

void lifecycle_cases(Check check) noexcept {
    UiVoiceElapsedClock clock{};
    check(!ui_voice_elapsed_observe(&clock, 7, false, 1'000) && clock.channel_id == 7 && !clock.started_ms &&
              !clock.label_bytes,
          "connecting shows no timer");
    check(ui_voice_elapsed_observe(&clock, 7, true, 5'000) && clock.started_ms == 5'000 && shows(clock, "0:00"),
          "the established connection starts the clock at 0:00");
    check(!ui_voice_elapsed_observe(&clock, 7, true, 9'000) && clock.started_ms == 5'000,
          "staying connected keeps the start");
    check(!ui_voice_elapsed_observe(&clock, 7, false, 10'000) && clock.started_ms == 5'000 &&
              !ui_voice_elapsed_observe(&clock, 7, true, 12'000) && clock.started_ms == 5'000,
          "a reconnect in the same channel keeps the start");
    check(ui_voice_elapsed_tick(&clock, 11'000, true) && shows(clock, "0:06"), "ticks advance the label");
    check(ui_voice_elapsed_observe(&clock, 8, false, 20'000) && clock.channel_id == 8 && !clock.started_ms &&
              !clock.label_bytes && ui_voice_elapsed_observe(&clock, 8, true, 21'000) &&
              clock.started_ms == 21'000 && shows(clock, "0:00"),
          "moving to another channel restarts the clock");
    check(ui_voice_elapsed_observe(&clock, 0, false, 22'000) && !clock.channel_id && !clock.label_bytes &&
              !ui_voice_elapsed_next_wake(clock),
          "leaving voice clears the timer");
    check(!ui_voice_elapsed_observe(&clock, 0, false, 23'000), "staying out of voice changes nothing");
}

// The host loop: wake at next_wake, tick, count repaints.
void wake_cases(Check check) noexcept {
    UiVoiceElapsedClock clock{};
    check(!ui_voice_elapsed_next_wake(clock) && !ui_voice_elapsed_tick(&clock, 1'000, true),
          "no voice, no wakes");
    ui_voice_elapsed_observe(&clock, 7, true, 10'250);
    check(!ui_voice_elapsed_next_wake(clock), "no wake before a tick reports the row visible");
    ui_voice_elapsed_tick(&clock, 10'250, true);
    u32 wakes = 0, repaints = 0;
    u64 now = 10'250;
    while (now < 10'250 + 120'000) {
        const u64 wake = ui_voice_elapsed_next_wake(clock);
        if (!wake || wake <= now)
            break;
        now = wake;
        ++wakes;
        repaints += ui_voice_elapsed_tick(&clock, now, true) ? 1u : 0u;
    }
    check(wakes == 120 && repaints == 120 && shows(clock, "2:00"),
          "one wake and one repaint per displayed second while visible");
    check(ui_voice_elapsed_next_wake(clock) == 10'250 + 121'000, "wakes land on the start's second boundaries");
    check(!ui_voice_elapsed_tick(&clock, now + 400, true), "a wake inside the same second does not repaint");
    check(!ui_voice_elapsed_tick(&clock, now + 1'000, false) && !ui_voice_elapsed_next_wake(clock),
          "a hidden row or window takes no wakes and no repaints");
    check(shows(clock, "2:01"), "the hidden label still follows the clock");
    check(ui_voice_elapsed_tick(&clock, now + 5'000, true) && shows(clock, "2:05") &&
              ui_voice_elapsed_next_wake(clock) == 10'250 + 126'000,
          "showing the row again repaints once and resumes wakes");
}
} // namespace

void test_voice_elapsed_timer(Check check) noexcept {
    format_cases(check);
    lifecycle_cases(check);
    wake_cases(check);
}
} // namespace maicord
