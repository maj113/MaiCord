// Decoded pictures that arrive as dma-bufs, end to end on the GPU API without a VA-API driver: a picture is written into
// a dumb buffer the way a decoder writes a surface, its planes are imported by the renderer's EGL (the same call
// export_picture makes), realized and converted by the video_frame compute shader on the test thread (the renderer's),
// read back and compared with the CPU conversion.
//
//   nv12     an NV12 picture in one linear buffer: R8 and GR88 planes
//   rewrite  the same imported planes after the buffer's content changed: the next conversion shows the new picture,
//            which is what keeping the images of an export relies on
//   p010     a 10-bit picture: R16 and GR1616 planes, converted to the nearest 8-bit levels (within one level of the
//            exact reduction)
//
// Status 77: no EGL device, no dma-buf import, no DRM device this user may use, or this EGL does not import the buffers.
#include "../../support/dumb_dmabuf.hpp"
#include "../../support/test_output.hpp"
#include "../../support/yuv_reference.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/gpu/video_planes.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr u32 kWidth = 64, kHeight = 32;

// The samples of a picture at 10 bits (an 8-bit picture is the same values divided by four).
struct Picture {
    u16 y[kHeight][kWidth];
    u16 cb[kHeight / 2][kWidth / 2];
    u16 cr[kHeight / 2][kWidth / 2];
};

// Limited-range samples that vary in every direction and use the low bits.
void make_picture(Picture* picture, u32 seed) noexcept {
    for (u32 row = 0; row < kHeight; ++row)
        for (u32 column = 0; column < kWidth; ++column)
            picture->y[row][column] = static_cast<u16>(64 + (row * 11 + column * 7 + seed * 131 + (column ^ row)) % 877);
    for (u32 row = 0; row < kHeight / 2; ++row)
        for (u32 column = 0; column < kWidth / 2; ++column) {
            picture->cb[row][column] = static_cast<u16>(64 + (row * 23 + column * 5 + seed * 71) % 897);
            picture->cr[row][column] = static_cast<u16>(64 + (row * 3 + column * 29 + seed * 53 + 300) % 897);
        }
}

// The picture as a decoder leaves it in an NV12 surface (one plane of luma, then interleaved chroma).
void write_nv12(const DumbBuffer& buffer, const Picture& picture) noexcept {
    for (u32 row = 0; row < kHeight; ++row)
        for (u32 column = 0; column < kWidth; ++column)
            buffer.pixels[usize{row} * buffer.pitch + column] = static_cast<u8>(picture.y[row][column] >> 2);
    u8* chroma = buffer.pixels + usize{kHeight} * buffer.pitch;
    for (u32 row = 0; row < kHeight / 2; ++row)
        for (u32 column = 0; column < kWidth / 2; ++column) {
            chroma[usize{row} * buffer.pitch + 2 * column] = static_cast<u8>(picture.cb[row][column] >> 2);
            chroma[usize{row} * buffer.pitch + 2 * column + 1] = static_cast<u8>(picture.cr[row][column] >> 2);
        }
}

// P010: the 10-bit value in the top bits of 16.
void write_p010(const DumbBuffer& buffer, const Picture& picture) noexcept {
    for (u32 row = 0; row < kHeight; ++row) {
        auto* line = reinterpret_cast<u16*>(buffer.pixels + usize{row} * buffer.pitch);
        for (u32 column = 0; column < kWidth; ++column)
            line[column] = static_cast<u16>(picture.y[row][column] << 6);
    }
    u8* chroma = buffer.pixels + usize{kHeight} * buffer.pitch;
    for (u32 row = 0; row < kHeight / 2; ++row) {
        auto* line = reinterpret_cast<u16*>(chroma + usize{row} * buffer.pitch);
        for (u32 column = 0; column < kWidth / 2; ++column) {
            line[2 * column] = static_cast<u16>(picture.cb[row][column] << 6);
            line[2 * column + 1] = static_cast<u16>(picture.cr[row][column] << 6);
        }
    }
}

struct Rig {
    GpuDevice* device;
    GpuContext* context;
    VideoFrameGpu converter;
    GpuTexture* target;
    GpuUav* uav;
    i32 node;
};

// The picture's conversion on the CPU with the shader's integer formula; `ten_bit` reduces each sample to 8 bits by
// rounding (v + 2) / 4, the exact reduction of a 10-bit sample.
u32 largest_error(Rig& rig, const Picture& picture, bool ten_bit, const GpuVideoFrame& frame) noexcept {
    static u8 rgba[kWidth * kHeight * 4];
    if (!gpu_ok(video_frame_gpu_present(&rig.converter, rig.device, rig.context, frame, rig.uav, 0, 0)) ||
        !gpu_ok(gpu_readback(rig.device, rig.context, rig.target, {0, 0, kWidth, kHeight}, rgba, kWidth * 4)))
        return ~0u;
    const auto reduce = [&](u32 sample) noexcept { return static_cast<i32>(ten_bit ? (sample + 2) >> 2 : sample >> 2); };
    const YuvRgbCoefficients coefficients = yuv_rgb_coefficients(frame.bt709, frame.full_range);
    u32 largest = 0;
    for (u32 row = 0; row < kHeight; ++row)
        for (u32 column = 0; column < kWidth; ++column) {
            u8 expected[4];
            yuv_rgb_pixel(expected, reduce(picture.y[row][column]), reduce(picture.cb[row / 2][column / 2]) - 128,
                          reduce(picture.cr[row / 2][column / 2]) - 128, coefficients);
            const u8* got = rgba + (usize{row} * kWidth + column) * 4;
            for (u32 c = 0; c < 3; ++c) {
                const u32 difference = got[c] > expected[c] ? got[c] - expected[c] : expected[c] - got[c];
                if (difference > largest)
                    largest = difference;
            }
        }
    return largest;
}

