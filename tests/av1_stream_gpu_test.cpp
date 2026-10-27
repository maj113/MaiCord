// Live AV1 stream decoder on real DXVA hardware (part of MaiCordH264StreamGpuTests,
// label "gpu"): the lossless SVT-AV1 fixture decodes to its exact block colours
// in the decoder's own shader-readable slices (zero copy) and converts from
// them; loss, the presentation hold, idle release and 10-bit refusal follow
// the stream decoder contract. Skipped on GPUs without AV1 decode.
#include "maicord/platform/gpu_stream_decoder.hpp"
#include "maicord/app/video_stream_decoders.hpp"
#include "maicord/protocol/voice_video_signaling.hpp"
#include "maicord/render/video_frame_gpu.hpp"
#include "maicord/media/yuv_convert_common.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dxva.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
#include "data/av1_stream_vectors.hpp"

struct Capture {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11Texture2D* staging; // NV12 readback of one slice.
    ID3D11Texture2D* target;  // RGBA conversion output.
    ID3D11UnorderedAccessView* target_view;
    ID3D11Texture2D* target_staging;
    VideoFrameGpu converter;
    u32 frames, mismatches, foreign_textures, keyframes, requests, held_hits;
    u32 held; // Slice a reader holds, 255 when none.
    u32 last_subresource;
};

void block_values(u32 frame, u32 block, u32* luma, u32* cb, u32* cr) noexcept {
    *luma = (40 + 23 * block + 11 * frame) % 220 + 16;
    *cb = (60 + 37 * block + 5 * frame) % 200 + 28;
    *cr = (90 + 53 * block + 7 * frame) % 200 + 28;
}

