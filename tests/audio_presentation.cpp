#include "maicord/ui/audio_presentation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/paint.hpp"

namespace maicord {
namespace {
bool same(Color a, Color b) noexcept {
    return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}
struct Owner {
    UiWidget widgets[2]{};
    UiLayoutNode nodes[2]{};
    UiWidgetManager manager{};
    UiWidgetHandle root{};
    bool initialize() noexcept {
        if (!ui_widget_manager_initialize(&manager, {widgets, 2}, {nodes, 2}))
            return false;
        const auto result =
            ui_widget_manager_add(&manager, ui_widget_invalid_handle(),
                                  {1,
                                   UiWidgetKind::button,
                                   ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable,
                                   {utf8_literal("Audio attachment"), ui_layout_style(), {}, 0, 1}});
        root = result.handle;
        return result.ok();
    }
};
UiAudioPlaybackState playback() noexcept {
    UiAudioPlaybackState state{};
    state.owner = 40;
    state.generation = 41;
    state.revision = 1;
    state.elapsed_ms = 30'000;
    state.duration_ms = 120'000;
    state.volume = 50;
    state.has_duration = state.can_play = state.can_seek = state.can_volume = state.can_download = true;
    state.title = utf8_literal("Original recording.ogg");
    return state;
}
bool waveform_geometry(const PaintStream& paint, const UiAudioControlsProjection& projection, u32 expected,
                       bool progress) noexcept {
    u32 bars{}, played{}, remaining{};
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& quad = paint.quads[i];
        const auto bounds = UiRect{quad.left, quad.top, quad.right, quad.bottom};
        const auto& seek = projection.geometry.seek;
        if (quad.left < seek.left || quad.right > seek.right || quad.top < seek.top ||
            quad.bottom > seek.bottom || !ui_rect_has_area(bounds))
            continue;
        if (same(quad.fill, projection.played))
            ++played;
        else if (same(quad.fill, projection.waveform))
            ++remaining;
        else
            continue;
        ++bars;
    }
    return bars == expected && remaining && (!progress || played);
}
} // namespace

