#include "maicord/ui/audio_controls.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool positive(float value) noexcept {
    return paint_coordinate_valid(value) && value > 0;
}
bool style_valid(const UiAudioControlsStyle& style) noexcept {
    if (!positive(style.padding) || !positive(style.gap) || !positive(style.target) ||
        !positive(style.title_height) || !positive(style.minimum_width) || !positive(style.maximum_width) ||
        !positive(style.time_width) || !positive(style.volume_width) ||
        !paint_coordinate_valid(style.corner_radius) || style.corner_radius < 0 ||
        !positive(style.waveform_gap) || !positive(style.minimum_peak_height) ||
        !positive(style.volume_thumb_extent) || style.volume_thumb_extent >= style.target ||
        style.maximum_width < style.minimum_width || style.volume_width < style.target ||
        style.time_width < style.volume_width || style.minimum_peak_height > style.target ||
        style.minimum_width < 2 * style.padding + 3 * style.target + 2 * style.gap)
        return false;
    const Utf8View labels[]{style.play,     style.pause,   style.retry, style.mute,       style.unmute,
                            style.download, style.loading, style.error, style.voice_title};
    for (const auto label : labels)
        if (!label.bytes || label.bytes > 128 || !utf8_validate(label).valid)
            return false;
    return true;
}
void append_number(char* text, u32& cursor, u64 value, bool padded = false) noexcept {
    char digits[24];
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    if (padded && count < 2)
        text[cursor++] = '0';
    while (count)
        text[cursor++] = digits[--count];
}
void append_time(char* text, u32& cursor, u64 milliseconds) noexcept {
    const u64 seconds = milliseconds / 1000, hours = seconds / 3600;
    if (hours) {
        append_number(text, cursor, hours);
        text[cursor++] = ':';
    }
    append_number(text, cursor, (seconds / 60) % 60, hours != 0);
    text[cursor++] = ':';
    append_number(text, cursor, seconds % 60, true);
}
bool action(const UiAudioControlsProjection& projection, UiAudioControl id, u32 value,
            UiAudioAction* out) noexcept {
    const auto* descriptor = ui_audio_controls_find(projection, id);
    if (!out || !descriptor || !descriptor->enabled)
        return false;
    const auto& state = projection.playback;
    UiAudioAction request{};
    request.control = id;
    request.owner = state.owner;
    request.generation = state.generation;
    request.revision = state.revision;
    switch (id) {
    case UiAudioControl::play_pause:
        request.value = state.error || !state.playing;
        break;
    case UiAudioControl::mute:
        request.value = !state.muted;
        break;
    case UiAudioControl::seek:
        value = minimum(value, 10000u);
        request.position_ms =
            (state.duration_ms / 10000) * value + (state.duration_ms % 10000) * value / 10000;
        break;
    case UiAudioControl::volume:
        request.volume = minimum(value, 100u);
        break;
    case UiAudioControl::download:
        break;
    default:
        return false;
    }
    *out = request;
    return true;
}
} // namespace

