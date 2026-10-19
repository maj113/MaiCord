// Live H.265 stream decoder on real DXVA hardware (CTest label "gpu"; not run
// on hosted CI). Synthetic PCM/skip streams make every decoded pixel
// predictable, so frames are verified by readback without a reference decoder.
#include "maicord/platform/gpu_stream_decoder.hpp"
#include "maicord/app/video_stream_decoders.hpp"
#include "maicord/protocol/voice_video_signaling.hpp"
#include "hevc_stream_fixture.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dxva.hpp"

namespace maicord {
namespace {
using namespace hevc_fixture;

void say(const char* text) noexcept {
    u32 count = 0;
    while (text[count])
        ++count;
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, count, &written, nullptr);
}

void say_number(const char* label, u64 value) noexcept {
    say(label);
    char digits[24]{};
    u32 n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value && n < 20);
    char text[24]{};
    u32 count = 0;
    while (n)
        text[count++] = digits[--n];
    text[count++] = '\n';
    say(text);
}

struct Capture {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11Texture2D* staging;
    u32 staging_width, staging_height;
    Sps sps;
    u32 pattern;
    u32 frames, keyframes, mismatches, requests, last_rtp;
    u32 subresources; // Slices pictures were presented from.
    bool zero_copy;   // Every picture was a slice of the decoder's own array.
    ID3D11Texture2D* texture;
    u32 texture_width, texture_height;
    bool skip_readback; // Timing: no CPU wait inside the frame callback.
};

// Reads slice `subresource` and checks one sample per CTU against the PCM pattern.
bool matches(Capture* capture, const GpuVideoFrame& gpu, const Sps& sps, u32 pattern) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    gpu.texture->GetDesc(&desc);
    if (!capture->staging || capture->staging_width != desc.Width ||
        capture->staging_height != desc.Height) {
        com_release(capture->staging);
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.ArraySize = 1;
        staging.BindFlags = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(capture->device->CreateTexture2D(&staging, nullptr, &capture->staging)))
            return false;
        capture->staging_width = desc.Width;
        capture->staging_height = desc.Height;
    }
    capture->context->CopySubresourceRegion(capture->staging, 0, 0, 0, 0, gpu.texture, gpu.subresource,
                                            nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(capture->context->Map(capture->staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const auto* luma = static_cast<const u8*>(mapped.pData);
    const auto* chroma = luma + u64{mapped.RowPitch} * desc.Height;
    const u32 size = ctb_size(sps), columns = ctb_columns(sps);
    bool ok =
        gpu.width == sps.width && gpu.height == sps.height - sps.crop_bottom && !gpu.crop_x && !gpu.crop_y;
    for (u32 ctu = 0; ctu < ctb_count(sps) && ok; ++ctu) {
        const u32 x = ctu % columns * size + size / 2u, y = ctu / columns * size + size / 2u;
        if (y >= gpu.height)
            continue;
        ok = luma[u64{y} * mapped.RowPitch + x] == sample(sps, luma_level(sps, ctu, pattern)) &&
             chroma[u64{y / 2u} * mapped.RowPitch + x / 2u * 2u] ==
                 sample(sps, chroma_level(sps, pattern, 0)) &&
             chroma[u64{y / 2u} * mapped.RowPitch + x / 2u * 2u + 1u] ==
                 sample(sps, chroma_level(sps, pattern, 1));
    }
    capture->context->Unmap(capture->staging, 0);
    return ok;
}

void on_frame(void* context, u32 ssrc, const GpuStreamFrame& frame) noexcept {
    auto* capture = static_cast<Capture*>(context);
    const GpuVideoFrame& gpu = *frame.gpu;
    D3D11_TEXTURE2D_DESC desc{};
    gpu.texture->GetDesc(&desc);
    // Zero copy: the decoder's shader-readable array with this slice's views.
    capture->zero_copy &= (desc.BindFlags & (D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE)) ==
                              (D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE) &&
                          desc.ArraySize > 1 && gpu.subresource < desc.ArraySize && gpu.luma && gpu.chroma;
    capture->subresources |= 1u << gpu.subresource;
    capture->texture = gpu.texture;
    capture->texture_width = desc.Width;
    capture->texture_height = desc.Height;
    if (ssrc != 0x2650)
        ++capture->mismatches;
    ++capture->frames;
    capture->keyframes += frame.keyframe ? 1u : 0u;
    capture->last_rtp = frame.rtp_timestamp;
    if (!capture->skip_readback && !matches(capture, gpu, capture->sps, capture->pattern))
        ++capture->mismatches;
}
void on_keyframe(void* context, u32) noexcept {
    ++static_cast<Capture*>(context)->requests;
}

struct Stream {
    Bits* bits;
    Output* out;
    GpuStreamDecoder* decoder;
    u32 push(u32 rtp, bool intact = true) noexcept {
        const u32 bytes = out->count;
        const bool ok = out->ok && gpu_stream_decoder_push(decoder, {out->data, out->count}, rtp, intact);
        out->count = 0;
        out->ok = true;
        return ok ? bytes : 0;
    }
};

u64 now_ticks() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<u64>(value.QuadPart);
}

// Decode CPU time and submit-to-completion GPU time of one picture.
void measure(Capture* capture, Stream* s, u32 rtp, const char* label) noexcept {
    ID3D11Device* device = capture->device;
    ID3D11DeviceContext* context = capture->context;
    ID3D11Query* query = nullptr;
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    if (FAILED(device->CreateQuery(&desc, &query)))
        return;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    u64 cycles_before = 0, cycles_after = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles_before);
    capture->skip_readback = true;
    const u64 start = now_ticks();
    s->push(rtp);
    capture->skip_readback = false;
    QueryThreadCycleTime(GetCurrentThread(), &cycles_after);
    const u64 submitted = now_ticks();
    context->End(query);
    BOOL done = FALSE;
    while (context->GetData(query, &done, sizeof(done), 0) != S_OK)
        SwitchToThread();
    const u64 complete = now_ticks();
    query->Release();
    const u64 hz = static_cast<u64>(frequency.QuadPart ? frequency.QuadPart : 1);
    say(label);
    say_number(" cpu_us ", (submitted - start) * 1'000'000ull / hz);
    say(label);
    say_number(" cpu_cycles ", cycles_after - cycles_before);
    say(label);
    say_number(" gpu_wait_us ", (complete - submitted) * 1'000'000ull / hz);
}