struct Planes {
    GpuTexture* luma_texture;
    GpuSrv* luma_view;
    GpuTexture* chroma_texture;
    GpuSrv* chroma_view;
};

// Imports the buffer's two planes as the renderer's EGL would take an export of it.
bool import_picture(const Rig& rig, const DumbBuffer& buffer, bool ten_bit, Planes* planes) noexcept {
    *planes = {};
    const i32 fd = dumb_export(rig.node, buffer);
    if (fd < 0)
        return false;
    const GpuDmabufPlane luma{fd, ten_bit ? kDrmFormatR16 : kDrmFormatR8, kWidth, kHeight, 0, buffer.pitch,
                              kDrmFormatModifierLinear, -1, 0, 0};
    const GpuDmabufPlane chroma{fd,
                                ten_bit ? kDrmFormatGr1616 : kDrmFormatGr88,
                                kWidth / 2,
                                kHeight / 2,
                                buffer.pitch * kHeight,
                                buffer.pitch,
                                kDrmFormatModifierLinear,
                                -1,
                                0,
                                0};
    const bool ok = gpu_ok(gpu_video_plane_import(rig.device, luma, &planes->luma_texture, &planes->luma_view)) &&
                    gpu_ok(gpu_video_plane_import(rig.device, chroma, &planes->chroma_texture, &planes->chroma_view));
    sys_close(fd);
    if (!ok) {
        gpu_release(planes->luma_view);
        gpu_release(planes->luma_texture);
        gpu_release(planes->chroma_view);
        gpu_release(planes->chroma_texture);
        *planes = {};
    }
    return ok;
}

void release_planes(Planes* planes) noexcept {
    gpu_release(planes->chroma_view);
    gpu_release(planes->chroma_texture);
    gpu_release(planes->luma_view);
    gpu_release(planes->luma_texture);
    *planes = {};
}

GpuVideoFrame frame_of(const Planes& planes, bool bt709) noexcept {
    GpuVideoFrame frame{};
    frame.texture = planes.luma_texture;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.bt709 = bt709;
    frame.luma = planes.luma_view;
    frame.chroma = planes.chroma_view;
    return frame;
}

void eight_bit(Rig& rig) noexcept {
    DumbBuffer buffer{};
    if (!dumb_create(rig.node, kWidth, kHeight * 3 / 2, 8, &buffer)) {
        test_write_line("nv12 skipped: no 8-bit dumb buffer");
        return;
    }
    static Picture first, second;
    make_picture(&first, 1);
    make_picture(&second, 2);
    write_nv12(buffer, first);
    Planes planes{};
    if (!import_picture(rig, buffer, false, &planes)) {
        test_write_line("nv12 skipped: this EGL does not import the buffer");
        dumb_destroy(rig.node, &buffer);
        return;
    }
    check(gpu_video_plane_owns(planes.luma_texture) && gpu_video_plane_owns(planes.chroma_texture),
          "nv12: the planes are the pool's");
    GpuVideoFrame frame = frame_of(planes, true);
    const u32 error = largest_error(rig, first, false, frame);
    test_write_metric("nv12: largest error ", error);
    check(error <= 1, "nv12: an imported NV12 picture converts to the picture");
    frame.bt709 = false;
    check(largest_error(rig, first, false, frame) <= 1, "nv12: and with the other matrix");
    // The decoder writes the next picture into the same surface; the images made from its export are unchanged.
    write_nv12(buffer, second);
    frame.bt709 = true;
    const u32 again = largest_error(rig, second, false, frame);
    test_write_metric("rewrite: largest error ", again);
    check(again <= 1, "rewrite: the same imported planes show the picture written into the buffer since");
    release_planes(&planes);
    dumb_destroy(rig.node, &buffer);
}