float ui_audio_card_height(const UiAudioControlsStyle& style, float width, bool voice) noexcept {
    if (!style_valid(style) || !positive(width) || width < style.minimum_width)
        return 0;
    return 2 * style.padding + 2 * style.target + style.gap + (voice ? 0 : style.title_height + style.gap);
}
bool ui_audio_card_geometry(UiRect bounds, const UiAudioControlsStyle& style, UiAudioCardGeometry* out,
                            bool voice, bool volume_expanded) noexcept {
    if (!out || !paint_coordinate_valid(bounds.left) || !paint_coordinate_valid(bounds.top) ||
        !paint_coordinate_valid(bounds.right) || !paint_coordinate_valid(bounds.bottom))
        return false;
    const float width = minimum(ui_rect_width(bounds), style.maximum_width);
    const float height = ui_audio_card_height(style, width, voice);
    if (!height || ui_rect_height(bounds) < height)
        return false;
    UiAudioCardGeometry geometry{};
    geometry.bounds = {bounds.left, bounds.top, bounds.left + width, bounds.top + height};
    geometry.voice = voice;
    geometry.footer_rows = 1;
    const float left = bounds.left + style.padding, right = geometry.bounds.right - style.padding;
    float top = bounds.top + style.padding;
    if (!voice) {
        geometry.title = {left, top, right, top + style.title_height};
        top += style.title_height + style.gap;
    }
    geometry.play_pause = {left, top, left + style.target, top + style.target};
    geometry.seek = {left + style.target + style.gap, top, right, top + style.target};
    geometry.status = geometry.seek;
    top += style.target + style.gap;
    geometry.download = {right - style.target, top, right, top + style.target};
    geometry.mute = {geometry.download.left - style.gap - style.target, top,
                     geometry.download.left - style.gap, top + style.target};
    const float remaining = geometry.mute.left - style.gap - left;
    geometry.compact = remaining < style.time_width + style.gap + style.volume_width;
    const float time_width = geometry.compact ? minimum(style.time_width, remaining) : style.time_width;
    geometry.time = {left, top, left + time_width, top + style.target};
    geometry.volume = geometry.compact ? UiRect{left, top, left + minimum(style.volume_width, remaining),
                                                top + style.target}
                                       : UiRect{geometry.mute.left - style.gap - style.volume_width, top,
                                                geometry.mute.left - style.gap, top + style.target};
    geometry.volume_expanded = !geometry.compact || volume_expanded;
    if (ui_rect_width(geometry.seek) < style.target || ui_rect_width(geometry.time) < style.target ||
        ui_rect_width(geometry.volume) < style.target)
        return false;
    *out = geometry;
    return true;
}
bool ui_audio_controls_project(const UiAudioPlaybackState& state, UiRect bounds,
                               const UiAudioControlsStyle& style, const Theme& theme,
                               UiAudioControlsProjection* out, bool volume_expanded) noexcept {
    if (!out || !state.owner || !state.generation || !state.revision || state.volume > 100 ||
        (state.has_duration && state.elapsed_ms > state.duration_ms) ||
        state.waveform.count > kUiAudioWaveformMaximum || (state.waveform.count && !state.waveform.data) ||
        state.title.bytes > 512 ||
        (state.title.bytes && (!state.title.data || !utf8_validate(state.title).valid)))
        return false;
    UiAudioControlsProjection projection{};
    if (!ui_audio_card_geometry(bounds, style, &projection.geometry, state.voice,
                                volume_expanded && state.can_volume && !state.error))
        return false;
    projection.playback = state;
    projection.volume_thumb_inset = style.volume_thumb_extent * .5f;
    projection.surface = theme.surface;
    projection.focus = theme.tokens.border.focus;
    projection.track = theme.interactive;
    projection.waveform = theme.text_secondary;
    projection.played = theme.tokens.emphasis.outline;
    append_time(projection.time_text, projection.time_bytes, state.elapsed_ms);
    if (projection.geometry.compact)
        projection.time_text[projection.time_bytes++] = '\n';
    else {
        projection.time_text[projection.time_bytes++] = ' ';
        projection.time_text[projection.time_bytes++] = '/';
        projection.time_text[projection.time_bytes++] = ' ';
    }
    if (state.has_duration)
        append_time(projection.time_text, projection.time_bytes, state.duration_ms);
    else {
        constexpr char unknown[] = "--:--";
        for (u32 i = 0; i < 5; ++i)
            projection.time_text[projection.time_bytes++] = unknown[i];
    }
    const auto& geometry = projection.geometry;
    const UiAudioControl ids[]{UiAudioControl::play_pause, UiAudioControl::seek,   UiAudioControl::time,
                               UiAudioControl::mute,       UiAudioControl::volume, UiAudioControl::download,
                               UiAudioControl::status,     UiAudioControl::title};
    const UiRect rectangles[]{geometry.play_pause, geometry.seek,     geometry.time,   geometry.mute,
                              geometry.volume,     geometry.download, geometry.status, geometry.title};
    for (u32 i = 0; i < kUiAudioControlCount; ++i) {
        auto& descriptor = projection.controls[i];
        descriptor.id = ids[i];
        descriptor.bounds = rectangles[i];
        descriptor.text_style = style.text_style;
        descriptor.foreground = theme.text_primary;
        descriptor.background = theme.interactive;
        descriptor.kind = i == 1 || i == 4   ? UiAudioControlKind::range
                          : i == 2 || i >= 6 ? UiAudioControlKind::label
                                             : UiAudioControlKind::button;
    }
    auto& play = projection.controls[0];
    play.label = state.error ? style.retry : state.playing ? style.pause : style.play;
    play.enabled = state.can_play && !state.loading;
    play.selected = state.playing;
    auto& seek = projection.controls[1];
    seek.enabled =
        state.can_seek && state.has_duration && state.duration_ms && !state.error && !state.loading;
    seek.maximum = 10000;
    seek.value = state.has_duration && state.duration_ms
                     ? static_cast<u32>(static_cast<double>(state.elapsed_ms) /
                                        static_cast<double>(state.duration_ms) * 10000.0)
                     : 0;
    seek.visible = !state.loading && !state.error;
    auto& mute = projection.controls[3];
    mute.label = state.muted ? style.unmute : style.mute;
    mute.enabled = state.can_volume && !state.error;
    mute.selected = state.muted;
    auto& volume = projection.controls[4];
    volume.enabled = state.can_volume && !state.error;
    volume.value = state.volume;
    volume.maximum = 100;
    volume.visible = geometry.volume_expanded && volume.enabled;
    projection.controls[2].visible = !geometry.compact || !volume.visible;
    auto& download = projection.controls[5];
    download.label = style.download;
    download.enabled = state.can_download;
    auto& status = projection.controls[6];
    status.label = state.error ? style.error : state.loading ? style.loading : Utf8View{};
    status.visible = status.label.bytes != 0;
    status.foreground = state.error ? ui_foreground_color(theme, theme.notification) : theme.text_secondary;
    auto& title = projection.controls[7];
    title.label = state.voice ? style.voice_title : state.title;
    title.visible = !state.voice && title.label.bytes != 0;
    for (auto& descriptor : projection.controls)
        if (!descriptor.enabled && descriptor.kind != UiAudioControlKind::label)
            descriptor.foreground = theme.text_secondary;
    *out = projection;
    return true;
}
const UiAudioControlDescriptor* ui_audio_controls_find(const UiAudioControlsProjection& projection,
                                                       UiAudioControl id) noexcept {
    for (const auto& descriptor : projection.controls)
        if (descriptor.id == id)
            return &descriptor;
    return nullptr;
}
Utf8View ui_audio_controls_label(const UiAudioControlsProjection& projection, UiAudioControl id) noexcept {
    if (id == UiAudioControl::time)
        return projection.time_bytes <= sizeof(projection.time_text)
                   ? Utf8View{projection.time_text, projection.time_bytes}
                   : Utf8View{};
    const auto* descriptor = ui_audio_controls_find(projection, id);
    return descriptor ? descriptor->label : Utf8View{};
}
UiAudioControl ui_audio_controls_hit(const UiAudioControlsProjection& projection, UiPoint point) noexcept {
    if (!paint_coordinate_valid(point.x) || !paint_coordinate_valid(point.y))
        return UiAudioControl::none;
    for (const auto& descriptor : projection.controls)
        if (descriptor.visible && descriptor.enabled && point.x >= descriptor.bounds.left &&
            point.x < descriptor.bounds.right && point.y >= descriptor.bounds.top &&
            point.y < descriptor.bounds.bottom)
            return descriptor.id;
    return UiAudioControl::none;
}
bool ui_audio_controls_range_action(const UiAudioControlsProjection& projection, UiAudioControl id,
                                    UiPoint point, UiAudioAction* out) noexcept {
    const auto* descriptor = ui_audio_controls_find(projection, id);
    if (!descriptor || descriptor->kind != UiAudioControlKind::range || !descriptor->enabled ||
        !paint_coordinate_valid(point.x) || !paint_coordinate_valid(point.y) ||
        !positive(ui_rect_width(descriptor->bounds)))
        return false;
    const auto track = ui_audio_controls_range_track(projection, id);
    if (!positive(ui_rect_width(track)))
        return false;
    const float ratio = maximum(0.0f, minimum(1.0f, (point.x - track.left) / ui_rect_width(track)));
    return action(projection, id, static_cast<u32>(ratio * descriptor->maximum + .5f), out);
}
UiRect ui_audio_controls_range_track(const UiAudioControlsProjection& projection,
                                     UiAudioControl id) noexcept {
    const auto* descriptor = ui_audio_controls_find(projection, id);
    if (!descriptor || descriptor->kind != UiAudioControlKind::range)
        return {};
    auto track = descriptor->bounds;
    if (id == UiAudioControl::volume) {
        track.left += projection.volume_thumb_inset;
        track.right -= projection.volume_thumb_inset;
    }
    return track;
}
bool ui_audio_controls_pointer_action(const UiAudioControlsProjection& projection, UiPoint point,
                                      UiAudioAction* out) noexcept {
    const auto id = ui_audio_controls_hit(projection, point);
    const auto* descriptor = ui_audio_controls_find(projection, id);
    if (!descriptor)
        return false;
    return descriptor->kind == UiAudioControlKind::range
               ? ui_audio_controls_range_action(projection, id, point, out)
               : action(projection, id, descriptor->value, out);
}
bool ui_audio_controls_key(const UiAudioControlsProjection& projection, UiAudioKey key,
                           UiAudioControl* focus, UiAudioAction* out) noexcept {
    if (!focus)
        return false;
    constexpr UiAudioControl order[]{UiAudioControl::play_pause, UiAudioControl::seek, UiAudioControl::mute,
                                     UiAudioControl::volume, UiAudioControl::download};
    if (key == UiAudioKey::previous || key == UiAudioKey::next) {
        u32 cursor = 5;
        for (u32 i = 0; i < 5; ++i)
            if (order[i] == *focus)
                cursor = i;
        for (u32 i = 0; i < 5; ++i) {
            cursor = cursor == 5 ? (key == UiAudioKey::next ? 0 : 4)
                                 : (key == UiAudioKey::next ? (cursor + 1) % 5 : (cursor + 4) % 5);
            const auto* descriptor = ui_audio_controls_find(projection, order[cursor]);
            if (descriptor && descriptor->enabled) {
                *focus = order[cursor];
                if (out)
                    *out = {};
                return true;
            }
        }
        *focus = UiAudioControl::none;
        return false;
    }
    const auto* descriptor = ui_audio_controls_find(projection, *focus);
    if (!descriptor || !descriptor->enabled)
        return false;
    if (key == UiAudioKey::activate)
        return descriptor->kind == UiAudioControlKind::button &&
               action(projection, *focus, descriptor->value, out);
    if (descriptor->kind != UiAudioControlKind::range)
        return false;
    u32 value = descriptor->value;
    const u32 step = *focus == UiAudioControl::seek ? 500 : 5;
    if (key == UiAudioKey::home)
        value = 0;
    else if (key == UiAudioKey::end)
        value = descriptor->maximum;
    else if (key == UiAudioKey::decrease)
        value = value > step ? value - step : 0;
    else if (key == UiAudioKey::increase)
        value = minimum(value + step, descriptor->maximum);
    else
        return false;
    return action(projection, *focus, value, out);
}
bool ui_audio_action_current(const UiAudioPlaybackState& state, const UiAudioAction& request) noexcept {
    if (!state.owner || !state.generation || !state.revision || request.owner != state.owner ||
        request.generation != state.generation || request.revision != state.revision)
        return false;
    switch (request.control) {
    case UiAudioControl::play_pause:
        return state.can_play && !state.loading;
    case UiAudioControl::seek:
        return state.can_seek && state.has_duration && state.duration_ms && !state.loading &&
               !state.error && request.position_ms <= state.duration_ms;
    case UiAudioControl::mute:
        return state.can_volume && !state.error;
    case UiAudioControl::volume:
        return state.can_volume && !state.error && request.volume <= 100;
    case UiAudioControl::download:
        return state.can_download;
    default:
        return false;
    }
}

