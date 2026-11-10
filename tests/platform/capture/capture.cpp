// Go Live capture without capturing anything of the user's (src/platform/capture): the decisions every backend shares
// (which windows the picker lists, the thumbnail reduction steps, the frame-rate cap, cut names), and per system:
//   Windows: the WinRT and process loopback ABI against the SDK (capture_abi.cpp), what this build of Windows gets
//            (the methods by build number), the source list's order and rules, and the thumbnail reduction on the GPU
//            over a texture the test fills (skipped without a hardware device);
//   Linux:   the PipeWire format negotiation PODs byte for byte against libspa's own builder (the golden words below
//            were written by spa_pod_builder from libspa-0.2-dev 1.0.5), reading back the formats a producer settles on,
//            and the DRM fourccs of the accepted formats.
#include "../../../src/platform/capture/capture_policy.hpp"
#include "maicord/platform/capture/screen_capture.hpp"
#include "maicord/platform/audio/stream_audio.hpp"
#include "../../support/test_output.hpp"
#if defined(_WIN32)
#include "../../../src/platform/capture/windows/thumbnail.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/core/win32_kernel.hpp"
#else
#include "../../../src/platform/capture/linux/pipewire_stream.hpp"
#include "../../../src/platform/capture/linux/stream_audio_routes.hpp"
#endif