void ten_bit(Rig& rig) noexcept {
    DumbBuffer buffer{};
    if (!dumb_create(rig.node, kWidth, kHeight * 3 / 2, 16, &buffer)) {
        test_write_line("p010 skipped: no 16-bit dumb buffer");
        return;
    }
    static Picture picture;
    make_picture(&picture, 3);
    write_p010(buffer, picture);
    Planes planes{};
    if (!import_picture(rig, buffer, true, &planes)) {
        test_write_line("p010 skipped: this EGL does not import R16 and GR1616");
        dumb_destroy(rig.node, &buffer);
        return;
    }
    check(gpu_video_plane_owns(planes.luma_texture) && gpu_video_plane_owns(planes.chroma_texture),
          "p010: the planes are the pool's");
    const GpuVideoFrame frame = frame_of(planes, true);
    const u32 error = largest_error(rig, picture, true, frame);
    test_write_metric("p010: largest error ", error);
    // The shader reads a normalised 16-bit sample times 255, which is 0.4% under the 10-bit value divided by four: up to
    // one level low in luma and in chroma, and a chroma level moves a channel by up to 541/256 levels (blue).
    check(error <= 4, "p010: an imported P010 picture converts to the picture within a level of luma and chroma");
    release_planes(&planes);
    dumb_destroy(rig.node, &buffer);
}

u64 now_nanoseconds() noexcept {
    TimeSpec time{};
    clock_gettime(kClockMonotonic, &time);
    return static_cast<u64>(time.seconds) * 1'000'000'000ull + static_cast<u64>(time.nanoseconds);
}

// What a frame costs the renderer's thread when its planes are imported again (a fresh EGL image and GL texture each
// frame: the importing, the texture made at the first conversion, its deletion when the planes are reaped) against
// keeping the images of an export: both write the picture, convert and read back. Metrics only.
void cost(Rig& rig) noexcept {
    constexpr u32 kFrames = 400;
    DumbBuffer buffer{};
    if (!dumb_create(rig.node, kWidth, kHeight * 3 / 2, 8, &buffer))
        return;
    static Picture pictures[2];
    static u8 rgba[kWidth * kHeight * 4];
    make_picture(&pictures[0], 5);
    make_picture(&pictures[1], 6);
    const auto convert = [&](const Planes& planes) noexcept {
        const GpuVideoFrame frame = frame_of(planes, true);
        return gpu_ok(video_frame_gpu_present(&rig.converter, rig.device, rig.context, frame, rig.uav, 0, 0)) &&
               gpu_ok(gpu_readback(rig.device, rig.context, rig.target, {0, 0, kWidth, kHeight}, rgba, kWidth * 4));
    };
    bool ok = true;
    Planes planes{};
    write_nv12(buffer, pictures[0]);
    // One warm-up conversion and the shader.
    ok = import_picture(rig, buffer, false, &planes) && convert(planes);
    release_planes(&planes);
    // The best of several rounds: a loaded machine only ever makes a round slower.
    constexpr u32 kRounds = 7;
    u64 fresh = ~0ull, kept = ~0ull;
    for (u32 round = 0; ok && round < kRounds; ++round) {
        const u64 start = now_nanoseconds();
        for (u32 i = 0; ok && i < kFrames; ++i) {
            write_nv12(buffer, pictures[i & 1]);
            ok = import_picture(rig, buffer, false, &planes) && convert(planes);
            release_planes(&planes);
        }
        const u64 elapsed = now_nanoseconds() - start;
        fresh = elapsed < fresh ? elapsed : fresh;
    }
    ok = ok && import_picture(rig, buffer, false, &planes);
    for (u32 round = 0; ok && round < kRounds; ++round) {
        const u64 start = now_nanoseconds();
        for (u32 i = 0; ok && i < kFrames; ++i) {
            write_nv12(buffer, pictures[i & 1]);
            ok = convert(planes);
        }
        const u64 elapsed = now_nanoseconds() - start;
        kept = elapsed < kept ? elapsed : kept;
    }
    release_planes(&planes);
    dumb_destroy(rig.node, &buffer);
    if (!ok) {
        test_write_line("cost skipped: a conversion failed");
        return;
    }
    test_write_metric("cost: imported again, microseconds per frame ", fresh / kFrames / 1000);
    test_write_metric("cost: images kept, microseconds per frame ", kept / kFrames / 1000);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_video_dmabuf_entry) {
    using namespace maicord;
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        test_write_line("skipped: no EGL device");
        test_process_exit(77);
    }
    if (!gpu_caps(device).dmabuf_import) {
        test_write_line("skipped: the device does not import dma-bufs");
        test_process_exit(77);
    }
    Rig rig{};
    rig.device = device;
    rig.context = context;
    rig.node = dumb_open();
    if (rig.node < 0) {
        test_write_line("skipped: no DRM device this user may open");
        test_process_exit(77);
    }
    if (!gpu_ok(gpu_texture_create(device, {kWidth, kHeight, GpuFormat::rgba8, GpuUsage::default_,
                                            static_cast<u8>(gpu_bind_srv | gpu_bind_uav)},
                                   nullptr, &rig.target)) ||
        !gpu_ok(gpu_uav_create(device, rig.target, &rig.uav))) {
        test_write_line("skipped: no render target");
        test_process_exit(77);
    }
    eight_bit(rig);
    ten_bit(rig);
    cost(rig);
    video_frame_gpu_release(&rig.converter);
    gpu_release(rig.uav);
    gpu_release(rig.target);
    sys_close(rig.node);
    gpu_device_destroy(&device, &context);
    test_exit(g_failures, "maicord_video_dmabuf");
}
