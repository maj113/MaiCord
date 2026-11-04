// What the Linux client tells Discord it can decode, and the pure parts of the hardware tier: no GPU, no VA-API driver.
//
//   capability   video_decode_* with mocked probe results (every combination of hardware and software the policy
//                distinguishes): the advertised set, the software-only subset and the word gpu_stream_decoder_codecs
//                returns
//   offers       Select Protocol offers built from those words: hardware codecs first in Discord's order, software-only
//                codecs after them, nothing offered that nothing decodes (the Linux rule; Windows always offers H.264)
//   layout       the dma-buf layout of an exported NV12 or P010 surface, accepted and refused cases, and the EGL
//                attribute list that imports a plane
//   profiles     the VA profile chosen for an H.264 stream from the profiles a driver lists
//   planes       the decoded-picture plane pool: capacity, reuse, and retain and release from several threads
//   --off, --no-library, --bad-node, --no-node
//                the hardware tier without MAICORD_VAAPI, without libva, on a node no driver serves, and (when this
//                machine has no render node) on none: each leaves no hardware codec
#include "../../support/test_output.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/gpu/video_planes.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/decode/video_capability.hpp"
#include "maicord/protocol/voice/video_signaling.hpp"
#include "maicord/sys/linux/abi.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
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

void capability() noexcept {
    struct Case {
        u32 hardware, software, advertised, software_only;
    };
    static constexpr Case cases[]{
        {0, 0, 0, 0},
        {kH264 | kH265 | kAv1, kH264 | kAv1, kH264 | kH265 | kAv1, 0},
        {0, kH264 | kAv1, kH264 | kAv1, kH264 | kAv1},
        {0, kH264, kH264, kH264},
        // HEVC has no software decoder: a library claim for it is ignored.
        {0, kH265 | kH264, kH264, kH264},
        {kH265, kH264 | kAv1 | kH265, kH265 | kH264 | kAv1, kH264 | kAv1},
        {kAv1, kH264, kAv1 | kH264, kH264},
        {kH264, kH264, kH264, 0},
        {kH265, 0, kH265, 0},
    };
    bool all = true;
    for (const Case& c : cases) {
        const VideoDecodeProbe probe{c.hardware, c.software};
        all = all && video_decode_advertised(probe) == c.advertised && video_decode_software_only(probe) == c.software_only;
        const u32 word = video_decode_codecs_word(probe);
        all = all && gpu_stream_codecs_decodable(word) == c.advertised && gpu_stream_codecs_software_only(word) == c.software_only;
    }
    check(all, "capability: advertised and software-only sets for every mocked probe result");
    check(video_decode_tier({kH264, kH264}, GpuStreamCodec::h264) == VideoDecodeTier::hardware &&
              video_decode_tier({0, kH264}, GpuStreamCodec::h264) == VideoDecodeTier::software &&
              video_decode_tier({0, kH265}, GpuStreamCodec::h265) == VideoDecodeTier::none &&
              video_decode_tier({kH265, 0}, GpuStreamCodec::h265) == VideoDecodeTier::hardware,
          "capability: hardware wins, software only where a decoder exists, HEVC never in software");
}

// The decodable word of voice_video_decode_offers as video_stream_decoders.cpp builds it from a codecs word.
u32 voice_word(u32 codecs_word) noexcept {
    const u32 codecs = gpu_stream_codecs_decodable(codecs_word), software = gpu_stream_codecs_software_only(codecs_word);
    u32 bits = 0;
    if (codecs & kH264)
        bits |= voice_video_codec_bit(VoiceVideoCodec::h264);
    if (codecs & kH265)
        bits |= voice_video_codec_bit(VoiceVideoCodec::h265);
    if (codecs & kAv1)
        bits |= voice_video_codec_bit(VoiceVideoCodec::av1);
    if (software & kH264)
        bits |= voice_video_software_bit(VoiceVideoCodec::h264);
    if (software & kAv1)
        bits |= voice_video_software_bit(VoiceVideoCodec::av1);
    return bits;
}

bool offers_are(u32 hardware, u32 software, const VoiceVideoCodec* expected, u32 count) noexcept {
    VoiceCodecOffer offers[kVoiceVideoMaximumCodecs];
    const u32 written = voice_video_decode_offers(voice_word(video_decode_codecs_word({hardware, software})),
                                                  {offers, kVoiceVideoMaximumCodecs});
    if (written != count)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (offers[i].codec != expected[i] || offers[i].encode || !offers[i].decode)
            return false;
    return true;
}

