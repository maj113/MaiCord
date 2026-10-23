// Attachment H.264 decoder on real DXVA hardware: an in-memory MP4 around the
// synthetic I_PCM/P_Skip fixture, converted by video_frame_gpu in place from
// the decoder array's plane views. A device without shader-readable decoder
// output (WARP) refuses decoding like a GPU without the codec.
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/render/video_frame_gpu.hpp"
#include "maicord/media/yuv_convert_common.hpp"
#include "../src/platform/gpu_decoder_surfaces.hpp"
#include "h264_stream_fixture.hpp"
#include "maicord/sys/d3d11.hpp"
#include "test_output.hpp"

namespace maicord {
namespace {
using namespace h264_fixture;

struct Buffers {
    Bits bits;
    Output out;
};


struct Movie {
    u8* bytes;
    u32 count, capacity;
    u32 sizes[16];
    u32 samples;
    u8 sps[64], pps[16];
    u32 sps_bytes, pps_bytes;
    bool ok;
    void u8v(u8 v) noexcept {
        if (count >= capacity) {
            ok = false;
            return;
        }
        bytes[count++] = v;
    }
    void u16v(u32 v) noexcept {
        u8v(static_cast<u8>(v >> 8));
        u8v(static_cast<u8>(v));
    }
    void u32v(u32 v) noexcept {
        u16v(v >> 16);
        u16v(v & 0xFFFFu);
    }
    void patch(u32 at, u32 v) noexcept {
        for (u32 i = 0; i < 4 && ok; ++i)
            bytes[at + i] = static_cast<u8>(v >> (24 - 8 * i));
    }
    u32 begin(const char* type) noexcept {
        const u32 at = count;
        u32v(0);
        for (u32 i = 0; i < 4; ++i)
            u8v(static_cast<u8>(type[i]));
        return at;
    }
    void end(u32 at) noexcept {
        patch(at, count - at);
    }
};

// Annex B access unit -> length-prefixed sample; SPS/PPS go to avcC, AUDs drop.
void append_sample(Movie* m, Output* annexb) noexcept {
    const u8* p = annexb->data;
    const u32 n = annexb->count;
    const u32 start = m->count;
    for (u32 i = 0; i + 3 <= n;) {
        if (!(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)) {
            ++i;
            continue;
        }
        const u32 first = i + 3;
        u32 last = first;
        while (last + 3 <= n &&
               !(p[last] == 0 && p[last + 1] == 0 &&
                 (p[last + 2] == 1 || (p[last + 2] == 0 && last + 3 < n && p[last + 3] == 1))))
            ++last;
        if (last + 3 > n)
            last = n;
        const u8 type = p[first] & 31u;
        const u32 bytes = last - first;
        if (type == 7 || type == 8) {
            u8* target = type == 7 ? m->sps : m->pps;
            const u32 room = type == 7 ? sizeof(m->sps) : sizeof(m->pps);
            if (bytes > room)
                m->ok = false;
            else {
                copy_bytes(target, p + first, bytes);
                (type == 7 ? m->sps_bytes : m->pps_bytes) = bytes;
            }
        } else if (type != 9) {
            m->u32v(bytes);
            for (u32 k = 0; k < bytes; ++k)
                m->u8v(p[first + k]);
        }
        i = last;
    }
    if (m->samples < 16)
        m->sizes[m->samples++] = m->count - start;
    else
        m->ok = false;
    annexb->count = 0;
}

// ftyp, mdat (all samples, one chunk), then moov. IDRs every `gop` samples.
void finish_movie(Movie* m, u32 mdat, u32 width, u32 height, u32 gop) noexcept {
    m->end(mdat);
    const u32 moov = m->begin("moov");
    const u32 trak = m->begin("trak");
    const u32 mdia = m->begin("mdia");
    const u32 mdhd = m->begin("mdhd");
    m->u32v(0);
    m->u32v(0);
    m->u32v(0);
    m->u32v(1'000);
    m->u32v(m->samples * 33u);
    m->u32v(0);
    m->end(mdhd);
    const u32 hdlr = m->begin("hdlr");
    m->u32v(0);
    m->u32v(0);
    m->u32v(0x76696465u);
    m->end(hdlr);
    const u32 minf = m->begin("minf");
    const u32 stbl = m->begin("stbl");
    const u32 stsd = m->begin("stsd");
    m->u32v(0);
    m->u32v(1);
    const u32 avc1 = m->begin("avc1");
    for (u32 i = 0; i < 78; ++i)
        m->u8v(i == 7    ? 1
               : i == 24 ? static_cast<u8>(width >> 8)
               : i == 25 ? static_cast<u8>(width)
               : i == 26 ? static_cast<u8>(height >> 8)
               : i == 27 ? static_cast<u8>(height)
                         : 0);
    const u32 avcc = m->begin("avcC");
    m->u8v(1);
    m->u8v(m->sps[1]);
    m->u8v(m->sps[2]);
    m->u8v(m->sps[3]);
    m->u8v(0xff);
    m->u8v(0xe1);
    m->u16v(m->sps_bytes);
    for (u32 i = 0; i < m->sps_bytes; ++i)
        m->u8v(m->sps[i]);
    m->u8v(1);
    m->u16v(m->pps_bytes);
    for (u32 i = 0; i < m->pps_bytes; ++i)
        m->u8v(m->pps[i]);
    m->end(avcc);
    m->end(avc1);
    m->end(stsd);
    const u32 stts = m->begin("stts");
    m->u32v(0);
    m->u32v(1);
    m->u32v(m->samples);
    m->u32v(33);
    m->end(stts);
    const u32 stsc = m->begin("stsc");
    m->u32v(0);
    m->u32v(1);
    m->u32v(1);
    m->u32v(m->samples);
    m->u32v(1);
    m->end(stsc);
    const u32 stsz = m->begin("stsz");
    m->u32v(0);
    m->u32v(0);
    m->u32v(m->samples);
    for (u32 i = 0; i < m->samples; ++i)
        m->u32v(m->sizes[i]);
    m->end(stsz);
    const u32 stco = m->begin("stco");
    m->u32v(0);
    m->u32v(1);
    m->u32v(mdat + 8);
    m->end(stco);
    const u32 stss = m->begin("stss");
    m->u32v(0);
    m->u32v((m->samples + gop - 1) / gop);
    for (u32 i = 0; i < m->samples; i += gop)
        m->u32v(i + 1);
    m->end(stss);
    m->end(stbl);
    m->end(minf);
    m->end(mdia);
    m->end(trak);
    m->end(moov);
}

// Each GOP is an IDR with its own pattern plus P_Skip copies of it.
bool build_movie(Movie* m, Buffers* b, const Sps& sps, u32 samples, u32 gop, bool compact) noexcept {
    const Pps pps{0, 0};
    m->count = m->samples = 0;
    m->ok = true;
    const u32 ftyp = m->begin("ftyp");
    m->u32v(0x69736f6du);
    m->u32v(0x200u);
    m->u32v(0x61766331u);
    m->end(ftyp);
    const u32 mdat = m->begin("mdat");
    for (u32 i = 0; i < samples; ++i) {
        if (i % gop == 0)
            write_idr(&b->out, &b->bits, sps, pps, 1u + i / gop, static_cast<u16>(i / gop), 1, true,
                      compact);
        else
            write_p(&b->out, &b->bits, sps, pps, static_cast<u16>(i % gop),
                    static_cast<u16>(2u * (i % gop)));
        if (!b->out.ok)
            return false;
        append_sample(m, &b->out);
    }
    finish_movie(m, mdat, sps.width_mbs * 16u, sps.height_mbs * 16u - sps.crop_bottom, gop);
    return m->ok;
}

struct Target {
    ID3D11Texture2D* texture;
    ID3D11UnorderedAccessView* view;
    ID3D11Texture2D* staging;
};

bool create_target(ID3D11Device* device, u32 width, u32 height, Target* t) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &t->texture)) ||
        FAILED(device->CreateUnorderedAccessView(t->texture, nullptr, &t->view)))
        return false;
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &t->staging));
}
void release_target(Target* t) noexcept {
    com_release(t->staging);
    com_release(t->view);
    com_release(t->texture);
}