void test_audio_presentation(void (*check)(bool, const char*) noexcept) noexcept {
    Owner owner{};
    PaintStream paint{};
    bool ready = owner.initialize() && paint_stream_initialize(&paint);
    check(ready, "audio paint: bounded original native owner and paint storage initialize");
    if (!ready) {
        paint_stream_release(&paint);
        return;
    }
    const UiWidgetReference reference{&owner.manager, owner.root};
    u8 peaks[kUiAudioWaveformMaximum]{};
    for (u32 i = 0; i < kUiAudioWaveformMaximum; ++i)
        peaks[i] = i & 1 ? 255 : 0;
    auto state = playback();
    state.voice = true;
    state.waveform = {peaks, sizeof(peaks)};
    UiAudioScene scene{};
    const UiRect bounds{16, 16, 336, 102};
    ready = ui_audio_scene_project(&scene, state, bounds, bounds, reference, UiAudioSourceSurface::timeline,
                                   kDefaultUiAudioControlsStyle, kDefaultTheme) &&
            paint_stream_reset(&paint, 400, 200) && ui_audio_scene_paint(scene, kDefaultTheme, &paint);
    check(ready && waveform_geometry(paint, scene.projection, 24, true) && paint.clip_depth == 1 &&
              ui_rect_height(scene.projection.geometry.bounds) == 86,
          "audio paint: compact voice card aggregates real 256 peaks into 24 clipped native bars with "
          "played colors");

    UiTextCommand commands[8]{};
    char bytes[640]{};
    UiTextCommandStream text{};
    UiTextOutput output{&text, nullptr};
    ready = ui_text_command_stream_initialize(&text, {commands, 8}, {bytes, sizeof(bytes)}) &&
            ui_audio_scene_text_output(scene, &output);
    const auto before_text = ui_audio_scene_text_revision(scene);
    const auto before_paint = ui_audio_scene_paint_revision(scene);
    const Utf8View retained = text.count ? text.storage[0].text : Utf8View{};
    scene.projection.time_text[0] = '9';
    check(ready && text.count == 1 && utf8_equal(retained, utf8_literal("0:30 / 2:00")),
          "audio text: generated voice duration is retained in caller bytes after projection buffer "
          "mutation");
    state.elapsed_ms = 30'500;
    ready = ui_audio_scene_project(&scene, state, bounds, bounds, reference, UiAudioSourceSurface::timeline,
                                   kDefaultUiAudioControlsStyle, kDefaultTheme);
    check(ready && ui_audio_scene_text_revision(scene) == before_text &&
              ui_audio_scene_paint_revision(scene) != before_paint,
          "audio retention: subsecond progress changes native paint while preserving exact text signature");
    state.elapsed_ms = 31'000;
    ready = ui_audio_scene_project(&scene, state, bounds, bounds, reference, UiAudioSourceSurface::timeline,
                                   kDefaultUiAudioControlsStyle, kDefaultTheme);
    check(ready && ui_audio_scene_text_revision(scene) != before_text,
          "audio retention: visible elapsed second changes text signature");

    state.voice = false;
    state.title = utf8_literal("Original Unicode \342\230\203 recording.ogg");
    const UiRect ordinary{16, 16, 336, 128};
    ready =
        ui_audio_scene_project(&scene, state, ordinary, ordinary, reference, UiAudioSourceSurface::timeline,
                               kDefaultUiAudioControlsStyle, kLightTheme) &&
        paint_stream_reset(&paint, 400, 200) && ui_audio_scene_paint(scene, kLightTheme, &paint);
    ui_text_command_stream_reset(&text);
    ready = ready && ui_audio_scene_text_output(scene, &output);
    check(ready && text.count == 2 && ui_rect_height(scene.projection.geometry.bounds) == 112 &&
              waveform_geometry(paint, scene.projection, 24, true) &&
              commands[0].wrapping == TextWrapping::no_wrap &&
              commands[1].wrapping == TextWrapping::no_wrap,
          "audio paint: Light ordinary recording retains original filename and one-line duration with "
          "aligned 112DIP card");

    state.waveform = {};
    ready =
        ui_audio_scene_project(&scene, state, ordinary, ordinary, reference, UiAudioSourceSurface::timeline,
                               kDefaultUiAudioControlsStyle, kLightTheme) &&
        paint_stream_reset(&paint, 400, 200) && ui_audio_scene_paint(scene, kLightTheme, &paint);
    u32 neutral{};
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& quad = paint.quads[i];
        neutral += same(quad.fill, scene.projection.track) &&
                           quad.left == scene.projection.geometry.seek.left &&
                           quad.right == scene.projection.geometry.seek.right && quad.bottom - quad.top == 4
                       ? 1u
                       : 0u;
    }
    check(ready && neutral == 1,
          "audio paint: absent metadata emits neutral real seek rail instead of fabricated waveform");
    state.error = true;
    ready =
        ui_audio_scene_project(&scene, state, ordinary, ordinary, reference, UiAudioSourceSurface::timeline,
                               kDefaultUiAudioControlsStyle, kLightTheme);
    ui_text_command_stream_reset(&text);
    ready = ready && ui_audio_scene_text_output(scene, &output);
    bool error_label{};
    for (u32 i = 0; i < text.count; ++i)
        error_label = error_label ||
                      (utf8_equal(commands[i].text, kDefaultUiAudioControlsStyle.error) &&
                       same(commands[i].color, ui_foreground_color(kLightTheme, kLightTheme.notification)));
    check(ready && error_label && text.count == 3, "audio text: explicit unavailable state publishes "
                                                   "visible semantic error alongside filename and time");

    state.error = false;
    state.elapsed_ms = 3'600'000;
    state.duration_ms = 7'200'000;
    const UiRect compact{16, 16, 176, 128};
    ready =
        ui_audio_scene_project(&scene, state, compact, compact, reference, UiAudioSourceSurface::timeline,
                               kDefaultUiAudioControlsStyle, kLightTheme);
    ui_text_command_stream_reset(&text);
    ready = ready && ui_audio_scene_text_output(scene, &output);
    check(ready && scene.projection.geometry.compact && text.count == 2 &&
              utf8_equal(commands[0].text, utf8_literal("1:00:00\n2:00:00")) &&
              commands[0].wrapping == TextWrapping::wrap &&
              ui_rect_height(commands[0].rectangle) == kDefaultUiAudioControlsStyle.target &&
              commands[1].wrapping == TextWrapping::no_wrap,
          "audio text: compact hour timestamps wrap explicit elapsed and duration lines while filename "
          "stays no-wrap");
    ready =
        ui_audio_scene_project(&scene, state, ordinary, ordinary, reference, UiAudioSourceSurface::timeline,
                               kDefaultUiAudioControlsStyle, kLightTheme);
    ui_text_command_stream_reset(&text);
    ready = ready && ui_audio_scene_text_output(scene, &output);
    check(ready && !scene.projection.geometry.compact && text.count == 2 &&
              utf8_equal(commands[0].text, utf8_literal("1:00:00 / 2:00:00")) &&
              commands[0].wrapping == TextWrapping::no_wrap &&
              commands[1].wrapping == TextWrapping::no_wrap,
          "audio text: wide hour timestamp remains one line with a separate no-wrap filename");
    ui_widget_manager_reset(&owner.manager);
    check(!ui_audio_scene_paint(scene, kLightTheme, &paint) &&
              !ui_audio_scene_text_output(scene, &output) && !ui_audio_scene_text_revision(scene) &&
              !ui_audio_scene_paint_revision(scene),
          "audio ownership: recycled attachment widget rejects stale native paint and text");
    paint_stream_release(&paint);
}
} // namespace maicord
