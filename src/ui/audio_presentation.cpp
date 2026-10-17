#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/ui/audio_presentation.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/base64.hpp"

namespace maicord {
namespace {
bool rounded(PaintStream* paint, UiRect rectangle, Color fill, float radius) noexcept {
    return !ui_rect_has_area(rectangle) ||
           paint_stream_push_rounded(paint, rectangle.left, rectangle.top, rectangle.right,
                                     rectangle.bottom, fill, radius);
}
bool owner_current(UiWidgetReference owner) noexcept {
    return owner.manager && ui_widget_manager_resolve(*owner.manager, owner.handle);
}
bool range_paint(PaintStream* paint, UiRect bounds, float fraction, Color track, Color played) noexcept {
    const float center = (bounds.top + bounds.bottom) * .5f;
    const UiRect rail{bounds.left, center - 2, bounds.right, center + 2};
    const UiRect progress{rail.left, rail.top, rail.left + ui_rect_width(rail) * fraction, rail.bottom};
    return rounded(paint, rail, track, 2) && rounded(paint, progress, played, 2);
}
bool waveform_paint(const UiAudioControlsProjection& projection, const UiAudioControlsStyle& style,
                    PaintStream* paint) noexcept {
    const auto& state = projection.playback;
    const auto bounds = projection.geometry.seek;
    const float fraction = state.duration_ms
                               ? static_cast<float>(minimum(state.elapsed_ms, state.duration_ms)) /
                                     static_cast<float>(state.duration_ms)
                               : 0;
    if (!state.waveform.count)
        return range_paint(paint, bounds, fraction, projection.track, projection.played);
    if (!state.waveform.data || state.waveform.count > kUiAudioWaveformMaximum)
        return false;
    const float width = ui_rect_width(bounds);
    const u32 count =
        minimum(24u, minimum(state.waveform.count,
                             static_cast<u32>(maximum(1.f, width / (2 + style.waveform_gap)))));
    const float step = width / count;
    const float bar = minimum(4.f, maximum(1.f, step - style.waveform_gap));
    const float center = (bounds.top + bounds.bottom) * .5f;
    const float maximum_height = maximum(style.minimum_peak_height, ui_rect_height(bounds) - 4);
    for (u32 i = 0; i < count; ++i) {
        const u32 first = i * state.waveform.count / count;
        const u32 end = (i + 1) * state.waveform.count / count;
        u8 peak{};
        for (u32 sample = first; sample < end; ++sample)
            peak = maximum(peak, state.waveform[sample]);
        const float height = maximum(style.minimum_peak_height, maximum_height * peak / 255.f);
        const float left = bounds.left + i * step + (step - bar) * .5f;
        const Color color =
            left + bar * .5f <= bounds.left + width * fraction ? projection.played : projection.waveform;
        if (!rounded(paint, {left, center - height * .5f, left + bar, center + height * .5f}, color,
                     bar * .5f))
            return false;
    }
    return true;
}
} // namespace

bool ui_audio_attachment_project(const MessageRichStore& store, const MessageRichAttachment& attachment,
                                 u64 generation, u64 revision, bool voice, UiRect bounds,
                                 const Theme& theme, Slice<u8> waveform_scratch,
                                 UiAudioControlsProjection* out) noexcept {
    if (!message_rich_attachment_audio_supported(store, attachment))
        return false;
    UiAudioPlaybackState state{};
    state.owner = attachment.id;
    state.generation = generation;
    state.revision = revision;
    state.voice = voice;
    state.has_duration = (attachment.state & 8u) != 0;
    state.duration_ms = attachment.duration_ms;
    state.volume = 100;
    state.can_play = message_rich_store_string(store, attachment.url).bytes != 0;
    state.can_download = state.can_play;
    state.title = message_rich_store_string(store, attachment.filename);
    if (!state.title.bytes)
        state.title = utf8_literal("Audio attachment");
    u32 title_bytes{};
    while (title_bytes < state.title.bytes && title_bytes < 512) {
        const auto scalar =
            utf8_decode_one(state.title.data + title_bytes, state.title.bytes - title_bytes);
        if (!scalar.valid)
            return false;
        if (scalar.width > 512 - title_bytes)
            break;
        title_bytes += scalar.width;
    }
    state.title.bytes = title_bytes;
    const auto waveform = message_rich_store_string(store, attachment.waveform);
    if (waveform.bytes && waveform_scratch.count) {
        const auto decoded = base64_decode(waveform, waveform_scratch);
        if (!decoded.ok() || decoded.bytes > kUiAudioWaveformMaximum)
            return false;
        state.waveform = {waveform_scratch.data, decoded.bytes};
    }
    return ui_audio_controls_project(state, bounds, ui_theme_audio_style(theme), theme, out);
}

bool ui_audio_controls_paint(const UiAudioControlsProjection& projection, const UiAudioControlsStyle& style,
                             UiRect clip, const Theme& theme, UiWidgetReference owner, PaintStream* paint,
                             UiAudioControl focus, UiAudioControl hover, bool keyboard_focus) noexcept {
    if (!paint || !owner_current(owner) || !ui_rect_has_area(projection.geometry.bounds) ||
        !ui_rect_valid_or_empty(clip))
        return false;
    const auto visible = ui_rect_intersection(projection.geometry.bounds, clip);
    if (!ui_rect_has_area(visible))
        return true;
    if (!paint_stream_push_clip(paint, visible.left, visible.top, visible.right, visible.bottom))
        return false;
    const auto paint_controls = [&]() noexcept {
        const auto& bounds = projection.geometry.bounds;
        if (!ui_background_paint_surface(paint, bounds, theme, projection.surface, theme.interactive,
                                         style.corner_radius, 1, 1, UiBackgroundSurfaceMode::gradient,
                                         UiBackgroundTier::high, UiBackgroundBorderTone::palette) ||
            !paint_stream_record_content(paint, owner, bounds, UiLayoutContentKind::control,
                                         ui_layout_content_bounds_allow_overlap))
            return false;
        if (!projection.playback.loading && !projection.playback.error &&
            !waveform_paint(projection, style, paint))
            return false;
        for (const auto& control : projection.controls) {
            if (!control.visible || !ui_rect_has_area(control.bounds))
                continue;
            if (control.kind == UiAudioControlKind::button) {
                const bool primary = control.id == UiAudioControl::play_pause;
                const auto fill = primary && control.enabled               ? theme.tokens.emphasis.fill
                                  : hover == control.id && control.enabled ? theme.surface_high
                                                                           : projection.surface;
                if (!rounded(paint, control.bounds, fill,
                             primary ? ui_rect_width(control.bounds) * .5f : kUiControlCornerRadius))
                    return false;
                UiIcon icon{};
                if (primary)
                    icon = projection.playback.playing ? UiIcon::pause : UiIcon::play;
                else if (control.id == UiAudioControl::mute)
                    icon = projection.playback.muted ? UiIcon::speaker_muted : UiIcon::speaker;
                else if (control.id == UiAudioControl::download)
                    icon = UiIcon::download;
                else
                    continue;
                const Color color =
                    primary && control.enabled ? ui_surface_foreground(theme, fill) : control.foreground;
                if (!ui_icon_paint(paint, icon, ui_rect_shrink(control.bounds, 6), color))
                    return false;
            } else if (control.id == UiAudioControl::volume) {
                const float fraction =
                    control.maximum ? static_cast<float>(control.value) / control.maximum : 0;
                const auto track = ui_audio_controls_range_track(projection, UiAudioControl::volume);
                if (!range_paint(paint, track, fraction, projection.track, projection.played))
                    return false;
                const float x = track.left + ui_rect_width(track) * fraction;
                const float y = (track.top + track.bottom) * .5f;
                const float radius = projection.volume_thumb_inset;
                if (!rounded(paint, {x - radius, y - radius, x + radius, y + radius}, control.foreground,
                             radius))
                    return false;
            }
            if (keyboard_focus && control.enabled && control.id == focus &&
                !paint_stream_push_rounded_border(paint, control.bounds.left, control.bounds.top,
                                                  control.bounds.right, control.bounds.bottom, {},
                                                  projection.focus, kUiControlCornerRadius, 2))
                return false;
        }
        return true;
    };
    const bool painted = paint_controls();
    const bool popped = paint_stream_pop_clip(paint);
    return painted && popped;
}

bool ui_audio_controls_text_output(const UiAudioControlsProjection& projection, UiRect clip,
                                   UiWidgetReference owner, UiTextOutput* output) noexcept {
    if (!output || !owner_current(owner) || !ui_rect_valid_or_empty(clip))
        return false;
    const auto visible = ui_rect_intersection(projection.geometry.bounds, clip);
    for (const auto& control : projection.controls) {
        if (!control.visible || control.kind != UiAudioControlKind::label)
            continue;
        const auto label = ui_audio_controls_label(projection, control.id);
        if (!label.bytes || !ui_rect_has_area(ui_rect_intersection(control.bounds, visible)))
            continue;
        bool compact_time_lines{};
        if (projection.geometry.compact && control.id == UiAudioControl::time)
            for (u32 i = 0; i < label.bytes; ++i)
                compact_time_lines = compact_time_lines || label.data[i] == '\n';
        if (!ui_text_output_push_owned(output,
                                       {label, owner, control.bounds, visible, control.foreground,
                                        control.text_style, UiTextAlignment::start, UiTextAlignment::center,
                                        compact_time_lines ? UiTextWrapping::wrap : UiTextWrapping::no_wrap,
                                        UiTextOverflow::ellipsis}))
            return false;
    }
    return true;
}

bool ui_audio_scene_paint(const UiAudioScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    return !scene.active ||
           ui_audio_controls_paint(scene.projection, scene.style, scene.clip, theme, scene.owner, paint,
                                   scene.focus, scene.hover, scene.keyboard_focus);
}
bool ui_audio_scene_text_output(const UiAudioScene& scene, UiTextOutput* output) noexcept {
    return !scene.active ||
           ui_audio_controls_text_output(scene.projection, scene.clip, scene.owner, output);
}
u64 ui_audio_scene_text_revision(const UiAudioScene& scene) noexcept {
    if (!scene.active || !owner_current(scene.owner))
        return 0;
    UiTextEmitter emitter{};
    UiTextOutput output{nullptr, &emitter};
    return ui_text_emitter_initialize(&emitter) && ui_audio_scene_text_output(scene, &output)
               ? emitter.signature
               : 0;
}
u64 ui_audio_scene_paint_revision(const UiAudioScene& scene) noexcept {
    if (!scene.active || !owner_current(scene.owner))
        return 0;
    const auto& projection = scene.projection;
    const auto& state = projection.playback;
    const auto& geometry = projection.geometry;
    const UiRect rectangles[]{geometry.bounds, geometry.title,    geometry.play_pause,
                              geometry.seek,   geometry.time,     geometry.mute,
                              geometry.volume, geometry.download, geometry.status};
    u64 hash = hash64_bytes(kHash64Seed, rectangles, sizeof(rectangles));
    hash = hash64_mix(hash, u32{geometry.voice} | (u32{geometry.compact} << 1u) |
                                (u32{geometry.volume_expanded} << 2u));
    hash = hash64_bytes(hash, &scene.clip, sizeof(scene.clip));
    hash = hash64_mix(hash, reinterpret_cast<usize>(scene.owner.manager));
    hash = hash64_mix(hash, scene.owner.handle.slot);
    hash = hash64_mix(hash, scene.owner.handle.generation);
    hash = hash64_mix(hash, state.owner);
    hash = hash64_mix(hash, state.generation);
    hash = hash64_mix(hash, state.revision);
    hash = hash64_mix(hash, state.elapsed_ms);
    hash = hash64_mix(hash, state.duration_ms);
    hash = hash64_mix(hash, u32{state.playing} | (u32{state.muted} << 1u) | (u32{state.loading} << 2u) |
                                (u32{state.error} << 3u));
    hash = hash64_mix(hash, state.volume);
    hash = hash64_mix(hash, static_cast<u8>(scene.focus));
    hash = hash64_mix(hash, static_cast<u8>(scene.hover));
    hash = hash64_mix(hash, scene.keyboard_focus);
    const float metrics[]{scene.style.corner_radius, scene.style.waveform_gap,
                          scene.style.minimum_peak_height, projection.volume_thumb_inset};
    hash = hash64_bytes(hash, metrics, sizeof(metrics));
    const Color colors[]{projection.surface, projection.focus, projection.track, projection.waveform,
                         projection.played};
    hash = hash64_bytes(hash, colors, sizeof(colors));
    for (const auto& control : projection.controls) {
        hash = hash64_mix(hash, static_cast<u8>(control.id));
        hash = hash64_bytes(hash, &control.bounds, sizeof(control.bounds));
        hash = hash64_bytes(hash, &control.foreground, sizeof(control.foreground));
        hash = hash64_mix(hash, control.value);
        hash = hash64_mix(hash, control.maximum);
        hash = hash64_mix(hash, u32{control.enabled} | (u32{control.visible} << 1u));
    }
    return hash64_counted_bytes(hash, state.waveform.data, state.waveform.count);
}
} // namespace maicord
