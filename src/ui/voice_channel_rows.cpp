#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/voice_panel.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

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
bool set_label(UiVoiceElapsedClock* clock, u64 second) noexcept {
    char text[kUiVoiceElapsedBytes];
    const u32 bytes = ui_voice_elapsed_format(second * 1'000, text, sizeof(text));
    clock->shown_second = second;
    if (bytes == clock->label_bytes && bytes_equal(text, clock->label, bytes))
        return false;
    copy_bytes(clock->label, text, bytes);
    clock->label_bytes = static_cast<u8>(bytes);
    return true;
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

bool ui_voice_elapsed_observe(UiVoiceElapsedClock* clock, u64 channel_id, bool established, u64 now_ms) noexcept {
    if (!clock)
        return false;
    if (channel_id != clock->channel_id) {
        const bool had_label = clock->label_bytes != 0;
        *clock = {};
        clock->channel_id = channel_id;
        if (!channel_id || !established)
            return had_label;
    }
    if (!channel_id || clock->started_ms || !established)
        return false;
    clock->started_ms = now_ms ? now_ms : 1;
    return set_label(clock, 0);
}

bool ui_voice_elapsed_tick(UiVoiceElapsedClock* clock, u64 now_ms, bool visible) noexcept {
    if (!clock)
        return false;
    clock->visible = visible;
    if (!clock->started_ms)
        return false;
    const u64 second = now_ms > clock->started_ms ? (now_ms - clock->started_ms) / 1'000 : 0;
    return second != clock->shown_second && set_label(clock, second) && visible;
}

u64 ui_voice_elapsed_next_wake(const UiVoiceElapsedClock& clock) noexcept {
    return clock.started_ms && clock.visible ? clock.started_ms + (clock.shown_second + 1) * 1'000 : 0;
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