int small_stream(ID3D11Device* device, ID3D11DeviceContext* context, Bits* bits, Output* out) noexcept {
    Capture capture{device, context};
    capture.zero_copy = true;
    GpuStreamDecoder* decoder = gpu_stream_decoder_create(
        {device, 0x2650, {&capture, on_frame, on_keyframe}, GpuStreamCodec::h265, true});
    if (!decoder)
        return 50;
    Stream s{bits, out, decoder};
    int status = 0;
    const auto fail = [&](int code) noexcept {
        if (!status)
            status = code;
    };
    // 128x96, CTB 16, 8-bit PCM: IDR with one slice segment per CTU, then P.
    const Sps base{0, 128, 96, 0, 4, 8, 2};
    const Pps pps{0, 0};
    capture.sps = base;
    capture.pattern = 1;
    write_picture(out, bits, base, pps, kTrailR, 1, 0); // Before any parameter set: damaged.
    s.push(10);
    write_idr(out, bits, base, pps, 1, 1);
    s.push(100);
    auto stats = gpu_stream_decoder_stats(decoder);
    if (stats.unsupported || !stats.reinitializations) {
        say("hevc stream gpu: DXVA HEVC short-slice decoding unavailable; skipped\n");
        gpu_stream_decoder_destroy(decoder);
        com_release(capture.staging);
        return 0;
    }
    u64 uploaded = stats.uploaded_bytes;
    u32 p_bytes = 0;
    for (u16 poc = 1; poc <= 3; ++poc) {
        write_picture(out, bits, base, pps, kTrailR, poc, 0);
        p_bytes = s.push(100u + poc);
    }
    stats = gpu_stream_decoder_stats(decoder);
    // A P picture uploads its parameters, the 128-byte padded bitstream (3-byte
    // start code) and one slice entry; nothing else crosses to the GPU.
    const u64 p_upload =
        sizeof(DXVA_PicParams_HEVC) + ((p_bytes - 4u + 3u + 128u) & ~127u) + sizeof(DXVA_Slice_HEVC_Short);
    if (capture.frames != 4 || capture.keyframes != 1 || capture.mismatches || capture.requests != 1 ||
        capture.last_rtp != 103 || stats.decoded != 4 || !capture.zero_copy ||
        stats.uploaded_bytes - uploaded != 3 * p_upload ||
        stats.surfaces < u32{base.max_buffering} + kVideoStreamPresentationSurfaces ||
        stats.surfaces > 32 || (capture.subresources & (capture.subresources - 1u)) == 0) {
        say_number("hevc frames ", capture.frames);
        say_number("hevc mismatches ", capture.mismatches);
        say_number("hevc requests ", capture.requests);
        say_number("hevc decoded ", stats.decoded);
        say_number("hevc damaged ", stats.damaged);
        say_number("hevc gpu_failures ", stats.gpu_failures);
        say_number("hevc last_status ", static_cast<u32>(stats.last_status));
        say_number("hevc uploaded ", stats.uploaded_bytes - uploaded);
        say_number("hevc expected ", 3 * p_upload);
        fail(51);
    }
    say_number("hevc stream surfaces ", stats.surfaces);
    say_number("hevc stream p_upload_bytes ", p_upload);
    // A lost reference (POC 4 missing): frozen output, one immediate request,
    // then throttled while waiting.
    if (!status) {
        write_picture(out, bits, base, pps, kTrailR, 5, 0);
        s.push(105);
        write_picture(out, bits, base, pps, kTrailR, 6, 0);
        s.push(106);
        stats = gpu_stream_decoder_stats(decoder);
        if (capture.frames != 4 || stats.gaps != 1 || capture.requests != 2)
            fail(52);
    }
    // Recovery and a resolution change at the next IDR (a resized window):
    // new surfaces, new texture, same zero-copy output.
    if (!status) {
        Sps resized = base;
        resized.width = 192;
        resized.height = 112;
        resized.max_buffering = 3;
        capture.sps = resized;
        capture.pattern = 2;
        write_idr(out, bits, resized, pps, 2);
        s.push(200);
        write_picture(out, bits, resized, pps, kTrailR, 1, 0);
        s.push(201);
        stats = gpu_stream_decoder_stats(decoder);
        // Surfaces are allocated in 64-pixel units.
        if (capture.frames != 6 || capture.mismatches || stats.reinitializations != 2 ||
            stats.coded_width != 192 || capture.texture_width != 192 || capture.texture_height != 128 ||
            !capture.zero_copy) {
            say_number("hevc resize frames ", capture.frames);
            say_number("hevc resize mismatches ", capture.mismatches);
            say_number("hevc resize reinitializations ", stats.reinitializations);
            say_number("hevc resize texture ", capture.texture_width * 10000u + capture.texture_height);
            say_number("hevc resize damaged ", stats.damaged);
            say_number("hevc resize last_status ", static_cast<u32>(stats.last_status));
            fail(53);
        }
    }
    // An incomplete access unit is never submitted and asks for a keyframe.
    if (!status) {
        write_picture(out, bits, capture.sps, pps, kTrailR, 2, 0);
        s.push(202, false);
        stats = gpu_stream_decoder_stats(decoder);
        if (capture.frames != 6 || stats.damaged != 2 || stats.gpu_failures)
            fail(54);
    }
    // A held picture's slice survives later pictures until the hold moves.
    if (!status) {
        capture.pattern = 3;
        capture.subresources = 0;
        write_idr(out, bits, capture.sps, pps, 3);
        s.push(300);
        u32 slice = 0;
        while (slice < 31 && !(capture.subresources & (1u << slice)))
            ++slice;
        gpu_stream_decoder_hold(decoder, slice);
        capture.subresources = 0;
        stats = gpu_stream_decoder_stats(decoder);
        for (u32 pattern = 4; pattern < 4 + 2 * stats.surfaces; ++pattern) {
            capture.pattern = pattern;
            write_idr(out, bits, capture.sps, pps, pattern);
            s.push(400 + pattern);
        }
        const GpuVideoFrame kept{capture.texture, slice, 0, 0, capture.sps.width, capture.sps.height};
        if ((capture.subresources & (1u << slice)) || !matches(&capture, kept, capture.sps, 3) ||
            capture.mismatches)
            fail(55);
    }
    // Idle release frees the decoder and surfaces; the next IDR recreates them.
    if (!status) {
        const auto before = gpu_stream_decoder_stats(decoder);
        if (!gpu_stream_decoder_release_idle(decoder) || gpu_stream_decoder_stats(decoder).surfaces ||
            gpu_stream_decoder_release_idle(decoder))
            fail(56);
        capture.pattern = 9;
        write_idr(out, bits, capture.sps, pps, 9);
        s.push(900);
        stats = gpu_stream_decoder_stats(decoder);
        if (stats.reinitializations != before.reinitializations + 1 || !stats.surfaces ||
            capture.mismatches)
            fail(57);
        write_picture(out, bits, capture.sps, pps, kTrailR, 1, 0);
        measure(&capture, &s, 901, "hevc small_p");
    }
    // Main10 is refused once and never asked for again.
    if (!status) {
        Sps ten = capture.sps;
        ten.bit_depth = 10;
        const u32 frames = capture.frames;
        write_idr(out, bits, ten, pps, 10);
        s.push(1000);
        const u32 requests = capture.requests;
        write_idr(out, bits, ten, pps, 10);
        s.push(1001);
        stats = gpu_stream_decoder_stats(decoder);
        if (!stats.unsupported || capture.frames != frames || capture.requests != requests)
            fail(58);
    }
    gpu_stream_decoder_destroy(decoder);
    com_release(capture.staging);
    return status;
}

