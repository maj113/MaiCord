// The hardware tier's glue without a GPU: how the stream decoder chooses and gives up a tier, and how a VA-API picture
// is submitted, against doubles (VA-API has no driver on this machine; hardware validation is pending).
//
//   tiers      a hardware backend that the probe lists is the one a stream is created on, with HEVC (no software
//              decoder) refused when the probe lists none, and the codecs word reporting hardware codecs as such
//   fallback   a hardware backend that gives up on a stream (the profile, size or format the driver refuses) hands it
//              to the software tier, which asks for a keyframe and then decodes the stream; HEVC has no tier to fall to
//   va session a vaapi::Session over a recording libva: the calls and their order for a picture (begin, parameter
//              buffers, slices in groups, end; no wait), the bytes of every buffer, a refused buffer (the picture still
//              ends, nothing leaks) and a picture of hundreds of slices
//   waits      the wait for a picture (vaSyncSurface2 with its timeout, or vaSyncSurface), the wait before a session
//              goes, and the surface requests (NV12 or P010, the usage hints and their refusal)
//   imports    what the renderer's import refuses before touching a buffer (needs an EGL device)
//   kept       export_picture over real dma-bufs (dumb buffers of a DRM device; skipped without one): the images of an
//              export are kept per surface while the buffers and layout are the same, replaced when they change, and
//              made again for a driver without stable exports
//
// The stream decoder's own link to the VA-API backend is two functions (hardware_probe_codecs and
// hardware_stream_backend); this test defines them itself, so it links no libva code but the session.
#include "../../support/dumb_dmabuf.hpp"
#include "../../fixtures/mp4/mp4_fixtures.hpp"
#include "../../fixtures/video/video_fixtures.hpp"
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"
#include "maicord/platform/decode/video_capability.hpp"
#include "../../../src/platform/decode/linux/stream.hpp"
#include "../../../src/platform/decode/software/software_decoder.hpp"
#include "../../../src/platform/decode/h264_gpu_configuration.hpp"
#include "../../../src/platform/decode/vaapi/va.hpp"
#include "../../../src/platform/decode/vaapi/va_file.hpp"
#include "maicord/media/containers/hevc_mp4_demux.hpp"
#include "maicord/media/video/hevc_references.hpp"

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
    enum Kind : u8 { create_config, create_surfaces, create_context, begin, render, end, sync, sync2, create_buffer,
                     destroy_buffer, destroy_context, destroy_surfaces, destroy_config } kind;
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
struct SurfaceRequest {
    u32 format, attribute_count, fourcc, hint;
};
SurfaceRequest g_surface_request[4]; // what vaCreateSurfaces was asked, call by call
u32 g_surface_requests;
bool g_refuse_hint;      // vaCreateSurfaces refuses a request that carries the usage hint
u32 g_config_format;     // the render target format of the last config
u64 g_sync_timeout;      // the timeout of the last vaSyncSurface2
vaapi::Status g_sync2_status;

void record(Call::Kind kind, i32 type = 0, u32 size = 0, u32 count = 0, u32 first = 0) noexcept {
    if (g_call_count < kMaxCalls)
        g_calls[g_call_count++] = {kind, type, size, count, first};
}

