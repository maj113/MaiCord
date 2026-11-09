#include "maicord/ui/voice/panel.hpp"
#include "maicord/ui/theme/background_presentation.hpp"
#include "maicord/ui/theme/background_surface.hpp"
#include "maicord/ui/widgets/icon.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/theme/styles.hpp"

namespace maicord {
namespace {
UiRect square(float right, float middle, float size) noexcept {
    return {right - size, middle - size * .5f, right, middle + size * .5f};
}
bool button(PaintStream* stream, const UiRect& r, UiIcon icon, const Color& ink, bool hovered, bool pressed, bool active,
            const Theme& theme, const Color* active_tint = nullptr) noexcept {
    if (active) {
        Color tint = active_tint ? *active_tint : theme.notification;
        tint.alpha = hovered ? .22f : .14f;
        if (!paint_stream_push_rounded(stream, r, tint, theme.scale.radius_sm))
            return false;
    } else if (hovered || pressed) {
        const Color fill = ui_background_hover_color(theme, theme.surface_high, pressed ? .32f : .20f);
        if (!paint_stream_push_rounded(stream, r, fill, theme.scale.radius_sm))
            return false;
    }
    return ui_icon_paint(stream, icon, ui_rect_shrink(r, 7), ink);
}
// The screen-share slot, whether or not the button shows.
UiRect share_slot(const UiRect& panel) noexcept {
    const UiRect leave = ui_voice_panel_disconnect(panel);
    return square(leave.left - 4, (panel.top + panel.bottom) * .5f, 32);
}
} // namespace

UiRect ui_voice_panel_rectangle(const UiRect& footer) noexcept {
    return {footer.left, footer.top - kUiVoicePanelGap - kUiVoicePanelHeight, footer.right,
            footer.top - kUiVoicePanelGap};
}
UiRect ui_voice_panel_disconnect(const UiRect& panel) noexcept {
    return square(panel.right - 8, (panel.top + panel.bottom) * .5f, 32);
}
UiRect ui_voice_panel_screen_share(const UiRect& panel, const UiVoicePanelView& view) noexcept {
    return view.state == UiVoicePanelState::connected ? share_slot(panel) : UiRect{};
}
UiRect ui_voice_panel_status_text(const UiRect& panel) noexcept {
    return {panel.left + 34, panel.top + 7, share_slot(panel).left - 4, panel.top + 26};
}
UiRect ui_voice_panel_channel_text(const UiRect& panel) noexcept {
    return {panel.left + 12, panel.top + 27, share_slot(panel).left - 4, panel.bottom - 7};
}
UiRect ui_voice_account_button(const UiRect& footer, UiVoiceControl control) noexcept {
    const float middle = (footer.top + footer.bottom) * .5f;
    // Settings sits at right - 4; each button is 32 DIP with a 4 DIP gap.
    return control == UiVoiceControl::deafen ? square(footer.right - 40, middle, 32)
           : control == UiVoiceControl::mute ? square(footer.right - 76, middle, 32)
                                             : UiRect{};
}
UiVoiceControl ui_voice_hit(const UiRect& footer, const UiVoicePanelView& view, float x, float y) noexcept {
    if (ui_rect_contains(ui_voice_account_button(footer, UiVoiceControl::mute), x, y))
        return UiVoiceControl::mute;
    if (ui_rect_contains(ui_voice_account_button(footer, UiVoiceControl::deafen), x, y))
        return UiVoiceControl::deafen;
    if (view.state == UiVoicePanelState::hidden)
        return UiVoiceControl::none;
    const UiRect panel = ui_voice_panel_rectangle(footer);
    if (ui_rect_contains(ui_voice_panel_disconnect(panel), x, y))
        return UiVoiceControl::disconnect;
    if (ui_rect_contains(ui_voice_panel_screen_share(panel, view), x, y))
        return UiVoiceControl::screen_share;
    // "Voice connected" and the channel line both show the call view.
    if (ui_rect_contains(ui_voice_panel_channel_text(panel), x, y) ||
        ui_rect_contains(ui_voice_panel_status_text(panel), x, y))
        return UiVoiceControl::channel;
    return UiVoiceControl::none;
}
Color ui_voice_panel_status_color(const UiVoicePanelView& view, const Theme& theme) noexcept {
    if (view.state == UiVoicePanelState::failed ||
        (view.state == UiVoicePanelState::connected && view.signal == UiVoiceSignal::poor))
        return ui_icon_role_color(theme, UiIconRole::danger);
    if (view.state == UiVoicePanelState::connecting || view.signal == UiVoiceSignal::fair)
        return ui_icon_role_color(theme, UiIconRole::warning);
    return ui_icon_role_color(theme, UiIconRole::success);
}
bool ui_account_speaking_ring_paint(PaintStream* stream, const UiVoicePanelView* view, const UiRect& avatar,
                                    const Theme& theme) noexcept {
    if (!view || !view->speaking || view->state != UiVoicePanelState::connected)
        return true;
    return paint_stream_push_rounded(stream, avatar.left - 2, avatar.top - 2, avatar.right + 2, avatar.bottom + 2,
                                     ui_icon_role_color(theme, UiIconRole::success),
                                     ui_rect_width(avatar) * .5f + 2);
}
bool ui_voice_panel_paint(PaintStream* stream, const UiVoicePanelView& view, const UiRect& footer,
                          const Theme& theme) noexcept {
    if (view.state == UiVoicePanelState::hidden)
        return true;
    const UiRect panel = ui_voice_panel_rectangle(footer);
    const UiIcon signal = view.state != UiVoicePanelState::connected ? UiIcon::signal_zero
                          : view.signal == UiVoiceSignal::poor       ? UiIcon::signal_low
                          : view.signal == UiVoiceSignal::fair       ? UiIcon::signal_medium
                                                                     : UiIcon::signal_high;
    const UiRect leave = ui_voice_panel_disconnect(panel);
    const bool hovered = view.hovered == UiVoiceControl::disconnect;
    const UiRect share = ui_voice_panel_screen_share(panel, view);
    const bool share_hovered = view.share_enabled && view.hovered == UiVoiceControl::screen_share;
    Color share_ink = view.share_live    ? ui_icon_role_color(theme, UiIconRole::success)
                      : view.share_enabled ? theme.text_primary
                                           : theme.text_secondary;
    if (!view.share_enabled)
        share_ink.alpha *= .45f;
    return ui_background_paint_surface(stream, panel, theme, theme.surface_high, theme.interactive,
                                       ui_theme_metrics(theme.styles).edit_radius, 1, 1,
                                       UiBackgroundSurfaceMode::gradient, UiBackgroundTier::highest,
                                       UiBackgroundBorderTone::palette) &&
           ui_icon_paint(stream, signal, {panel.left + 11, panel.top + 8, panel.left + 28, panel.top + 25},
                         ui_voice_panel_status_color(view, theme)) &&
           (!ui_rect_has_area(share) ||
            button(stream, share, UiIcon::screen_share, share_ink, share_hovered,
                   share_hovered && view.pressed == UiVoiceControl::screen_share, view.share_live, theme,
                   &theme.tokens.accent.success)) &&
           button(stream, leave, UiIcon::phone_off, hovered ? theme.notification : theme.text_primary, hovered,
                  view.pressed == UiVoiceControl::disconnect, false, theme);
}
bool ui_voice_account_controls_paint(PaintStream* stream, const UiVoicePanelView& view, const UiRect& footer,
                                     const Theme& theme) noexcept {
    const bool mic_off = view.self_mute || view.self_deaf || view.server_mute;
    const bool deaf = view.self_deaf || view.server_deaf;
    const UiRect mic = ui_voice_account_button(footer, UiVoiceControl::mute);
    const UiRect ears = ui_voice_account_button(footer, UiVoiceControl::deafen);
    return button(stream, mic, mic_off ? UiIcon::mic_off : UiIcon::mic,
                  mic_off ? theme.notification : theme.text_secondary, view.hovered == UiVoiceControl::mute,
                  view.pressed == UiVoiceControl::mute, mic_off, theme) &&
           button(stream, ears, deaf ? UiIcon::headphones_off : UiIcon::headphones,
                  deaf ? theme.notification : theme.text_secondary, view.hovered == UiVoiceControl::deafen,
                  view.pressed == UiVoiceControl::deafen, deaf, theme);
}
} // namespace maicord