// Hash of the visible picture; also checks one pixel per macroblock against
// the fixture's samples converted with the shader's integer math.
bool read_target(ID3D11DeviceContext1* context, Target* t, const GpuVideoFrame& frame, const Sps& sps,
                 u32 pattern, bool compact, u64* hash) noexcept {
    context->CopyResource(t->staging, t->texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(t->staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const auto* pixels = static_cast<const u8*>(mapped.pData);
    u64 h = 1469598103934665603ull;
    for (u32 y = 0; y < frame.height; ++y)
        for (u32 x = 0; x < frame.width * 4u; ++x)
            h = (h ^ pixels[u64{y} * mapped.RowPitch + x]) * 1099511628211ull;
    const auto c = yuv_rgb_coefficients(frame.bt709, frame.full_range);
    bool ok = true;
    for (u32 my = 0; my < sps.height_mbs && ok; ++my)
        for (u32 mx = 0; mx < sps.width_mbs && ok; ++mx) {
            const u32 y = my * 16u + 5u, x = mx * 16u + 9u;
            if (y >= frame.height)
                continue;
            u8 expected[4];
            const u32 m = compact ? 0 : my * sps.width_mbs + mx;
            const i32 chroma = static_cast<i32>(pcm_chroma(pattern)) - 128;
            yuv_rgb_pixel(expected, pcm_luma(m, pattern), chroma, chroma, c);
            const u8* actual = pixels + u64{y} * mapped.RowPitch + x * 4u;
            ok = actual[0] == expected[0] && actual[1] == expected[1] && actual[2] == expected[2] &&
                 actual[3] == 255;
        }
    context->Unmap(t->staging, 0);
    *hash = h;
    return ok;
}

// Converts every frame from the decoder's own plane views. Returns 0 or a failure code.
int run_movie(ID3D11Device* device, ID3D11DeviceContext1* context, const Movie& movie, const Sps& sps,
              u32 gop, bool compact) noexcept {
    Mp4Decoder decoder{};
    if (!mp4_decoder_initialize(&decoder, {movie.bytes, movie.count}, 0, 0, device))
        return 60;
    const u32 width = sps.width_mbs * 16u, height = sps.height_mbs * 16u - sps.crop_bottom;
    Target target{};
    VideoFrameGpu converter{};
    int status = create_target(device, width, height, &target) ? 0 : 61;
    u32 frames = 0;
    for (u32 index = 0; !status && index < movie.samples; ++index) {
        const Mp4DecodedFrame decoded = mp4_decoder_read(&decoder, false);
        if (!decoded.ok() || !decoded.gpu || !decoded.gpu->luma || !decoded.gpu->chroma) {
            status = 63;
            break;
        }
        const GpuVideoFrame& frame = *decoded.gpu;
        u64 hash = 0;
        if (FAILED(video_frame_gpu_present(&converter, device, context, frame, target.view, 0, 0)))
            status = 64;
        else if (!read_target(context, &target, frame, sps, 1u + index / gop, compact, &hash))
            status = 65;
        if (!mp4_decoder_acknowledge_frame(&decoder) && !status)
            status = 67;
        ++frames;
    }
    if (!status && frames != movie.samples)
        status = 68;
    if (!status &&
        (converter.frames != frames || converter.converted_pixels != u64{width} * height * frames))
        status = 72;
    video_frame_gpu_release(&converter);
    release_target(&target);
    mp4_decoder_release(&decoder);
    return status;
}

// WARP has no hardware decoder: the capability check fails and decoder
// creation fails as on any GPU without one; nothing falls back to a copy.
int software_device_refuses(const Movie& movie) noexcept {
    ID3D11Device* device = nullptr;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // WARP (and the Basic Render Driver built on it) rejects VIDEO_SUPPORT,
    // exposes no ID3D11VideoDevice and reports no NV12 decoder output.
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                 &device, nullptr, nullptr)))
        return 0; // No WARP device to compare.
    Mp4Decoder decoder{};
    const bool initialized = mp4_decoder_initialize(&decoder, {movie.bytes, movie.count}, 0, 0, device);
    const bool refused = !gpu_decoder_surfaces_supported(device) && !initialized;
    mp4_decoder_release(&decoder);
    device->Release();
    return refused ? 0 : 55;
}

} // namespace