void offers() noexcept {
    using C = VoiceVideoCodec;
    static constexpr C all[]{C::av1, C::h265, C::h264};
    static constexpr C mixed[]{C::h265, C::av1, C::h264};
    static constexpr C software_pair[]{C::av1, C::h264};
    static constexpr C hardware_av1[]{C::av1, C::h264};
    static constexpr C only_h264[]{C::h264};
    static constexpr C hevc_first[]{C::h265, C::h264, C::av1};
    check(offers_are(kH264 | kH265 | kAv1, kH264 | kAv1, all, 3),
          "offers: all hardware codecs in Discord's order AV1, H265, H264");
    check(offers_are(kH265, kH264 | kAv1, mixed, 3),
          "offers: the hardware codec first, then the software-only ones in Discord's order");
    check(offers_are(0, kH264 | kAv1, software_pair, 2), "offers: with no hardware, the software codecs");
    check(offers_are(kAv1, kH264, hardware_av1, 2), "offers: hardware AV1 before software H.264");
    check(offers_are(0, kH264, only_h264, 1), "offers: software H.264 alone keeps mixed rooms working");
    check(offers_are(kH264 | kH265, kAv1, hevc_first, 3),
          "offers: a software AV1 does not outrank hardware H265 and H264");
    check(offers_are(0, 0, nullptr, 0), "offers: nothing is offered that nothing decodes");
    // The wire: priorities follow the order of the offer, and only offered codecs appear.
    VoiceCodecOffer list[kVoiceVideoMaximumCodecs];
    const u32 count = voice_video_decode_offers(voice_word(video_decode_codecs_word({kH265, kH264})), {list, kVoiceVideoMaximumCodecs});
    u8 output[1024];
    const VoiceSelectProtocolMedia selection{{{"192.0.2.7", 9}, 4000, voice_transport_aes256_gcm_rtpsize},
                                             {list, count},
                                             {"0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0", 36}};
    const auto written = voice_gateway_write_select_protocol_media(selection, {output, sizeof(output)});
    const ConstSlice<u8> json{output, written.bytes};
    const auto at = [&](const char* needle) noexcept {
        u32 length = 0;
        while (needle[length])
            ++length;
        for (u32 i = 0; i + length <= json.count; ++i)
            if (bytes_equal(json.data + i, needle, length))
                return static_cast<i32>(i);
        return -1;
    };
    check(written.ok() && at("\"name\":\"H265\"") >= 0 && at("\"name\":\"H264\"") > at("\"name\":\"H265\"") &&
              at("\"name\":\"AV1\"") < 0 && at("\"priority\":1000,\"payload_type\":101") > 0,
          "offers: Select Protocol lists the codecs in offer order and none that was not offered");
}

