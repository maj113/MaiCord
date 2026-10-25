#include "maicord/ui/video_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {
namespace {
bool rectangle(UiRect bounds) noexcept {
    return paint_coordinate_valid(bounds.left) && paint_coordinate_valid(bounds.top) &&
           paint_coordinate_valid(bounds.right) && paint_coordinate_valid(bounds.bottom) &&
           bounds.right > bounds.left && bounds.bottom > bounds.top;
}
bool contains(UiRect bounds, UiPoint point) noexcept {
    return paint_coordinate_valid(point.x) && paint_coordinate_valid(point.y) && point.x >= bounds.left &&
           point.x < bounds.right && point.y >= bounds.top && point.y < bounds.bottom;
}
bool metric(float value) noexcept {
    return paint_coordinate_valid(value) && value > 0;
}
bool affordance_valid(const UiVideoPlayAffordanceStyle& style) noexcept {
    return metric(style.target) && metric(style.minimum_target) && style.minimum_target <= style.target &&
           paint_coordinate_valid(style.inset) && style.inset >= 0 &&
           paint_coordinate_valid(style.radius) && style.radius >= 0 && metric(style.stroke) &&
           style.stroke <= style.minimum_target * .5f && metric(style.opacity) && style.opacity <= 1;
}
bool scene_style_valid(const UiVideoSceneStyle& style, const UiVideoControlsStyle& controls) noexcept {
    return metric(style.radius) && metric(style.track_height) && metric(style.thumb_size) &&
           metric(style.focus_width) && style.track_height <= controls.range_height &&
           style.thumb_size <= controls.range_height && metric(style.icon_extent) &&
           metric(style.surface_opacity) && style.surface_opacity <= 1 && metric(style.hover_opacity) &&
           style.hover_opacity <= 1 && metric(style.track_opacity) && style.track_opacity <= 1 &&
           paint_coordinate_valid(style.preview_radius) && style.preview_radius >= 0 &&
           paint_coordinate_valid(style.inset) && style.inset >= 0 && metric(style.spinner_extent) &&
           metric(style.spinner.stroke) && style.spinner.stroke * 4 <= style.spinner_extent &&
           paint_coordinate_valid(style.spinner.tail_opacity) && style.spinner.tail_opacity >= 0 &&
           style.spinner.tail_opacity <= 1 && style.fade_ms <= 1'000 &&
           paint_coordinate_valid(style.scrim_lead) && style.scrim_lead >= 0 && style.idle_ms <= 60'000;
}
const UiVideoControlDescriptor* descriptor(const UiVideoScene& scene, UiVideoControl id) noexcept {
    for (const auto& control : scene.projection.controls)
        if (control.id == id)
            return &control;
    return nullptr;
}
bool eligible(const UiVideoScene& scene, UiVideoControl id) noexcept {
    const auto* control = descriptor(scene, id);
    return scene.active && control && control->enabled && control->visible &&
           rectangle(ui_rect_overlap(control->bounds, scene.clip));
}
bool same_playback_source(const UiVideoScene& scene, const UiVideoPlaybackState& playback) noexcept {
    return scene.active && scene.projection.playback.owner == playback.owner &&
           scene.projection.playback.generation == playback.generation &&
           scene.projection.playback.revision == playback.revision;
}
bool controls_requested(const UiVideoScene& scene) noexcept {
    return scene.projection.playback.error || (scene.preview_hovered && !scene.idle) ||
           scene.capture != UiVideoControl::none || (scene.keyboard_focus && eligible(scene, scene.focus));
}
bool idle_countdown(const UiVideoScene& scene) noexcept {
    const auto& playback = scene.projection.playback;
    return scene.style.idle_ms && scene.preview_hovered && playback.playing && !playback.buffering &&
           !playback.error && scene.capture == UiVideoControl::none &&
           !(scene.keyboard_focus && eligible(scene, scene.focus));
}
void settle_immediate_controls(UiVideoScene& scene) noexcept {
    if (!scene.style.fade_ms)
        ui_opacity_transition_reset(&scene.controls_transition, controls_requested(scene) ? 1.0f : 0.0f);
}
struct ControlsPresentationState {
    UiOpacityTransition transition{};
    u64 activity_ms{};
    float hover_x{};
    bool preview_hovered{}, idle{};
};
ControlsPresentationState controls_presentation_state(const UiVideoScene& scene) noexcept {
    return {scene.controls_transition, scene.activity_ms, scene.hover_x, scene.preview_hovered, scene.idle};
}
void restore_controls_presentation(UiVideoScene& scene, const ControlsPresentationState& previous,
                                   bool same_source, bool same_geometry) noexcept {
    if (same_source) {
        scene.controls_transition = previous.transition;
        scene.activity_ms = previous.activity_ms;
        scene.preview_hovered = same_geometry && previous.preview_hovered;
        scene.idle = previous.idle && idle_countdown(scene);
        scene.hover_x = previous.hover_x;
    } else {
        ui_opacity_transition_reset(&scene.controls_transition, controls_requested(scene) ? 1.0f : 0.0f);
    }
    settle_immediate_controls(scene);
}
bool volume_expanded(const UiVideoScene& scene) noexcept {
    if (scene.capture == UiVideoControl::seek) {
        return scene.projection.volume_expanded;
    }
    return scene.capture == UiVideoControl::volume || scene.capture == UiVideoControl::mute ||
           scene.hover == UiVideoControl::volume || scene.hover == UiVideoControl::mute ||
           (scene.keyboard_focus &&
            (scene.focus == UiVideoControl::volume || scene.focus == UiVideoControl::mute));
}
UiRect range_track(UiRect bounds, const UiVideoSceneStyle& style, bool collection = false) noexcept {
    if (collection) bounds.top = (bounds.top + bounds.bottom) * .5f;
    const float center_y = (bounds.top + bounds.bottom) * .5f;
    const float half_thumb = style.thumb_size * .5f;
    return {bounds.left + half_thumb, center_y - style.track_height * .5f,
            bounds.right - half_thumb, center_y + style.track_height * .5f};
}
constexpr u32 kVolumeSteps = 101;
// The shared settings slider with the player's thumb and track sizes, so seek
// and volume match. The media chrome is light on black in every appearance.
UiSettingsTokens volume_tokens(const UiVideoSceneStyle& style) noexcept {
    auto tokens = ui_settings_tokens(kDefaultTheme);
    tokens.metric.slider_grabber = style.thumb_size;
    tokens.metric.slider_bar = style.track_height;
    return tokens;
}
// Centred in the volume control; its grabber sets the pill behind it.
UiSliderGeometry volume_slider(const UiSettingsTokens& tokens, UiRect bounds, u32 value) noexcept {
    const float middle = (bounds.top + bounds.bottom) * .5f, half = ui_slider_height(tokens, false) * .5f;
    return ui_slider_geometry(tokens, {bounds.left, middle - half, bounds.right, middle + half}, kVolumeSteps,
                              value, false);
}
void append_clock(char* text, u32& cursor, u64 milliseconds, bool hours) noexcept {
    const u64 seconds = milliseconds / 1000;
    if (hours) {
        cursor += decimal_write(text + cursor, seconds / 3600);
        text[cursor++] = ':';
    }
    const u64 minutes = hours ? (seconds / 60) % 60 : seconds / 60;
    if (hours && minutes < 10)
        text[cursor++] = '0';
    cursor += decimal_write(text + cursor, minutes);
    text[cursor++] = ':';
    if (seconds % 60 < 10)
        text[cursor++] = '0';
    cursor += decimal_write(text + cursor, seconds % 60);
}
// The bubble follows the hovered second rather than the raw pointer, so
// its glyphs move only when the label changes.
void update_hover_time(UiVideoScene& scene) noexcept {
    const u16 previous_track = scene.hover_track;
    scene.hover_track = 0;
    if ((scene.hover == UiVideoControl::track || scene.capture == UiVideoControl::track) && eligible(scene, UiVideoControl::track)) {
        const auto* selected = descriptor(scene, UiVideoControl::track);
        const auto track = range_track(selected->bounds, scene.style, true);
        const float ratio = maximum(0.0f, minimum(1.0f, (scene.hover_x - track.left) / ui_rect_width(track)));
        scene.hover_track = static_cast<u16>(minimum(selected->maximum, static_cast<u32>(ratio * selected->maximum + .5f)) + 1);
        if (previous_track != scene.hover_track) {
            scene.track_hover_since_ms = 0; scene.track_hover_requested = false;
            copy_bytes(scene.hover_time, "Track ", 6); scene.hover_time_bytes = 6;
            scene.hover_time_bytes += decimal_write(scene.hover_time + scene.hover_time_bytes, scene.hover_track);
        }
        scene.hover_x = track.left + ui_rect_width(track) * (scene.hover_track - 1) / maximum(1u, selected->maximum);
        return;
    }
    scene.track_hover_since_ms = 0; scene.track_hover_requested = false;
    scene.hover_time_bytes = 0;
    const auto* control = descriptor(scene, UiVideoControl::seek);
    const u64 duration = scene.projection.playback.duration_ms;
    if ((scene.hover != UiVideoControl::seek && scene.capture != UiVideoControl::seek) || !eligible(scene, UiVideoControl::seek) ||
        !control || !duration)
        return;
    const auto track = range_track(control->bounds, scene.style);
    if (track.right <= track.left)
        return;
    const float ratio = maximum(0.0f, minimum(1.0f, (scene.hover_x - track.left) / ui_rect_width(track)));
    const u64 at = static_cast<u64>(static_cast<double>(duration) * ratio) / 1000 * 1000;
    append_clock(scene.hover_time, scene.hover_time_bytes, at, duration >= 3'600'000);
    scene.hover_x = track.left + ui_rect_width(track) * static_cast<float>(static_cast<double>(at) / duration);
}
bool range_request(UiVideoScene& scene, UiVideoControl id, UiPoint point,
                   UiVideoAction* out) noexcept {
    const auto* control = descriptor(scene, id);
    if (!eligible(scene, id) || !control || control->kind != UiVideoControlKind::range ||
        !paint_coordinate_valid(point.x) || !paint_coordinate_valid(point.y))
        return false;
    const auto visible = ui_rect_overlap(control->bounds, scene.clip);
    const float clamped_x = maximum(visible.left, minimum(visible.right, point.x));
    // Requests follow the painted thumb centers. The larger rectangle remains
    // the pointer target, so its end gutters also reach the exact endpoints.
    const auto track = range_track(control->bounds, scene.style, id == UiVideoControl::track);
    if (track.right <= track.left)
        return false;
    const float ratio =
        maximum(0.0f, minimum(1.0f, (clamped_x - track.left) / ui_rect_width(track)));
    const u32 value = id == UiVideoControl::volume
                          ? ui_slider_index_at(volume_slider(volume_tokens(scene.style), control->bounds,
                                                             control->value),
                                               kVolumeSteps, clamped_x)
                          : minimum(control->maximum, static_cast<u32>(ratio * control->maximum + .5f));
    UiVideoAction request{};
    request.control = id;
    request.owner = scene.projection.playback.owner;
    request.generation = scene.projection.playback.generation;
    request.revision = scene.projection.playback.revision;
    if (id == UiVideoControl::seek) {
        const u64 duration = scene.projection.playback.duration_ms;
        request.position_ms = (duration / 10000) * value + (duration % 10000) * value / 10000;
    } else if (id == UiVideoControl::track) request.collection_track = static_cast<u16>(value + 1);
    else request.volume = value;
    const u64 requested = id == UiVideoControl::seek ? request.position_ms :
        id == UiVideoControl::track ? request.collection_track : request.volume;
    if (scene.capture_value_valid && scene.capture_value == requested)
        return false;
    scene.capture_value = requested;
    scene.capture_value_valid = true;
    *out = request;
    return true;
}
bool quad(PaintStream* paint, UiRect bounds, Color color, float radius) noexcept {
    return paint_stream_push_rounded(paint, bounds.left, bounds.top, bounds.right, bounds.bottom, color,
                                     radius);
}
bool symbol(PaintStream* paint, UiRect bounds, UiVideoControl id, const UiVideoPlaybackState& playback,
            float extent, Color color, bool error = false, float offset = 0) noexcept {
    const float size = minimum(extent, minimum(ui_rect_width(bounds), ui_rect_height(bounds)));
    const float left = (bounds.left + bounds.right - size) * .5f;
    const float top = (bounds.top + bounds.bottom - size) * .5f + offset;
    const UiIcon icon =
        error                              ? UiIcon::warning
        : id == UiVideoControl::play_pause ? (playback.playing ? UiIcon::pause : UiIcon::play)
        : id == UiVideoControl::previous_track ? UiIcon::chevron_left
        : id == UiVideoControl::next_track ? UiIcon::chevron_right
        : id == UiVideoControl::expand     ? (playback.expanded ? UiIcon::minimize : UiIcon::maximize)
        : playback.muted || !playback.has_audio || !playback.volume ? UiIcon::speaker_muted
        : playback.volume < 50                                       ? UiIcon::volume_low
                                                                     : UiIcon::speaker;
    return ui_icon_paint(paint, icon, {left, top, left + size, top + size}, color);
}
u64 text_revision(const UiVideoScene& scene, const Theme& theme) noexcept {
    u64 hash = hash64_bytes(kHash64Seed, &theme, sizeof(theme));
    hash = hash64_bytes(hash, &scene.projection.bounds, sizeof(UiRect));
    hash = hash64_bytes(hash, &scene.clip, sizeof(UiRect));
    for (const auto& control : scene.projection.controls) {
        hash = hash64_bytes(hash, &control.bounds, sizeof(UiRect));
        hash = hash64_mix(hash, control.text_style);
        hash = hash64_mix(hash, control.visible);
        if (control.visible &&
            (control.id == UiVideoControl::time || control.id == UiVideoControl::status || control.id == UiVideoControl::track)) {
            const auto label = ui_video_scene_label(scene, control.id);
            hash = hash64_counted_bytes(hash, label.data, label.bytes);
        }
    }
    hash = hash64_mix(hash, scene.projection.playback.error);
    return hash ? hash : 1;
}
} // namespace

