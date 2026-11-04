// The hardware tier's glue without a GPU: how the stream decoder chooses and gives up a tier, and how a VA-API picture
// is submitted, against doubles (WSL has no /dev/dri; hardware validation is pending).
//
//   tiers      a hardware backend that the probe lists is the one a stream is created on, with HEVC (no software
//              decoder) refused when the probe lists none, and the codecs word reporting hardware codecs as such
//   fallback   a hardware backend that gives up on a stream (the profile, size or format the driver refuses) hands it
//              to the software tier, which asks for a keyframe and then decodes the stream; HEVC has no tier to fall to
//   va session a vaapi::Session over a recording libva: the calls and their order for a picture (begin, parameter
//              buffers, slices in groups, end, sync), the bytes of every buffer, a refused buffer (the picture still
//              ends, nothing leaks) and a picture of hundreds of slices
//
// The stream decoder's own link to the VA-API backend is two functions (hardware_probe_codecs and
// hardware_stream_backend); this test defines them itself, so it links no libva code but the session.
#include "../../fixtures/video/video_fixtures.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"
#include "maicord/platform/decode/video_capability.hpp"
#include "../../../src/platform/decode/linux/stream.hpp"
#include "../../../src/platform/decode/software/software_decoder.hpp"
#include "../../../src/platform/decode/vaapi/va.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr u32 kH264 = 1u << static_cast<u8>(GpuStreamCodec::h264);
constexpr u32 kH265 = 1u << static_cast<u8>(GpuStreamCodec::h265);
constexpr u32 kAv1 = 1u << static_cast<u8>(GpuStreamCodec::av1);

// ---- The hardware backend double ---------------------------------------------------------------------------------

u32 g_probe;          // what hardware_probe_codecs reports
bool g_give_up;       // the double's decode declares the stream unsupported
u32 g_decodes, g_presented;

