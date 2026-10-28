#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/voice_panel.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/model/voice_channel_info_store.hpp"

namespace maicord {
bool ui_voice_participant_row(const ChannelViewRow& row, UiRect row_rectangle,
                              const VoiceStateStore& store, u32 index,
                              UiVoiceParticipantRow* output) noexcept {
    if (!output || index >= row.voice_count)
        return false;
    const auto* state = voice_state_store_channel_at(store, row.id, index);
    if (!state)
        return false;
    const float top = row_rectangle.top + shell_navigation_voice_top(row) +
                      static_cast<float>(index) * kShellNavigationVoiceUserHeight;
    const float left = row_rectangle.left + 34.0f + static_cast<float>(row.depth) * 12.0f;
    const float right = row_rectangle.right - 10.0f;
    UiVoiceParticipantRow result{state};
    result.speaking = (state->flags & voice_speaking) != 0;
    result.row = {row_rectangle.left, top, row_rectangle.right, top + kShellNavigationVoiceUserHeight};
    result.avatar = {left, top + 2.0f, left + 20.0f, top + 22.0f};
    float indicator = right;
    if (state->flags & (voice_deaf | voice_self_deaf)) {
        result.deafened = {indicator - 16.0f, top + 4.0f, indicator, top + 20.0f};
        indicator -= 20.0f;
    }
    if (state->flags & (voice_mute | voice_self_mute)) {
        result.muted = {indicator - 16.0f, top + 4.0f, indicator, top + 20.0f};
        indicator -= 20.0f;
    }
    if (state->flags & voice_self_video) {
        result.video = {indicator - 16.0f, top + 4.0f, indicator, top + 20.0f};
        indicator -= 20.0f;
    }
    if (voice_state_live(*state)) {
        result.live = {indicator - kUiVoiceLiveBadgeWidth, top + 4.0f, indicator, top + 4.0f + kUiVoiceLiveBadgeHeight};
        indicator -= kUiVoiceLiveBadgeWidth + 4.0f;
    }
    result.label = {result.avatar.right + 8.0f, top, maximum(result.avatar.right + 8.0f, indicator - 4.0f),
                    top + kShellNavigationVoiceUserHeight};
    *output = result;
    return true;
}

UiRect ui_voice_disconnect_rectangle(UiRect footer) noexcept {
    return ui_voice_panel_disconnect(ui_voice_panel_rectangle(footer));
}

namespace {
u32 two_digits(char* output, u64 value) noexcept {
    output[0] = static_cast<char>('0' + value / 10);
    output[1] = static_cast<char>('0' + value % 10);
    return 2;
}
} // namespace

u32 ui_voice_elapsed_format(u64 elapsed_ms, char* output, u32 capacity) noexcept {
    const u64 seconds = elapsed_ms / 1'000, hours = seconds / 3'600, minutes = seconds / 60 % 60;
    if (!output || capacity < kUiVoiceElapsedBytes)
        return 0;
    u32 used = 0;
    if (hours) {
        used = decimal_write(output, hours);
        output[used++] = ':';
        used += two_digits(output + used, minutes);
    } else {
        used = decimal_write(output, minutes);
    }
    output[used++] = ':';
    return used + two_digits(output + used, seconds % 60);
}

bool ui_voice_timer_tick(UiVoiceTimerClock* clock, u64 now_ms, u64 unix_ms, bool visible, bool focused) noexcept {
    if (!clock)
        return false;
    if (!visible) {
        clock->visible = false;
        clock->wake_ms = 0;
        return false;
    }
    const bool shown = clock->visible;
    clock->visible = true;
    const u64 second = unix_ms / 1'000, period = focused ? 1 : kUiVoiceTimerSlowSeconds;
    // Coming on screen, a missed boundary or a clock set back shows the time at once.
    const bool changed = second != clock->shown_s &&
                         (!shown || second >= clock->shown_s + period || second < clock->shown_s);
    if (changed)
        clock->shown_s = second;
    const u64 next_ms = (clock->shown_s + period) * 1'000;
    clock->wake_ms = now_ms + (next_ms > unix_ms ? next_ms - unix_ms : 1);
    return changed;
}

u32 ui_voice_timer_label(const ChannelViewRow& row, const UiVoiceChannelExtras* extras, char* output,
                         u32 capacity) noexcept {
    if (row.type != 2 || row.mention_count || !extras || !extras->info || !extras->clock_s)
        return 0;
    const auto* info = voice_channel_info_store_find(*extras->info, row.id);
    if (!info || !info->start_s)
        return 0;
    const u64 elapsed = extras->clock_s > info->start_s ? extras->clock_s - info->start_s : 0;
    return ui_voice_elapsed_format(elapsed * 1'000, output, capacity);
}

bool ui_voice_invite_observe(UiVoiceInviteCountdown* countdown, u64 connected_channel_id) noexcept {
    if (!countdown || countdown->channel_id == connected_channel_id)
        return false;
    const bool shown = ui_voice_invite_channel(*countdown) != 0;
    *countdown = {connected_channel_id, 0, false, false};
    return shown || connected_channel_id;
}

bool ui_voice_invite_tick(UiVoiceInviteCountdown* countdown, u64 now_ms, bool running) noexcept {
    if (!countdown || !ui_voice_invite_channel(*countdown))
        return false;
    if (!running) {
        countdown->counting = false;
        return false;
    }
    if (!countdown->counting) {
        countdown->counting = true;
        countdown->deadline_ms = now_ms + kUiVoiceInviteDismissMs;
        return false;
    }
    return now_ms >= countdown->deadline_ms && ui_voice_invite_dismiss(countdown);
}

bool ui_voice_invite_dismiss(UiVoiceInviteCountdown* countdown) noexcept {
    if (!countdown || !ui_voice_invite_channel(*countdown))
        return false;
    countdown->dismissed = true;
    countdown->counting = false;
    return true;
}

UiRect ui_voice_status_rectangle(const ChannelViewRow& row, UiRect row_rectangle) noexcept {
    const float left = row_rectangle.left + 34.0f + static_cast<float>(row.depth) * 12.0f;
    const float top = row_rectangle.top + kShellNavigationVoiceHeadHeight - 4.0f;
    return {left, top, maximum(left, row_rectangle.right - 10.0f), top + kShellNavigationStageTopicHeight};
}

bool ui_voice_invite_row(const ChannelViewRow& row, UiRect row_rectangle, UiVoiceInviteRow* output) noexcept {
    if (!output || !row.voice_invite)
        return false;
    const float top = row_rectangle.top + shell_navigation_voice_top(row) +
                      static_cast<float>(row.voice_count) * kShellNavigationVoiceUserHeight;
    const float left = row_rectangle.left + 30.0f + static_cast<float>(row.depth) * 12.0f;
    const float right = maximum(left, row_rectangle.right - 8.0f);
    UiVoiceInviteRow result{};
    result.row = {left, top + 2.0f, right, top + kShellNavigationVoiceInviteHeight - 2.0f};
    const float middle = top + kShellNavigationVoiceInviteHeight * 0.5f;
    result.icon = {left + 4.0f, middle - 12.0f, left + 28.0f, middle + 12.0f};
    result.close = {maximum(result.icon.right, right - 28.0f), middle - 12.0f, maximum(result.icon.right, right - 4.0f),
                    middle + 12.0f};
    result.label = {result.icon.right + 8.0f, top, maximum(result.icon.right + 8.0f, result.close.left - 4.0f),
                    top + kShellNavigationVoiceInviteHeight};
    *output = result;
    return true;
}

UiRect ui_voice_elapsed_rectangle(UiRect label, u32 bytes, float gap, float* name_right) noexcept {
    // Subtext digit advance; ':' is narrower, which only leaves slack.
    constexpr float advance = 6.25f;
    const float left = maximum(label.left, label.right - static_cast<float>(bytes) * advance);
    if (name_right)
        *name_right = bytes ? maximum(label.left, left - gap) : label.right;
    const float top = label.top;
    return bytes ? UiRect{left, top, label.right, top + kShellNavigationVoiceHeadHeight - 10.0f} : UiRect{};
}
} // namespace maicord
