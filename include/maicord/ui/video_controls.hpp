#pragma once
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/surface_geometry.hpp"

namespace maicord {
enum class UiVideoControl : u8 { none, play_pause, seek, time, mute, volume, status, expand };
enum class UiVideoControlKind : u8 { button, range, label };
enum class UiVideoKey : u8 { activate, previous, next, decrease, increase, home, end };
// Player-level shortcuts, independent of which descriptor holds focus.
enum class UiVideoPlayerKey : u8 {
    toggle_play,
    seek_backward,
    seek_forward,
    seek_percent,
    toggle_mute,
    volume_down,
    volume_up,
    expand
};
struct UiVideoPlaybackState {
    // Revision identifies interaction authority/capabilities, not each elapsed
    // time tick; the caller decides when outstanding actions become stale.
    u64 owner{}, generation{}, revision{}, elapsed_ms{}, duration_ms{};
    u32 volume{}; // Actual output volume, 0..100; UI never fabricates playback.
    bool playing{}, muted{}, buffering{}, error{}, can_play{}, can_seek{}, has_audio{}, can_volume{};
    bool can_expand{}, expanded{};
    // Buffered media in 1/10000 of the duration, drawn on the seek track like the official player.
    u16 buffered_begin[4]{}, buffered_end[4]{};
    u8 buffered_count{};
};
// The official inline player: a 32 DIP bar, 24 DIP glyphs in 32 DIP targets and
// 12-13 DIP tabular time. Seek/volume keys step by these amounts.
inline constexpr float kUiVideoControlBarHeight = 32.0f;
inline constexpr float kUiVideoControlIconExtent = 24.0f;
inline constexpr u64 kUiVideoSeekStepMs = 5'000;
inline constexpr u32 kUiVideoVolumeStep = 5;
struct UiVideoControlsStyle {
    float padding{0}, gap{4};
    float target{kUiVideoControlBarHeight}, range_height{kUiVideoControlBarHeight}, status_height{20};
    float normal_width{360}, minimum_width{196}, time_width{56}, volume_width{72};
    // Per-glyph advance used to size the time slot; the UI face's digits are
    // tabular, so the slot never jitters as the clock advances.
    float time_advance{6.25f};
    u32 text_style{text_style_subtext};
    Utf8View play{utf8_literal("Play")}, pause{utf8_literal("Pause")};
    Utf8View mute{utf8_literal("Mute")}, unmute{utf8_literal("Unmute")};
    Utf8View unavailable_audio{utf8_literal("Audio unavailable")};
    Utf8View buffering{utf8_literal("Buffering…")}, error{utf8_literal("Playback unavailable")};
    Utf8View expand{utf8_literal("Expand")}, collapse{utf8_literal("Exit expanded view")};
};
struct UiVideoControlDescriptor {
    UiVideoControl id{};
    UiVideoControlKind kind{};
    UiRect bounds{};
    Utf8View label{};
    Color foreground{}, background{};
    u32 value{}, maximum{}, text_style{};
    bool enabled{}, selected{};
    bool visible{true}; // Hidden volume remains available to keyboard navigation.
};
inline constexpr u32 kUiVideoControlCount = 7;
struct UiVideoControlsProjection {
    UiVideoPlaybackState playback{};
    UiVideoControlDescriptor controls[kUiVideoControlCount]{};
    UiRect bounds{};
    Color surface{}, focus{};
    char time_text[64]{};
    u32 time_bytes{};
    bool compact{};
    bool volume_expanded{};
};
struct UiVideoAction {
    UiVideoControl control{};
    u64 owner{}, generation{}, revision{}, position_ms{};
    u32 volume{};
    bool value{}; // Requested playing/muted/expanded value, never an optimistic state.
};
// One selected player. No allocations, retained widgets, clocks or OS calls.
// Metrics and labels are host-controlled reusable descriptors; the current
// plugin ABI does not itself register media control surfaces. Borrowed labels
// remain valid until the next projection. Time text is accessed from its owning
// projection, so copying projections never retains a pointer to the old buffer.
float ui_video_controls_height(const UiVideoControlsStyle&, float width) noexcept;
bool ui_video_controls_project(const UiVideoPlaybackState&, UiRect,
    const UiVideoControlsStyle&, const Theme&, UiVideoControlsProjection*, bool volume_expanded = false) noexcept;
// "m:ss / m:ss", hours when the duration needs them and "-:--" before metadata.
u32 ui_video_format_time(const UiVideoPlaybackState&, char* text, u32 capacity) noexcept;
Utf8View ui_video_controls_label(const UiVideoControlsProjection&, UiVideoControl) noexcept;
UiVideoControl ui_video_controls_hit(const UiVideoControlsProjection&, UiPoint) noexcept;
bool ui_video_controls_pointer_action(const UiVideoControlsProjection&, UiPoint, UiVideoAction*) noexcept;
// Tab/Shift+Tab move focus; arrows/Home/End on ranges emit bounded requests.
// Caller owns focus/capture and revalidates owner/generation/revision before use.
bool ui_video_controls_key(const UiVideoControlsProjection&, UiVideoKey,
    UiVideoControl* focus, UiVideoAction*) noexcept;
// Space/arrows/digits/M/F semantics. Percent is used only by seek_percent (0..100).
bool ui_video_controls_player_key(const UiVideoControlsProjection&, UiVideoPlayerKey, u32 percent,
    UiVideoAction*) noexcept;
bool ui_video_action_current(const UiVideoPlaybackState&, const UiVideoAction&) noexcept;
}
