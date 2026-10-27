#include "maicord/ui/video_controls.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool positive(float value) noexcept {
    return paint_coordinate_valid(value) && value > 0;
}
bool non_negative(float value) noexcept {
    return paint_coordinate_valid(value) && value >= 0;
}
bool style_valid(const UiVideoControlsStyle& style) noexcept {
    if (!non_negative(style.padding) || !non_negative(style.gap) || !positive(style.target) ||
        !positive(style.range_height) || !positive(style.status_height) || !positive(style.normal_width) ||
        !positive(style.minimum_width) || !positive(style.time_width) || !positive(style.volume_width) ||
        !positive(style.time_advance) || style.normal_width < style.minimum_width ||
        style.minimum_width <
            2 * style.padding + 2 * style.target + 3 * style.gap + style.time_width + style.range_height ||
        style.volume_width < style.range_height * .5f)
        return false;
    const Utf8View labels[]{style.play,      style.pause, style.mute,   style.unmute,  style.unavailable_audio,
                            style.buffering, style.error, style.expand, style.collapse, style.previous_track, style.next_track};
    for (const auto label : labels)
        if (!label.bytes || label.bytes > 128 || !utf8_validate(label).valid)
            return false;
    return true;
}
const UiVideoControlDescriptor* find_control(const UiVideoControlsProjection& projection,
                                             UiVideoControl id) noexcept {
    for (const auto& descriptor : projection.controls)
        if (descriptor.id == id)
            return &descriptor;
    return nullptr;
}
u64 seek_position(u64 duration, u32 value) noexcept {
    return (duration / 10000) * value + (duration % 10000) * value / 10000;
}
u32 seek_progress(u64 elapsed, u64 duration) noexcept {
    if (!duration)
        return 0;
    return static_cast<u32>(static_cast<double>(elapsed) / static_cast<double>(duration) * 10000.0);
}
void append_number(char* text, u32& cursor, u64 value, bool pad_two_digits = false) noexcept {
    if (pad_two_digits && decimal_digits(value) < 2)
        text[cursor++] = '0';
    cursor += decimal_write(text + cursor, value);
}
void append_unknown(char* text, u32& cursor) noexcept {
    static constexpr char unknown[] = "-:--";
    for (u32 i = 0; i < 4; ++i)
        text[cursor++] = unknown[i];
}
UiVideoAction fenced(const UiVideoPlaybackState& playback, UiVideoControl id) noexcept {
    UiVideoAction request{};
    request.control = id;
    request.owner = playback.owner;
    request.generation = playback.generation;
    request.revision = playback.revision;
    return request;
}
bool make_control_action(const UiVideoControlsProjection& projection, UiVideoControl id, u32 value,
                         UiVideoAction* out) noexcept {
    const auto* descriptor = find_control(projection, id);
    if (!out || !descriptor || !descriptor->enabled)
        return false;
    auto request = fenced(projection.playback, id);
    if (id == UiVideoControl::play_pause)
        request.value = !projection.playback.playing;
    else if (id == UiVideoControl::mute)
        request.value = !projection.playback.muted;
    else if (id == UiVideoControl::expand)
        request.value = !projection.playback.expanded;
    else if (id == UiVideoControl::seek)
        request.position_ms = seek_position(projection.playback.duration_ms, minimum(value, 10000u));
    else if (id == UiVideoControl::previous_track)
        request.collection_track = projection.playback.collection_track - 1;
    else if (id == UiVideoControl::next_track)
        request.collection_track = projection.playback.collection_track + 1;
    else if (id == UiVideoControl::track)
        request.collection_track = static_cast<u16>(minimum(value, descriptor->maximum) + 1);
    else if (id == UiVideoControl::volume)
        request.volume = minimum(value, 100u);
    else
        return false;
    *out = request;
    return true;
}
} // namespace
void ui_video_append_clock(char* text, u32& cursor, u64 milliseconds, bool hours_field) noexcept {
    const u64 seconds = milliseconds / 1000, hours = seconds / 3600;
    if (hours || hours_field) {
        append_number(text, cursor, hours);
        text[cursor++] = ':';
    }
    append_number(text, cursor, (seconds / 60) % 60, hours || hours_field);
    text[cursor++] = ':';
    append_number(text, cursor, seconds % 60, true);
}
float ui_video_controls_height(const UiVideoControlsStyle& style, float width, bool collection) noexcept {
    if (!style_valid(style) || !positive(width) || width < style.minimum_width)
        return 0;
    return 2 * style.padding + maximum(style.target, style.range_height) * (collection ? 2 : 1);
}
u32 ui_video_format_time(const UiVideoPlaybackState& state, char* text, u32 capacity) noexcept {
    // Longest output is two 20-digit hour fields plus separators.
    if (!text || capacity < 64)
        return 0;
    if (state.live) { text[0] = 'L'; text[1] = 'I'; text[2] = 'V'; text[3] = 'E'; return 4; }
    u32 cursor = 0;
    const bool hours = state.duration_ms >= 3'600'000;
    if (!state.duration_ms && !state.elapsed_ms)
        append_unknown(text, cursor);
    else
        ui_video_append_clock(text, cursor, state.elapsed_ms, hours);
    text[cursor++] = ' ';
    text[cursor++] = '/';
    text[cursor++] = ' ';
    if (state.duration_ms)
        ui_video_append_clock(text, cursor, state.duration_ms, false);
    else
        append_unknown(text, cursor);
    return cursor;
}
bool ui_video_controls_project(const UiVideoPlaybackState& state, UiRect bounds,
                               const UiVideoControlsStyle& style, const Theme& theme,
                               UiVideoControlsProjection* out, bool volume_expanded) noexcept {
    if (!out || !state.owner || !state.generation || !state.revision || state.volume > 100 ||
        state.collection_count > 8192 || state.collection_track > state.collection_count ||
        (state.collection_count && !state.collection_track) ||
        (state.duration_ms && state.elapsed_ms > state.duration_ms) ||
        !paint_coordinate_valid(bounds.left) || !paint_coordinate_valid(bounds.top) ||
        !paint_coordinate_valid(bounds.right) || !paint_coordinate_valid(bounds.bottom))
        return false;
    const bool collection = state.collection_count != 0;
    const float row_height = maximum(style.target, style.range_height);
    const float width = ui_rect_width(bounds), height = ui_video_controls_height(style, width, collection);
    if (!height || ui_rect_height(bounds) < height)
        return false;
    UiVideoControlsProjection projection{};
    projection.playback = state;
    projection.bounds = bounds;
    projection.compact = width < style.normal_width;
    projection.volume_expanded = volume_expanded && state.has_audio && state.can_volume && !state.error;
    projection.surface = theme.base_lowest;
    projection.focus = theme.tokens.border.focus;
    projection.time_bytes = ui_video_format_time(state, projection.time_text, sizeof(projection.time_text));
    const float left = bounds.left + style.padding;
    const float right = bounds.right - style.padding;
    const float center_y = (bounds.top + bounds.bottom + (collection ? row_height : 0)) * .5f;
    const float button_top = center_y - style.target * .5f;
    const float range_top = center_y - style.range_height * .5f;
    // The time slot follows its glyphs so h:mm:ss never collides with seek.
    float time_width = maximum(style.time_width, projection.time_bytes * style.time_advance);
    const float time_left = left + style.target + style.gap;
    // Optional chrome yields before the seek range: first expand, then an
    // h:mm:ss label too wide for a narrow player (its slot shrinks to the minimum).
    bool expand = state.can_expand, time_clipped = false, swap = false;
    float mute_left = 0, volume_left = 0, seek_left = 0, seek_right = 0;
    for (u32 attempt = 0; attempt < 3; ++attempt) {
        const float trailing = expand ? right - style.target : right;
        mute_left = trailing - style.target;
        // The slider opens beside the speaker. A player too narrow to
        // keep a usable seek range hands the time slot to the slider instead.
        volume_left = mute_left - style.volume_width;
        const float time_right = time_left + time_width + style.gap;
        swap = projection.volume_expanded && volume_left - style.gap - time_right < style.range_height;
        if (swap)
            volume_left = time_left;
        seek_left = swap ? volume_left + style.volume_width + style.gap : time_right;
        seek_right = (projection.volume_expanded && !swap ? volume_left : mute_left) - style.gap;
        if (seek_right - seek_left >= style.range_height)
            break;
        if (expand)
            expand = false;
        else if (!time_clipped && time_width > style.time_width) {
            time_width = style.time_width;
            time_clipped = true;
        } else
            break;
    }
    const float collection_top = button_top - row_height;
    const UiRect rectangles[]{
        {left, button_top, left + style.target, button_top + style.target},
        {seek_left, range_top, seek_right, range_top + style.range_height},
        {time_left, button_top, time_left + time_width, button_top + style.target},
        {mute_left, button_top, mute_left + style.target, button_top + style.target},
        {volume_left, range_top, volume_left + style.volume_width, range_top + style.range_height},
        {left, button_top, right, button_top + style.target},
        {right - style.target, button_top, right, button_top + style.target},
        {left, collection_top, left + style.target, collection_top + style.target},
        {right - style.target, collection_top, right, collection_top + style.target},
        {left + style.target + style.gap, collection_top, right - style.target - style.gap, collection_top + style.range_height}};
    const UiVideoControl ids[]{UiVideoControl::play_pause, UiVideoControl::seek,   UiVideoControl::time,
                               UiVideoControl::mute,       UiVideoControl::volume, UiVideoControl::status,
                               UiVideoControl::expand, UiVideoControl::previous_track, UiVideoControl::next_track, UiVideoControl::track};
    for (u32 i = 0; i < kUiVideoControlCount; ++i) {
        auto& descriptor = projection.controls[i];
        descriptor.id = ids[i];
        descriptor.bounds = rectangles[i];
        descriptor.text_style = style.text_style;
        descriptor.foreground = theme.text_primary;
        descriptor.background = theme.interactive;
        descriptor.kind = i == 1 || i == 4 || i == 9   ? UiVideoControlKind::range
                          : i == 2 || i == 5 ? UiVideoControlKind::label
                                             : UiVideoControlKind::button;
    }
    projection.controls[0].label = state.playing ? style.pause : style.play;
    projection.controls[0].enabled = state.can_play && !state.error;
    projection.controls[0].selected = state.playing;
    projection.controls[1].enabled = !state.live && state.can_seek && state.duration_ms && !state.error;
    projection.controls[1].value = state.live ? 0 : seek_progress(state.elapsed_ms, state.duration_ms);
    projection.controls[1].maximum = 10000;
    projection.controls[3].label = !state.has_audio ? style.unavailable_audio
                                   : state.muted    ? style.unmute
                                                    : style.mute;
    projection.controls[3].enabled = state.has_audio && !state.error;
    projection.controls[3].selected = state.muted;
    projection.controls[4].enabled = state.has_audio && state.can_volume && !state.error;
    projection.controls[4].value = state.volume;
    projection.controls[4].maximum = 100;
    projection.controls[4].visible = projection.volume_expanded;
    projection.controls[2].visible = !swap;
    projection.controls[5].label = state.error       ? style.error
                                   : state.buffering ? style.buffering
                                                     : Utf8View{};
    projection.controls[5].visible = projection.controls[5].label.bytes != 0;
    projection.controls[5].foreground = state.error ? theme.notification : theme.text_secondary;
    projection.controls[6].label = state.expanded ? style.collapse : style.expand;
    projection.controls[6].visible = expand;
    projection.controls[6].enabled = expand && !state.error;
    projection.controls[6].selected = state.expanded;
    projection.controls[7].label = style.previous_track;
    projection.controls[8].label = style.next_track;
    projection.controls[7].enabled = collection && state.can_change_track && state.collection_track > 1;
    projection.controls[8].enabled = collection && state.can_change_track && state.collection_track < state.collection_count;
    projection.controls[9].enabled = collection && state.can_change_track && state.collection_count > 1;
    projection.controls[9].value = collection ? state.collection_track - 1 : 0;
    projection.controls[9].maximum = collection ? state.collection_count - 1 : 0;
    for (u32 i = 7; i < kUiVideoControlCount; ++i) projection.controls[i].visible = collection;
    if (collection) {
        copy_bytes(projection.track_text, "Track ", 6); projection.track_bytes = 6;
        append_number(projection.track_text, projection.track_bytes, state.collection_track);
        copy_bytes(projection.track_text + projection.track_bytes, " / ", 3); projection.track_bytes += 3;
        append_number(projection.track_text, projection.track_bytes, state.collection_count);
        if (state.collection_more) projection.track_text[projection.track_bytes++] = '+';
    }
    for (auto& descriptor : projection.controls)
        if (!descriptor.enabled && descriptor.kind != UiVideoControlKind::label)
            descriptor.foreground = theme.text_secondary;
    // Narrow players temporarily replace time with volume instead of wrapping
    // into another row. The seek and speaker targets remain available.
    if (seek_right - seek_left < style.range_height || seek_left < time_left)
        return false;
    copy_value(out, projection);
    return true;
}
Utf8View ui_video_controls_label(const UiVideoControlsProjection& projection, UiVideoControl id) noexcept {
    if (id == UiVideoControl::track) return {projection.track_text, minimum<u32>(projection.track_bytes, sizeof(projection.track_text))};
    if (id == UiVideoControl::time)
        return projection.time_bytes <= sizeof(projection.time_text)
                   ? Utf8View{projection.time_text, projection.time_bytes}
                   : Utf8View{};
    const auto* descriptor = find_control(projection, id);
    return descriptor ? descriptor->label : Utf8View{};
}
UiVideoControl ui_video_controls_hit(const UiVideoControlsProjection& projection, UiPoint point) noexcept {
    if (!paint_coordinate_valid(point.x) || !paint_coordinate_valid(point.y))
        return UiVideoControl::none;
    for (const auto& descriptor : projection.controls)
        if (descriptor.visible && descriptor.enabled && point.x >= descriptor.bounds.left &&
            point.x < descriptor.bounds.right && point.y >= descriptor.bounds.top &&
            point.y < descriptor.bounds.bottom)
            return descriptor.id;
    return UiVideoControl::none;
}
bool ui_video_controls_pointer_action(const UiVideoControlsProjection& projection, UiPoint point,
                                      UiVideoAction* out) noexcept {
    const auto id = ui_video_controls_hit(projection, point);
    const auto* descriptor = find_control(projection, id);
    if (!descriptor)
        return false;
    const float ratio = (point.x - descriptor->bounds.left) / ui_rect_width(descriptor->bounds);
    return make_control_action(projection, id, static_cast<u32>(ratio * descriptor->maximum + .5f), out);
}
bool ui_video_controls_key(const UiVideoControlsProjection& projection, UiVideoKey key,
                           UiVideoControl* focus, UiVideoAction* out) noexcept {
    if (!focus)
        return false;
    static constexpr UiVideoControl order[]{UiVideoControl::play_pause, UiVideoControl::seek, UiVideoControl::mute,
                                     UiVideoControl::volume, UiVideoControl::expand, UiVideoControl::previous_track, UiVideoControl::track, UiVideoControl::next_track};
    constexpr u32 count = sizeof(order) / sizeof(order[0]);
    if (key == UiVideoKey::previous || key == UiVideoKey::next) {
        u32 cursor = count;
        for (u32 i = 0; i < count; ++i)
            if (order[i] == *focus)
                cursor = i;
        for (u32 i = 0; i < count; ++i) {
            cursor = cursor == count ? (key == UiVideoKey::next ? 0 : count - 1)
                                     : (key == UiVideoKey::next ? (cursor + 1) % count
                                                                : (cursor + count - 1) % count);
            const auto* descriptor = find_control(projection, order[cursor]);
            if (descriptor && descriptor->enabled) {
                *focus = order[cursor];
                if (out)
                    reset_value(out);
                return true;
            }
        }
        *focus = UiVideoControl::none;
        return false;
    }
    const auto* descriptor = find_control(projection, *focus);
    if (!descriptor || !descriptor->enabled)
        return false;
    if (key == UiVideoKey::activate)
        return descriptor->kind == UiVideoControlKind::button &&
               make_control_action(projection, *focus, descriptor->value, out);
    if (descriptor->kind != UiVideoControlKind::range)
        return false;
    u32 value = descriptor->value;
    const u32 step = *focus == UiVideoControl::seek ? 500 : *focus == UiVideoControl::track ? 1 : kUiVideoVolumeStep;
    if (key == UiVideoKey::home)
        value = 0;
    else if (key == UiVideoKey::end)
        value = descriptor->maximum;
    else if (key == UiVideoKey::decrease)
        value = value > step ? value - step : 0;
    else if (key == UiVideoKey::increase)
        value = minimum(value + step, descriptor->maximum);
    else
        return false;
    return make_control_action(projection, *focus, value, out);
}
bool ui_video_controls_player_key(const UiVideoControlsProjection& projection, UiVideoPlayerKey key,
                                  u32 percent, UiVideoAction* out) noexcept {
    if (!out)
        return false;
    const auto& playback = projection.playback;
    const auto enabled = [&](UiVideoControl id) noexcept {
        const auto* descriptor = find_control(projection, id);
        return descriptor && descriptor->enabled;
    };
    switch (key) {
    case UiVideoPlayerKey::toggle_play:
        return make_control_action(projection, UiVideoControl::play_pause, 0, out);
    case UiVideoPlayerKey::toggle_mute:
        return make_control_action(projection, UiVideoControl::mute, 0, out);
    case UiVideoPlayerKey::expand:
        // Expansion is a host surface change, so a narrow layout that hid the
        // button still accepts the shortcut while the capability is present.
        if (!playback.can_expand || playback.error)
            return false;
        *out = fenced(playback, UiVideoControl::expand);
        out->value = !playback.expanded;
        return true;
    case UiVideoPlayerKey::volume_down:
    case UiVideoPlayerKey::volume_up: {
        if (!enabled(UiVideoControl::volume))
            return false;
        const u32 volume = playback.volume;
        return make_control_action(projection, UiVideoControl::volume,
                                   key == UiVideoPlayerKey::volume_up
                                       ? minimum(volume + kUiVideoVolumeStep, 100u)
                                       : (volume > kUiVideoVolumeStep ? volume - kUiVideoVolumeStep : 0),
                                   out);
    }
    case UiVideoPlayerKey::seek_backward:
    case UiVideoPlayerKey::seek_forward:
    case UiVideoPlayerKey::seek_percent: {
        if (!enabled(UiVideoControl::seek) || (key == UiVideoPlayerKey::seek_percent && percent > 100))
            return false;
        const u64 duration = playback.duration_ms, elapsed = minimum(playback.elapsed_ms, duration);
        u64 position = 0;
        if (key == UiVideoPlayerKey::seek_percent)
            position = (duration / 100) * percent + (duration % 100) * percent / 100;
        else if (key == UiVideoPlayerKey::seek_forward)
            position = minimum(elapsed + kUiVideoSeekStepMs, duration);
        else
            position = elapsed > kUiVideoSeekStepMs ? elapsed - kUiVideoSeekStepMs : 0;
        *out = fenced(playback, UiVideoControl::seek);
        out->position_ms = position;
        return true;
    }
    }
    return false;
}
bool ui_video_action_current(const UiVideoPlaybackState& playback, const UiVideoAction& request) noexcept {
    return playback.owner && playback.generation && playback.revision && request.owner == playback.owner &&
           request.generation == playback.generation && request.revision == playback.revision;
}
} // namespace maicord