void layout() noexcept {
    using namespace vaapi;
    DrmPrimeDescriptor d{};
    d.fourcc = kFourccNv12;
    d.width = 1920;
    d.height = 1088;
    d.num_objects = 1;
    d.objects[0] = {40, 1920 * 1088 * 2, 0x0100000000000001ull};
    d.num_layers = 2;
    d.layers[0] = {kDrmFormatR8, 1, {0}, {0}, {2048}};
    d.layers[1] = {kDrmFormatGr88, 1, {0}, {2048u * 1088u}, {2048}};
    ExportLayout l{};
    check(export_layout(d, &l) && l.objects == 1 && l.plane[0].fourcc == kDrmFormatR8 && l.plane[0].width == 1920 &&
              l.plane[0].height == 1088 && l.plane[0].pitch == 2048 && l.plane[0].fd == 40 &&
              l.plane[1].fourcc == kDrmFormatGr88 && l.plane[1].width == 960 && l.plane[1].height == 544 &&
              l.plane[1].offset == 2048u * 1088u && l.plane[1].modifier == 0x0100000000000001ull,
          "layout: NV12 in two layers of one buffer");
    DrmPrimeDescriptor two = d;
    two.num_objects = 2;
    two.objects[1] = {41, 1 << 20, 0};
    two.layers[1].object_index[0] = 1;
    two.layers[1].offset[0] = 0;
    check(export_layout(two, &l) && l.plane[1].fd == 41 && l.plane[1].modifier == 0 && l.objects == 2,
          "layout: each plane takes its own object, descriptor and modifier");
    DrmPrimeDescriptor p010 = d;
    p010.fourcc = kFourccP010;
    p010.layers[0].drm_format = kDrmFormatR16;
    p010.layers[1].drm_format = kDrmFormatGr1616;
    check(export_layout(p010, &l) && l.plane[0].fourcc == kDrmFormatR16 && l.plane[1].fourcc == kDrmFormatGr1616,
          "layout: P010 maps to R16 and GR1616");
    DrmPrimeDescriptor bad = d;
    bad.num_layers = 1;
    bool refused = !export_layout(bad, &l);
    bad = d;
    bad.layers[0].drm_format = kDrmFormatGr88;
    refused = refused && !export_layout(bad, &l);
    bad = d;
    bad.layers[1].num_planes = 2;
    refused = refused && !export_layout(bad, &l);
    bad = d;
    bad.layers[1].object_index[0] = 1;
    refused = refused && !export_layout(bad, &l);
    bad = d;
    bad.fourcc = 0x32315659; // YV12
    refused = refused && !export_layout(bad, &l);
    bad = d;
    bad.objects[0].fd = -1;
    refused = refused && !export_layout(bad, &l);
    bad = d;
    bad.layers[0].pitch[0] = 0;
    refused = refused && !export_layout(bad, &l);
    check(refused, "layout: a composed layer, a swapped format, a missing plane or object, another fourcc, no fd, no pitch");

    // The import attributes: width, height, format, fd, offset, pitch, then the modifier when it is known.
    i32 attributes[24];
    GpuDmabufPlane plane{7, kDrmFormatGr88, 960, 544, 4096, 2048, 0x0100000000000001ull};
    u32 n = gpu_dmabuf_attributes(plane, true, attributes);
    check(n == 17 && attributes[0] == 0x3057 && attributes[1] == 960 && attributes[2] == 0x3056 && attributes[3] == 544 &&
              attributes[4] == 0x3271 && static_cast<u32>(attributes[5]) == kDrmFormatGr88 && attributes[6] == 0x3272 &&
              attributes[7] == 7 && attributes[8] == 0x3273 && attributes[9] == 4096 && attributes[10] == 0x3274 &&
              attributes[11] == 2048 && attributes[12] == 0x3443 && attributes[13] == 1 && attributes[14] == 0x3444 &&
              attributes[15] == 0x01000000 && attributes[16] == 0x3038,
          "layout: the EGL attribute list with the modifier");
    n = gpu_dmabuf_attributes(plane, false, attributes);
    check(n == 13 && attributes[12] == 0x3038, "layout: no modifier attributes without the extension");
    plane.modifier = kDrmFormatModifierInvalid;
    n = gpu_dmabuf_attributes(plane, true, attributes);
    check(n == 13 && attributes[12] == 0x3038, "layout: an unknown modifier is left out");
}

void profiles() noexcept {
    using namespace vaapi;
    static Driver d;
    clear_bytes(&d, sizeof(d));
    d.profiles[0][0] = kProfileH264High;
    d.profiles[0][1] = kProfileH264Main;
    d.profiles[0][2] = kProfileH264ConstrainedBaseline;
    check(h264_profile_for(d, 66) == kProfileH264ConstrainedBaseline && h264_profile_for(d, 77) == kProfileH264Main &&
              h264_profile_for(d, 100) == kProfileH264High,
          "profiles: each H.264 stream takes the lowest profile that holds it");
    d.profiles[0][2] = 0;
    check(h264_profile_for(d, 66) == kProfileH264Main, "profiles: a Baseline stream falls to Main when the driver lacks Baseline");
    d.profiles[0][0] = 0;
    check(h264_profile_for(d, 100) == 0 && h264_profile_for(d, 77) == kProfileH264Main,
          "profiles: a High stream needs a High decoder");
    d.profiles[0][1] = 0;
    check(h264_profile_for(d, 66) == 0, "profiles: no H.264 profile, none chosen");
}

struct PoolWork {
    GpuTexture* texture;
    GpuSrv* view;
    u32 rounds;
};

u32 hammer(void* argument) noexcept {
    auto* work = static_cast<PoolWork*>(argument);
    for (u32 i = 0; i < work->rounds; ++i) {
        GpuTexture* texture = work->texture;
        GpuSrv* view = work->view;
        gpu_retain(texture);
        gpu_retain(view);
        gpu_release(view);
        gpu_release(texture);
    }
    return 0;
}