void mock_reset(GpuStream* stream) noexcept {
    stream->h264.state.waiting = true;
}
void mock_drain(GpuStream*) noexcept {}
void mock_release(GpuStream* stream) noexcept {
    stream_surfaces_release(stream);
}
void mock_decode(GpuStream* stream, ConstSlice<u8>, u32 rtp_timestamp, bool, bool) noexcept {
    ++g_decodes;
    if (g_give_up) {
        stream->h264.state.unsupported = true;
        return;
    }
    if (!stream->surfaces.count && !stream_surfaces_create_cpu(stream, 3, 64, 48))
        return;
    stream_set_format(stream, 0, 0, 64, 48, 1, false, 1, 1);
    stream_present(stream, static_cast<u8>(g_presented++ % 3), rtp_timestamp, true);
}
void mock_stats(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept {
    stats->unsupported = stream.h264.state.unsupported;
}
const StreamBackend g_mock{true, mock_reset, mock_decode, mock_drain, mock_release, mock_stats};

// ---- The stream under test ---------------------------------------------------------------------------------------

struct Capture {
    u32 frames, requests;
};

void on_frame(void* context, u32, const GpuStreamFrame&) noexcept {
    ++static_cast<Capture*>(context)->frames;
}
void on_keyframe(void* context, u32) noexcept {
    ++static_cast<Capture*>(context)->requests;
}

void tiers(GpuDevice* device, const VideoFixture& h264) noexcept {
    Capture capture{};
    g_probe = kH264 | kH265 | kAv1;
    const u32 word = gpu_stream_decoder_codecs(device);
    check(gpu_stream_codecs_decodable(word) == (kH264 | kH265 | kAv1) && gpu_stream_codecs_software_only(word) == 0,
          "tiers: codecs the hardware lists are not software-only, whatever the libraries add");
    g_decodes = g_presented = 0;
    g_give_up = false;
    GpuStreamDecoder* decoder = gpu_stream_decoder_create({device, 1, {&capture, on_frame, on_keyframe}, GpuStreamCodec::h264, true});
    check(decoder != nullptr, "tiers: a stream on the hardware backend");
    if (decoder) {
        gpu_stream_decoder_push(decoder, {h264.packet[0], h264.packet_bytes[0]}, 0);
        gpu_stream_decoder_push(decoder, {h264.packet[1], h264.packet_bytes[1]}, 3000);
        const GpuStreamDecoderStats stats = gpu_stream_decoder_stats(decoder);
        check(g_decodes == 2 && capture.frames == 2 && stats.surfaces == 3 && !stats.unsupported && !capture.requests,
              "tiers: the hardware backend decodes, its pictures reach the sink");
        gpu_stream_decoder_destroy(decoder);
    }
    // HEVC has no software decoder: without hardware it cannot be created at all.
    g_probe = kH264;
    check(!gpu_stream_decoder_create({device, 2, {&capture, on_frame, on_keyframe}, GpuStreamCodec::h265, true}),
          "tiers: no hardware HEVC, no HEVC stream");
    g_probe = 0;
    GpuStreamDecoder* software = gpu_stream_decoder_create({device, 3, {&capture, on_frame, on_keyframe}, GpuStreamCodec::h264, true});
    check(software != nullptr && g_decodes == 2, "tiers: without hardware the software tier takes the stream");
    gpu_stream_decoder_destroy(software);
}

void fallback(GpuDevice* device, const VideoFixture& h264) noexcept {
    Capture capture{};
    g_probe = kH264 | kH265;
    g_give_up = true;
    g_decodes = 0;
    GpuStreamDecoder* decoder = gpu_stream_decoder_create({device, 4, {&capture, on_frame, on_keyframe}, GpuStreamCodec::h264, true});
    if (!decoder) {
        check(false, "fallback: a stream on the hardware backend");
        return;
    }
    // The first unit reaches the hardware backend, which gives up; the stream moves to the software tier at once and
    // asks for a keyframe (nothing of this unit was decoded).
    gpu_stream_decoder_push(decoder, {h264.packet[0], h264.packet_bytes[0]}, 0);
    GpuStreamDecoderStats stats = gpu_stream_decoder_stats(decoder);
    check(g_decodes == 1 && !stats.unsupported && capture.requests == 1 && capture.frames == 0,
          "fallback: the unsupported verdict hands the stream to the software tier and asks for a keyframe");
    // The keyframe again, now in software: every picture of the stream.
    for (u32 i = 0; i < h264.packet_count; ++i)
        gpu_stream_decoder_push(decoder, {h264.packet[i], h264.packet_bytes[i]}, i * 3000);
    stats = gpu_stream_decoder_stats(decoder);
    check(g_decodes == 1 && capture.frames == h264.packet_count && stats.decoded == h264.packet_count && !stats.unsupported,
          "fallback: the software tier decodes the stream from its keyframe, the hardware backend is not asked again");
    gpu_stream_decoder_destroy(decoder);
    // HEVC: nothing to fall back to, the verdict stands and no keyframe request can change it.
    Capture hevc{};
    GpuStreamDecoder* stuck = gpu_stream_decoder_create({device, 5, {&hevc, on_frame, on_keyframe}, GpuStreamCodec::h265, true});
    if (stuck) {
        gpu_stream_decoder_push(stuck, {h264.packet[0], h264.packet_bytes[0]}, 0);
        check(gpu_stream_decoder_stats(stuck).unsupported && !hevc.frames, "fallback: HEVC stays unsupported");
        gpu_stream_decoder_destroy(stuck);
    } else {
        check(false, "fallback: a hardware HEVC stream");
    }
}

// ---- A recording libva -------------------------------------------------------------------------------------------

struct Call {
    enum Kind : u8 { create_config, create_surfaces, create_context, begin, render, end, sync, create_buffer, destroy_buffer,
                     destroy_context, destroy_surfaces, destroy_config } kind;
    i32 type;
    u32 size;
    u32 count;
    u32 first;
};
constexpr u32 kMaxCalls = 4096;
Call g_calls[kMaxCalls];
u32 g_call_count;
u8 g_buffer_data[600][32]; // the first bytes of every buffer made
u32 g_buffer_size[600], g_buffer_type[600];
u32 g_buffers_made, g_buffers_alive;
u32 g_fail_buffer = ~0u; // the buffer creation number that is refused

void record(Call::Kind kind, i32 type = 0, u32 size = 0, u32 count = 0, u32 first = 0) noexcept {
    if (g_call_count < kMaxCalls)
        g_calls[g_call_count++] = {kind, type, size, count, first};
}

vaapi::Status fake_create_config(vaapi::Display, i32, i32, vaapi::ConfigAttrib*, i32, vaapi::ConfigId* id) {
    record(Call::create_config);
    *id = 7;
    return 0;
}
vaapi::Status fake_destroy_config(vaapi::Display, vaapi::ConfigId) {
    record(Call::destroy_config);
    return 0;
}
vaapi::Status fake_create_surfaces(vaapi::Display, u32, u32, u32, vaapi::SurfaceId* ids, u32 count, vaapi::SurfaceAttrib*, u32) {
    record(Call::create_surfaces, 0, 0, count);
    for (u32 i = 0; i < count; ++i)
        ids[i] = 0x500 + i;
    return 0;
}
vaapi::Status fake_destroy_surfaces(vaapi::Display, vaapi::SurfaceId*, i32 count) {
    record(Call::destroy_surfaces, 0, 0, static_cast<u32>(count));
    return 0;
}
vaapi::Status fake_create_context(vaapi::Display, vaapi::ConfigId, i32, i32, i32, vaapi::SurfaceId*, i32 targets, vaapi::ContextId* id) {
    record(Call::create_context, 0, 0, static_cast<u32>(targets));
    *id = 9;
    return 0;
}
vaapi::Status fake_destroy_context(vaapi::Display, vaapi::ContextId) {
    record(Call::destroy_context);
    return 0;
}
vaapi::Status fake_create_buffer(vaapi::Display, vaapi::ContextId, i32 type, u32 size, u32, void* data, vaapi::BufferId* id) {
    const u32 number = g_buffers_made;
    record(Call::create_buffer, type, size, 0, number);
    if (number == g_fail_buffer || number >= 600)
        return 2;
    g_buffer_size[number] = size;
    g_buffer_type[number] = static_cast<u32>(type);
    copy_bytes(g_buffer_data[number], data, size < 32 ? size : 32);
    ++g_buffers_made;
    ++g_buffers_alive;
    *id = 0x1000 + number;
    return 0;
}
vaapi::Status fake_destroy_buffer(vaapi::Display, vaapi::BufferId) {
    record(Call::destroy_buffer);
    --g_buffers_alive;
    return 0;
}
vaapi::Status fake_begin(vaapi::Display, vaapi::ContextId, vaapi::SurfaceId target) {
    record(Call::begin, 0, 0, 0, target);
    return 0;
}
vaapi::Status fake_render(vaapi::Display, vaapi::ContextId, vaapi::BufferId* buffers, i32 count) {
    record(Call::render, 0, 0, static_cast<u32>(count), buffers[0]);
    return 0;
}
vaapi::Status fake_end(vaapi::Display, vaapi::ContextId) {
    record(Call::end);
    return 0;
}
vaapi::Status fake_sync(vaapi::Display, vaapi::SurfaceId target) {
    record(Call::sync, 0, 0, 0, target);
    return 0;
}

void install(vaapi::Driver* d) noexcept {
    clear_bytes(d, sizeof(*d));
    d->display = reinterpret_cast<vaapi::Display>(1);
    d->api.vaCreateConfig = fake_create_config;
    d->api.vaDestroyConfig = fake_destroy_config;
    d->api.vaCreateSurfaces = fake_create_surfaces;
    d->api.vaDestroySurfaces = fake_destroy_surfaces;
    d->api.vaCreateContext = fake_create_context;
    d->api.vaDestroyContext = fake_destroy_context;
    d->api.vaCreateBuffer = fake_create_buffer;
    d->api.vaDestroyBuffer = fake_destroy_buffer;
    d->api.vaBeginPicture = fake_begin;
    d->api.vaRenderPicture = fake_render;
    d->api.vaEndPicture = fake_end;
    d->api.vaSyncSurface = fake_sync;
}

u32 count_calls(Call::Kind kind) noexcept {
    u32 n = 0;
    for (u32 i = 0; i < g_call_count; ++i)
        n += g_calls[i].kind == kind;
    return n;
}

u32 position(Call::Kind kind, u32 occurrence = 0) noexcept {
    for (u32 i = 0; i < g_call_count; ++i)
        if (g_calls[i].kind == kind && occurrence-- == 0)
            return i;
    return ~0u;
}

void reset_log() noexcept {
    g_call_count = g_buffers_made = g_buffers_alive = 0;
    g_fail_buffer = ~0u;
}

void session() noexcept {
    static vaapi::Driver driver;
    install(&driver);
    reset_log();
    vaapi::Session* s = vaapi::session_create(&driver, vaapi::kProfileH264Main, 64, 48, 5);
    check(s && s->count == 5 && s->surface[0] == 0x500 && s->surface[4] == 0x504 && position(Call::create_config) == 0 &&
              position(Call::create_surfaces) == 1 && position(Call::create_context) == 2 && g_calls[2].count == 5,
          "va session: a config, the surfaces, a context over every surface as a render target");
    if (!s)
        return;
    // One picture: parameters, an IQ matrix, two slices.
    const u8 parameters[40] = {1, 2, 3, 4}, matrix[8] = {9, 9}, slice0[16] = {0xA0}, slice1[16] = {0xB0}, data0[300] = {0xD0},
             data1[100] = {0xD1};
    check(vaapi::picture_begin(s, 2) && vaapi::picture_buffer(s, vaapi::kPictureParameterBuffer, parameters, sizeof(parameters)) &&
              vaapi::picture_buffer(s, vaapi::kIqMatrixBuffer, matrix, sizeof(matrix)) &&
              vaapi::picture_slice(s, slice0, sizeof(slice0), data0, sizeof(data0)) &&
              vaapi::picture_slice(s, slice1, sizeof(slice1), data1, sizeof(data1)) && vaapi::picture_end(s),
          "va session: a picture goes through");
    const u32 begin = position(Call::begin), end = position(Call::end), sync = position(Call::sync);
    check(g_calls[begin].first == 0x502 && begin < position(Call::render) && end > position(Call::render, 2) && sync > end &&
              g_calls[sync].first == 0x502,
          "va session: begin on the target surface, end after the renders, then the sync on the same surface");
    // The renders: parameters, matrix, then the four slice buffers together in the order parameter, data, parameter, data.
    check(count_calls(Call::render) == 3 && g_calls[position(Call::render, 0)].count == 1 &&
              g_calls[position(Call::render, 1)].count == 1 && g_calls[position(Call::render, 2)].count == 4,
          "va session: parameters and matrix one by one, the slices in one render call");
    check(g_buffer_type[0] == vaapi::kPictureParameterBuffer && g_buffer_size[0] == 40 && g_buffer_data[0][0] == 1 &&
              g_buffer_type[1] == vaapi::kIqMatrixBuffer && g_buffer_type[2] == vaapi::kSliceParameterBuffer &&
              g_buffer_data[2][0] == 0xA0 && g_buffer_type[3] == vaapi::kSliceDataBuffer && g_buffer_size[3] == 300 &&
              g_buffer_data[3][0] == 0xD0 && g_buffer_type[4] == vaapi::kSliceParameterBuffer && g_buffer_data[4][0] == 0xB0 &&
              g_buffer_type[5] == vaapi::kSliceDataBuffer && g_buffer_size[5] == 100 && g_buffer_data[5][0] == 0xD1,
          "va session: every buffer has its type, size and bytes");
    check(g_buffers_made == 6 && g_buffers_alive == 0 && count_calls(Call::destroy_buffer) == 6,
          "va session: every buffer is destroyed after the picture");

    // A refused buffer: the failure is reported by picture_end, the picture is still ended and nothing leaks.
    reset_log();
    g_fail_buffer = 3;
    bool accepted = vaapi::picture_begin(s, 0);
    accepted = vaapi::picture_buffer(s, vaapi::kPictureParameterBuffer, parameters, sizeof(parameters)) && accepted;
    accepted = vaapi::picture_slice(s, slice0, sizeof(slice0), data0, sizeof(data0)) && accepted;
    const bool second = vaapi::picture_slice(s, slice1, sizeof(slice1), data1, sizeof(data1));
    check(accepted && !second && !vaapi::picture_end(s) && count_calls(Call::end) == 1 && g_buffers_alive == 0 &&
              count_calls(Call::sync) == 0,
          "va session: a refused buffer fails the picture, which is still ended, with no buffer left and no sync");

    // Hundreds of slices (H.264 allows 512): rendered in groups that fit the pending list, in order.
    reset_log();
    bool all = vaapi::picture_begin(s, 1) && vaapi::picture_buffer(s, vaapi::kPictureParameterBuffer, parameters, sizeof(parameters));
    u8 mark[16]{};
    for (u32 i = 0; i < 280 && all; ++i) {
        mark[0] = static_cast<u8>(i);
        all = vaapi::picture_slice(s, mark, sizeof(mark), data1, 4);
        if (g_buffers_made >= 590)
            break;
    }
    check(all && vaapi::picture_end(s) && g_buffers_alive == 0, "va session: a picture of many slices");
    bool grouped = true;
    u32 largest = 0;
    for (u32 i = 0; i < g_call_count; ++i)
        if (g_calls[i].kind == Call::render) {
            largest = g_calls[i].count > largest ? g_calls[i].count : largest;
            grouped = grouped && g_calls[i].count <= 128;
        }
    check(grouped && largest == 128 && count_calls(Call::render) >= 3, "va session: slice buffers go out in groups of at most 128");

    vaapi::session_destroy(s);
    check(count_calls(Call::destroy_context) == 1 && count_calls(Call::destroy_surfaces) == 1 && count_calls(Call::destroy_config) == 1,
          "va session: destroy releases the context, the surfaces and the config");
    // A session that cannot be completed gives back what it made.
    reset_log();
    driver.api.vaCreateContext = [](vaapi::Display, vaapi::ConfigId, i32, i32, i32, vaapi::SurfaceId*, i32, vaapi::ContextId*) -> vaapi::Status { return 4; };
    check(!vaapi::session_create(&driver, vaapi::kProfileHevcMain, 64, 64, 3) && count_calls(Call::destroy_surfaces) == 1 &&
              count_calls(Call::destroy_config) == 1 && count_calls(Call::destroy_context) == 0,
          "va session: a refused context releases the surfaces and the config");
}

} // namespace

// What the stream decoder asks of the VA-API backend (src/platform/decode/vaapi/va_stream.cpp), answered by the double.
u32 hardware_probe_codecs(GpuDevice*) noexcept {
    return g_probe;
}
const StreamBackend* hardware_stream_backend(GpuStreamCodec) noexcept {
    return &g_mock;
}

} // namespace maicord

MAICORD_TEST_ENTRY(maicord_video_hardware_tier_entry) {
    using namespace maicord;
    VideoFixture h264;
    check(video_fixture_parse("h264_baseline", kVideoH264Baseline, sizeof(kVideoH264Baseline), &h264), "video fixture parses");
    session();
    if (software_codec_available(GpuStreamCodec::h264)) {
        GpuDevice* device = nullptr;
        GpuContext* context = nullptr;
        if (gpu_ok(gpu_device_create(&device, &context))) {
            tiers(device, h264);
            fallback(device, h264);
            gpu_device_destroy(&device, &context);
        } else {
            test_write_line("tiers and fallback skipped: no EGL device");
        }
    } else {
        test_write_line("tiers and fallback skipped: libopenh264 is not installed");
    }
    test_exit(g_failures, "maicord_video_hardware_tier");
}
