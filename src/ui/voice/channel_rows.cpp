#include "maicord/ui/voice/channel_rows.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/ui/shell/navigation_scene.hpp"
#include "maicord/ui/voice/panel.hpp"

namespace maicord {
bool ui_voice_participant_row(const ChannelViewRow& row, const UiRect& row_rectangle,
                              const VoiceStateStore& store, u32 index,
                              UiVoiceParticipantRow* output) noexcept {
    if (!output || index >= row.voice_count)
        return false;
    const auto* state = voice_state_store_channel_at(store, row.id, index);
    if (!state)
        return false;
    const float top = row_rectangle.top + shell_navigation_voice_top(row) +
                      static_cast<float>(index) * kShellNavigationVoiceUserHeight;
    // 32-DIP rows with a 24-DIP avatar under the channel name, 16-DIP state
    // icons 4 DIPs apart at the highlight's end.
    const float left = row_rectangle.left + kShellChannelLabelLeft + static_cast<float>(row.depth) * kShellChannelIndent;
    const float right = row_rectangle.right - kShellChannelHighlightRight - kShellChannelHighlightLeft;
    UiVoiceParticipantRow result{state};
    result.speaking = (state->flags & voice_speaking) != 0;
    result.row = {row_rectangle.left, top, row_rectangle.right, top + kShellNavigationVoiceUserHeight};
    result.avatar = {left, top + 4.0f, left + 24.0f, top + 28.0f};
    float indicator = right;
    const auto icon = [&](UiRect* box) noexcept {
        *box = {indicator - 16.0f, top + 8.0f, indicator, top + 24.0f};
        indicator -= 20.0f;
    };
    if (state->flags & (voice_deaf | voice_self_deaf))
        icon(&result.deafened);
    if (state->flags & (voice_mute | voice_self_mute))
        icon(&result.muted);
    if (state->flags & voice_self_video)
        icon(&result.video);
    if (voice_state_live(*state)) {
        const float live_top = top + (kShellNavigationVoiceUserHeight - kUiVoiceLiveBadgeHeight) * .5f;
        result.live = {indicator - kUiVoiceLiveBadgeWidth, live_top, indicator, live_top + kUiVoiceLiveBadgeHeight};
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

u32 ui_voice_duration_text(u64 elapsed_s, char* output, u32 capacity) noexcept {
    if (!output || capacity < kUiVoiceDurationBytes)
        return 0;
    const u64 amounts[3]{elapsed_s / 3'600, elapsed_s / 60 % 60, elapsed_s % 60};
    const char* const units[3]{" hour", " minute", " second"};
    u32 used = 0;
    for (u32 i = 0; i < 3; ++i) {
        if (!amounts[i] && (i < 2 || used))
            continue;
        if (used) {
            output[used++] = ',';
            output[used++] = ' ';
        }
        used += decimal_write(output + used, amounts[i]);
        for (const char* unit = units[i]; *unit; ++unit)
            output[used++] = *unit;
        if (amounts[i] != 1)
            output[used++] = 's';
    }
    return used;
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
    const float left = row_rectangle.left + kShellChannelLabelLeft + static_cast<float>(row.depth) * kShellChannelIndent;
    const float top = row_rectangle.top + shell_navigation_head_height() - 4.0f;
    return {left, top, maximum(left, row_rectangle.right - 10.0f), top + kShellNavigationStageTopicHeight};
}

bool ui_voice_invite_row(const ChannelViewRow& row, const UiRect& row_rectangle, UiVoiceInviteRow* output) noexcept {
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
} // namespace maicord
