// The .mcfx background effects through the real pass executor (d3d11_effect_*, src/render/d3d11/effect_renderer.cpp)
// on whichever GPU backend the build has: every sample effect of samples/effects and Silk Waves, built as interface 4
// packages (tests/gpu/effects/*.mcfx: the DXBC the Windows build compiled and the GLSL tools/maicord-plugin added),
// are selected, drawn for 24 frames of a fixed clock and compared, at the frames named below, with the pictures
// Direct3D 11 made (tests/gpu/golden/effects/*.png) by their peak signal to noise ratio. spark-ribbon and Silk Waves
// are pass graphs with persistent targets, grids, quads, additive blending and RGBA32F / RGBA16F planes: their
// pictures at three frames show the persistent state carried across passes and frames. After aurora's last frame the
// composite pass puts its target over six canvases in every blend mode, the light-adapted screen and add (the
// dual-source blend) and a rounded tile, then in normal blend over an opaque background in a fading paint layer,
// checked against the arithmetic of the composite.
//
// The same source runs on Windows and Linux. MAICORD_GPU_EFFECT_CAPTURE=<prefix> (on Windows: the goldens' source)
// or MAICORD_GPU_EFFECT_DUMP=<prefix> (elsewhere, to look at a difference) writes every picture to
// <prefix>.<effect>.<frame> and <prefix>.composite as raw RGBA bytes; tools/update_gpu_goldens.py turns a capture into
// the PNGs. Linux runs it with MAICORD_GLES_NO_TIMER_QUERY=1 so the cost probe cannot refuse an effect or move its
// resolution.
#include "maicord/app/packages/mcfx.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/render/d3d11/effect_renderer.hpp"
#include "maicord/ui/theme/theme.hpp"
#include "../support/test_output.hpp"
#include "capture_io.hpp"
#include "png_golden.hpp"