// bits and out: fixture scratch (bitstream and Annex B output).
int h264_attachment_gpu_test(ID3D11Device* device, ID3D11DeviceContext* immediate, Bits bits,
                             Output out) noexcept {
    Buffers buffers{bits, out};
    Buffers* b = &buffers;
    ID3D11DeviceContext1* context = nullptr;
    if (FAILED(
            immediate->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context))))
        return 59;
    constexpr u32 kMovieBytes = 4u * 1024u * 1024u;
    Movie movie{};
    movie.bytes =
        static_cast<u8*>(VirtualAlloc(nullptr, kMovieBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    movie.capacity = kMovieBytes;
    int status = movie.bytes ? 0 : 58;
    // This test runs only where the stream decoder found hardware H.264 decode.
    if (!status && !gpu_decoder_surfaces_supported(device))
        status = 54;
    // Per-macroblock I_PCM samples: a wrong slice or plane offset changes pixels.
    const Sps small{0, 66, 12, 9, 2, 1, false, 0, 8, 31};
    if (!status && !build_movie(&movie, b, small, 8, 4, false))
        status = 57;
    if (!status)
        status = run_movie(device, context, movie, small, 4, false);
    if (!status)
        status = software_device_refuses(movie);
    // 1080p (coded 1088) compact pictures.
    const Sps hd{0, 100, 120, 68, 2, 1, false, 0, 8, 40};
    if (!status && !build_movie(&movie, b, hd, 12, 6, true))
        status = 56;
    if (!status)
        status = run_movie(device, context, movie, hd, 6, true);
    if (status)
        test_write_text("h264 attachment gpu: failed\n");
    if (movie.bytes)
        VirtualFree(movie.bytes, 0, MEM_RELEASE);
    context->Release();
    return status;
}

} // namespace maicord
