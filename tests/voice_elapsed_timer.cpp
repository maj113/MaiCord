#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/model/voice_channel_info_store.hpp"
#include "maicord/core/memory.hpp"

// Voice channel session timers (server start times, one wall clock) and the
// "Invite to Voice" countdown.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

bool formats(u64 elapsed_ms, const char* expected) noexcept {
    char text[kUiVoiceElapsedBytes];
    const u32 bytes = ui_voice_elapsed_format(elapsed_ms, text, sizeof(text));
    return bytes == c_string_length(expected) && bytes_equal(text, expected, bytes);
}
bool label_is(const ChannelViewRow& row, const UiVoiceChannelExtras& extras,
              const char* expected) noexcept {
    char text[kUiVoiceElapsedBytes];
    const u32 bytes = ui_voice_timer_label(row, &extras, text, sizeof(text));
    return bytes == c_string_length(expected) && bytes_equal(text, expected, bytes);
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

void label_cases(Check check) noexcept {
    static VoiceChannelInfoStore store;
    voice_channel_info_store_reset(&store);
    voice_channel_info_store_set_start(&store, 5, 7, 1'000'000);
    voice_channel_info_store_set_start(&store, 5, 9, 1'000'000);
    UiVoiceChannelExtras extras{};
    extras.info = &store;
    extras.clock_s = 1'000'000 + 3'723;
    ChannelViewRow voice{};
    voice.id = 7;
    voice.type = 2;
    check(label_is(voice, extras, "1:02:03"), "the label is the wall clock minus the server start");
    ChannelViewRow other = voice;
    other.id = 9;
    check(label_is(other, extras, "1:02:03"), "every voice channel with a start time shows its own timer");
    ChannelViewRow mentioned = voice;
    mentioned.mention_count = 2;
    ChannelViewRow stage = voice;
    stage.type = 13;
    ChannelViewRow idle = voice;
    idle.id = 11;
    check(label_is(mentioned, extras, "") && label_is(stage, extras, "") && label_is(idle, extras, ""),
          "a mention badge takes the spot; stages and idle channels show none");
    extras.clock_s = 999'990;
    check(label_is(voice, extras, "0:00"), "a start ahead of the local clock shows 0:00");
    extras.clock_s = 0;
    check(label_is(voice, extras, ""), "no clock, no timers");
}

// The host loop: wake at next_wake, tick, count repaints. unix_ms runs 250 ms
// ahead of the monotonic clock's second boundaries.
void wake_cases(Check check) noexcept {
    UiVoiceTimerClock clock{};
    check(!ui_voice_timer_tick(&clock, 1'000, 5'000'250, false, true) && !ui_voice_timer_next_wake(clock),
          "no visible timer, no wakes");
    check(ui_voice_timer_tick(&clock, 1'000, 5'000'250, true, true) && clock.shown_s == 5'000 &&
              ui_voice_timer_next_wake(clock) == 1'750,
          "coming on screen shows the time at once and wakes on the next wall second");
    const auto unix_of = [](u64 monotonic) noexcept { return monotonic + 4'999'250; };
    u32 repaints = 0;
    bool spaced = true;
    u64 now = 1'000;
    for (u32 i = 0; i < 120; ++i) {
        const u64 wake = ui_voice_timer_next_wake(clock);
        spaced = spaced && wake == now + (i ? 1'000u : 750u);
        now = wake;
        repaints += ui_voice_timer_tick(&clock, now, unix_of(now), true, true) ? 1u : 0u;
    }
    check(spaced && repaints == 120 && clock.shown_s == 5'120,
          "focused: one wake and one repaint per displayed second");
    check(!ui_voice_timer_tick(&clock, now + 300, unix_of(now + 300), true, true) &&
              ui_voice_timer_next_wake(clock) == now + 1'000,
          "an early wake inside the same second does not repaint");
    check(!ui_voice_timer_tick(&clock, now, unix_of(now), true, false) &&
              ui_voice_timer_next_wake(clock) == now + 15'000,
          "losing focus keeps the label and waits 15 s");
    repaints = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u64 wake = ui_voice_timer_next_wake(clock);
        spaced = spaced && wake == now + 15'000;
        now = wake;
        repaints += ui_voice_timer_tick(&clock, now, unix_of(now), true, false) ? 1u : 0u;
    }
    check(spaced && repaints == 4, "unfocused: a wake and a repaint every 15 s");
    check(ui_voice_timer_tick(&clock, now + 2'000, unix_of(now + 2'000), true, true),
          "regaining focus shows the current second at once");
    check(!ui_voice_timer_tick(&clock, now + 5'000, unix_of(now + 5'000), false, true) &&
              !ui_voice_timer_next_wake(clock),
          "minimised, hidden or covered by settings: no wakes and no repaints");
    check(ui_voice_timer_tick(&clock, now + 9'000, unix_of(now + 9'000), true, true) &&
              clock.shown_s == unix_of(now + 9'000) / 1'000,
          "showing the timer again repaints once with the current time");
    check(ui_voice_timer_tick(&clock, now + 9'100, unix_of(now + 9'000) - 60'000, true, true) &&
              clock.shown_s == (unix_of(now + 9'000) - 60'000) / 1'000,
          "a wall clock set back is followed at once");
}

void invite_cases(Check check) noexcept {
    UiVoiceInviteCountdown invite{};
    check(!ui_voice_invite_observe(&invite, 0) && !ui_voice_invite_channel(invite),
          "not connected, no row");
    check(ui_voice_invite_observe(&invite, 7) && ui_voice_invite_channel(invite) == 7 &&
              !ui_voice_invite_next_wake(invite),
          "a new connection shows the row without a countdown until it is on screen");
    check(!ui_voice_invite_tick(&invite, 1'000, true) && ui_voice_invite_next_wake(invite) == 11'000,
          "on screen the row counts down 10 s");
    check(!ui_voice_invite_tick(&invite, 6'000, false) && !ui_voice_invite_next_wake(invite) &&
              !ui_voice_invite_tick(&invite, 7'000, true) && ui_voice_invite_next_wake(invite) == 17'000,
          "hovering or leaving the screen restarts the full 10 s");
    check(!ui_voice_invite_tick(&invite, 16'999, true) && ui_voice_invite_tick(&invite, 17'000, true) &&
              !ui_voice_invite_channel(invite) && !ui_voice_invite_next_wake(invite),
          "the countdown dismisses the row");
    check(!ui_voice_invite_observe(&invite, 7) && !ui_voice_invite_channel(invite),
          "staying connected keeps it dismissed");
    check(ui_voice_invite_observe(&invite, 0) == false && ui_voice_invite_observe(&invite, 7) &&
              ui_voice_invite_channel(invite) == 7,
          "a new connection to the channel shows it again");
    check(ui_voice_invite_dismiss(&invite) && !ui_voice_invite_channel(invite) &&
              !ui_voice_invite_dismiss(&invite),
          "the close button dismisses it once");
    check(ui_voice_invite_observe(&invite, 8) && ui_voice_invite_channel(invite) == 8,
          "moving to another channel shows it for that channel");
    check(ui_voice_invite_observe(&invite, 0) && !ui_voice_invite_channel(invite),
          "leaving voice hides it");
}

void layout_cases(Check check) noexcept {
    ChannelViewRow row{};
    row.id = 7;
    row.type = 2;
    row.voice_count = 2;
    const float plain = shell_navigation_row_height(row);
    row.voice_subtitle = true;
    row.voice_invite = true;
    check(shell_navigation_row_height(row) ==
              plain + kShellNavigationStageTopicHeight + kShellNavigationVoiceInviteHeight,
          "a status line and the invite row add their heights");
    const UiRect box{0, 100, 240, 100 + shell_navigation_row_height(row)};
    UiVoiceInviteRow invite{};
    const UiRect status = ui_voice_status_rectangle(row, box);
    check(ui_voice_invite_row(row, box, &invite) &&
              status.top >= box.top + kShellNavigationVoiceHeadHeight - 4 &&
              invite.row.top >= status.bottom + 2 * kShellNavigationVoiceUserHeight - 2 &&
              invite.row.bottom <= box.bottom && invite.close.right <= box.right &&
              invite.label.left > invite.icon.right,
          "the invite row follows the users inside the channel row");
    row.voice_invite = false;
    check(!ui_voice_invite_row(row, box, &invite), "no invite row without the flag");
}
} // namespace

void test_voice_elapsed_timer(Check check) noexcept {
    format_cases(check);
    label_cases(check);
    wake_cases(check);
    invite_cases(check);
    layout_cases(check);
}
} // namespace maicord