namespace maicord {
namespace {

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
alignas(16) constexpr u8 k_ascii_rain_package[] = {
#embed "effects/ascii-rain.mcfx"
};
alignas(16) constexpr u8 k_aurora_package[] = {
#embed "effects/aurora.mcfx"
};
alignas(16) constexpr u8 k_bokeh_package[] = {
#embed "effects/bokeh.mcfx"
};
alignas(16) constexpr u8 k_day_night_package[] = {
#embed "effects/day-night.mcfx"
};
alignas(16) constexpr u8 k_film_grain_package[] = {
#embed "effects/film-grain.mcfx"
};
alignas(16) constexpr u8 k_plasma_package[] = {
#embed "effects/plasma.mcfx"
};
alignas(16) constexpr u8 k_spark_ribbon_package[] = {
#embed "effects/spark-ribbon.mcfx"
};
alignas(16) constexpr u8 k_silk_waves_package[] = {
#embed "effects/silk-waves.mcfx"
};
alignas(16) constexpr u8 k_starfield_package[] = {
#embed "effects/starfield.mcfx"
};
alignas(16) constexpr u8 k_wave_lines_package[] = {
#embed "effects/wave-lines.mcfx"
};
alignas(16) constexpr u8 k_ascii_rain_11[] = {
#embed "golden/effects/effect_ascii-rain_11.png"
};
alignas(16) constexpr u8 k_aurora_11[] = {
#embed "golden/effects/effect_aurora_11.png"
};
alignas(16) constexpr u8 k_bokeh_11[] = {
#embed "golden/effects/effect_bokeh_11.png"
};
alignas(16) constexpr u8 k_day_night_11[] = {
#embed "golden/effects/effect_day-night_11.png"
};
alignas(16) constexpr u8 k_film_grain_11[] = {
#embed "golden/effects/effect_film-grain_11.png"
};
alignas(16) constexpr u8 k_plasma_11[] = {
#embed "golden/effects/effect_plasma_11.png"
};
alignas(16) constexpr u8 k_spark_ribbon_1[] = {
#embed "golden/effects/effect_spark-ribbon_1.png"
};
alignas(16) constexpr u8 k_spark_ribbon_11[] = {
#embed "golden/effects/effect_spark-ribbon_11.png"
};
alignas(16) constexpr u8 k_spark_ribbon_23[] = {
#embed "golden/effects/effect_spark-ribbon_23.png"
};
alignas(16) constexpr u8 k_silk_waves_1[] = {
#embed "golden/effects/effect_silk-waves_1.png"
};
alignas(16) constexpr u8 k_silk_waves_11[] = {
#embed "golden/effects/effect_silk-waves_11.png"
};
alignas(16) constexpr u8 k_silk_waves_23[] = {
#embed "golden/effects/effect_silk-waves_23.png"
};
alignas(16) constexpr u8 k_starfield_11[] = {
#embed "golden/effects/effect_starfield_11.png"
};
alignas(16) constexpr u8 k_wave_lines_11[] = {
#embed "golden/effects/effect_wave-lines_11.png"
};
alignas(16) constexpr u8 k_composite[] = {
#embed "golden/effects/effect_composite.png"
};
#pragma clang diagnostic pop

constexpr u32 kWidth = 192, kHeight = 108, kFrames = 24;
constexpr u32 kPixels = kWidth * kHeight * 4;

struct Frame {
    u32 index;
    ConstSlice<u8> golden;
    u32 decibels; // the picture must reach this PSNR
};

struct Case {
    const char* name;
    ConstSlice<u8> package;
    const Frame* frames;
    u32 frame_count;
    bool composite;
};

#define MC_SLICE(array) ConstSlice<u8>{array, static_cast<u32>(sizeof(array))}
// The thresholds are what the two backends reach on the developer GPU and on llvmpipe, less a margin: a hash that
// amplifies a rounding difference at a few pixels costs a few dB, a wrong blend or a lost plane costs tens.
constexpr u32 kDecibels = 48;
const Frame k_ascii_rain_frames[]{{11, MC_SLICE(k_ascii_rain_11), kDecibels}};
const Frame k_aurora_frames[]{{11, MC_SLICE(k_aurora_11), kDecibels}};
const Frame k_bokeh_frames[]{{11, MC_SLICE(k_bokeh_11), kDecibels}};
const Frame k_day_night_frames[]{{11, MC_SLICE(k_day_night_11), kDecibels}};
const Frame k_film_grain_frames[]{{11, MC_SLICE(k_film_grain_11), kDecibels}};
const Frame k_plasma_frames[]{{11, MC_SLICE(k_plasma_11), kDecibels}};
const Frame k_spark_ribbon_frames[]{{1, MC_SLICE(k_spark_ribbon_1), kDecibels},
                                    {11, MC_SLICE(k_spark_ribbon_11), kDecibels},
                                    {23, MC_SLICE(k_spark_ribbon_23), kDecibels}};
const Frame k_silk_waves_frames[]{{1, MC_SLICE(k_silk_waves_1), kDecibels},
                                  {11, MC_SLICE(k_silk_waves_11), kDecibels},
                                  {23, MC_SLICE(k_silk_waves_23), kDecibels}};
const Frame k_starfield_frames[]{{11, MC_SLICE(k_starfield_11), kDecibels}};
const Frame k_wave_lines_frames[]{{11, MC_SLICE(k_wave_lines_11), kDecibels}};

#define MC_CASE(name_text, slug, composite)                                                                            \
    {name_text, MC_SLICE(k_##slug##_package), k_##slug##_frames, static_cast<u32>(sizeof(k_##slug##_frames) / sizeof(Frame)), \
     composite}
const Case kCases[]{
    MC_CASE("ascii-rain", ascii_rain, false), MC_CASE("aurora", aurora, true),
    MC_CASE("bokeh", bokeh, false),           MC_CASE("day-night", day_night, false),
    MC_CASE("film-grain", film_grain, false), MC_CASE("plasma", plasma, false),
    MC_CASE("spark-ribbon", spark_ribbon, false), MC_CASE("silk-waves", silk_waves, false),
    MC_CASE("starfield", starfield, false),   MC_CASE("wave-lines", wave_lines, false),
};
#undef MC_CASE

u8 g_pixels[kPixels];
u8 g_golden[kPixels];
McfxWorkspace g_workspace;
u32 g_failures = 0;
char g_capture[512];
bool g_capturing;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

// "<prefix>.<name>.<frame>" ("" frame: no suffix of its own) written as raw bytes when a capture or dump is on.
void write_capture(const char* name, u32 frame, const u8* pixels) noexcept {
    if (!g_capture[0])
        return;
    char path[640];
    u32 at = 0;
    for (const char* c = g_capture; *c; ++c)
        path[at++] = *c;
    path[at++] = '.';
    for (const char* c = name; *c; ++c)
        path[at++] = *c;
    if (frame != 0xFFFFFFFFu) {
        path[at++] = '.';
        char digits[12];
        u32 count = 0;
        for (u32 value = frame; value || !count; value /= 10)
            digits[count++] = static_cast<char>('0' + value % 10);
        while (count)
            path[at++] = digits[--count];
    }
    path[at] = 0;
    check(capture_io::write_file(path, pixels, kPixels), "a capture file is written");
}

// The effect's own picture (what the canvases composite), top-left origin, premultiplied.
bool read_target(GpuDevice* device, GpuContext* context, const D3D11EffectOwner& owner, u8* out) noexcept {
    return owner.target && owner.used_width == kWidth && owner.used_height == kHeight &&
           gpu_ok(gpu_readback(device, context, owner.target, {0, 0, kWidth, kHeight}, out, kWidth * 4));
}

// A glyph grid for the effects that read one (ascii-rain): 16 x 16 cells of a fixed pattern, as the renderer's
// atlas would hold a font's, since the pattern is all the comparison needs.
GpuSrv* make_glyph_view(GpuDevice* device) noexcept {
    static u8 pixels[512 * 512];
    for (u32 y = 0; y < 512; ++y)
        for (u32 x = 0; x < 512; ++x) {
            const u32 cell_x = x / 32, cell_y = y / 32, inside_x = x % 32, inside_y = y % 32;
            const bool stroke = (inside_x + inside_y * (1 + cell_x % 3)) % 11 < 4 + cell_y % 4;
            pixels[y * 512 + x] = stroke ? static_cast<u8>(255 - (cell_x * 7 + cell_y * 13) % 90) : 0;
        }
    const GpuInitialData data{pixels, 512};
    GpuTexture* texture = nullptr;
    GpuSrv* view = nullptr;
    if (gpu_ok(gpu_texture_create(device, {512, 512, GpuFormat::r8, GpuUsage::immutable, gpu_bind_srv}, &data, &texture)))
        gpu_srv_create(device, texture, {GpuViewKind::texture, GpuFormat::r8}, &view);
    gpu_release(texture);
    return view;
}

// Compares g_pixels with a golden PNG by PSNR and reports it.
void compare(const char* name, u32 frame, ConstSlice<u8> png, u32 decibels) noexcept {
    test_write_text(name);
    test_write_text(" frame ");
    test_write_u64(frame);
    if (!png_golden::decode(png, kWidth, kHeight, 4, g_golden)) {
        test_write_line(": the golden does not decode");
        check(false, "a golden PNG decodes");
        return;
    }
    const double mse = png_golden::mean_square_error(g_pixels, g_golden, kPixels);
    const u32 reached = png_golden::psnr_decibels(mse);
    const png_golden::Difference difference = png_golden::compare(g_pixels, g_golden, kWidth, kHeight, 4, 255);
    test_write_text(": psnr_db>=");
    test_write_u64(reached);
    test_write_text(" maximum difference=");
    test_write_u64(difference.maximum);
    test_write_line("");
    check(reached >= decibels, "the picture reaches its PSNR");
}

struct UiFrame {
    float scale_x, scale_y, translation_x, translation_y, opacity, pad[3];
};

// Aurora's target over six canvases of 64 x 54: normal, screen, add, then the light-adapted normal, screen (the
// cross-fade: the dual-source blend) and add (settled: a multiply), the last as a rounded tile.
void composite_scene(GpuDevice* device, GpuContext* context, D3D11EffectOwner* owner) noexcept {
    GpuTexture* texture = nullptr;
    GpuRtv* view = nullptr;
    GpuBuffer* frame = nullptr;
    const UiFrame constants{2.0f / kWidth, -2.0f / kHeight, -1.0f, 1.0f, 1.0f, {}};
    // The six canvases' backgrounds, written as the target's initial texels.
    static constexpr float kBackgrounds[6][4]{{0.55f, 0.20f, 0.20f, 1}, {0.20f, 0.50f, 0.25f, 1}, {0.20f, 0.30f, 0.60f, 1},
                                              {0.85f, 0.80f, 0.70f, 1}, {0.75f, 0.80f, 0.85f, 1}, {0.30f, 0.30f, 0.35f, 1}};
    for (u32 y = 0; y < kHeight; ++y)
        for (u32 x = 0; x < kWidth; ++x)
            for (u32 c = 0; c < 4; ++c)
                g_pixels[(y * kWidth + x) * 4 + c] = static_cast<u8>(kBackgrounds[y / 54 * 3 + x / 64][c] * 255.0f + 0.5f);
    const GpuInitialData backgrounds{g_pixels, kWidth * 4};
    check(gpu_ok(gpu_texture_create(device, {kWidth, kHeight, GpuFormat::rgba8, GpuUsage::default_, gpu_bind_srv | gpu_bind_rtv},
                                    &backgrounds, &texture)) &&
              gpu_ok(gpu_rtv_create(device, texture, &view)) &&
              gpu_ok(gpu_buffer_create(device, {sizeof(constants), GpuUsage::default_, gpu_bind_constant, false}, &constants,
                                       &frame)),
          "the composite target");
    if (!view || !frame) {
        gpu_release(frame);
        gpu_release(view);
        gpu_release(texture);
        return;
    }
    static constexpr EffectBlend kBlends[6]{EffectBlend::normal, EffectBlend::screen, EffectBlend::add,
                                            EffectBlend::normal, EffectBlend::screen, EffectBlend::add};
    static constexpr float kLight[6]{0, 0, 0, 0.6f, 0.5f, 1.0f};
    gpu_set_render_target(context, view);
    gpu_set_viewport(context, 0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight));
    gpu_set_scissor(context, {0, 0, static_cast<i32>(kWidth), static_cast<i32>(kHeight)});
    gpu_set_raster(context, owner->rasterizer);
    gpu_set_strip_topology(context);
    GpuBuffer* ui[1] = {frame};
    gpu_set_constants(context, GpuStage::vertex, 0, 1, ui);
    GpuSampler* sampler[1] = {owner->samplers[2]};
    gpu_set_samplers(context, GpuStage::pixel, 0, 1, sampler);
    bool drawn = true;
    for (u32 k = 0; k < 6; ++k) {
        const u32 column = k % 3, row = k / 3;
        owner->settings.blend = kBlends[k];
        owner->light = kLight[k];
        owner->layer_opacity = k == 1 ? 0.7f : 1.0f;
        const UiRect canvas{static_cast<float>(column * 64), static_cast<float>(row * 54), static_cast<float>(column * 64 + 64),
                            static_cast<float>(row * 54 + 54)};
        drawn = d3d11_effect_draw(owner, context, canvas, k, k == 5 ? 10.0f : 0.0f) && drawn;
    }
    check(drawn, "the composite draws six canvases");
    d3d11_effect_end_frame(owner, context);
    check(gpu_ok(gpu_readback(device, context, texture, {0, 0, kWidth, kHeight}, g_pixels, kWidth * 4)),
          "the composite readback");
    write_capture("composite", 0xFFFFFFFFu, g_pixels);
    if (!g_capturing)
        compare("composite", 0, MC_SLICE(k_composite), kDecibels);
    gpu_release(frame);
    gpu_release(view);
    gpu_release(texture);
}

// A paint layer fading in (a modal's entrance) at a quarter of its opacity: Aurora's target at 60 % in normal blend
// over an opaque background. The composite weighs what is behind by the share of the effect the layer lets through,
// so the background stays opaque (the window composes with premultiplied alpha: a translucent pixel shows the
// desktop through it) and takes the effect at that share.
void fading_layer(GpuDevice* device, GpuContext* context, D3D11EffectOwner* owner) noexcept {
    static u8 picture[kPixels];
    GpuTexture* texture = nullptr;
    GpuRtv* view = nullptr;
    GpuBuffer* frame = nullptr;
    const UiFrame constants{2.0f / kWidth, -2.0f / kHeight, -1.0f, 1.0f, 1.0f, {}};
    check(gpu_ok(gpu_texture_create(device, {kWidth, kHeight, GpuFormat::rgba8, GpuUsage::default_, gpu_bind_srv | gpu_bind_rtv},
                                    nullptr, &texture)) &&
              gpu_ok(gpu_rtv_create(device, texture, &view)) &&
              gpu_ok(gpu_buffer_create(device, {sizeof(constants), GpuUsage::default_, gpu_bind_constant, false}, &constants,
                                       &frame)) &&
              read_target(device, context, *owner, picture),
          "the fading layer's target and the effect's picture");
    if (!view || !frame) {
        gpu_release(frame);
        gpu_release(view);
        gpu_release(texture);
        return;
    }
    constexpr float kLayer = 0.25f, kOpacity = 0.6f;
    const float background[4]{0.85f, 0.80f, 0.70f, 1.0f};
    gpu_clear_rtv(context, view, background);
    gpu_set_render_target(context, view);
    gpu_set_viewport(context, 0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight));
    gpu_set_scissor(context, {0, 0, static_cast<i32>(kWidth), static_cast<i32>(kHeight)});
    gpu_set_raster(context, owner->rasterizer);
    gpu_set_strip_topology(context);
    GpuBuffer* ui[1] = {frame};
    gpu_set_constants(context, GpuStage::vertex, 0, 1, ui);
    GpuSampler* sampler[1] = {owner->samplers[2]};
    gpu_set_samplers(context, GpuStage::pixel, 0, 1, sampler);
    owner->settings.blend = EffectBlend::normal;
    owner->settings.opacity = kOpacity;
    owner->light = 0;
    owner->layer_opacity = kLayer;
    check(d3d11_effect_draw(owner, context, {0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)}, 6, 0.0f),
          "the fading layer draws");
    d3d11_effect_end_frame(owner, context);
    check(gpu_ok(gpu_readback(device, context, texture, {0, 0, kWidth, kHeight}, g_pixels, kWidth * 4)),
          "the fading layer readback");
    u32 translucent = 0, off = 0;
    for (u32 i = 0; i < kWidth * kHeight; ++i) {
        const u8* got = g_pixels + i * 4;
        const u8* effect = picture + i * 4;
        translucent += got[3] != 255 ? 1u : 0u;
        const float share = kLayer * kOpacity, behind = 1.0f - share * static_cast<float>(effect[3]) / 255.0f;
        bool near = true;
        for (u32 c = 0; c < 3; ++c) {
            const float expected = share * static_cast<float>(effect[c]) + background[c] * 255.0f * behind;
            const float difference = static_cast<float>(got[c]) - expected;
            near = near && difference <= 2.0f && difference >= -2.0f;
        }
        off += near ? 0u : 1u;
    }
    test_write_metric("fading_layer_translucent=", translucent);
    test_write_metric("fading_layer_off=", off);
    check(translucent == 0, "a fading layer leaves an opaque background opaque");
    check(off == 0, "a fading layer adds the effect at its share over what is behind");
    gpu_release(frame);
    gpu_release(view);
    gpu_release(texture);
}