bool ui_video_scene_project(UiVideoScene* scene, const UiVideoPlaybackState& playback, UiRect preview,
                            UiRect clip, const UiVideoControlsStyle& controls,
                            const UiVideoSceneStyle& style, const Theme& theme) noexcept {
    if (!scene)
        return false;
    UiVideoControlsProjection projected_controls{};
    const float width = ui_rect_width(preview) - 2 * style.inset;
    const float height = ui_video_controls_height(controls, width, playback.collection_count != 0);
    const UiRect bounds{preview.left + style.inset, preview.bottom - style.inset - height,
                        preview.right - style.inset, preview.bottom - style.inset};
    const bool same_source = same_playback_source(*scene, playback);
    if (!rectangle(clip) || !scene_style_valid(style, controls) || !rectangle(preview) ||
        ui_rect_height(preview) < height + 2 * style.inset ||
        !ui_video_controls_project(playback, bounds, controls, theme, &projected_controls,
                                   same_source && volume_expanded(*scene)) ||
        !rectangle(ui_rect_overlap(preview, clip))) {
        reset_value(scene);
        return false;
    }
    auto& status = projected_controls.controls[5];
    // The descriptor retains its semantic loading label; this visual scene
    // renders loading as native geometry and keeps error text available.
    status.visible = playback.error && status.label.bytes != 0;
    const float status_bottom = bounds.top - maximum(controls.gap, 4.0f);
    status.bounds = {bounds.left + controls.padding + 8, status_bottom - controls.status_height,
                     bounds.right - controls.padding - 8, status_bottom};
    if (status.visible && status.bounds.top < preview.top + style.inset) {
        status.bounds = {
            projected_controls.controls[2].bounds.left, projected_controls.controls[2].bounds.top,
            projected_controls.controls[1].bounds.right, projected_controls.controls[2].bounds.bottom};
        projected_controls.controls[2].visible = false;
        projected_controls.controls[1].visible = false;
        projected_controls.controls[4].visible = false;
    }
    const UiRect effective = ui_rect_overlap(preview, clip);
    const bool geometry_same = same_source && ui_rect_equal(scene->clip, effective) &&
                               ui_rect_equal(scene->projection.bounds, bounds);
    const auto old_focus = scene->focus, old_hover = scene->hover, old_capture = scene->capture;
    const u64 old_capture_value = scene->capture_value;
    const bool old_capture_value_valid = scene->capture_value_valid;
    const auto previous_presentation = controls_presentation_state(*scene);
    const bool keyboard_focus = scene->keyboard_focus;
    const auto control_geometry_same = [&](UiVideoControl id) noexcept {
        const auto* previous = descriptor(*scene, id);
        for (const auto& control : projected_controls.controls) {
            if (control.id == id) {
                return geometry_same && previous && ui_rect_equal(previous->bounds, control.bounds);
            }
        }
        return false;
    };
    const bool preserve_hover = control_geometry_same(old_hover);
    const bool preserve_capture = control_geometry_same(old_capture);
    char old_title[sizeof(scene->track_title)]{};
    const u16 old_title_bytes = same_source && scene->projection.playback.collection_track == playback.collection_track ? scene->track_title_bytes : 0;
    copy_bytes(old_title, scene->track_title, old_title_bytes);
    char old_tip[sizeof(scene->hover_time)]{};
    const u32 old_tip_bytes = preserve_hover ? scene->hover_time_bytes : 0;
    copy_bytes(old_tip, scene->hover_time, old_tip_bytes);
    const u16 old_track = preserve_hover ? scene->hover_track : 0;
    const u64 old_since = preserve_hover ? scene->track_hover_since_ms : 0;
    const bool old_requested = preserve_hover && scene->track_hover_requested;
    reset_value(scene);
    copy_bytes(scene->track_title, old_title, old_title_bytes); scene->track_title_bytes = old_title_bytes;
    copy_bytes(scene->hover_time, old_tip, old_tip_bytes); scene->hover_time_bytes = old_tip_bytes;
    scene->hover_track = old_track; scene->track_hover_since_ms = old_since; scene->track_hover_requested = old_requested;
    copy_value(&scene->projection, projected_controls);
    scene->style = style;
    scene->clip = effective;
    scene->preview = preview;
    scene->text_advance = controls.time_advance;
    scene->active = true;
    if (playback.buffering && !playback.error)
        scene->buffering_indicator = ui_video_play_affordance_rectangle(preview, {});
    if (!playback.playing && playback.can_play && !playback.error && !playback.buffering) {
        scene->center_play = ui_video_play_affordance_rectangle(preview, {});
        if (rectangle(ui_rect_overlap(scene->center_play, bounds))) {
            reset_value(&scene->center_play);
        }
    }
    u32 label_cursor = 0;
    for (u32 i = 0; i < kUiVideoControlCount; ++i) {
        const auto label = ui_video_controls_label(projected_controls, projected_controls.controls[i].id);
        if (label.bytes > 128 || label.bytes > sizeof(scene->labels) - label_cursor) {
            reset_value(scene);
            return false;
        }
        if (label.bytes)
            copy_bytes(scene->labels + label_cursor, label.data, label.bytes);
        scene->label_offsets[i] = static_cast<u16>(label_cursor); label_cursor += label.bytes;
        scene->label_bytes[i] = label.bytes;
        reset_value(&scene->projection.controls[i].label);
    }
    if (same_source && eligible(*scene, old_focus)) {
        scene->focus = old_focus;
        scene->keyboard_focus = keyboard_focus;
    }
    if (preserve_hover && eligible(*scene, old_hover))
        scene->hover = old_hover;
    if (preserve_capture && eligible(*scene, old_capture)) {
        scene->capture = old_capture;
        scene->capture_value = old_capture_value;
        scene->capture_value_valid = old_capture_value_valid;
    }
    restore_controls_presentation(*scene, previous_presentation, same_source, geometry_same);
    update_hover_time(*scene);
    scene->text_revision = text_revision(*scene, theme);
    return true;
}
UiRect ui_video_play_affordance_rectangle(UiRect preview,
                                          const UiVideoPlayAffordanceStyle& style) noexcept {
    if (!rectangle(preview) || !affordance_valid(style))
        return {};
    const float size =
        minimum(style.target, minimum(ui_rect_width(preview), ui_rect_height(preview)) - 2 * style.inset);
    if (size < style.minimum_target)
        return {};
    const float x = (preview.left + preview.right - size) * .5f,
                y = (preview.top + preview.bottom - size) * .5f;
    return {x, y, x + size, y + size};
}
bool ui_video_play_affordance_paint(UiRect preview, UiRect clip, const UiVideoPlayAffordanceStyle& style,
                                    const Theme& theme, PaintStream* paint) noexcept {
    if (!rectangle(preview) || !ui_rect_valid_or_empty(clip) || !affordance_valid(style))
        return false;
    const UiRect button = ui_video_play_affordance_rectangle(preview, style),
                 visible = ui_rect_overlap(preview, clip);
    if (!rectangle(button) || !rectangle(visible) || !rectangle(ui_rect_overlap(button, visible)))
        return true;
    if (!paint_stream_push_clip(paint, visible.left, visible.top, visible.right, visible.bottom))
        return false;
    // The media scrim's black at the affordance's own opacity.
    Color background = theme.tokens.overlay.media;
    background.alpha = style.opacity;
    const bool ok = quad(paint, button, background, style.radius) &&
                    symbol(paint, button, UiVideoControl::play_pause, {}, kUiMediaControlTarget,
                           kDefaultTheme.text_primary);

    return paint_stream_pop_clip(paint) && ok;
}
bool ui_video_scene_covers_preview(const UiVideoScene& scene, UiRect preview) noexcept {
    const auto bounds = scene.projection.bounds;
    return scene.active && rectangle(preview) && rectangle(bounds) && bounds.left >= preview.left &&
           bounds.top >= preview.top && bounds.right <= preview.right && bounds.bottom <= preview.bottom &&
           rectangle(ui_rect_overlap(scene.clip, preview));
}
bool ui_video_scene_project_play_only(UiVideoScene* scene, const UiVideoPlaybackState& playback,
                                      UiRect preview, UiRect clip, const UiVideoControlsStyle& controls,
                                      const UiVideoSceneStyle& style,
                                      const UiVideoPlayAffordanceStyle& affordance,
                                      const Theme& theme) noexcept {
    if (!scene)
        return false;
    const UiRect button = ui_video_play_affordance_rectangle(preview, affordance),
                 visible = ui_rect_overlap(preview, clip);
    UiVideoControlsProjection validated{};
    const float height = ui_video_controls_height(controls, controls.minimum_width, playback.collection_count != 0);
    if (!rectangle(clip) || !rectangle(button) || !rectangle(ui_rect_overlap(button, visible)) ||
        !scene_style_valid(style, controls) ||
        !ui_video_controls_project(playback, {0, 0, controls.minimum_width, height}, controls, theme,
                                   &validated)) {
        reset_value(scene);
        return false;
    }
    const bool same_source = scene->active && scene->play_only && same_playback_source(*scene, playback);
    const bool same_geometry = same_source && ui_rect_equal(scene->projection.bounds, button) &&
                               ui_rect_equal(scene->clip, visible);
    const auto focus = scene->focus, hover = scene->hover, capture = scene->capture;
    const auto previous_presentation = controls_presentation_state(*scene);
    const bool keyboard_focus = scene->keyboard_focus;
    const auto descriptor = validated.controls[0];
    const auto label =
        playback.error ? controls.error : ui_video_controls_label(validated, UiVideoControl::play_pause);
    if (label.bytes > 128) {
        reset_value(scene);
        return false;
    }
    reset_value(scene);
    scene->active = true;
    scene->play_only = true;
    scene->play_opacity = affordance.opacity;
    scene->clip = visible;
    scene->style = style;
    scene->preview = preview;
    scene->center_play = button;
    if (playback.buffering && !playback.error)
        scene->buffering_indicator = button;
    scene->style.radius = affordance.radius;
    scene->style.focus_width = affordance.stroke;
    scene->projection.playback = playback;
    scene->projection.bounds = button;
    scene->projection.controls[0] = descriptor;
    scene->projection.controls[0].bounds = button;
    reset_value(&scene->projection.controls[0].label);
    scene->label_bytes[0] = label.bytes;
    copy_bytes(scene->labels, label.data, label.bytes);
    if (same_source && eligible(*scene, focus)) {
        scene->focus = focus;
        scene->keyboard_focus = keyboard_focus;
    }
    if (same_geometry && eligible(*scene, hover))
        scene->hover = hover;
    if (same_geometry && eligible(*scene, capture))
        scene->capture = capture;
    restore_controls_presentation(*scene, previous_presentation, same_source, same_geometry);
    scene->text_revision = text_revision(*scene, theme);
    return true;
}
Utf8View ui_video_scene_label(const UiVideoScene& scene, UiVideoControl id) noexcept {
    for (u32 i = 0; i < kUiVideoControlCount; ++i)
        if (scene.projection.controls[i].id == id && scene.label_bytes[i] <= 128)
            return {scene.labels + scene.label_offsets[i], scene.label_bytes[i]};
    return {};
}
UiVideoControl ui_video_scene_hit(const UiVideoScene& scene, UiPoint point) noexcept {
    if (!scene.active || !contains(scene.clip, point))
        return UiVideoControl::none;
    if (eligible(scene, UiVideoControl::play_pause) && contains(scene.center_play, point)) {
        if (!scene.projection.playback.playing || scene.projection.playback.error ||
            controls_requested(scene))
            return UiVideoControl::play_pause;
    }
    if (!controls_requested(scene))
        return UiVideoControl::none;
    return ui_video_controls_hit(scene.projection, point);
}
bool ui_video_scene_hover(UiVideoScene* scene, UiPoint point) noexcept {
    if (!scene)
        return false;
    const bool previous_preview = scene->preview_hovered;
    const auto previous_control = scene->hover;
    scene->preview_hovered = scene->active && contains(ui_rect_overlap(scene->preview, scene->clip), point);
    scene->hover = ui_video_scene_hit(*scene, point);
    if (scene->hover == UiVideoControl::seek || scene->capture == UiVideoControl::seek || scene->hover == UiVideoControl::track || scene->capture == UiVideoControl::track)
        scene->hover_x = point.x;
    update_hover_time(*scene);
    settle_immediate_controls(*scene);
    return previous_preview != scene->preview_hovered || previous_control != scene->hover;
}
void ui_video_scene_titles(UiVideoScene* scene, Utf8View current, u16 track, Utf8View hovered) noexcept {
    if (!scene || !scene->active) return;
    scene->track_title_bytes = static_cast<u16>(utf8_copy_truncated(scene->track_title, sizeof(scene->track_title), current));
    if (track && track == scene->hover_track && hovered.bytes) {
        copy_bytes(scene->hover_time, "Track ", 6); scene->hover_time_bytes = 6;
        scene->hover_time_bytes += decimal_write(scene->hover_time + scene->hover_time_bytes, track);
        copy_bytes(scene->hover_time + scene->hover_time_bytes, " - ", 3); scene->hover_time_bytes += 3;
        scene->hover_time_bytes += utf8_copy_truncated(scene->hover_time + scene->hover_time_bytes, sizeof(scene->hover_time) - scene->hover_time_bytes, hovered);
    }
}
bool ui_video_scene_focus(UiVideoScene* scene, UiVideoControl id, bool keyboard) noexcept {
    if (!scene)
        return false;
    const auto* control = descriptor(*scene, id);
    const bool keyboard_volume = keyboard && id == UiVideoControl::volume && control && control->enabled &&
                                 rectangle(ui_rect_overlap(control->bounds, scene->clip));
    if (!eligible(*scene, id) && !keyboard_volume)
        return false;
    scene->focus = id;
    scene->keyboard_focus = keyboard;
    settle_immediate_controls(*scene);
    return true;
}
void ui_video_scene_blur(UiVideoScene* scene) noexcept {
    if (scene) {
        scene->focus = UiVideoControl::none;
        scene->keyboard_focus = false;
        scene->hover = UiVideoControl::none;
        scene->capture = UiVideoControl::none;
        scene->preview_hovered = false;
        update_hover_time(*scene);
        settle_immediate_controls(*scene);
    }
}
UiVideoSceneResult ui_video_scene_pointer(UiVideoScene* scene, UiVideoPointerEvent event,
                                          UiPoint point) noexcept {
    UiVideoSceneResult result{};
    if (!scene)
        return result;
    if (event == UiVideoPointerEvent::leave) {
        scene->preview_hovered = false;
        scene->hover = UiVideoControl::none;
        update_hover_time(*scene);
        settle_immediate_controls(*scene);
        return result;
    }
    if (event == UiVideoPointerEvent::cancel) {
        result.handled = result.release_capture = scene->capture != UiVideoControl::none;
        scene->capture = scene->hover = UiVideoControl::none;
        scene->preview_hovered = false;
        update_hover_time(*scene);
        settle_immediate_controls(*scene);
        return result;
    }
    if (!scene->active)
        return result;
    ui_video_scene_hover(scene, point);
    const auto hit = ui_video_scene_hit(*scene, point);
    scene->hover = hit;
    if (event == UiVideoPointerEvent::down) {
        if (scene->capture != UiVideoControl::none)
            return result;
        if (hit == UiVideoControl::none) {
            // The shown bar's inert parts (a disabled speaker on a silent
            // video, the gaps between controls) are not the picture: a press
            // there must not fall through and toggle playback.
            result.handled =
                controls_requested(*scene) && contains(ui_rect_overlap(scene->projection.bounds, scene->clip), point);
            return result;
        }
        scene->capture = hit;
        scene->capture_value_valid = false;
        ui_video_scene_focus(scene, hit, false);
        result.handled = result.request_capture = true;
        const auto* control = descriptor(*scene, hit);
        if (control && control->kind == UiVideoControlKind::range) {
            result.action_requested = range_request(*scene, hit, point, &result.action);
            if (hit == UiVideoControl::track) result.action_requested = false;
        }
    } else if (scene->capture != UiVideoControl::none) {
        result.handled = true;
        const auto captured = scene->capture;
        const auto* control = descriptor(*scene, captured);
        if (control && control->kind == UiVideoControlKind::range) {
            if (captured == UiVideoControl::track && event == UiVideoPointerEvent::up) scene->capture_value_valid = false;
            result.action_requested = range_request(*scene, captured, point, &result.action);
            if (captured == UiVideoControl::track && event != UiVideoPointerEvent::up) result.action_requested = false;
        }
        if (event == UiVideoPointerEvent::up) {
            if (control && control->kind == UiVideoControlKind::button && hit == captured)
                result.action_requested =
                    ui_video_controls_pointer_action(scene->projection,
                                                     {(control->bounds.left + control->bounds.right) * .5f,
                                                      (control->bounds.top + control->bounds.bottom) * .5f},
                                                     &result.action);
            scene->capture = UiVideoControl::none;
            result.release_capture = true;
        }
    } else
        result.handled = hit != UiVideoControl::none;
    settle_immediate_controls(*scene);
    return result;
}
UiVideoSceneResult ui_video_scene_player_key(UiVideoScene* scene, UiVideoPlayerKey key, u32 percent,
                                             u64 now_ms) noexcept {
    UiVideoSceneResult result{};
    if (!scene || !scene->active)
        return result;
    ui_video_scene_activity(scene, now_ms);
    result.action_requested = ui_video_controls_player_key(scene->projection, key, percent, &result.action);
    if (result.action_requested && result.action.control == scene->capture)
        scene->capture_value_valid = false;
    result.handled = true;
    return result;
}
UiVideoSceneResult ui_video_scene_key(UiVideoScene* scene, UiVideoKey key) noexcept {

    UiVideoSceneResult result{};
    if (!scene || !scene->active)
        return result;
    // Descriptor navigation is filtered through the scene's visible clip.
    auto projection = scene->projection;
    for (auto& control : projection.controls) {
        const bool hidden_volume = control.id == UiVideoControl::volume && control.enabled &&
                                   rectangle(ui_rect_overlap(control.bounds, scene->clip));
        if (!eligible(*scene, control.id) && !hidden_volume) {
            control.enabled = false;
        }
    }
    result.handled = ui_video_controls_key(projection, key, &scene->focus, &result.action);
    if (result.handled)
        scene->keyboard_focus = true;
    settle_immediate_controls(*scene);
    result.action_requested = result.handled && result.action.control != UiVideoControl::none;
    if (result.action_requested && result.action.control == scene->capture)
        scene->capture_value_valid = false;
    return result;
}
u64 ui_video_scene_paint_revision(const UiVideoScene& scene) noexcept {
    u64 hash = hash64_mix(kHash64Seed, scene.active);
    if (!scene.active)
        return hash;
    hash = hash64_mix(hash, scene.text_revision);
    hash = hash64_mix(hash, scene.play_only);
    hash = hash64_bytes(hash, &scene.play_opacity, sizeof(scene.play_opacity));
    hash = hash64_bytes(hash, &scene.style, sizeof(scene.style));
    hash = hash64_bytes(hash, &scene.preview, sizeof(scene.preview));
    hash = hash64_bytes(hash, &scene.center_play, sizeof(scene.center_play));
    hash = hash64_bytes(hash, &scene.buffering_indicator, sizeof(scene.buffering_indicator));
    hash = hash64_mix(hash, scene.spinner_phase);
    hash = hash64_mix(hash, static_cast<u8>(scene.focus));
    hash = hash64_mix(hash, static_cast<u8>(scene.hover));
    hash = hash64_mix(hash, static_cast<u8>(scene.capture));
    if (scene.capture == UiVideoControl::track) hash = hash64_mix(hash, scene.capture_value_valid ? scene.capture_value : 0);
    hash = hash64_mix(hash, scene.keyboard_focus);
    hash = hash64_mix(hash, scene.preview_hovered);
    hash = hash64_mix(hash, scene.idle);
    if (scene.hover_time_bytes)
        hash = hash64_bytes(hash, &scene.hover_x, sizeof(scene.hover_x));
    hash = hash64_bytes(hash, &scene.controls_transition.value, sizeof(scene.controls_transition.value));
    hash = hash64_mix(hash, scene.projection.playback.playing);
    hash = hash64_mix(hash, scene.projection.playback.muted);
    hash = hash64_mix(hash, scene.projection.playback.has_audio);
    hash = hash64_mix(hash, scene.projection.playback.expanded);
    hash = hash64_mix(hash, scene.projection.playback.buffered_count);
    hash = hash64_bytes(hash, scene.projection.playback.buffered_begin,
                        sizeof(scene.projection.playback.buffered_begin));
    hash = hash64_bytes(hash, scene.projection.playback.buffered_end,
                        sizeof(scene.projection.playback.buffered_end));
    for (const auto& control : scene.projection.controls) {
        hash = hash64_mix(hash, control.value);
        hash = hash64_mix(hash, control.maximum);
        hash = hash64_mix(hash, control.enabled);
    }
    hash = hash64_counted_bytes(hash, scene.hover_time, scene.hover_time_bytes);
    return hash64_counted_bytes(hash, scene.track_title, scene.track_title_bytes);
}
u64 ui_video_scene_text_paint_revision(const UiVideoScene& scene) noexcept {
    u64 hash = hash64_bytes(scene.text_revision, &scene.controls_transition.value,
                            sizeof(scene.controls_transition.value));
    if (scene.hover_time_bytes)
        hash = hash64_bytes(hash64_counted_bytes(hash, scene.hover_time, scene.hover_time_bytes), &scene.hover_x,
                            sizeof(scene.hover_x));
    return hash64_counted_bytes(hash, scene.track_title, scene.track_title_bytes);
}
float ui_video_scene_chrome_opacity(const UiVideoScene& scene) noexcept {
    return scene.active && !scene.play_only && scene.preview_hovered ? scene.controls_transition.value : 1.0f;
}
UiRect ui_video_scene_hover_bubble(const UiVideoScene& scene) noexcept {
    const auto* control = descriptor(scene, UiVideoControl::seek);
    if (!scene.active || !scene.hover_time_bytes || !control)
        return {};
    constexpr float padding = 8, height = 20, gap = 4;
    const UiRect bounds = ui_rect_overlap(scene.preview, scene.clip);
    const float width = minimum(ui_rect_width(bounds), scene.hover_time_bytes * scene.text_advance + 2 * padding);
    const float left = maximum(bounds.left, minimum(bounds.right - width, scene.hover_x - width * .5f));
    const float bottom = scene.projection.bounds.top - gap;
    return {left, bottom - height, left + width, bottom};
}
u64 ui_video_scene_next_wake(const UiVideoScene& scene, u64 now_ms, bool reduced_motion) noexcept {
    if (!scene.active)
        return 0;
    // Idleness changes visibility, not motion, so it also wakes reduced motion.
    u64 wake = 0;
    if (!scene.idle && scene.activity_ms && idle_countdown(scene) &&
        rectangle(ui_rect_overlap(scene.projection.bounds, scene.clip))) {
        const u64 deadline = scene.activity_ms + scene.style.idle_ms;
        wake = deadline > now_ms ? deadline : now_ms + 1;
    }
    if (scene.hover_track && scene.track_hover_since_ms && !scene.track_hover_requested) {
        const u64 due = scene.track_hover_since_ms + 150;
        if (!wake || due < wake) wake = maximum(due, now_ms + 1);
    }
    if (reduced_motion)
        return wake;
    const u64 spinner =
        ui_spinner_next_wake(now_ms, rectangle(ui_rect_overlap(scene.buffering_indicator, scene.clip)), false);
    const u64 transition =
        ui_opacity_transition_next_wake(scene.controls_transition, now_ms, false,
                                        rectangle(ui_rect_overlap(scene.projection.bounds, scene.clip)));
    if (spinner && (!wake || spinner < wake))
        wake = spinner;
    if (transition && (!wake || transition < wake))
        wake = transition;
    return wake;
}
bool ui_video_scene_activity(UiVideoScene* scene, u64 now_ms) noexcept {
    if (!scene || !scene->active || !now_ms)
        return false;
    const bool was_idle = scene->idle;
    scene->activity_ms = now_ms;
    scene->idle = false;
    settle_immediate_controls(*scene);
    return was_idle;
}
bool ui_video_scene_tick(UiVideoScene* scene, u64 now_ms, bool reduced_motion) noexcept {
    if (!scene || !scene->active)
        return false;
    if (scene->hover_track && !scene->track_hover_since_ms) scene->track_hover_since_ms = now_ms ? now_ms : 1;
    if (!scene->activity_ms)
        scene->activity_ms = now_ms ? now_ms : 1;
    const bool idle = idle_countdown(*scene) && now_ms >= scene->activity_ms &&
                      now_ms - scene->activity_ms >= scene->style.idle_ms;
    bool changed = idle != scene->idle;
    scene->idle = idle;
    settle_immediate_controls(*scene);
    changed = ui_opacity_transition_tick(
                  &scene->controls_transition, controls_requested(*scene) ? 1.0f : 0.0f, now_ms,
                  scene->style.fade_ms, reduced_motion,
                  rectangle(ui_rect_overlap(scene->projection.bounds, scene->clip))) ||
              changed;
    if (rectangle(ui_rect_overlap(scene->buffering_indicator, scene->clip))) {
        const u8 phase = ui_spinner_phase(now_ms, reduced_motion);
        changed = changed || scene->spinner_phase != phase;
        scene->spinner_phase = phase;
    }
    return changed;
}
bool ui_video_scene_paint(const UiVideoScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    if (!scene.active)
        return true;
    if (!paint_stream_push_clip(paint, scene.clip.left, scene.clip.top, scene.clip.right,
                                scene.clip.bottom))
        return false;
    const float opacity =
        scene.play_only && (!scene.projection.playback.playing || scene.projection.playback.error ||
                            scene.projection.playback.buffering)
            ? 1.0f
            : scene.controls_transition.value;
    // Media chrome sits on video pixels: a black scrim with light glyphs in
    // every appearance; only the accent follows the active theme.
    const Theme& ink = kDefaultTheme;
    Color surface = theme.tokens.overlay.media;
    surface.alpha = scene.style.surface_opacity * opacity;
    const auto bar = scene.projection.bounds;
    bool ok = true;
    if (scene.track_title_bytes) {
        const UiRect title{scene.preview.left, scene.preview.top, scene.preview.right, scene.preview.top + kUiVideoControlBarHeight};
        ok = quad(paint, title, surface, scene.style.radius);
    }
    if (!scene.play_only && opacity > 0) {
        const float lead = minimum(scene.style.scrim_lead, maximum(0.0f, bar.top - scene.preview.top));
        // The bar shares the preview's rounded bottom edge; its top stays square.
        const float radius = bar.bottom >= scene.preview.bottom - .5f ? scene.style.preview_radius : 0;
        ok = (lead <= 0 || paint_stream_push_gradient(paint, {bar.left, bar.top - lead, bar.right, bar.top},
                                                      {0, 0, 0, 0}, surface)) &&
             paint_stream_push_clip(paint, bar.left, bar.top, bar.right, bar.bottom) &&
             quad(paint, {bar.left, bar.top - radius, bar.right, bar.bottom}, surface, radius) &&
             paint_stream_pop_clip(paint);
    }
    for (const auto& control : scene.projection.controls) {
        if (!ok || opacity == 0 || !control.visible || !rectangle(ui_rect_overlap(control.bounds, scene.clip)))
            continue;
        const bool engaged = scene.hover == control.id || scene.capture == control.id;
        if (control.kind == UiVideoControlKind::button) {
            if (scene.play_only && !rectangle(scene.buffering_indicator)) {
                Color fill = theme.base_lowest;
                fill.alpha = scene.play_opacity * opacity;
                ok = quad(paint, control.bounds, fill, scene.style.radius);
            }
            if (ok && !(scene.play_only && rectangle(scene.buffering_indicator))) {
                // The glyph brightens on hover and nudges while pressed.
                Color color = scene.play_only && scene.projection.playback.error ? theme.notification
                              : control.enabled                                  ? ink.text_primary
                                                                                 : ink.text_secondary;
                if (control.enabled && !engaged && !scene.play_only)
                    color.alpha *= .82f;
                ok = symbol(paint, control.bounds, control.id, scene.projection.playback,
                            scene.play_only ? kUiMediaControlTarget : scene.style.icon_extent,
                            ui_theme_color_alpha(color, opacity), scene.play_only && scene.projection.playback.error,
                            scene.capture == control.id && scene.hover == control.id ? 1.0f : 0.0f);
            }
        } else if (control.kind == UiVideoControlKind::range) {
            const float center_y = control.id == UiVideoControl::track ? (control.bounds.top + 3 * control.bounds.bottom) * .25f :
                (control.bounds.top + control.bounds.bottom) * .5f;
            const float half_thumb = scene.style.thumb_size * .5f;
            const auto track = range_track(control.bounds, scene.style, control.id == UiVideoControl::track);
            const float left = track.left, right = track.right;
            const u32 selected = control.id == UiVideoControl::track && scene.capture == control.id && scene.capture_value_valid
                ? static_cast<u32>(scene.capture_value - 1) : control.value;
            const float value = control.maximum ? static_cast<float>(selected) / control.maximum : 0;
            const float position = left + (right - left) * value;
            const float round = scene.style.track_height * .5f;
            if (control.id == UiVideoControl::volume) {
                // The shared slider on a pill whose ends share the grabber's centres.
                auto tokens = volume_tokens(scene.style);
                tokens.color.brand = tokens.color.brand_hover = theme.tokens.emphasis.fill;
                const auto slider = volume_slider(tokens, control.bounds, control.value);
                const float inset = theme.scale.space_2;
                const UiRect pill{control.bounds.left, slider.grabber.top - inset, control.bounds.right,
                                  slider.grabber.bottom + inset};
                // The resting slider: its pill already marks hover, and a grown
                // grabber would leave the pill.
                const u32 first = paint->count;
                const float radius = ui_rect_height(pill) * .5f;
                ok = quad(paint, pill, theme.tokens.overlay.dense, radius) &&
                     ui_slider_paint(paint, tokens, slider, kVolumeSteps, kVolumeSteps, {control.enabled});
                if (ok && scene.keyboard_focus && scene.focus == control.id) {
                    const float gap = scene.style.focus_width;
                    ok = paint_stream_push_rounded_border(paint, pill.left - gap, pill.top - gap, pill.right + gap,
                                                          pill.bottom + gap, {}, theme.tokens.border.focus,
                                                          radius + gap, scene.style.focus_width);
                }
                for (u32 i = first; i < paint->count; ++i) {
                    paint->quads[i].fill.alpha *= opacity;
                    paint->quads[i].border.alpha *= opacity;
                }
                continue;
            }
            Color track_color = ink.text_primary;
            track_color.alpha *= scene.style.track_opacity * opacity;
            ok = ok && quad(paint, track, track_color, round);
            if (control.id == UiVideoControl::seek) {
                // Buffered ranges sit between the empty track and the played fill.
                const auto& playback = scene.projection.playback;
                const float scale = (right - left) / 10000.0f;
                for (u32 i = 0; ok && i < minimum<u32>(playback.buffered_count, 4); ++i) {
                    const float from = left + scale * minimum(playback.buffered_begin[i], u16{10000});
                    const float to = left + scale * minimum(playback.buffered_end[i], u16{10000});
                    if (to > from)
                        ok = quad(paint, {from, track.top, to, track.bottom},
                                  ui_theme_color_alpha(ink.text_primary, .25f * opacity), round);
                }
            }
            if (ok && control.id == UiVideoControl::seek && scene.hover == UiVideoControl::seek &&
                scene.capture == UiVideoControl::none && scene.hover_x > left) {
                // Hover preview: the would-be position, below the played fill.
                ok = quad(paint, {left, track.top, minimum(right, scene.hover_x), track.bottom},
                          ui_theme_color_alpha(ink.text_primary, .3f * opacity), round);
            }
            if (ok && position > left) {
                ok = quad(paint, {left, track.top, position, track.bottom},
                          ui_theme_color_alpha(control.enabled ? theme.tokens.emphasis.fill : ink.text_secondary, opacity), round);
            }
            if (ok && (engaged || (scene.keyboard_focus && scene.focus == control.id))) {
                ok = quad(paint,
                          {position - half_thumb, center_y - half_thumb, position + half_thumb,
                           center_y + half_thumb},
                          ui_theme_color_alpha(theme.tokens.emphasis.outline, opacity), half_thumb);
            }
            if (ok && (control.id == UiVideoControl::seek && !scene.hover_track || control.id == UiVideoControl::track && scene.hover_track) && scene.hover_time_bytes) {
                const auto bubble = ui_video_scene_hover_bubble(scene);
                ok = quad(paint, bubble, ui_theme_color_alpha(theme.tokens.overlay.dense, opacity),
                          theme.scale.radius_sm);
            }
        } else if (control.id == UiVideoControl::status && ui_video_scene_label(scene, control.id).bytes) {
            ok = quad(paint, control.bounds, surface, scene.style.radius);
        }
        if (ok && scene.keyboard_focus && scene.focus == control.id && control.enabled) {
            ok = paint_stream_push_rounded_border(
                paint, control.bounds.left, control.bounds.top, control.bounds.right, control.bounds.bottom,
                {}, ui_theme_color_alpha(theme.tokens.border.focus, opacity), scene.style.radius, scene.style.focus_width);
        }
    }
    if (ok && !scene.play_only && rectangle(scene.center_play)) {
        ok = ui_video_play_affordance_paint(scene.preview, scene.clip, {}, theme, paint);
    }
    if (ok && rectangle(ui_rect_overlap(scene.buffering_indicator, scene.clip))) {
        const auto backdrop = scene.buffering_indicator;
        Color background = theme.base_lowest;
        background.alpha *= scene.style.surface_opacity;
        const float size = minimum(scene.style.spinner_extent,
                                   minimum(ui_rect_width(backdrop), ui_rect_height(backdrop)) * .6f);
        const float x = (backdrop.left + backdrop.right - size) * .5f;
        const float y = (backdrop.top + backdrop.bottom - size) * .5f;
        auto spinner = scene.style.spinner;
        spinner.stroke = minimum(spinner.stroke, size * .25f);
        ok = quad(paint, backdrop, background, ui_rect_width(backdrop) * .5f) &&
             ui_spinner_paint(paint, {x, y, x + size, y + size}, theme.text_primary, scene.spinner_phase,
                              spinner);
    }
    return paint_stream_pop_clip(paint) && ok;
}
bool ui_video_scene_text_output(const UiVideoScene& scene, const Theme& theme, UiTextOutput* text,
                                UiWidgetReference text_owner) noexcept {
    if (!scene.active)
        return true;
    if (scene.track_title_bytes) {
        const UiRect title{scene.preview.left + 8, scene.preview.top, scene.preview.right - 8, scene.preview.top + kUiVideoControlBarHeight};
        Color color = kDefaultTheme.text_primary; color.alpha *= scene.controls_transition.value;
        if (!ui_text_output_push(text, {{scene.track_title, scene.track_title_bytes}, text_owner, title, ui_rect_overlap(title, scene.clip), color, text_style_subtext, UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::ellipsis})) return false;
    }
    if (const auto bubble = ui_video_scene_hover_bubble(scene); ui_rect_has_area(bubble)) {
        Color color = kDefaultTheme.text_primary;
        color.alpha *= scene.controls_transition.value;
        if (!ui_text_output_push(text, {{scene.hover_time, scene.hover_time_bytes}, text_owner, bubble, scene.clip,
                                        color, text_style_subtext, UiTextAlignment::center, UiTextAlignment::center,
                                        UiTextWrapping::no_wrap, UiTextOverflow::ellipsis}))
            return false;
    }
    for (const auto& control : scene.projection.controls) {
        const auto label = ui_video_scene_label(scene, control.id);
        const auto clip = ui_rect_overlap(control.bounds, scene.clip);
        if (!control.visible || !label.bytes || !rectangle(clip) ||
            control.kind == UiVideoControlKind::button)
            continue;
        Color color = control.enabled || control.kind == UiVideoControlKind::label
                          ? kDefaultTheme.text_primary
                          : kDefaultTheme.text_secondary;

        if (control.id == UiVideoControl::status)
            color = scene.projection.playback.error ? theme.notification : theme.text_secondary;
        color.alpha *= scene.controls_transition.value;
        auto label_bounds = control.bounds;
        if (control.id == UiVideoControl::track) label_bounds.bottom = (label_bounds.top + label_bounds.bottom) * .5f;
        if (!ui_text_output_push(text, {label, text_owner, label_bounds, ui_rect_overlap(label_bounds, clip), color, control.text_style,
                                        control.id == UiVideoControl::track ? UiTextAlignment::center
                                                                                   : UiTextAlignment::start,
                                        UiTextAlignment::center, UiTextWrapping::no_wrap,
                                        UiTextOverflow::ellipsis}))
            return false;
    }
    return true;
}
} // namespace maicord
