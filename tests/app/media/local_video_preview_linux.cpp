// The composer's local video attachment preview (account_media_decode_local_video_preview, the code the worker lane runs
// for a file the user attached) on the Linux movie decoder, with the software tier forced (MAICORD_VAAPI=off): the file
// is streamed through the exact-read callback into the lane's sparse source (the header and the first samples), the
// decoder opens on it, the first picture is published as a GPU frame, and the GLES conversion of that frame equals the
// reference's first picture. Also the refusals the lane reports without a retry: no device, a file that is no movie.
//
// Status 77: no EGL device or no libopenh264.
#include "../../fixtures/mp4/mp4_fixtures.hpp"
#include "../../support/test_output.hpp"
#include "../../support/yuv_reference.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/app/media/local_draft_preview.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/render/media/video_frame_gpu.hpp"
#include "../../../src/platform/decode/software/software_decoder.hpp"

namespace maicord {
namespace {

u32 g_failures;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

constexpr u32 kMaximumWidth = 64, kMaximumHeight = 48;
u8 g_rgba[kMaximumWidth * kMaximumHeight * 4];
AccountMedia g_media;

struct File {
    const u8* bytes;
    u32 count;
    u32 reads;
};

// The lane's exact-read callback: the whole range or false.
bool read_file(void* context, u64 offset, Slice<u8> output) noexcept {
    auto& file = *static_cast<File*>(context);
    ++file.reads;
    if (offset > file.count || output.count > file.count - offset)
        return false;
    copy_bytes(output.data, file.bytes + offset, output.count);
    return true;
}

AccountMediaLocalPreviewRequest request(const File& file, GpuDevice* device) noexcept {
    AccountMediaLocalPreviewRequest r{};
    r.snapshot = {{9, 1, 1, 0}, file.count};
    r.key = media_asset_local_draft_preview(77, 1u | kLocalDraftPreviewThumbnailVariant, MediaAttachmentFormat::mp4);
    r.owner_scope = 700;
    r.owner_generation = 8;
    r.video_device = device;
    r.content.bytes = 0; // read through the callback
    return r;
}

void poster(GpuDevice* device, GpuContext* context, const Mp4Reference& reference, const u8* movie, u32 movie_bytes,
            const char* name) noexcept {
    check(account_media_initialize(&g_media).state == AccountMediaRequestState::ready, "preview: the media owner starts");
    File file{movie, movie_bytes, 0};
    // The request names the file by a path the lane would open; the test reads from memory instead (as the lane
    // does through its callback), so the path is a placeholder that satisfies the request's shape.
    static const wchar_t path[] = L"movie.mp4";
    AccountMediaLocalPreviewRequest r = request(file, device);
    r.path = {path, 9};
    const AccountMediaRequestResult decoded = account_media_decode_local_video_preview(&g_media, r, read_file, &file);
    if (decoded.state != AccountMediaRequestState::ready) {
        test_write_metric("preview state ", static_cast<u32>(decoded.state));
        test_write_metric("preview error ", static_cast<u32>(decoded.error));
        test_write_metric("preview video failure ", static_cast<u32>(decoded.video_failure));
    }
    check(decoded.state == AccountMediaRequestState::ready, name);
    check(account_media_local_video_resident(&g_media), "preview: the decoder stays on the lane for the poster");
    AccountMediaFrame frame{};
    check(account_media_take_frame(&g_media, &frame) && frame.gpu_video && frame.kind == AccountMediaFrameKind::video_nv12_gpu &&
              frame.width == reference.width && frame.height == reference.height,
          "preview: the first picture is published as a GPU frame of the movie's extent");
    test_write_metric("preview reads ", file.reads);
    // The published picture, converted on the GPU, is the reference's first picture.
    GpuTexture* target = nullptr;
    GpuUav* uav = nullptr;
    VideoFrameGpu converter{};
    const bool prepared =
        gpu_ok(gpu_texture_create(device, {reference.width, reference.height, GpuFormat::rgba8, GpuUsage::default_,
                                           static_cast<u8>(gpu_bind_srv | gpu_bind_uav)},
                                  nullptr, &target)) &&
        gpu_ok(gpu_uav_create(device, target, &uav));
    bool same = false;
    if (prepared && frame.gpu_video &&
        gpu_ok(video_frame_gpu_present(&converter, device, context, *frame.gpu_video, uav, 0, 0)) &&
        gpu_ok(gpu_readback(device, context, target, {0, 0, reference.width, reference.height}, g_rgba,
                            reference.width * 4))) {
        const u8* y = mp4_reference_picture(reference, 0);
        const u8* cb = y + usize{reference.width} * reference.height;
        const u8* cr = cb + usize{reference.width / 2} * (reference.height / 2);
        const YuvRgbCoefficients coefficients = yuv_rgb_coefficients(frame.gpu_video->bt709, frame.gpu_video->full_range);
        u32 largest = 0;
        for (u32 row = 0; row < reference.height; ++row)
            for (u32 column = 0; column < reference.width; ++column) {
                const usize chroma = usize{row / 2} * (reference.width / 2) + column / 2;
                u8 expected[4];
                yuv_rgb_pixel(expected, y[usize{row} * reference.width + column], cb[chroma] - 128, cr[chroma] - 128,
                              coefficients);
                const u8* got = g_rgba + (usize{row} * reference.width + column) * 4;
                for (u32 c = 0; c < 3; ++c) {
                    const u32 d = got[c] > expected[c] ? got[c] - expected[c] : expected[c] - got[c];
                    largest = d > largest ? d : largest;
                }
            }
        same = largest <= 1;
        test_write_metric("preview largest error ", largest);
    }
    check(same, "preview: the poster equals the reference's first picture");
    video_frame_gpu_release(&converter);
    gpu_release(uav);
    gpu_release(target);
    account_media_release_video(&g_media);
    check(!account_media_local_video_resident(&g_media), "preview: releasing the video frees the lane");
}

void refused(GpuDevice* device) noexcept {
    check(account_media_initialize(&g_media).state == AccountMediaRequestState::ready, "refusals: the media owner starts");
    static const u8 garbage[512]{0, 0, 0, 24, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm'};
    File file{garbage, sizeof(garbage), 0};
    static const wchar_t path[] = L"movie.mp4";
    AccountMediaLocalPreviewRequest r = request(file, device);
    r.path = {path, 9};
    const auto bad = account_media_decode_local_video_preview(&g_media, r, read_file, &file);
    check(bad.state == AccountMediaRequestState::unsupported && bad.retry_at_ms == ~0ull,
          "refusals: a file that is no movie is unsupported, without a retry");
    r = request(file, nullptr);
    r.path = {path, 9};
    const auto none = account_media_decode_local_video_preview(&g_media, r, read_file, &file);
    check(none.state == AccountMediaRequestState::unsupported && none.retry_at_ms == ~0ull,
          "refusals: no video device leaves the tile on its file icon, without a retry");
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_local_video_preview_entry) {
    using namespace maicord;
    if (!software_codec_available(GpuStreamCodec::h264)) {
        test_write_line("skipped: libopenh264 is not installed");
        test_process_exit(77);
    }
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    if (!gpu_ok(gpu_device_create(&device, &context))) {
        test_write_line("skipped: no EGL device");
        test_process_exit(77);
    }
    Mp4Reference baseline, bframes;
    check(mp4_reference_parse(kMp4H264BaselineGopsRef, sizeof(kMp4H264BaselineGopsRef), &baseline) &&
              mp4_reference_parse(kMp4H264AacBFramesRef, sizeof(kMp4H264AacBFramesRef), &bframes),
          "references parse");
    poster(device, context, baseline, kMp4H264BaselineGops, sizeof(kMp4H264BaselineGops),
           "preview: a Baseline movie's poster decodes");
    poster(device, context, bframes, kMp4H264AacBFrames, sizeof(kMp4H264AacBFrames),
           "preview: a movie with B pictures (a reordered first picture) posters from its first displayed picture");
    refused(device);
    gpu_device_destroy(&device, &context);
    test_exit(g_failures, "maicord_local_video_preview");
}