void run_case(GpuDevice* device, GpuContext* context, GpuSrv* glyphs, const Case& test, u64 identity) noexcept {
    test_write_text("== ");
    test_write_line(test.name);
    const McfxResult parsed = mcfx_parse(test.package, &g_workspace);
    check(parsed.error == McfxError::none, "the package parses");
    if (parsed.error != McfxError::none)
        return;
    const McfxPackage& package = g_workspace.package;
    check(package.interface_version == 4, "the package is interface 4");
    BackgroundEffectSpec spec{};
    spec.bytecode = package.shaders[0];
    if (package.graph.pass_count) {
        spec.graph = &package.graph;
        spec.shaders = package.shaders;
    }
    spec.identity = identity;
    spec.uses = package.uses;
    spec.blend = package.blend;
    spec.requested_resolution = package.requested_resolution;
    spec.requested_frame_rate = package.requested_frame_rate;
    McfxKnobValue values[kMcfxKnobMaximum];
    mcfx_knob_defaults(package, values);
    mcfx_knob_pack(package, values, spec.knobs);
    // The full-resolution step, so the target is the window and every backend draws the same pixels.
    const EffectSettings settings{1.0f, 1.0f, package.blend, static_cast<u8>(kEffectResolutionSteps - 1),
                                  static_cast<u8>(kEffectFrameRateSteps - 1), EffectBudget::high, false};
    static D3D11EffectOwner owner;
    reset_value(&owner);
    // The executor owns the glyph grid it is given (the renderer builds one per select and releases it on a refusal), so
    // the shared test grid is retained for it, and only for an effect that takes glyphs.
    GpuSrv* given = nullptr;
    if (spec.uses & effect_use_glyphs) {
        given = glyphs;
        gpu_retain(given);
    }
    const EffectSelectResult selected = d3d11_effect_select(&owner, device, context, spec, settings,
                                                            gpu_adapter_key(device), given, kWidth, kHeight);
    if (selected.state == EffectSelect::rejected)
        gpu_release(given);
    check(selected.state == EffectSelect::changed, "the effect is selected");
    if (selected.state != EffectSelect::changed) {
        test_write_metric("native_error=", static_cast<u32>(selected.native_error));
        d3d11_effect_release(&owner);
        return;
    }
    owner.step = kEffectResolutionSteps - 1;
    effect_budget_start(&owner.budget, owner.step, owner.step);
    owner.dirty = true;
    owner.ambient[effect_ambient_day] = 0.3f; // a morning, in the middle of the year, dark
    owner.ambient[effect_ambient_year] = 0.45f;
    owner.ambient[effect_ambient_month] = 6.0f;
    owner.ambient[effect_ambient_dark] = 1.0f;
    u32 next = 0;
    bool moved = false;
    for (u32 index = 0; index < kFrames; ++index) {
        const bool ok = d3d11_effect_begin_frame(&owner, device, context, kDefaultTheme,
                                                 static_cast<float>(index) / 24.0f, true, kWidth, kHeight, 0, 96, false,
                                                 index * 42u);
        moved = moved || owner.step != kEffectResolutionSteps - 1;
        if (!ok || !owner.rendered) {
            check(false, "every frame is drawn");
            break;
        }
        if (next < test.frame_count && test.frames[next].index == index) {
            check(read_target(device, context, owner, g_pixels), "the target is read back");
            write_capture(test.name, index, g_pixels);
            if (!g_capturing)
                compare(test.name, index, test.frames[next].golden, test.frames[next].decibels);
            ++next;
        }
    }
    check(next == test.frame_count, "every named frame was reached");
    check(!moved, "the budget controller leaves the resolution alone");
    if (test.composite) {
        composite_scene(device, context, &owner);
        fading_layer(device, context, &owner);
    }
    d3d11_effect_release(&owner);
}

void run() noexcept {
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        check(false, "device creation");
        return;
    }
    test_write_text("device: ");
    test_write_line(gpu_device_description(device));
    g_capturing = capture_io::environment("MAICORD_GPU_EFFECT_CAPTURE", g_capture, sizeof(g_capture));
    if (!g_capturing)
        capture_io::environment("MAICORD_GPU_EFFECT_DUMP", g_capture, sizeof(g_capture));
    GpuSrv* glyphs = make_glyph_view(device);
    check(glyphs != nullptr, "the glyph grid");
    u64 identity = 1;
    // MAICORD_GPU_EFFECT_ONLY=<name> runs one effect (for looking at a difference).
    char only[48];
    const bool filtered = capture_io::environment("MAICORD_GPU_EFFECT_ONLY", only, sizeof(only));
    for (const Case& test : kCases) {
        u32 length = 0;
        while (test.name[length] && test.name[length] == only[length])
            ++length;
        if (!filtered || (!test.name[length] && !only[length]))
            run_case(device, context, glyphs, test, identity);
        ++identity;
    }
    gpu_release(glyphs);
    gpu_device_destroy(&device, &context);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_effects_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_gpu_effects");
}
