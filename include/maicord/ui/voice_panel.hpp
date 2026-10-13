#pragma once
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {
// Voice connection card floating above the account panel, plus the account
// panel's microphone and deafen buttons. Geometry here is shared by paint,
// text emission and input so all three agree.
enum class UiVoicePanelState : u8 {
    hidden,
    connecting,
    connected,
    failed
};
enum class UiVoiceSignal : u8 {
    unknown,
    good,
    fair,
    poor
};
enum class UiVoiceControl : u8 {
    none,
    mute,
    deafen,
    disconnect,
    channel
};
struct UiVoicePanelView {
    u64 guild_id, channel_id;
    UiVoicePanelState state;
    UiVoiceSignal signal;
    bool self_mute, self_deaf, server_mute, server_deaf, speaking;
    UiVoiceControl hovered, pressed;
};
inline constexpr float kUiVoicePanelHeight = 52, kUiVoicePanelGap = 8;
// Extra sidebar height reserved above the account panel while the card shows.
constexpr float ui_voice_panel_reserve(const UiVoicePanelView* view) noexcept {
    return view && view->state != UiVoicePanelState::hidden ? kUiVoicePanelHeight + kUiVoicePanelGap : 0;
}
UiRect ui_voice_panel_rectangle(UiRect footer) noexcept;
UiRect ui_voice_panel_disconnect(UiRect panel) noexcept;
UiRect ui_voice_panel_status_text(UiRect panel) noexcept;
UiRect ui_voice_panel_channel_text(UiRect panel) noexcept;
// Microphone and deafen buttons left of the settings button in the account panel.
UiRect ui_voice_account_button(UiRect footer, UiVoiceControl control) noexcept;
// Right edge reserved by the account panel buttons (settings included).
inline constexpr float kUiVoiceAccountButtonsWidth = 108;
UiVoiceControl ui_voice_hit(UiRect footer, const UiVoicePanelView& view, float x, float y) noexcept;
Color ui_voice_panel_status_color(const UiVoicePanelView& view, const Theme& theme) noexcept;
bool ui_voice_panel_paint(PaintStream* stream, const UiVoicePanelView& view, UiRect footer,
                          const Theme& theme) noexcept;
bool ui_voice_account_controls_paint(PaintStream* stream, const UiVoicePanelView& view, UiRect footer,
                                     const Theme& theme) noexcept;
// The user's avatar at the left of the account panel.
constexpr UiRect ui_account_avatar_rectangle(UiRect footer) noexcept {
    const float top = (footer.top + footer.bottom - 32) * .5f;
    return {footer.left + 10, top, footer.left + 42, top + 32};
}
// While the user speaks in a connected call: a success-coloured disc 2 DIP
// larger than the avatar, painted beneath it as voice channel rows do.
bool ui_account_speaking_ring_paint(PaintStream* stream, const UiVoicePanelView* view, UiRect avatar,
                                    const Theme& theme) noexcept;
} // namespace maicord