// 1080p (cropped), 1440p and 4K keyframes, one slice per CTU row, 1-bit PCM.
int large_streams(ID3D11Device* device, ID3D11DeviceContext* context, Bits* bits, Output* out) noexcept {
    Capture capture{device, context};
    capture.zero_copy = true;
    GpuStreamDecoder* decoder = gpu_stream_decoder_create(
        {device, 0x2650, {&capture, on_frame, on_keyframe}, GpuStreamCodec::h265, true});
    if (!decoder)
        return 60;
    Stream s{bits, out, decoder};
    const Sps large[] = {{1, 1920, 1088, 8, 5, 1, 2, 8, 123},
                         {2, 2560, 1440, 0, 5, 1, 2, 8, 150},
                         {3, 3840, 2176, 16, 5, 1, 2, 8, 153}};
    const char* labels[] = {"hevc 1080p", "hevc 1440p", "hevc 4k"};
    int status = 0;
    for (u32 i = 0; i < 3 && !status; ++i) {
        const Pps pps{static_cast<u8>(10 + i), large[i].id};
        capture.sps = large[i];
        capture.pattern = 4 + i;
        const u32 before = capture.frames;
        write_idr(out, bits, large[i], pps, capture.pattern, ctb_columns(large[i]));
        const u32 idr_bytes = s.push(400 + i);
        write_picture(out, bits, large[i], pps, kTrailR, 1, 0);
        s.push(500 + i);
        const auto stats = gpu_stream_decoder_stats(decoder);
        if (i == 2 && stats.unsupported) {
            say("hevc stream gpu: this GPU does not decode 4K HEVC; 4K case skipped\n");
            break;
        }
        if (capture.frames != before + 2 || capture.mismatches || stats.unsupported || stats.gpu_failures ||
            !capture.zero_copy) {
            say_number("hevc large frames ", capture.frames - before);
            say_number("hevc large mismatches ", capture.mismatches);
            say_number("hevc large unsupported ", stats.unsupported);
            say_number("hevc large gpu_failures ", stats.gpu_failures);
            say_number("hevc large last_status ", static_cast<u32>(stats.last_status));
            say_number("hevc large damaged ", stats.damaged);
            status = 61 + static_cast<int>(i);
            break;
        }
        say(labels[i]);
        say_number(" idr_bytes ", idr_bytes);
        say(labels[i]);
        say_number(" surfaces ", stats.surfaces);
        say(labels[i]);
        say_number(" surface_bytes ", u64{stats.surfaces} * ((large[i].width + 63u) & ~63u) *
                                          ((large[i].height + 63u) & ~63u) * 3u / 2u);
        write_picture(out, bits, large[i], pps, kTrailR, 2, 0);
        measure(&capture, &s, 600 + i, labels[i]);
    }
    gpu_stream_decoder_destroy(decoder);
    com_release(capture.staging);
    return status;
}

