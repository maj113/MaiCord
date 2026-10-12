#pragma once

#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/surface_geometry.hpp"

namespace maicord {

enum class UiAudioControl : u8 {
    none,
    play_pause,
    seek,
    time,
    mute,
    volume,
    download,
    status,
    title
};
enum class UiAudioControlKind : u8 {
    button,
    range,
    label
};
enum class UiAudioKey : u8 {
    activate,
    previous,
    next,
    decrease,
    increase,
    home,
    end
};

struct UiAudioPlaybackState {
    // Authority changes invalidate actions; elapsed ticks alone need not change revision.
    u64 owner{}, generation{}, revision{}, elapsed_ms{}, duration_ms{};
    u32 volume{};
    bool playing{}, muted{}, loading{}, error{}, can_play{}, can_seek{}, can_volume{}, can_download{};
    bool has_duration{}, voice{};
    Utf8View title{};
    // Decoded amplitudes, 0..255. The owner retains storage through paint/input.
    // Missing data uses a neutral seek rail, never a fabricated waveform.
    ConstSlice<u8> waveform{};
};

struct UiAudioControlsStyle {
    float padding{8}, gap{6}, target{32}, title_height{20};
    float minimum_width{160}, maximum_width{480}, time_width{112}, volume_width{64};
    float corner_radius{kUiEditCornerRadius}, waveform_gap{2}, minimum_peak_height{2};
    float volume_thumb_extent{6};
    u32 text_style{text_style_subtext};
    Utf8View play{utf8_literal("Play")}, pause{utf8_literal("Pause")};
    Utf8View retry{utf8_literal("Retry")};
    Utf8View mute{utf8_literal("Mute")}, unmute{utf8_literal("Unmute")};
    Utf8View download{utf8_literal("Download")}, loading{utf8_literal("Loading audio…")};
    Utf8View error{utf8_literal("Can't play audio")}, voice_title{utf8_literal("Voice message")};
};

inline constexpr UiAudioControlsStyle kDefaultUiAudioControlsStyle{};
inline constexpr u32 kUiAudioControlCount = 8;
inline constexpr u32 kUiAudioWaveformMaximum = 256;

struct UiAudioCardGeometry {
    UiRect bounds{}, title{}, play_pause{}, seek{}, time{}, mute{}, volume{}, download{}, status{};
    u8 footer_rows{};
    bool voice{}, compact{}, volume_expanded{};
};

struct UiAudioControlDescriptor {
    UiAudioControl id{};
    UiAudioControlKind kind{};
    UiRect bounds{};
    Utf8View label{};
    Color foreground{}, background{};
    u32 value{}, maximum{}, text_style{};
    bool enabled{}, selected{}, visible{true};
};

struct UiAudioControlsProjection {
    UiAudioPlaybackState playback{};
    UiAudioCardGeometry geometry{};
    UiAudioControlDescriptor controls[kUiAudioControlCount]{};
    Color surface{}, focus{}, track{}, waveform{}, played{};
    char time_text[64]{};
    u32 time_bytes{};
    float volume_thumb_inset{};
};

struct UiAudioAction {
    UiAudioControl control{};
    u64 owner{}, generation{}, revision{}, position_ms{};
    u32 volume{};
    bool value{};
};

// One selected borrowed projection; no clocks, allocations or retained child widgets.
// Static cards and the selected player share these exact responsive dimensions.
float ui_audio_card_height(const UiAudioControlsStyle&, float width, bool voice = false) noexcept;
bool ui_audio_card_geometry(UiRect, const UiAudioControlsStyle&, UiAudioCardGeometry*, bool voice = false,
                            bool volume_expanded = false) noexcept;
bool ui_audio_controls_project(const UiAudioPlaybackState&, UiRect, const UiAudioControlsStyle&,
                               const Theme&, UiAudioControlsProjection*,
                               bool volume_expanded = false) noexcept;
const UiAudioControlDescriptor* ui_audio_controls_find(const UiAudioControlsProjection&,
                                                       UiAudioControl) noexcept;
Utf8View ui_audio_controls_label(const UiAudioControlsProjection&, UiAudioControl) noexcept;
UiAudioControl ui_audio_controls_hit(const UiAudioControlsProjection&, UiPoint) noexcept;
// Waveform seek spans its full bounds; volume endpoints are actual thumb centers.
UiRect ui_audio_controls_range_track(const UiAudioControlsProjection&, UiAudioControl) noexcept;
bool ui_audio_controls_pointer_action(const UiAudioControlsProjection&, UiPoint, UiAudioAction*) noexcept;
// Captured range drags clamp outside the range without admitting another control.
bool ui_audio_controls_range_action(const UiAudioControlsProjection&, UiAudioControl, UiPoint,
                                    UiAudioAction*) noexcept;
bool ui_audio_controls_key(const UiAudioControlsProjection&, UiAudioKey, UiAudioControl* focus,
                           UiAudioAction*) noexcept;
bool ui_audio_action_current(const UiAudioPlaybackState&, const UiAudioAction&) noexcept;

enum class UiAudioSourceSurface : u8 {
    timeline,
    search,
    pins,
    forward
};
enum class UiAudioPointerEvent : u8 {
    down,
    move,
    up,
    leave,
    cancel
};
struct UiAudioScene {
    UiAudioControlsProjection projection{};
    UiAudioControlsStyle style{};
    UiRect clip{};
    UiWidgetReference owner{};
    UiAudioControl focus{}, hover{}, capture{};
    UiAudioSourceSurface surface{};
    bool keyboard_focus{}, active{};
};
struct UiAudioSceneResult {
    UiAudioAction action{};
    bool handled{}, request_capture{}, release_capture{}, action_requested{};
};
// Metadata/style labels remain borrowed from the host's publication. Reprojection
// clears capture when authority, owner or geometry changes; the host releases OS capture.
bool ui_audio_scene_project(UiAudioScene*, const UiAudioPlaybackState&, UiRect bounds, UiRect clip,
                            UiWidgetReference owner, UiAudioSourceSurface, const UiAudioControlsStyle&,
                            const Theme&) noexcept;
UiAudioControl ui_audio_scene_hit(const UiAudioScene&, UiPoint) noexcept;
bool ui_audio_scene_hover(UiAudioScene*, UiPoint) noexcept;
bool ui_audio_scene_focus(UiAudioScene*, UiAudioControl, bool keyboard) noexcept;
void ui_audio_scene_blur(UiAudioScene*) noexcept;
UiAudioSceneResult ui_audio_scene_pointer(UiAudioScene*, UiAudioPointerEvent, UiPoint) noexcept;
UiAudioSceneResult ui_audio_scene_key(UiAudioScene*, UiAudioKey) noexcept;

} // namespace maicord
