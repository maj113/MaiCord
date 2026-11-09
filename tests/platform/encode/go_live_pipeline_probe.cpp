// The opt-in probe of Go Live's producer chain on the real desktop (MAICORD_CAPTURE_PROBE=1; status 77 otherwise, so the
// suite never captures anyone's screen): the primary screen captured at 30 fps into the real hardware encoder for six
// seconds. It shows no window and prints counts only, never a name or a pixel: per second, the pictures the capture
// delivered, the pictures the encoder took and dropped, the units it produced and the self-preview's picture sequence,
// to see where pictures stop between the capture and the wire. A still screen delivers few pictures (the capture only
// reports changes), so a second with no picture is not a failure; a picture that arrives must reach the units.
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/capture/screen_capture.hpp"
#include "maicord/platform/encode/gpu_stream_encoder.hpp"
#include "../../support/test_output.hpp"

namespace maicord {
namespace {
constexpr u32 kSeconds = 6;

struct Counts {
    volatile LONG frames, units, keyframes, ended;
};
Counts g_counts;
GpuStreamEncoder* g_encoder;
CaptureSource g_sources[64];

void on_frame(void*, const CaptureFrame& frame) noexcept {
    InterlockedIncrement(&g_counts.frames);
    gpu_stream_encoder_submit(g_encoder, frame);
}
void on_capture_ended(void*, CaptureEnd) noexcept {
    InterlockedIncrement(&g_counts.ended);
}
void on_unit(void*, const GpuStreamAccessUnit& unit) noexcept {
    InterlockedIncrement(&g_counts.units);
    if (unit.keyframe)
        InterlockedIncrement(&g_counts.keyframes);
}
void on_encoder_ended(void*, GpuStreamEncoderEnd) noexcept {
    InterlockedIncrement(&g_counts.ended);
}
} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_go_live_pipeline_probe_entry) {
    using namespace maicord;
    wchar_t flag[4]{};
    if (GetEnvironmentVariableW(L"MAICORD_CAPTURE_PROBE", flag, 4) == 0) {
        test_write_line("SKIP go_live_pipeline_probe (opt-in: MAICORD_CAPTURE_PROBE=1 captures this desktop's screen)");
        test_process_exit(77);
    }
    u32 failures = 0;
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    test_check(&failures, gpu_ok(gpu_device_create(&device, &context)), "probe: a hardware Direct3D 11 device");
    const u32 count = device ? capture_sources({g_sources, 64}) : 0;
    u32 screen = count;
    for (u32 i = 0; i < count && screen == count; ++i)
        if (g_sources[i].kind == CaptureSourceKind::screen)
            screen = i;
    GpuStreamCodec codec{};
    const u32 codecs = device ? gpu_stream_encoder_codecs(device) : 0;
    if (!codecs || screen == count || !gpu_stream_encoder_pick(codecs, codecs, &codec)) {
        test_write_line("SKIP go_live_pipeline_probe (no screen or no hardware encoder)");
        test_process_exit(77);
    }
    g_encoder = gpu_stream_encoder_create({device, codec, {1280, 720, 30, 4'000'000}, {nullptr, on_unit, on_encoder_ended}});
    test_check(&failures, g_encoder != nullptr, "probe: the encoder starts");
    CaptureOptions options{};
    options.device = device;
    options.source = g_sources[screen];
    options.max_fps = 30;
    options.cursor = true;
    ScreenCapture* capture = g_encoder ? capture_start(options, {nullptr, on_frame, on_capture_ended, nullptr}) : nullptr;
    test_check(&failures, capture != nullptr, "probe: the capture starts");
    u32 previous_frames = 0, previous_units = 0, previous_sequence = 0;
    bool delivered = true, previewed = true;
    for (u32 second = 0; capture && second < kSeconds; ++second) {
        thread_sleep(1000);
        const u32 frames = static_cast<u32>(g_counts.frames), units = static_cast<u32>(g_counts.units);
        const auto stats = gpu_stream_encoder_stats(g_encoder);
        GpuVideoFrame preview{};
        u32 sequence = 0;
        if (gpu_stream_encoder_preview(g_encoder, &preview, &sequence)) {
            gpu_release(preview.chroma);
            gpu_release(preview.luma);
            gpu_release(preview.texture);
        }
        test_write_metric("second: ", second + 1);
        test_write_metric("  capture frames: ", frames - previous_frames);
        test_write_metric("  encoder took: ", stats.submitted);
        test_write_metric("  encoder dropped: ", stats.dropped);
        test_write_metric("  units: ", units - previous_units);
        test_write_metric("  preview sequence: ", sequence);
        // A second that delivered pictures produced units.
        if (second > 0)
            delivered = delivered && (units > previous_units || frames == previous_frames);
        previewed = previewed && (sequence != previous_sequence || units == previous_units);
        previous_frames = frames;
        previous_units = units;
        previous_sequence = sequence;
    }
    capture_stop(capture);
    gpu_stream_encoder_destroy(g_encoder);
    test_write_metric("capture frames: ", static_cast<u32>(g_counts.frames));
    test_write_metric("units: ", static_cast<u32>(g_counts.units));
    test_write_metric("keyframes: ", static_cast<u32>(g_counts.keyframes));
    test_check(&failures, g_counts.frames > 0 && g_counts.units > 0, "probe: pictures reach the encoder's units");
    test_check(&failures, delivered, "probe: units keep coming while the capture delivers pictures");
    test_check(&failures, previewed, "probe: the preview picture changes while units come");
    test_check(&failures, !g_counts.ended, "probe: neither the capture nor the encoder ended");
    gpu_device_destroy(&device, &context);
    test_exit(failures, "go_live_pipeline_probe");
}