void planes() noexcept {
    GpuDevice* device = reinterpret_cast<GpuDevice*>(1); // CPU planes are made without the device
    static GpuTexture* textures[kGpuVideoPlaneCapacity];
    static GpuSrv* views[kGpuVideoPlaneCapacity];
    u8* pixels = nullptr;
    u32 pitch = 0, made = 0;
    bool all_made = true;
    for (u32 i = 0; i < kGpuVideoPlaneCapacity; ++i) {
        const GpuStatus status = gpu_video_plane_create(device, i & 1 ? GpuVideoPlaneFormat::rg8 : GpuVideoPlaneFormat::r8,
                                                        16, 4, &textures[i], &views[i], &pixels, &pitch);
        all_made = all_made && gpu_ok(status) && textures[i] && views[i] && pixels && pitch == (i & 1 ? 32u : 16u) &&
                   gpu_video_plane_owns(textures[i]);
        made += gpu_ok(status);
    }
    check(all_made && made == kGpuVideoPlaneCapacity, "planes: the pool hands out every plane it has");
    GpuTexture* extra_texture = nullptr;
    GpuSrv* extra_view = nullptr;
    check(gpu_video_plane_create(device, GpuVideoPlaneFormat::r8, 16, 4, &extra_texture, &extra_view, &pixels, &pitch) ==
                  kGpuOutOfMemory &&
              !extra_texture && !extra_view,
          "planes: a full pool refuses without leaving anything behind");
    // Four threads retain and release one plane's handles while the owner holds its own: nothing is freed early.
    PoolWork work{textures[0], views[0], 20000};
    Thread* threads[4];
    for (Thread*& thread : threads)
        thread = thread_start(hammer, &work, 64 * 1024, nullptr);
    for (Thread* thread : threads) {
        thread_wait(thread, INFINITE);
        thread_close(thread);
    }
    pixels = nullptr;
    GpuStatus again = gpu_video_plane_create(device, GpuVideoPlaneFormat::r8, 16, 4, &extra_texture, &extra_view, &pixels, &pitch);
    check(again == kGpuOutOfMemory, "planes: concurrent retains and releases leave the counts exact");
    // Releasing the creator's references frees the slot, which is reused.
    gpu_release(views[5]);
    gpu_release(textures[5]);
    again = gpu_video_plane_create(device, GpuVideoPlaneFormat::r8, 16, 4, &extra_texture, &extra_view, &pixels, &pitch);
    check(gpu_ok(again) && extra_texture && pixels, "planes: a released plane's slot is reused");
    GpuTexture* reused_texture = extra_texture;
    GpuSrv* reused_view = extra_view;
    // A reference kept by another owner keeps the plane alive past the creator's release.
    gpu_retain(textures[7]);
    gpu_release(views[7]);
    gpu_release(textures[7]);
    check(gpu_video_plane_create(device, GpuVideoPlaneFormat::r8, 16, 4, &extra_texture, &extra_view, &pixels, &pitch) ==
              kGpuOutOfMemory,
          "planes: a retained plane is not reused");
    gpu_release(textures[7]);
    gpu_release(reused_view);
    gpu_release(reused_texture);
    for (u32 i = 0; i < kGpuVideoPlaneCapacity; ++i) {
        if (i == 5 || i == 7)
            continue;
        gpu_release(views[i]);
        gpu_release(textures[i]);
    }
    check(gpu_ok(gpu_video_plane_create(device, GpuVideoPlaneFormat::r8, 16, 4, &extra_texture, &extra_view, &pixels, &pitch)),
          "planes: every plane is back after the releases");
    gpu_release(extra_view);
    gpu_release(extra_texture);
}

bool has_argument(const char* name) noexcept {
    const ProcessStartup& startup = process_startup();
    for (int i = 1; i < startup.argc; ++i) {
        const char* a = startup.argv[i];
        u32 k = 0;
        while (a[k] && a[k] == name[k])
            ++k;
        if (a[k] == name[k])
            return true;
    }
    return false;
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_video_capability_entry) {
    using namespace maicord;
    // The environment cases run in a process of their own: the probe answers once. Each leaves no hardware codec, and
    // an advertised word with no software library holds nothing but what the libraries give.
    if (has_argument("--off") || has_argument("--no-library") || has_argument("--bad-node")) {
        check(!vaapi::driver(nullptr), "hardware tier: unavailable with this configuration");
        test_exit(g_failures, "maicord_vaapi_unavailable");
    }
    if (has_argument("--no-node")) {
        // A machine with a render node may well have a driver for it: only the node-less ones are a test.
        if (sys_open("/dev/dri/renderD128", kOpenReadWrite) >= 0) {
            test_write_line("skipped: this machine has a render node");
            test_process_exit(77);
        }
        check(!vaapi::driver(nullptr), "hardware tier: unavailable without a render node");
        test_exit(g_failures, "maicord_vaapi_no_node");
    }
    capability();
    offers();
    layout();
    profiles();
    planes();
    test_exit(g_failures, "maicord_video_capability");
}
