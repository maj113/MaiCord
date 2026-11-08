// The GPU VP8 gate on OpenGL ES. Registered with MAICORD_GLES_MAX_COMPUTE_INVOCATIONS=128, the device reports the
// group size the specification guarantees, so it fails media_atlas_gpu_vp8_supported (vp8_predict needs 512
// invocations), and the media pipeline, told so (account_media_worker_set_vp8_cpu), decodes lossy WebP stills and
// lossy animation frames on the CPU into straight RGBA8: no VP8 frame reaches the renderer and no VP8 shader is made.
// Without the gate the same inputs decode to VP8 keyframes for the GPU.
#include "maicord/app/media/cdn_media_pipeline.hpp"
#include "maicord/core/log.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/media/core/encoded_cache.hpp"
#include "maicord/protocol/media/cdn_http.hpp"
#include "maicord/render/media/atlas_gpu.hpp"
#include "../../fixtures/webp_fixture.hpp"
#include "../../support/media_fixture.hpp"
#include "../../support/test_output.hpp"
#include "../../support/transport_test_shared.hpp"

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

u8 g_encoded[17'000];
alignas(16) u8 g_scanlines[64 * 1024];
Vp8lWorkspace g_vp8l;
LossyWebpWorkspace g_lossy;
AnimatedWebpWorkspace g_animated;
EncodedMediaCacheEntry g_entries[4];
u16 g_page_owners[8];
u8 g_storage[8 * kEncodedMediaCachePageBytes];
u32 g_buckets[8];
EncodedMediaCache g_cache;
CdnMediaPipeline g_pipeline;

CdnResponseResult response(u32 bytes, u64 now_ms) noexcept {
    return validate_static_image_response(CdnImageType::webp,
                                          byte_literal("HTTP/1.1 200 OK\r\n"
                                                       "Content-Type: image/webp\r\n"
                                                       "Transfer-Encoding: chunked\r\n"
                                                       "ETag: \"vp8-route\"\r\n"
                                                       "Cache-Control: max-age=60\r\n\r\n"),
                                          bytes, now_ms);
}

void run() noexcept {
    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    check(gpu_ok(gpu_device_create(&device, &context)) && device, "device creation");
    if (!device)
        return;
    const bool vp8_gpu = media_atlas_gpu_vp8_supported(gpu_caps(device));
    test_write_metric("caps max_compute_invocations=", gpu_caps(device).max_compute_invocations);
    check(!vp8_gpu, "a 128-invocation device has no GPU VP8 path");

    check(encoded_media_cache_initialize(&g_cache, {g_entries, 4}, {g_page_owners, 8}, {g_storage, sizeof(g_storage)},
                                         {g_buckets, 8}) &&
              cdn_media_pipeline_initialize(&g_pipeline, &g_cache, {g_scanlines, sizeof(g_scanlines)},
                                            sizeof(g_scanlines)),
          "pipeline");
    g_pipeline.webp_workspace = &g_vp8l;
    g_pipeline.lossy_webp_workspace = &g_lossy;
    g_pipeline.animated_webp_workspace = &g_animated;
    // What a decode lane takes from the worker before each decode.
    g_pipeline.vp8_on_cpu = !vp8_gpu;

    CdnStaticWebpResult still{};
    cdn_media_pipeline_admit_static_webp(&still, &g_pipeline, media_asset_custom_emoji(1, false),
                                         response(sizeof(kWebpLossy4x4), 1'000),
                                         ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)});
    check(still.decoded() && still.image.codec == WebpCodec::none && still.image.width == 4 &&
              still.image.height == 4 && still.rgba8.count == 4u * 4u * 4u && still.vp8_frame.width == 0,
          "a lossy WebP still decodes on the CPU into RGBA8");

    AnimatedWebpFixtureResult fixture{};
    check(build_mixed_animated_webp_fixture(Slice<u8>{g_encoded, sizeof(g_encoded)}, &fixture), "animation fixture");
    const MediaAssetKey animated = media_asset_custom_emoji(2, true);
    CdnAnimatedWebpResult admitted{};
    cdn_media_pipeline_admit_animated_webp(&admitted, &g_pipeline, animated, response(fixture.encoded_bytes, 2'000),
                                           ConstSlice<u8>{g_encoded, fixture.encoded_bytes});
    CdnAnimatedWebpResult lossy{};
    cdn_media_pipeline_decode_animated_webp_frame(&lossy, &g_pipeline, animated, admitted.cache_generation, 1, 2'010);
    CdnAnimatedWebpResult with_alpha{};
    cdn_media_pipeline_decode_animated_webp_frame(&with_alpha, &g_pipeline, animated, admitted.cache_generation, 2,
                                                  2'020);
    bool alpha_exact = with_alpha.rgba8.count == 64;
    for (u32 index = 3; index < with_alpha.rgba8.count; index += 4)
        alpha_exact = alpha_exact && with_alpha.rgba8[index] == 128;
    check(admitted.decoded() && lossy.decoded() && lossy.decode.frame.codec == AnimatedWebpCodec::vp8_lossy &&
              lossy.rgba8.count == 64 && lossy.alpha.count == 0 && lossy.vp8_frame.width == 0,
          "a lossy animation frame decodes on the CPU into RGBA8");
    check(with_alpha.decoded() && with_alpha.decode.frame.has_alpha && with_alpha.alpha.count == 0 && alpha_exact,
          "a lossy animation frame's ALPH plane applies on the CPU");

    // The GPU path for comparison: the same still becomes a VP8 keyframe for the renderer.
    g_pipeline.vp8_on_cpu = false;
    CdnStaticWebpResult gpu{};
    cdn_media_pipeline_admit_static_webp(&gpu, &g_pipeline, media_asset_custom_emoji(3, false),
                                         response(sizeof(kWebpLossy4x4), 3'000),
                                         ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)});
    check(gpu.decoded() && gpu.image.codec == WebpCodec::vp8_lossy && gpu.vp8_frame.width == 4 && gpu.rgba8.count == 0,
          "with a GPU VP8 path the still stays a VP8 keyframe");
    gpu_device_destroy(&device, &context);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_vp8_cpu_route_entry) {
    maicord::log_open();
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_vp8_cpu_route");
}