void release_frame(VideoStreamFrame* frame) noexcept {
    com_release(frame->gpu.chroma);
    com_release(frame->gpu.luma);
    com_release(frame->gpu.texture);
}

bool wait_frame(VideoStreamDecoders* decoders, u32 sequence, VideoStreamFrame* frame) noexcept {
    const u64 deadline = GetTickCount64() + 2'500;
    do {
        if (video_stream_decoders_frame(decoders, 0x2650, frame)) {
            if (frame->sequence >= sequence)
                return true;
            release_frame(frame);
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return false;
}

// The call decoders advertise H.265 and decode it on their thread; a stream
// switching codec at a keyframe gets a new decoder.
int call_decoders(ID3D11Device* device, ID3D11DeviceContext* context, Bits* bits, Output* out,
                  h264_fixture::Bits h264_bits, h264_fixture::Output h264_out) noexcept {
    VideoStreamDecoders decoders{};
    video_stream_decoders_initialize(&decoders);
    video_stream_decoders_set_device(&decoders, device);
    const VideoFrameSink sink = video_stream_decoders_sink(&decoders);
    int status = 0;
    if (!sink.decodable || !(sink.decodable(sink.context) & voice_video_codec_bit(VoiceVideoCodec::h265)))
        status = 70;
    const Sps sps{0, 64, 48, 0, 4, 8, 2};
    const Pps pps{0, 0};
    VideoStreamFrame first{}, second{}, switched{};
    Capture capture{device, context};
    if (!status) {
        write_idr(out, bits, sps, pps, 6);
        const bool pushed = sink.push_frame(sink.context, 0x2650, VoiceVideoCodec::h265,
                                            {out->data, out->count}, 10, true, true);
        out->count = 0;
        if (!pushed || !wait_frame(&decoders, 1, &first) || !matches(&capture, first.gpu, sps, 6))
            status = 71;
    }
    if (!status) {
        write_picture(out, bits, sps, pps, kTrailR, 1, 0);
        sink.push_frame(sink.context, 0x2650, VoiceVideoCodec::h265, {out->data, out->count}, 11, false,
                        true);
        out->count = 0;
        // first is held: the P picture lands in another slice of the same array.
        if (!wait_frame(&decoders, first.sequence + 1, &second) ||
            second.gpu.texture != first.gpu.texture || second.gpu.subresource == first.gpu.subresource ||
            !matches(&capture, second.gpu, sps, 6))
            status = 72;
    }
    if (!status) {
        const h264_fixture::Sps h264{0, 66, 4, 3, 2, 1, false, 0, 0, 31};
        h264_fixture::write_idr(&h264_out, &h264_bits, h264, {0, 0}, 1, 0);
        sink.push_frame(sink.context, 0x2650, VoiceVideoCodec::h264, {h264_out.data, h264_out.count}, 12,
                        true, true);
        if (!wait_frame(&decoders, 1, &switched) || switched.gpu.texture == first.gpu.texture ||
            switched.gpu.width != 64)
            status = 73;
    }
    release_frame(&first);
    release_frame(&second);
    release_frame(&switched);
    video_stream_decoders_release(&decoders);
    com_release(capture.staging);
    return status;
}

} // namespace

int hevc_stream_gpu_test(ID3D11Device* device, ID3D11DeviceContext* context, h264_fixture::Bits bits,
                         h264_fixture::Output out) noexcept {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const u64 probe_start = now_ticks();
    const u32 codecs = gpu_stream_decoder_codecs(device);
    say_number("hevc capability probe us ",
               (now_ticks() - probe_start) * 1'000'000ull / static_cast<u64>(frequency.QuadPart ? frequency.QuadPart : 1));
    if (!(codecs & gpu_stream_codec_bit(GpuStreamCodec::h265))) {
        say("hevc stream gpu: no hardware HEVC Main decode with shader-readable output; skipped\n");
        return 0;
    }
    // The H.264 switch case uses the upper half of the shared buffers.
    const u32 half = out.capacity / 2u;
    h264_fixture::Bits h264_bits{bits.data + bits.capacity / 2u, bits.capacity / 2u, 0, true};
    h264_fixture::Output h264_out{out.data + half, half, 0, true};
    int status = small_stream(device, context, &bits, &out);
    if (!status)
        status = large_streams(device, context, &bits, &out);
    bits.capacity /= 2u;
    out.capacity = half;
    if (!status)
        status = call_decoders(device, context, &bits, &out, h264_bits, h264_out);
    return status;
}

} // namespace maicord