vaapi::Status fake_create_config(vaapi::Display, i32, i32, vaapi::ConfigAttrib* attributes, i32, vaapi::ConfigId* id) {
    record(Call::create_config);
    g_config_format = attributes[0].value;
    *id = 7;
    return 0;
}
vaapi::Status fake_destroy_config(vaapi::Display, vaapi::ConfigId) {
    record(Call::destroy_config);
    return 0;
}
vaapi::Status fake_create_surfaces(vaapi::Display, u32 format, u32, u32, vaapi::SurfaceId* ids, u32 count,
                                   vaapi::SurfaceAttrib* attributes, u32 attribute_count) {
    record(Call::create_surfaces, 0, 0, count);
    if (g_surface_requests < 4) {
        SurfaceRequest& request = g_surface_request[g_surface_requests++];
        request = {format, attribute_count, 0, 0};
        for (u32 i = 0; i < attribute_count; ++i) {
            if (attributes[i].type == vaapi::kSurfaceAttribPixelFormat)
                request.fourcc = static_cast<u32>(attributes[i].value.value.i);
            if (attributes[i].type == vaapi::kSurfaceAttribUsageHint)
                request.hint = static_cast<u32>(attributes[i].value.value.i);
        }
    }
    if (g_refuse_hint && attribute_count == 2)
        return 12; // VA_STATUS_ERROR_ATTR_NOT_SUPPORTED
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
vaapi::Status fake_sync2(vaapi::Display, vaapi::SurfaceId target, u64 timeout) {
    record(Call::sync2, 0, 0, 0, target);
    g_sync_timeout = timeout;
    return g_sync2_status;
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
    g_surface_requests = 0;
    g_refuse_hint = false;
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
    const u32 begin = position(Call::begin), end = position(Call::end);
    check(g_calls[begin].first == 0x502 && begin < position(Call::render) && end > position(Call::render, 2) &&
              count_calls(Call::sync) == 0,
          "va session: begin on the target surface, end after the renders, and no wait in picture_end");
    check(vaapi::session_sync(s, 2) && count_calls(Call::sync) == 1 && g_calls[position(Call::sync)].first == 0x502,
          "va session: session_sync waits for the surface with vaSyncSurface");
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

// ---- Waits and surface requests ----------------------------------------------------------------------------------

void one_picture(vaapi::Session* s, u32 surface) noexcept {
    const u8 parameters[8] = {1};
    vaapi::picture_begin(s, surface);
    vaapi::picture_buffer(s, vaapi::kPictureParameterBuffer, parameters, sizeof(parameters));
    vaapi::picture_end(s);
}

void waits() noexcept {
    static vaapi::Driver driver;
    install(&driver);
    driver.api.vaSyncSurface2 = fake_sync2;
    driver.timed_sync = true;
    reset_log();
    g_sync2_status = 0;
    vaapi::Session* s = vaapi::session_create(&driver, vaapi::kProfileH264Main, 64, 48, 3);
    if (!s) {
        check(false, "waits: a session");
        return;
    }
    one_picture(s, 1);
    check(count_calls(Call::sync2) == 0 && count_calls(Call::sync) == 0, "waits: a picture is submitted without a wait");
    check(vaapi::session_sync(s, 1) && count_calls(Call::sync2) == 1 && count_calls(Call::sync) == 0 &&
              g_calls[position(Call::sync2)].first == 0x501 && g_sync_timeout == vaapi::kSyncTimeoutNanoseconds,
          "waits: a driver with vaSyncSurface2 is waited on with the bounded timeout");
    g_sync2_status = vaapi::kStatusTimedOut;
    check(!vaapi::session_sync(s, 1), "waits: a timeout fails the wait");
    g_sync2_status = 0;
    check(!vaapi::session_sync(s, 3) && !vaapi::session_sync(nullptr, 0), "waits: a surface the session does not have is refused");
    // A picture nobody waited for is waited for before its surfaces go.
    one_picture(s, 2);
    reset_log();
    vaapi::session_destroy(s);
    check(count_calls(Call::sync2) == 1 && g_calls[position(Call::sync2)].first == 0x502 &&
              position(Call::sync2) < position(Call::destroy_context),
          "waits: destroying a session waits for the picture still in flight, before the context goes");
    // A picture that was waited for is not waited for again.
    s = vaapi::session_create(&driver, vaapi::kProfileH264Main, 64, 48, 3);
    if (!s) {
        check(false, "waits: a second session");
        return;
    }
    one_picture(s, 0);
    vaapi::session_sync(s, 0);
    reset_log();
    vaapi::session_destroy(s);
    check(count_calls(Call::sync2) == 0 && count_calls(Call::sync) == 0, "waits: a waited-for picture is not waited for again");
    // A driver without vaSyncSurface2 is waited on with vaSyncSurface.
    driver.timed_sync = false;
    s = vaapi::session_create(&driver, vaapi::kProfileH264Main, 64, 48, 3);
    if (!s) {
        check(false, "waits: a third session");
        return;
    }
    one_picture(s, 0);
    reset_log();
    check(vaapi::session_sync(s, 0) && count_calls(Call::sync) == 1 && count_calls(Call::sync2) == 0,
          "waits: without vaSyncSurface2 the wait is vaSyncSurface");
    vaapi::session_destroy(s);
}

void surface_requests() noexcept {
    using namespace vaapi;
    static Driver driver;
    install(&driver);
    reset_log();
    Session* s = session_create(&driver, kProfileH264Main, 64, 48, 3);
    check(s && g_surface_requests == 1 && g_surface_request[0].attribute_count == 2 &&
              g_surface_request[0].fourcc == kFourccNv12 &&
              g_surface_request[0].hint == (kSurfaceUsageHintDecoder | kSurfaceUsageHintExport) &&
              g_surface_request[0].format == kRtFormatYuv420 && g_config_format == kRtFormatYuv420,
          "surfaces: NV12 with the decoder and export hints");
    session_destroy(s);
    // A driver that refuses the hint gets the plain request, and the session works.
    reset_log();
    g_refuse_hint = true;
    s = session_create(&driver, kProfileH264Main, 64, 48, 3);
    check(s && g_surface_requests == 2 && g_surface_request[1].attribute_count == 1 &&
              g_surface_request[1].fourcc == kFourccNv12 && g_surface_request[1].hint == 0 && s->count == 3,
          "surfaces: a refused hint is dropped from a second request");
    session_destroy(s);
    // 10-bit: P010 surfaces in the 10-bit render target format, in a config of that format.
    reset_log();
    s = session_create(&driver, kProfileAv1Profile0, 64, 64, 2, true);
    check(s && g_surface_requests == 1 && g_surface_request[0].fourcc == kFourccP010 &&
              g_surface_request[0].format == kRtFormatYuv420_10 && g_config_format == kRtFormatYuv420_10,
          "surfaces: a 10-bit session asks for P010");
    session_destroy(s);
}

// ---- Movie samples through the session ---------------------------------------------------------------------------

// The slice parameter and data buffers of a picture, in the order the picture was built: each slice's data buffer is
// the NAL unit (header byte first, no start code, no length prefix) of the sample.
struct Nal {
    const u8* data;
    u32 bytes;
};

u32 sample_nal_units(ConstSlice<u8> sample, u8 length_bytes, Nal* out, u32 capacity) noexcept {
    u32 at = 0, count = 0;
    while (at + length_bytes <= sample.count && count < capacity) {
        u32 length = 0;
        for (u32 i = 0; i < length_bytes; ++i)
            length = (length << 8) | sample[at++];
        if (length > sample.count - at)
            break;
        out[count++] = {sample.data + at, length};
        at += length;
    }
    return count;
}

void movie_samples() noexcept {
    static vaapi::Driver driver;
    install(&driver);
    // H.264: the first two pictures of a Baseline movie (an IDR, then a P picture that has the IDR as a reference).
    Mp4Demuxer demux{};
    Mp4DemuxError error = mp4_demux_initialize(&demux, {kMp4H264BaselineGops, sizeof(kMp4H264BaselineGops)});
    H264GpuPicture first{};
    check(error == Mp4DemuxError::none && h264_gpu_configuration_from_demux(demux, &first) == H264Error::none,
          "movie samples: the Baseline movie's first picture parses");
    reset_log();
    vaapi::Session* h264 = vaapi::session_create(&driver, vaapi::kProfileH264ConstrainedBaseline, first.sps.coded_width,
                                                 first.sps.coded_height, 4);
    if (!h264) {
        check(false, "movie samples: a session for the movie");
        return;
    }
    H264ReferenceState references{};
    H264ReferencePlan plan{};
    bool planned = h264_reference_plan(references, first.sps, first.header.first, 0, &plan) == H264Error::none;
    check(planned && vaapi::submit_h264_sample(h264, first.sps, first.pps, first.header, first.unit.bytes, first.length_bytes,
                                               references, plan.decode_order, 0),
          "movie samples: an IDR sample goes through as one VA picture");
    Nal nals[16];
    const u32 idr_nals = sample_nal_units(first.unit.bytes, first.length_bytes, nals, 16);
    u32 slices = 0;
    for (u32 i = 0; i < idr_nals; ++i)
        slices += (nals[i].data[0] & 31u) == 1 || (nals[i].data[0] & 31u) == 5;
    check(slices == first.header.slice_count && g_buffers_made == 2 + 2 * slices && g_buffers_alive == 0 &&
              g_buffer_type[0] == vaapi::kPictureParameterBuffer && g_buffer_type[1] == vaapi::kIqMatrixBuffer &&
              g_buffer_type[2] == vaapi::kSliceParameterBuffer && g_buffer_type[3] == vaapi::kSliceDataBuffer,
          "movie samples: parameters, matrix, then a parameter and a data buffer per slice, all destroyed");
    bool data_is_nal = true;
    u32 slice_index = 0;
    for (u32 i = 0; i < idr_nals && data_is_nal; ++i) {
        const u32 type = nals[i].data[0] & 31u;
        if (type != 1 && type != 5)
            continue;
        data_is_nal = g_buffer_size[3 + 2 * slice_index] == nals[i].bytes &&
                      g_buffer_data[3 + 2 * slice_index][0] == nals[i].data[0];
        ++slice_index;
    }
    check(data_is_nal, "movie samples: every slice data buffer is its NAL unit, header byte first");
    // The P picture: references from the IDR, the NAL units of its own sample.
    Mp4Demuxer cursor = demux;
    mp4_demux_next(&cursor);
    const Mp4AccessUnit second = mp4_demux_next(&cursor);
    H264Picture header{};
    const bool parsed = second.ok() && h264_parse_picture(second.bytes, first.length_bytes, first.sps, first.pps, &header,
                                                          mp4_demux_avcc(cursor)) == H264Error::none;
    H264ReferencePlan next{};
    reset_log();
    check(parsed && h264_reference_plan(plan.after, first.sps, header.first, 1, &next) == H264Error::none &&
              vaapi::submit_h264_sample(h264, first.sps, first.pps, header, second.bytes, first.length_bytes, plan.after,
                                        next.decode_order, 1) &&
              g_buffers_made == 2 + 2 * header.slice_count && count_calls(Call::sync) == 0,
          "movie samples: a P sample goes through with the IDR as its reference");
    // A sample cut short: refused, still ended, no buffer left behind.
    reset_log();
    const ConstSlice<u8> cut{first.unit.bytes.data, first.unit.bytes.count / 2};
    check(!vaapi::submit_h264_sample(h264, first.sps, first.pps, first.header, cut, first.length_bytes, H264ReferenceState{},
                                     plan.decode_order, 0) &&
              count_calls(Call::begin) == 1 && count_calls(Call::end) == 1 && g_buffers_alive == 0,
          "movie samples: a sample cut short fails the picture, which is still ended, with nothing left");
    vaapi::session_destroy(h264);

    // HEVC: the first picture (an IRAP) of the HEVC movie.
    Mp4Demuxer hevc_demux{};
    HevcConfiguration config;
    HevcPicture picture;
    bool hevc_ok = hevc_demux_initialize(&hevc_demux, {kMp4HevcMainAac, sizeof(kMp4HevcMainAac)}) == Mp4DemuxError::none;
    Mp4Demuxer hevc_cursor = hevc_demux;
    const Mp4AccessUnit irap = hevc_ok ? mp4_demux_next_ready(&hevc_cursor) : Mp4AccessUnit{};
    u8 pps = 0;
    hevc_ok = hevc_ok && irap.ok() &&
              hevc_picture_parameter_id(irap.bytes, hevc_demux.nal_length_bytes, &pps) == HevcError::none &&
              hevc_select_parameters(hevc_demux_hvcc(hevc_demux), pps, &config) == HevcError::none &&
              hevc_parse_picture(irap.bytes, config, &picture) == HevcError::none && picture.irap;
    check(hevc_ok, "movie samples: the HEVC movie's first picture parses");
    if (!hevc_ok)
        return;
    vaapi::Session* hevc = vaapi::session_create(&driver, vaapi::kProfileHevcMain, (config.sps.coded_width + 63u) & ~63u,
                                                 (config.sps.coded_height + 63u) & ~63u, 4);
    if (!hevc) {
        check(false, "movie samples: a session for the HEVC movie");
        return;
    }
    reset_log();
    HevcReferenceState none{};
    HevcReferencePlan hevc_plan{};
    const bool hevc_planned = hevc_reference_plan(none, config.sps, picture, 0, &hevc_plan) == HevcError::none;
    const u32 matrices = config.sps.scaling_enabled ? 1u : 0u;
    check(hevc_planned && vaapi::submit_hevc_sample(hevc, config, picture, irap.bytes, none, hevc_plan, 0) &&
              g_buffers_made == 1 + matrices + 2 * picture.slice_count && g_buffers_alive == 0 &&
              g_buffer_type[0] == vaapi::kPictureParameterBuffer &&
              g_buffer_type[1 + matrices] == vaapi::kSliceParameterBuffer &&
              g_buffer_type[2 + matrices] == vaapi::kSliceDataBuffer,
          "movie samples: an HEVC IRAP sample goes through as one VA picture");
    bool hevc_data = true;
    for (u32 i = 0; i < picture.slice_count && hevc_data; ++i)
        hevc_data = g_buffer_size[2 + matrices + 2 * i] == picture.slices[i].bytes &&
                    g_buffer_data[2 + matrices + 2 * i][0] == irap.bytes.data[picture.slices[i].offset];
    check(hevc_data, "movie samples: every HEVC slice data buffer is its NAL unit");
    vaapi::session_destroy(hevc);
}

// ---- The renderer's dma-buf import -------------------------------------------------------------------------------

// What the import accepts and refuses before it touches a buffer; whether a dma-buf is imported at all depends on the
// EGL driver (llvmpipe has none), so only the refusals that hold everywhere are asserted.
void imports(GpuDevice* device) noexcept {
    const bool available = gpu_caps(device).dmabuf_import;
    test_write_metric("dmabuf import: ", available ? 1u : 0u);
    GpuDmabufPlane plane{7, kDrmFormatR8, 64, 64, 0, 128, kDrmFormatModifierLinear, -1, 0, 0};
    check(gpu_dmabuf_plane_supported(device, plane) == available,
          "imports: a linear plane is importable exactly when the device imports dma-bufs");
    plane.modifier = kDrmFormatModifierInvalid;
    check(gpu_dmabuf_plane_supported(device, plane) == available,
          "imports: so is one whose modifier the exporter left unspecified");
    plane.aux_pitch = 64;
    plane.aux_fd = 8;
    check(!gpu_dmabuf_plane_supported(device, plane), "imports: metadata needs a tiled modifier");
    plane.modifier = kDrmFormatModifierLinear;
    check(!gpu_dmabuf_plane_supported(device, plane), "imports: metadata is not for a linear plane either");
    GpuTexture* texture = nullptr;
    GpuSrv* view = nullptr;
    GpuDmabufPlane bad{-1, kDrmFormatR8, 64, 64, 0, 128, kDrmFormatModifierLinear, -1, 0, 0};
    check(gpu_video_plane_import(device, bad, &texture, &view) == kGpuInvalidArgument && !texture && !view,
          "imports: a plane without a descriptor is refused");
    bad = {7, kDrmFormatR8, 64, 64, 0, 128, kDrmFormatModifierLinear, -1, 0, 64};
    check(gpu_video_plane_import(device, bad, &texture, &view) == kGpuInvalidArgument && !texture && !view,
          "imports: a metadata plane without a descriptor is refused");
    bad = {7, 0x20203859, 64, 64, 0, 128, kDrmFormatModifierLinear, -1, 0, 0}; // 'Y8  '
    check(gpu_video_plane_import(device, bad, &texture, &view) == kGpuInvalidArgument, "imports: another format is refused");
    // A modifier the display does not list (or without the extension): never imported as a linear buffer.
    bad = {7, kDrmFormatR8, 64, 64, 0, 128, 0x00dead0000000001ull, -1, 0, 0};
    check(!gpu_ok(gpu_video_plane_import(device, bad, &texture, &view)) && !texture && !view,
          "imports: an unlisted modifier is refused");
}

// ---- Kept exports against a real dma-buf -------------------------------------------------------------------------

// A dma-buf the test exports again and again, as a driver does (tests/support/dumb_dmabuf.hpp): skipped where there is
// no DRM device this user may use or this EGL does not import its buffers.
struct ExportSource {
    i32 node;
    DumbBuffer buffer[2];
    u32 current; // which one the next export names
    u32 exports;
    bool fail; // the next exports fail
    i32 last_fd;
};
ExportSource g_source;

vaapi::Status fake_export(vaapi::Display, vaapi::SurfaceId, u32, u32, void* descriptor) {
    ++g_source.exports;
    if (g_source.fail)
        return 2;
    const DumbBuffer& buffer = g_source.buffer[g_source.current];
    g_source.last_fd = dumb_export(g_source.node, buffer);
    if (g_source.last_fd < 0)
        return 2;
    auto* d = static_cast<vaapi::DrmPrimeDescriptor*>(descriptor);
    *d = {};
    d->fourcc = vaapi::kFourccNv12;
    d->width = 64;
    d->height = 64;
    d->num_objects = 1;
    d->objects[0] = {g_source.last_fd, static_cast<u32>(buffer.size), kDrmFormatModifierLinear};
    d->num_layers = 2;
    d->layers[0] = {kDrmFormatR8, 1, {0}, {0}, {buffer.pitch}};
    d->layers[1] = {kDrmFormatGr88, 1, {0}, {buffer.pitch * 64}, {buffer.pitch}};
    return 0;
}

bool same_picture(const vaapi::ExportedPicture& a, const vaapi::ExportedPicture& b) noexcept {
    return a.luma_texture == b.luma_texture && a.luma_view == b.luma_view && a.chroma_texture == b.chroma_texture &&
           a.chroma_view == b.chroma_view;
}

void release_picture(vaapi::ExportedPicture* picture) noexcept {
    gpu_release(picture->luma_view);
    gpu_release(picture->luma_texture);
    gpu_release(picture->chroma_view);
    gpu_release(picture->chroma_texture);
    *picture = {};
}

void kept_exports(GpuDevice* device) noexcept {
    using namespace vaapi;
    if (!gpu_caps(device).dmabuf_import) {
        test_write_line("kept exports skipped: the device does not import dma-bufs");
        return;
    }
    clear_bytes(&g_source, sizeof(g_source));
    g_source.node = dumb_open();
    if (g_source.node < 0) {
        test_write_line("kept exports skipped: no DRM device this user may open");
        return;
    }
    // NV12 of 64 x 64: 96 rows of one byte.
    const bool made = dumb_create(g_source.node, 64, 96, 8, &g_source.buffer[0]) &&
                      dumb_create(g_source.node, 64, 96, 8, &g_source.buffer[1]);
    static Driver driver;
    install(&driver);
    driver.api.vaExportSurfaceHandle = fake_export;
    driver.stable_exports = true;
    reset_log();
    Session* s = made ? session_create(&driver, kProfileH264Main, 64, 64, 2) : nullptr;
    ExportedPicture a{}, b{}, c{}, d{}, e{};
    if (!s || !export_picture(device, s, 0, &a)) {
        test_write_line("kept exports skipped: no buffers, or this EGL does not import them");
    } else {
        check(g_source.exports == 1 && sys_fcntl(g_source.last_fd, 1 /* F_GETFD */, 0) < 0 && count_calls(Call::sync) == 1,
              "kept: a picture waits for its surface and its descriptor is closed after the import");
        check(export_picture(device, s, 0, &b) && g_source.exports == 2 && count_calls(Call::sync) == 2 && same_picture(a, b),
              "kept: the next export of a surface that names the same buffer is the same images, exported and waited for again");
        check(export_picture(device, s, 1, &c) && !same_picture(a, c), "kept: another surface has images of its own");
        // The surface moves to another buffer: new images, and the old ones go.
        g_source.current = 1;
        check(export_picture(device, s, 0, &d) && !same_picture(a, d) && sys_fcntl(g_source.last_fd, 1, 0) < 0,
              "kept: a surface exported as another buffer is imported again");
        check(export_picture(device, s, 0, &e) && same_picture(d, e), "kept: and the new images are the ones kept");
        // A driver whose exports are not stable is imported every time.
        release_picture(&e);
        driver.stable_exports = false;
        check(export_picture(device, s, 1, &e) && !same_picture(c, e), "kept: a driver without stable exports is imported again");
        // A decoder slot takes the export's planes, and keeps its own when the export fails.
        StreamSurface slot{};
        check(stream_slot_export(device, s, 0, &slot) && slot.luma_texture && slot.chroma_view &&
                  gpu_video_plane_owns(slot.luma_texture),
              "kept: a slot takes the planes of an export");
        const GpuTexture* held = slot.luma_texture;
        g_source.fail = true;
        check(!stream_slot_export(device, s, 0, &slot) && slot.luma_texture == held,
              "kept: a failed export leaves the slot's planes in place");
        g_source.fail = false;
        stream_slot_release(&slot);
    }
    release_picture(&a);
    release_picture(&b);
    release_picture(&c);
    release_picture(&d);
    release_picture(&e);
    session_destroy(s);
    dumb_destroy(g_source.node, &g_source.buffer[0]);
    dumb_destroy(g_source.node, &g_source.buffer[1]);
    sys_close(g_source.node);
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
    waits();
    surface_requests();
    movie_samples();
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (gpu_ok(gpu_device_create(&device, &context))) {
        imports(device);
        kept_exports(device);
        if (software_codec_available(GpuStreamCodec::h264)) {
            tiers(device, h264);
            fallback(device, h264);
        } else {
            test_write_line("tiers and fallback skipped: libopenh264 is not installed");
        }
        gpu_device_destroy(&device, &context);
    } else {
        test_write_line("device tests skipped: no EGL device");
    }
    test_exit(g_failures, "maicord_video_hardware_tier");
}