namespace {
bool rectangle(UiRect bounds) noexcept {
    return paint_coordinate_valid(bounds.left) && paint_coordinate_valid(bounds.top) &&
           paint_coordinate_valid(bounds.right) && paint_coordinate_valid(bounds.bottom) &&
           bounds.right > bounds.left && bounds.bottom > bounds.top;
}
UiRect intersection(UiRect a, UiRect b) noexcept {
    return {maximum(a.left, b.left), maximum(a.top, b.top), minimum(a.right, b.right),
            minimum(a.bottom, b.bottom)};
}
bool contains(UiRect bounds, UiPoint point) noexcept {
    return paint_coordinate_valid(point.x) && paint_coordinate_valid(point.y) && point.x >= bounds.left &&
           point.x < bounds.right && point.y >= bounds.top && point.y < bounds.bottom;
}
bool scene_current(const UiAudioScene& scene) noexcept {
    const auto* widget =
        scene.owner.manager ? ui_widget_manager_resolve(*scene.owner.manager, scene.owner.handle) : nullptr;
    return scene.active && widget && (widget->flags & ui_widget_enabled) &&
           !(widget->flags & ui_widget_block_input);
}
bool eligible(const UiAudioScene& scene, UiAudioControl id, bool require_visible = true) noexcept {
    const auto* descriptor = ui_audio_controls_find(scene.projection, id);
    return scene_current(scene) && descriptor && descriptor->enabled &&
           (!require_visible || descriptor->visible) &&
           rectangle(intersection(descriptor->bounds, scene.clip));
}
void expand_volume(UiAudioScene& scene) noexcept {
    auto& projection = scene.projection;
    auto& volume = projection.controls[4];
    const bool expanded = !projection.geometry.compact || scene.capture == UiAudioControl::volume ||
                          scene.hover == UiAudioControl::mute || scene.hover == UiAudioControl::volume ||
                          (scene.keyboard_focus &&
                           (scene.focus == UiAudioControl::mute || scene.focus == UiAudioControl::volume));
    projection.geometry.volume_expanded = expanded;
    volume.visible = volume.enabled && expanded;
    projection.controls[2].visible = !projection.geometry.compact || !volume.visible;
}
bool captured_action(const UiAudioScene& scene, UiPoint point, UiAudioAction* out) noexcept {
    const auto* descriptor = ui_audio_controls_find(scene.projection, scene.capture);
    if (!eligible(scene, scene.capture) || !descriptor || descriptor->kind != UiAudioControlKind::range ||
        !paint_coordinate_valid(point.x) || !paint_coordinate_valid(point.y))
        return false;
    const auto visible = intersection(descriptor->bounds, scene.clip);
    point.x = maximum(visible.left, minimum(visible.right, point.x));
    return ui_audio_controls_range_action(scene.projection, scene.capture, point, out);
}
} // namespace
bool ui_audio_scene_project(UiAudioScene* scene, const UiAudioPlaybackState& state, UiRect bounds,
                            UiRect clip, UiWidgetReference owner, UiAudioSourceSurface surface,
                            const UiAudioControlsStyle& style, const Theme& theme) noexcept {
    if (!scene)
        return false;
    const auto* widget = owner.manager ? ui_widget_manager_resolve(*owner.manager, owner.handle) : nullptr;
    UiAudioControlsProjection projection{};
    const bool same_source = scene->active && scene->surface == surface &&
                             scene->owner.manager == owner.manager &&
                             scene->owner.handle.slot == owner.handle.slot &&
                             scene->owner.handle.generation == owner.handle.generation &&
                             scene->projection.playback.owner == state.owner &&
                             scene->projection.playback.generation == state.generation &&
                             scene->projection.playback.revision == state.revision;
    if (!widget || !(widget->flags & ui_widget_enabled) || (widget->flags & ui_widget_block_input) ||
        static_cast<u32>(surface) > static_cast<u32>(UiAudioSourceSurface::forward) || !rectangle(clip) ||
        !ui_audio_controls_project(state, bounds, style, theme, &projection) ||
        !rectangle(intersection(projection.geometry.bounds, clip))) {
        *scene = {};
        return false;
    }
    const auto old_focus = scene->focus, old_hover = scene->hover, old_capture = scene->capture;
    const bool keyboard_focus = scene->keyboard_focus;
    const UiRect effective = intersection(projection.geometry.bounds, clip);
    const bool same_geometry =
        same_source && ui_rect_equal(scene->clip, effective) &&
        ui_rect_equal(scene->projection.geometry.bounds, projection.geometry.bounds) &&
        scene->projection.volume_thumb_inset == projection.volume_thumb_inset;
    bool preserve_capture = false;
    if (same_geometry) {
        const auto* previous = ui_audio_controls_find(scene->projection, old_capture);
        const auto* current = ui_audio_controls_find(projection, old_capture);
        preserve_capture =
            previous && current && ui_rect_equal(previous->bounds, current->bounds) && current->enabled;
    }
    *scene = {};
    scene->projection = projection;
    scene->style = style;
    scene->clip = effective;
    scene->owner = owner;
    scene->surface = surface;
    scene->active = true;
    if (same_source && eligible(*scene, old_focus, false)) {
        scene->focus = old_focus;
        scene->keyboard_focus = keyboard_focus;
    }
    if (same_geometry && eligible(*scene, old_hover, false))
        scene->hover = old_hover;
    if (preserve_capture)
        scene->capture = old_capture;
    expand_volume(*scene);
    return true;
}
UiAudioControl ui_audio_scene_hit(const UiAudioScene& scene, UiPoint point) noexcept {
    return scene_current(scene) && contains(scene.clip, point)
               ? ui_audio_controls_hit(scene.projection, point)
               : UiAudioControl::none;
}
bool ui_audio_scene_hover(UiAudioScene* scene, UiPoint point) noexcept {
    if (!scene)
        return false;
    const auto previous = scene->hover;
    const bool previous_volume = scene->projection.controls[4].visible;
    scene->hover = ui_audio_scene_hit(*scene, point);
    expand_volume(*scene);
    return scene->hover != previous || scene->projection.controls[4].visible != previous_volume;
}
bool ui_audio_scene_focus(UiAudioScene* scene, UiAudioControl id, bool keyboard) noexcept {
    if (!scene || !eligible(*scene, id, false))
        return false;
    scene->focus = id;
    scene->keyboard_focus = keyboard;
    expand_volume(*scene);
    return true;
}
void ui_audio_scene_blur(UiAudioScene* scene) noexcept {
    if (!scene)
        return;
    scene->focus = scene->hover = scene->capture = UiAudioControl::none;
    scene->keyboard_focus = false;
    expand_volume(*scene);
}
UiAudioSceneResult ui_audio_scene_pointer(UiAudioScene* scene, UiAudioPointerEvent event,
                                          UiPoint point) noexcept {
    UiAudioSceneResult result{};
    if (!scene)
        return result;
    if (event == UiAudioPointerEvent::cancel || !scene_current(*scene)) {
        result.handled = scene->capture != UiAudioControl::none;
        result.release_capture = result.handled;
        ui_audio_scene_blur(scene);
        return result;
    }
    if (event == UiAudioPointerEvent::leave) {
        scene->hover = UiAudioControl::none;
        expand_volume(*scene);
        result.handled = scene->capture != UiAudioControl::none;
        return result;
    }
    if (event == UiAudioPointerEvent::move) {
        ui_audio_scene_hover(scene, point);
        result.handled = scene->capture != UiAudioControl::none;
        if (result.handled)
            result.action_requested = captured_action(*scene, point, &result.action);
        return result;
    }
    if (event == UiAudioPointerEvent::down) {
        const auto id = ui_audio_scene_hit(*scene, point);
        if (id == UiAudioControl::none)
            return result;
        scene->focus = scene->capture = id;
        scene->keyboard_focus = false;
        scene->hover = id;
        expand_volume(*scene);
        result.handled = result.request_capture = true;
        const auto* descriptor = ui_audio_controls_find(scene->projection, id);
        if (descriptor && descriptor->kind == UiAudioControlKind::range)
            result.action_requested = captured_action(*scene, point, &result.action);
        return result;
    }
    if (event == UiAudioPointerEvent::up && scene->capture != UiAudioControl::none) {
        const auto id = scene->capture;
        result.handled = result.release_capture = true;
        const auto* descriptor = ui_audio_controls_find(scene->projection, id);
        if (descriptor && descriptor->kind == UiAudioControlKind::range)
            result.action_requested = captured_action(*scene, point, &result.action);
        else if (ui_audio_scene_hit(*scene, point) == id)
            result.action_requested =
                action(scene->projection, id, descriptor ? descriptor->value : 0, &result.action);
        scene->capture = UiAudioControl::none;
        ui_audio_scene_hover(scene, point);
        return result;
    }
    return result;
}
UiAudioSceneResult ui_audio_scene_key(UiAudioScene* scene, UiAudioKey key) noexcept {
    UiAudioSceneResult result{};
    if (!scene || !scene_current(*scene))
        return result;
    if (key == UiAudioKey::next || key == UiAudioKey::previous) {
        for (u32 i = 0; i < 5; ++i) {
            if (!ui_audio_controls_key(scene->projection, key, &scene->focus, &result.action))
                break;
            if (eligible(*scene, scene->focus, false)) {
                result.handled = true;
                break;
            }
        }
        if (!result.handled) {
            scene->focus = UiAudioControl::none;
            expand_volume(*scene);
        }
    } else if (eligible(*scene, scene->focus, false))
        result.handled = ui_audio_controls_key(scene->projection, key, &scene->focus, &result.action);
    if (result.handled) {
        scene->keyboard_focus = true;
        expand_volume(*scene);
        result.action_requested = result.action.control != UiAudioControl::none &&
                                  ui_audio_action_current(scene->projection.playback, result.action);
    }
    return result;
}
} // namespace maicord
