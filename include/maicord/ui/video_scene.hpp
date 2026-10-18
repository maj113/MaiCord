#pragma once
#include "maicord/ui/video_controls.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/spinner.hpp"
#include "maicord/ui/transition.hpp"

namespace maicord {
inline constexpr u32 kUiVideoControlsFadeMs = 120;
// Pointer idleness after which a playing player's controls hide; expanded and
// fullscreen players use the shorter delay.
inline constexpr u32 kUiVideoControlsIdleMs = 3'000;
inline constexpr u32 kUiVideoControlsExpandedIdleMs = 1'000;
// Host metrics are independent of the control layout and theme colors.
struct UiVideoSceneStyle {
    float radius{kUiControlCornerRadius}, track_height{6}, thumb_size{10}, focus_width{2};
    float inset{0}, icon_extent{kUiVideoControlIconExtent};
    // The bar sits on video, so it is a black scrim rather than a theme surface.
    float surface_opacity{.6f}, preview_radius{kUiControlCornerRadius};
    float hover_opacity{kUiMediaHoverOpacity}, track_opacity{.3f};
    float spinner_extent{32}, scrim_lead{20};
    UiSpinnerStyle spinner{};
    u32 fade_ms{kUiVideoControlsFadeMs};
    u32 idle_ms{kUiVideoControlsIdleMs}; // Zero keeps hovered controls visible.
};
struct UiVideoPlayAffordanceStyle {
    float target{kUiMediaPlayTarget}, minimum_target{24}, inset{8}, radius{kUiMediaPlayTarget * .5f},
        stroke{2};
    float opacity{kUiMediaSurfaceOpacity};
};
struct UiVideoScene {
    UiVideoControlsProjection projection{};
    UiVideoSceneStyle style{};
    UiRect clip{};
    UiRect preview{}, center_play{}, buffering_indicator{};
    char labels[kUiVideoControlCount][129]{};
    u32 label_bytes[kUiVideoControlCount]{};
    // Fingerprint of emitted text, geometry, text style and theme. Subsecond
    // playback ticks, hover, focus and range paint do not invalidate glyphs.
    u64 text_revision{};
    UiVideoControl focus{}, hover{}, capture{};
    float play_opacity{1};
    UiOpacityTransition controls_transition{};
    // Last host-reported pointer/key activity; zero arms on the next tick.
    u64 activity_ms{};
    float hover_x{};
    // Time under the pointer on the seek track ("m:ss"), shown above it while
    // hovered or dragged; text_advance sizes its bubble.
    char hover_time[24]{};
    u32 hover_time_bytes{};
    float text_advance{6.25f};
    u8 spinner_phase{};
    bool active{}, keyboard_focus{}, play_only{};
    bool preview_hovered{}, idle{};
};
enum class UiVideoPointerEvent : u8 {
    down,
    move,
    up,
    leave,
    cancel
};
struct UiVideoSceneResult {
    UiVideoAction action{};
    bool handled{}, request_capture{}, release_capture{}, action_requested{};
};
// Lay out a bottom control bar inside the supplied preview. Widths below the descriptor
// minimum are refused; the host can provide a separate small play overlay.
// Invalid layout deactivates the scene and clears interaction. The host
// must release OS capture after a relayout that clears scene.capture.
// State or layout identity changes cancel capture; elapsed ticks alone do not.
bool ui_video_scene_project(UiVideoScene*, const UiVideoPlaybackState&, UiRect preview, UiRect clip,
                            const UiVideoControlsStyle&, const UiVideoSceneStyle&, const Theme&) noexcept;
// Small/short selected previews keep a play/pause button;
// seeking and audio controls remain available only in the full layout.
bool ui_video_scene_project_play_only(UiVideoScene*, const UiVideoPlaybackState&, UiRect preview,
                                      UiRect clip, const UiVideoControlsStyle&, const UiVideoSceneStyle&,
                                      const UiVideoPlayAffordanceStyle&, const Theme&) noexcept;
UiRect ui_video_play_affordance_rectangle(UiRect preview, const UiVideoPlayAffordanceStyle&) noexcept;
bool ui_video_play_affordance_paint(UiRect preview, UiRect clip, const UiVideoPlayAffordanceStyle&,
                                    const Theme&, PaintStream*) noexcept;
bool ui_video_scene_covers_preview(const UiVideoScene&, UiRect preview) noexcept;
// No internal pointers: labels and time remain valid on a copied scene.
Utf8View ui_video_scene_label(const UiVideoScene&, UiVideoControl) noexcept;
UiVideoControl ui_video_scene_hit(const UiVideoScene&, UiPoint) noexcept;
// Observation only: never dispatches an action or changes capture.
bool ui_video_scene_hover(UiVideoScene*, UiPoint) noexcept;
bool ui_video_scene_focus(UiVideoScene*, UiVideoControl, bool keyboard) noexcept;
void ui_video_scene_blur(UiVideoScene*) noexcept;
UiVideoSceneResult ui_video_scene_pointer(UiVideoScene*, UiVideoPointerEvent, UiPoint) noexcept;
UiVideoSceneResult ui_video_scene_key(UiVideoScene*, UiVideoKey) noexcept;
// Player shortcuts reveal the controls like pointer activity does.
UiVideoSceneResult ui_video_scene_player_key(UiVideoScene*, UiVideoPlayerKey, u32 percent, u64 now_ms) noexcept;
// Actual pointer movement or key input restarts the idle countdown.
bool ui_video_scene_activity(UiVideoScene*, u64 now_ms) noexcept;
// Paint-cache fingerprint; interaction updates are independent of
// text_revision and retained glyph text.
u64 ui_video_scene_paint_revision(const UiVideoScene&) noexcept;
// Emitted glyph alpha varies independently from semantic/layout identity.
u64 ui_video_scene_text_paint_revision(const UiVideoScene&) noexcept;
// Changes paint only; steady buffering never rebuilds glyph text.
bool ui_video_scene_tick(UiVideoScene*, u64 now_ms, bool reduced_motion) noexcept;
u64 ui_video_scene_next_wake(const UiVideoScene&, u64 now_ms, bool reduced_motion) noexcept;
bool ui_video_scene_paint(const UiVideoScene&, const Theme&, PaintStream*) noexcept;
// The seek bubble above the track; empty without a hover time.
UiRect ui_video_scene_hover_bubble(const UiVideoScene&) noexcept;
// Opacity for chrome around the player (the media viewer's toolbar): it fades
// with the controls while the pointer rests on the picture, else stays shown.
float ui_video_scene_chrome_opacity(const UiVideoScene&) noexcept;
// The host supplies a live, generation-checked handle. Keeping it outside the
// scene keeps the scene safe to copy and preserves the shared text ownership contract.
bool ui_video_scene_text_output(const UiVideoScene&, const Theme&, UiTextOutput*,
                                UiWidgetReference text_owner) noexcept;
} // namespace maicord