bool verify_planes(Capture* c, const GpuVideoFrame& frame, u32 index) noexcept {
    c->context->CopySubresourceRegion(c->staging, 0, 0, 0, 0, frame.texture, frame.subresource, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(c->context->Map(c->staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    c->staging->GetDesc(&desc);
    const auto* luma = static_cast<const u8*>(mapped.pData);
    const auto* chroma = luma + u64{mapped.RowPitch} * desc.Height;
    bool ok = true;
    for (u32 block = 0; block < (kAv1FixtureWidth / 32) * (kAv1FixtureHeight / 32) && ok; ++block) {
        const u32 bx = block % (kAv1FixtureWidth / 32), by = block / (kAv1FixtureWidth / 32);
        u32 y = 0, cb = 0, cr = 0;
        block_values(index, block, &y, &cb, &cr);
        // Every pixel of the block's centre 16x16 and its chroma.
        for (u32 row = 8; row < 24 && ok; ++row)
            for (u32 column = 8; column < 24 && ok; ++column)
                ok = luma[u64{by * 32 + row} * mapped.RowPitch + bx * 32 + column] == y;
        const u8* pair = chroma + u64{by * 16 + 8} * mapped.RowPitch + (bx * 16 + 8) * 2;
        ok = ok && pair[0] == cb && pair[1] == cr;
    }
    c->context->Unmap(c->staging, 0);
    return ok;
}

bool verify_converted(Capture* c, const GpuVideoFrame& frame, u32 index) noexcept {
    auto* context1 = static_cast<ID3D11DeviceContext1*>(nullptr);
    if (FAILED(c->context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                          reinterpret_cast<void**>(&context1))))
        return false;
    const HRESULT status =
        video_frame_gpu_present(&c->converter, c->device, context1, frame, c->target_view, 0, 0);
    context1->Release();
    if (FAILED(status))
        return false;
    c->context->CopyResource(c->target_staging, c->target);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(c->context->Map(c->target_staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const auto coefficients = yuv_rgb_coefficients(frame.bt709, frame.full_range);
    bool ok = true;
    for (u32 block = 0; block < (kAv1FixtureWidth / 32) * (kAv1FixtureHeight / 32) && ok; ++block) {
        u32 y = 0, cb = 0, cr = 0;
        block_values(index, block, &y, &cb, &cr);
        u8 expected[4];
        yuv_rgb_pixel(expected, static_cast<i32>(y), static_cast<i32>(cb) - 128, static_cast<i32>(cr) - 128,
                      coefficients);
        const u8* actual = static_cast<const u8*>(mapped.pData) +
                           u64{block / (kAv1FixtureWidth / 32) * 32 + 16} * mapped.RowPitch +
                           (block % (kAv1FixtureWidth / 32) * 32 + 16) * 4;
        ok = actual[0] == expected[0] && actual[1] == expected[1] && actual[2] == expected[2] &&
             actual[3] == 255;
    }
    c->context->Unmap(c->target_staging, 0);
    return ok;
}

void on_frame(void* context, u32 ssrc, const GpuStreamFrame& frame) noexcept {
    auto* c = static_cast<Capture*>(context);
    const GpuVideoFrame& gpu = *frame.gpu;
    D3D11_TEXTURE2D_DESC desc{};
    gpu.texture->GetDesc(&desc);
    // Zero copy: the picture is a slice of the decoder's output array itself.
    if (ssrc != 0xA1 || !(desc.BindFlags & D3D11_BIND_DECODER) ||
        !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || gpu.subresource >= desc.ArraySize || !gpu.luma ||
        !gpu.chroma || gpu.width != kAv1FixtureWidth || gpu.height != kAv1FixtureHeight || gpu.crop_x ||
        gpu.crop_y)
        ++c->foreign_textures;
    if (gpu.subresource == c->held)
        ++c->held_hits;
    const u32 index = c->frames % kAv1FixtureFrames;
    if (!verify_planes(c, gpu, index) || !verify_converted(c, gpu, index))
        ++c->mismatches;
    c->keyframes += frame.keyframe;
    c->last_subresource = gpu.subresource;
    ++c->frames;
}
// Counts pictures without checking pixels (lossy fixture).
void on_counted(void* context, u32, const GpuStreamFrame&) noexcept {
    ++static_cast<Capture*>(context)->frames;
}
void on_keyframe(void* context, u32) noexcept {
    ++static_cast<Capture*>(context)->requests;
}

u8 units[kAv1FixtureFrames][2048];
u32 unit_bytes[kAv1FixtureFrames];

bool push_all(GpuStreamDecoder* decoder, u32 rtp) noexcept {
    bool ok = true;
    for (u32 i = 0; i < kAv1FixtureFrames; ++i)
        ok = gpu_stream_decoder_push(decoder, {units[i], unit_bytes[i]}, rtp + i * 3000) && ok;
    return ok;
}

bool create_capture(Capture* c) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 256; // Surfaces are superblock aligned: 256x64 already is.
    desc.Height = 64;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(c->device->CreateTexture2D(&desc, nullptr, &c->staging)))
        return false;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(c->device->CreateTexture2D(&desc, nullptr, &c->target_staging)))
        return false;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    return SUCCEEDED(c->device->CreateTexture2D(&desc, nullptr, &c->target)) &&
           SUCCEEDED(c->device->CreateUnorderedAccessView(c->target, nullptr, &c->target_view));
}

void release_capture(Capture* c) noexcept {
    video_frame_gpu_release(&c->converter);
    com_release(c->target_view);
    com_release(c->target);
    com_release(c->target_staging);
    com_release(c->staging);
}

int run(ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    for (u32 i = 0; i < kAv1FixtureFrames; ++i)
        unit_bytes[i] = test_unhex(av1_fixture_units[i], units[i]);
    static Capture capture;
    capture = {device, context};
    capture.held = 255;
    if (!create_capture(&capture))
        return 1;
    GpuStreamDecoder* decoder = gpu_stream_decoder_create(
        {device, 0xA1, {&capture, on_frame, on_keyframe}, GpuStreamCodec::av1, true});
    if (!decoder)
        return 2;
    int status = 0;
    const auto finish = [&](int code) noexcept {
        if (!status)
            status = code;
    };
    // Six temporal units: four decoded shown frames, two hidden, two shown existing.
    if (!push_all(decoder, 1000))
        finish(3);
    auto stats = gpu_stream_decoder_stats(decoder);
    if (capture.frames != 6 || capture.mismatches || capture.foreign_textures || capture.keyframes != 1)
        finish(4);
    if (stats.decoded != 6 || stats.presented != 6 || stats.keyframe_requests || stats.damaged ||
        stats.dropped || stats.gpu_failures || stats.unsupported)
        finish(5);
    // Pool: three distinct references, the picture being decoded, presented and held.
    if (stats.surfaces != 6 || stats.coded_width != kAv1FixtureWidth ||
        stats.coded_height != kAv1FixtureHeight)
        finish(6);
    // Uploads are the picture parameters, one tile entry per tile and the
    // 128-byte-padded tile data; nothing else crosses to the GPU.
    const u64 tile_bytes =
        stats.uploaded_bytes - 6 * (sizeof(DXVA_PicParams_AV1) + 2 * sizeof(DXVA_Tile_AV1));
    if (tile_bytes % 128 || tile_bytes > 6 * 128 + 3669)
        finish(7);

    // A reader holds the newest slice: the next pass never decodes into it.
    capture.held = capture.last_subresource;
    gpu_stream_decoder_hold(decoder, capture.held);
    if (!push_all(decoder, 2000))
        finish(8);
    stats = gpu_stream_decoder_stats(decoder);
    if (capture.frames != 12 || capture.held_hits || capture.mismatches || stats.reinitializations != 1)
        finish(9);
    capture.held = 255;

    // Loss: a damaged inter unit freezes the stream until the next keyframe.
    const u32 frames = capture.frames;
    gpu_stream_decoder_push(decoder, {units[3], unit_bytes[3]}, 3009, false);
    gpu_stream_decoder_push(decoder, {units[5], unit_bytes[5]}, 3015);
    stats = gpu_stream_decoder_stats(decoder);
    if (capture.frames != frames || capture.requests != 1 || stats.damaged != 1 || stats.dropped != 1)
        finish(10);
    capture.frames = 0; // The keyframe restarts the picture sequence.
    if (!push_all(decoder, 4000) || capture.frames != 6 || capture.mismatches)
        finish(11);

    // Idle release frees the decoder and surfaces; the next keyframe recreates them.
    if (!gpu_stream_decoder_release_idle(decoder))
        finish(12);
    stats = gpu_stream_decoder_stats(decoder);
    if (stats.surfaces || stats.idle_releases != 1)
        finish(13);
    gpu_stream_decoder_push(decoder, {units[1], unit_bytes[1]}, 5003);
    capture.frames = 0;
    if (!push_all(decoder, 6000) || capture.frames != 6 || capture.mismatches ||
        gpu_stream_decoder_stats(decoder).surfaces != 6 || capture.requests != 2)
        finish(14);
    gpu_stream_decoder_destroy(decoder);

    // A five-level hierarchical GOP keeps more distinct references than the
    // initial pool: the stream freezes, asks for a keyframe and grows to all
    // eight references plus the working surfaces at it.
    decoder = gpu_stream_decoder_create(
        {device, 0xA2, {&capture, on_counted, on_keyframe}, GpuStreamCodec::av1, true});
    constexpr u32 deep_units = sizeof(av1_fixture_deep_units) / sizeof(av1_fixture_deep_units[0]);
    static u8 deep[deep_units][512];
    static u32 deep_bytes[deep_units];
    for (u32 i = 0; i < deep_units; ++i)
        deep_bytes[i] = test_unhex(av1_fixture_deep_units[i], deep[i]);
    capture.frames = capture.requests = 0;
    for (u32 i = 0; decoder && i < deep_units; ++i)
        gpu_stream_decoder_push(decoder, {deep[i], deep_bytes[i]}, i * 3000);
    stats = gpu_stream_decoder_stats(decoder);
    const u32 frozen = capture.frames;
    if (!decoder || frozen >= deep_units || capture.requests != 1 || stats.surfaces != 6)
        finish(15);
    for (u32 i = 0; decoder && i < deep_units; ++i)
        gpu_stream_decoder_push(decoder, {deep[i], deep_bytes[i]}, 100000 + i * 3000);
    stats = gpu_stream_decoder_stats(decoder);
    if (capture.frames - frozen != deep_units || stats.surfaces != 11 || capture.requests != 1)
        finish(16);
    gpu_stream_decoder_destroy(decoder);

    // 10-bit needs P010 output, which is not negotiated: refused without keyframe requests.
    capture.frames = 0;
    capture.requests = 0;
    decoder = gpu_stream_decoder_create(
        {device, 0xA1, {&capture, on_frame, on_keyframe}, GpuStreamCodec::av1, true});
    static u8 ten_bit[1024];
    const u32 ten_bit_bytes = test_unhex(av1_fixture_ten_bit, ten_bit);
    if (!decoder || !gpu_stream_decoder_push(decoder, {ten_bit, ten_bit_bytes}, 1))
        finish(17);
    stats = gpu_stream_decoder_stats(decoder);
    if (capture.frames || capture.requests || !stats.unsupported || stats.surfaces)
        finish(18);
    gpu_stream_decoder_destroy(decoder);
    release_capture(&capture);
    return status;
}
} // namespace