namespace maicord {
#if defined(_WIN32)
void test_capture_abi(void (*check)(bool, const char*) noexcept) noexcept;
#endif
namespace {

u32 g_failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

void test_window_rules() noexcept {
    const CaptureWindowFacts app{true, false, false, false, false, false, false, 12, 800, 600};
    check(capture_window_listed(app), "picker: a visible, titled application window is listed");
    CaptureWindowFacts other = app;
    other.visible = false;
    check(!capture_window_listed(other), "picker: a hidden window is not listed");
    other = app;
    other.cloaked = true;
    check(!capture_window_listed(other), "picker: a cloaked window (another desktop, a suspended app) is not listed");
    other = app;
    other.tool_window = true;
    check(!capture_window_listed(other), "picker: a tool window is not listed");
    other = app;
    other.owned = true;
    check(!capture_window_listed(other), "picker: an owned window (a dialog) is not listed");
    other = app;
    other.own_process = true;
    check(!capture_window_listed(other), "picker: MaiCord's own windows are not listed");
    other = app;
    other.shell = true;
    check(!capture_window_listed(other), "picker: the desktop is not listed as a window");
    other = app;
    other.title_chars = 0;
    check(!capture_window_listed(other), "picker: an untitled window is not listed");
    other = app;
    other.width = 0;
    check(!capture_window_listed(other), "picker: a window without a client area is not listed");
    other.minimized = true;
    check(capture_window_listed(other), "picker: a minimized window keeps its place");
}

void test_thumbnail_steps() noexcept {
    check(capture_thumbnail_level(1920, 1080, 1920, 1080) == 0, "thumbnail: a picture that fits is not reduced");
    check(capture_thumbnail_level(1920, 1080, 320, 180) == 3 && capture_level_extent(1920, 3) == 240 &&
              capture_level_extent(1080, 3) == 135,
          "thumbnail: 1920x1080 halves three times into 320x180 (240x135)");
    check(capture_thumbnail_level(2560, 1440, 320, 180) == 3 && capture_level_extent(2560, 3) == 320,
          "thumbnail: 2560x1440 fits 320x180 exactly at the third level");
    check(capture_thumbnail_level(1080, 1920, 320, 180) == 4 && capture_level_extent(1920, 4) == 120 &&
              capture_level_extent(1080, 4) == 67,
          "thumbnail: a portrait picture fits by its height");
    check(capture_thumbnail_level(5, 3, 2, 2) == 1 && capture_level_extent(5, 1) == 2 && capture_level_extent(3, 1) == 1 &&
              capture_thumbnail_level(5, 3, 1, 1) == 2 && capture_level_extent(5, 2) == 1,
          "thumbnail: odd extents round down and never below one pixel");
    check(capture_thumbnail_level(1, 1, 0, 0) == 0, "thumbnail: a single pixel has nothing to reduce");
}

void test_pacer() noexcept {
    CapturePacer pacer{};
    check(capture_pacer_admit(&pacer, 1'000'000, 0) && capture_pacer_admit(&pacer, 1'000'001, 0),
          "frame cap: no cap admits every frame");
    pacer = {};
    // A 60 Hz source under a 30 fps cap: every second frame.
    u32 admitted = 0;
    for (u32 i = 0; i < 60; ++i)
        admitted += capture_pacer_admit(&pacer, 1'000'000 + i * 16'667, 30);
    check(admitted == 30, "frame cap: a 60 Hz source under a 30 fps cap gives 30 frames a second");
    pacer = {};
    admitted = 0;
    // A 30 Hz source with jitter under a 30 fps cap: nothing is lost to the jitter.
    const u32 jitter[] = {0, 3'000, 0, 2'000, 4'000, 0, 1'000, 3'500};
    for (u32 i = 0; i < 30; ++i)
        admitted += capture_pacer_admit(&pacer, 2'000'000 + i * 33'333 + jitter[i % 8], 30);
    check(admitted == 30, "frame cap: a source at the cap keeps every frame through jitter");
    pacer = {};
    admitted = 0;
    // 144 Hz under 60: about 60 a second, never above.
    for (u32 i = 0; i < 144; ++i)
        admitted += capture_pacer_admit(&pacer, 3'000'000 + i * 6'944, 60);
    check(admitted >= 58 && admitted <= 60, "frame cap: 144 Hz under a 60 fps cap gives at most 60 frames a second");
    // After a still screen (a gap), the cadence starts again at the next frame.
    pacer = {};
    check(capture_pacer_admit(&pacer, 4'000'000, 30) && capture_pacer_admit(&pacer, 9'000'000, 30) &&
              !capture_pacer_admit(&pacer, 9'010'000, 30) && capture_pacer_admit(&pacer, 9'033'334, 30),
          "frame cap: a gap restarts the cadence");
}

void test_names() noexcept {
    const char ascii[] = "Notepad";
    check(capture_name_cut(ascii, 7, 192) == 7 && capture_name_cut(ascii, 7, 8) == 7 && capture_name_cut(ascii, 7, 4) == 3,
          "names: ASCII is cut at the capacity, keeping the terminator");
    // "a" then U+00E9 (2 bytes) then U+20AC (3 bytes).
    const char text[] = "a\xC3\xA9\xE2\x82\xAC";
    check(capture_name_cut(text, 6, 3) == 1 && capture_name_cut(text, 6, 4) == 3 && capture_name_cut(text, 6, 6) == 3 &&
              capture_name_cut(text, 6, 7) == 6,
          "names: a cut never splits a UTF-8 sequence");
    check(capture_name_cut(text, 6, 0) == 0, "names: no capacity, no name");
}

#if defined(_WIN32)
u32 os_build() noexcept {
    return *reinterpret_cast<const volatile u32*>(0x7FFE0260ull) & 0xFFFFu;
}

void test_system_methods() noexcept {
    const u32 build = os_build();
    const CaptureSupport support = capture_support();
    check(support.screens && support.windows && !support.system_picker,
          "Windows: screens and windows through MaiCord's own picker");
    check(support.cursor_control == (build >= 19041), "Windows: the cursor can be left out from 2004 (build 19041)");
    check(stream_audio_method() == (build >= 19041 ? StreamAudioMethod::process_loopback
                                                   : StreamAudioMethod::unavailable_windows_version),
          "Windows: process loopback from 2004 (build 19041), no sound sharing before (it would carry the call)");
}

// The real source list, read but never captured or printed: screens first, the primary first, named "Screen N", and
// no window of this process.
CaptureSource g_sources[64];
void test_source_list() noexcept {
    const u32 count = capture_sources({g_sources, 64});
    u32 screens = 0;
    bool ordered = true, named = true, foreign = true;
    for (u32 i = 0; i < count; ++i) {
        const CaptureSource& source = g_sources[i];
        if (source.kind == CaptureSourceKind::screen) {
            ordered = ordered && screens == i;
            ++screens;
            named = named && source.name_bytes >= 8 && bytes_equal(source.name, "Screen ", 7) && source.width &&
                    source.height;
        } else {
            foreign = foreign && source.process_id != GetCurrentProcessId() && source.name_bytes &&
                      source.name_bytes < kCaptureSourceNameBytes;
        }
    }
    check(screens >= 1 && g_sources[0].primary, "sources: at least one screen, the primary first");
    check(ordered && named, "sources: screens come before windows, each named Screen N with its size");
    check(foreign, "sources: windows are named and none is this process's");
    check(capture_sources({g_sources, 1}) == 1 && capture_sources({}) == 0, "sources: the list respects its capacity");
}

// A BGRA picture of four colored quadrants, 64x48, reduced on the GPU to fit 16x16 (16x12) and read back as RGBA.
u32 g_bgra[64 * 48];
u8 g_rgba[64 * 64 * 4];
void test_gpu_thumbnail() noexcept {
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        test_write_line("SKIP capture thumbnail on the GPU (no hardware Direct3D 11 device)");
        return;
    }
    // BGRA in memory: 0xAARRGGBB as a little-endian word.
    const u32 quadrant[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0x80FFFFFFu};
    for (u32 y = 0; y < 48; ++y)
        for (u32 x = 0; x < 64; ++x)
            g_bgra[y * 64 + x] = quadrant[(y >= 24 ? 2 : 0) + (x >= 32 ? 1 : 0)];
    GpuTexture* texture = nullptr;
    const GpuTextureDesc desc{64, 48, GpuFormat::bgra8, GpuUsage::default_, gpu_bind_srv};
    const GpuInitialData initial{g_bgra, 64 * 4};
    check(gpu_ok(gpu_texture_create(device, desc, &initial, &texture)), "thumbnail: the synthetic picture");
    u32 width = 0, height = 0;
    const bool reduced = texture && capture_reduce_to_rgba(device, texture, 64, 48, 16, 16, g_rgba, &width, &height);
    check(reduced && width == 16 && height == 12, "thumbnail: 64x48 reduces to 16x12 for a 16x16 box");
    const u8 expected[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
    bool colors = reduced;
    for (u32 y = 0; reduced && y < height; ++y)
        for (u32 x = 0; x < width; ++x) {
            const u8* pixel = g_rgba + (y * width + x) * 4;
            const u8* want = expected[(y >= 6 ? 2 : 0) + (x >= 8 ? 1 : 0)];
            colors = colors && pixel[0] == want[0] && pixel[1] == want[1] && pixel[2] == want[2] && pixel[3] == 255;
        }
    check(colors, "thumbnail: each quadrant keeps its color, as opaque RGBA");
    // A picture that already fits is read back as it is; the region is the top-left of a larger texture.
    const bool direct = texture && capture_reduce_to_rgba(device, texture, 40, 30, 64, 64, g_rgba, &width, &height);
    check(direct && width == 40 && height == 30 && g_rgba[0] == 255 && g_rgba[1] == 0 &&
              g_rgba[(29 * 40 + 39) * 4 + 1] == 255 && g_rgba[(29 * 40 + 39) * 4 + 3] == 255,
          "thumbnail: a picture that fits is read back unreduced, from the top-left region");
    gpu_release(texture);
    gpu_device_destroy(&device, &context);
}
#else
using namespace pipewire;

const u32 kGoldenEnumBgrx[] = {
    0xf8,    0xf,  0x40003, 0x3,     0x1,      0x0,  0x4,    0x3,    0x2,      0x0,      0x2,  0x0,      0x4,
    0x3,     0x1,  0x0,     0x20001, 0x0,      0x4,  0x3,    0x8,    0x0,      0x20002,  0x18, 0x28,     0x13,
    0x3,     0x0,  0x8,     0x5,     0x0,      0x0,  0x0,    0x0,    0xffffffff, 0xffffff, 0x20003, 0x0, 0x28,
    0x13,    0x1,  0x0,     0x8,     0xa,      0x780, 0x438, 0x1,    0x1,      0x4000,   0x4000, 0x20004, 0x0,
    0x28,    0x13, 0x1,     0x0,     0x8,      0xb,  0x1e,   0x1,    0x0,      0x1,      0x3e8, 0x1};
const u32 kGoldenFixedRgba[] = {
    0xd8,  0xf,      0x40003, 0x3,  0x1,     0x0,  0x4,  0x3,  0x2,  0x0,    0x2,    0x0,     0x4,   0x3,
    0x1,   0x0,      0x20001, 0x0,  0x4,     0x3,  0xb,  0x0,  0x20002, 0x8, 0x8,    0x5,     0x2,   0x1000000,
    0x20003, 0x0,    0x28,    0x13, 0x1,     0x0,  0x8,  0xa,  0x780, 0x438, 0x1,    0x1,     0x4000, 0x4000,
    0x20004, 0x0,    0x28,    0x13, 0x1,     0x0,  0x8,  0xb,  0x3c,  0x1,   0x0,    0x1,     0x3e8, 0x1};
const u32 kGoldenBuffers[] = {0x60, 0xf, 0x40004, 0x5,  0x1, 0x0, 0x1c, 0x13, 0x1, 0x0, 0x4, 0x4, 0x4,
                              0x2,  0x8, 0x0,     0x6,  0x0, 0x14, 0x13, 0x4, 0x0, 0x4, 0x4, 0x8, 0x0};
const u32 kGoldenMeta[] = {0x38, 0xf, 0x40005, 0x6, 0x1, 0x0, 0x4, 0x3, 0x1, 0x0, 0x2, 0x0, 0x4, 0x4, 0x20, 0x0};

alignas(8) u8 g_pods[4096];

bool pod_equals(const Pod* pod, const u32* golden, u32 words) noexcept {
    return pod && 8 + pod->size == words * 4 && bytes_equal(pod, golden, words * 4);
}

void test_pods() noexcept {
    const Pod* pods[4]{};
    const u32 count = build_enum_formats(g_pods, sizeof(g_pods), pods, 30, false, 0, 0);
    check(count == 4, "pods: four packed RGB formats are offered");
    check(count == 4 && pod_equals(pods[0], kGoldenEnumBgrx, sizeof(kGoldenEnumBgrx) / 4),
          "pods: the BGRx offer (modifier choice linear or implicit, sizes, rates) is libspa's byte for byte");
    u32 formats[4]{};
    for (u32 i = 0; i < count; ++i) {
        VideoFormat parsed{};
        // An offer is not a settled format (its size is a range): the parse reads the defaults.
        check(parse_video_format(pods[i], &parsed) && parsed.modifier_choice && parsed.modifier == 0 &&
                  parsed.width == 1920 && parsed.height == 1080,
              "pods: each offer reads back with its modifier left to choose");
        formats[i] = parsed.video_format;
    }
    check(formats[0] == kVideoFormatBgrx && formats[1] == kVideoFormatBgra && formats[2] == kVideoFormatRgbx &&
              formats[3] == kVideoFormatRgba,
          "pods: BGRx first (what compositors give), then BGRA, RGBx, RGBA");
    const u32 fixed = build_enum_formats(g_pods, sizeof(g_pods), pods, 60, true, kVideoFormatRgba, 0x0100000000000002ull);
    check(fixed == 1 && pod_equals(pods[0], kGoldenFixedRgba, sizeof(kGoldenFixedRgba) / 4),
          "pods: a fixed modifier for the settled format is libspa's byte for byte");
    VideoFormat settled{};
    check(parse_video_format(pods[0], &settled) && !settled.modifier_choice && settled.modifier == 0x0100000000000002ull &&
              settled.video_format == kVideoFormatRgba,
          "pods: a fixed modifier reads back as settled");
    check(build_enum_formats(g_pods, 64, pods, 30, false, 0, 0) == 0, "pods: storage that is too small writes nothing");
    buffer_params(pods);
    check(pod_equals(pods[0], kGoldenBuffers, sizeof(kGoldenBuffers) / 4) &&
              pod_equals(pods[1], kGoldenMeta, sizeof(kGoldenMeta) / 4),
          "pods: dma-buf buffers and the header meta are libspa's byte for byte");
    // Hostile input: a truncated object, another object type, an unaccepted format.
    alignas(8) u8 broken[sizeof(kGoldenFixedRgba)];
    copy_bytes(broken, kGoldenFixedRgba, sizeof(broken));
    VideoFormat parsed{};
    reinterpret_cast<Pod*>(broken)->size = 40;
    const bool truncated = parse_video_format(reinterpret_cast<const Pod*>(broken), &parsed);
    copy_bytes(broken, kGoldenBuffers, sizeof(kGoldenBuffers));
    const bool other = parse_video_format(reinterpret_cast<const Pod*>(broken), &parsed);
    copy_bytes(broken, kGoldenFixedRgba, sizeof(broken));
    reinterpret_cast<u32*>(broken)[20] = 2; // SPA_VIDEO_FORMAT_I420
    const bool planar = parse_video_format(reinterpret_cast<const Pod*>(broken), &parsed);
    check(!truncated && !other && !planar && !parse_video_format(nullptr, &parsed),
          "pods: a truncated object, another object and a planar format are refused");
}

const u32 kGoldenAudio[] = {0xa0,  0xf, 0x40003, 0x3,     0x1,  0x0, 0x4,     0x3,    0x1, 0x0, 0x2,   0x0,
                            0x4,   0x3, 0x1,     0x0,     0x10001, 0x0, 0x4,  0x3,    0x103, 0x0, 0x10003, 0x0,
                            0x4,   0x4, 0xbb80,  0x0,     0x10004, 0x0, 0x4,  0x4,    0x2, 0x0, 0x10005, 0x0,
                            0x10,  0xd, 0x4,     0x3,     0x3,  0x4};

void test_stream_audio_rules() noexcept {
    check(pod_equals(audio_format(), kGoldenAudio, sizeof(kGoldenAudio) / 4),
          "stream audio: the 48 kHz stereo S16LE capture format is libspa's byte for byte");
    const StreamAudioOptions screen{StreamAudioScope::everything, 0};
    check(stream_audio_node_recorded(true, 4242, 1000, screen),
          "stream audio: another application's playback stream is recorded");
    check(!stream_audio_node_recorded(true, 1000, 1000, screen),
          "stream audio: MaiCord's own playback (the call) is never recorded");
    check(!stream_audio_node_recorded(false, 4242, 1000, screen),
          "stream audio: recordings, sinks and nodes without a class are not playback");
    check(stream_audio_node_recorded(true, 0, 1000, screen),
          "stream audio: a stream whose node and client name no process is another application's");
    const StreamAudioOptions window{StreamAudioScope::application, 4242};
    check(stream_audio_node_recorded(true, 4242, 1000, window) && !stream_audio_node_recorded(true, 5151, 1000, window) &&
              stream_audio_node_recorded(true, 5151, 1000, {StreamAudioScope::application, 0}),
          "stream audio: a window's process alone, or everything but MaiCord without one");
    check(stream_audio_channel_inputs("FL") == 1 && stream_audio_channel_inputs("RL") == 1 &&
              stream_audio_channel_inputs("FR") == 2 && stream_audio_channel_inputs("SR") == 2 &&
              stream_audio_channel_inputs("MONO") == 3 && stream_audio_channel_inputs("FC") == 3 &&
              stream_audio_channel_inputs("LFE") == 0 && stream_audio_channel_inputs(nullptr) == 0,
          "stream audio: left to left, right to right, mono and centre to both, LFE to none");
    check(stream_audio_property_number("123") == 123 && stream_audio_property_number("") == 0 &&
              stream_audio_property_number("12a") == 0 && stream_audio_property_number("99999999999") == 0,
          "stream audio: property numbers parse strictly");
}

void test_fourcc() noexcept {
    check(drm_fourcc(kVideoFormatBgrx) == 0x34325258 && drm_fourcc(kVideoFormatBgra) == 0x34325241 &&
              drm_fourcc(kVideoFormatRgbx) == 0x34324258 && drm_fourcc(kVideoFormatRgba) == 0x34324241 &&
              drm_fourcc(2) == 0,
          "fourcc: BGRx XR24, BGRA AR24, RGBx XB24, RGBA AB24, nothing else");
    check(capture_sources({}) == 0 && !capture_thumbnail(nullptr, CaptureSource{}, 1, 1, g_pods, nullptr, nullptr),
          "Linux: the portal's picker, no source list or thumbnails of ours");
}
#endif

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_capture_entry) {
    using namespace maicord;
    test_window_rules();
    test_thumbnail_steps();
    test_pacer();
    test_names();
#if defined(_WIN32)
    test_capture_abi(check);
    test_system_methods();
    test_source_list();
    test_gpu_thumbnail();
#else
    test_pods();
    test_fourcc();
    test_stream_audio_rules();
#endif
    test_exit(g_failures, "maicord_capture");
}
