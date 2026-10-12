#include "maicord/ui/voice_channel_rows.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/voice_panel.hpp"

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
} // namespace maicord