namespace {
// The capability that Select Protocol advertises: the decoders report AV1
// only for a device that decodes it into shader-readable NV12; none without one.
int capability(ID3D11Device* device, bool av1) noexcept {
    static VideoStreamDecoders decoders;
    video_stream_decoders_initialize(&decoders);
    const VideoFrameSink sink = video_stream_decoders_sink(&decoders);
    video_stream_decoders_set_device(&decoders, device);
    const bool reported = (sink.decodable(sink.context) & voice_video_codec_bit(VoiceVideoCodec::av1)) != 0;
    video_stream_decoders_set_device(&decoders, nullptr);
    const bool cleared = !sink.decodable(sink.context);
    video_stream_decoders_release(&decoders);
    ID3D11Device* warp = nullptr;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const bool software = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                                      D3D11_SDK_VERSION, &warp, nullptr, nullptr)) &&
                          !gpu_stream_decoder_codecs(warp);
    com_release(warp);
    return reported == av1 && cleared && software ? 0 : 20;
}
} // namespace

int av1_stream_gpu_test(ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    const bool av1 = (gpu_stream_decoder_codecs(device) & gpu_stream_codec_bit(GpuStreamCodec::av1)) != 0;
    int status = capability(device, av1);
    if (!av1 && !status) {
        test_write_line("av1 stream gpu: no AV1 hardware decoder; skipped");
        return 0;
    }
    if (!status)
        status = run(device, context);
    if (status) {
        test_write_text("av1 stream gpu failed: ");
        test_write_u64(static_cast<u64>(status));
        test_write_line("");
    }
    return status;
}
} // namespace maicord
