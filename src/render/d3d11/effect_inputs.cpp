#include "effect_inputs.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"

namespace maicord {
namespace {

// 64x64 tiling blue noise, one byte per texel (tools/generate_blue_noise.py).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc++26-extensions"
constexpr u8 kBlueNoise[4096] = {
#embed "../effects/blue_noise_64.bin"
};
#pragma clang diagnostic pop

constexpr u32 kNoiseExtent = 256, kBlueExtent = 64;

// PCG output function (O'Neill, pcg-random.org) over a counter: the same noise on every GPU and run.
constexpr u32 pcg(u32 index) noexcept {
    const u32 state = index * 747796405u + 2891336453u;
    const u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

GpuStatus make_view(GpuDevice* device, u32 width, u32 height, GpuFormat format, const void* pixels, u32 pitch,
                    GpuSrv** view) noexcept {
    const GpuInitialData data{pixels, pitch};
    GpuTexture* texture = nullptr;
    GpuStatus result = gpu_texture_create(device, {width, height, format, GpuUsage::immutable, gpu_bind_srv}, &data,
                                          &texture);
    if (SUCCEEDED(result))
        result = gpu_srv_create(device, texture, {GpuViewKind::texture, format}, view);
    gpu_release(texture);
    return result;
}

GpuStatus make_noise(GpuDevice* device, GpuSrv** view) noexcept {
    constexpr u32 count = kNoiseExtent * kNoiseExtent;
    auto* pixels = commit_pages<u32>(count * sizeof(u32));
    if (!pixels)
        return E_OUTOFMEMORY;
    for (u32 i = 0; i < count; ++i)
        pixels[i] = pcg(i);
    const GpuStatus result =
        make_view(device, kNoiseExtent, kNoiseExtent, GpuFormat::rgba8, pixels, kNoiseExtent * 4, view);
    pages_release(pixels);
    return result;
}

GpuStatus make_package_texture(GpuDevice* device, const EffectTexturePixels& texture,
                               GpuSrv** view) noexcept {
    // A declared texture the package did not ship reads as opaque black.
    constexpr u32 black = 0xFF000000u;
    const bool shipped = texture.pixels && texture.width && texture.height && texture.width <= kEffectTextureMaximumExtent &&
                         texture.height <= kEffectTextureMaximumExtent;
    if (!shipped)
        return make_view(device, 1, 1, GpuFormat::rgba8, &black, 4, view);
    return make_view(device, texture.width, texture.height,
                     texture.rgba32f ? GpuFormat::rgba32f : GpuFormat::rgba8, texture.pixels,
                     texture.width * (texture.rgba32f ? 16u : 4u), view);
}

} // namespace

GpuStatus effect_inputs_create(GpuDevice* device, const BackgroundEffectSpec& spec,
                               GpuSrv* glyph,
                               GpuSrv* (&views)[kD3D11EffectInputs]) noexcept {
    GpuStatus result = kGpuOk;
    if (spec.uses & effect_use_noise)
        result = make_noise(device, &views[0]);
    if (SUCCEEDED(result) && (spec.uses & effect_use_glyphs)) {
        if (!glyph)
            result = E_INVALIDARG;
        else
            views[1] = glyph;
    }
    if (SUCCEEDED(result) && (spec.uses & effect_use_blue_noise))
        result = make_view(device, kBlueExtent, kBlueExtent, GpuFormat::r8, kBlueNoise, kBlueExtent,
                           &views[2]);
    // TEXTURE0..3 at t3..t6.
    for (u32 i = 0; SUCCEEDED(result) && i < kEffectTextureCount; ++i)
        if (spec.uses & (i < 2 ? effect_use_texture0 << i : effect_use_texture2 << (i - 2)))
            result = make_package_texture(device, spec.textures[i], &views[3 + i]);
    if (FAILED(result))
        for (u32 i = 0; i < kD3D11EffectInputs; ++i) {
            // The glyph grid stays the caller's.
            if (i == 1)
                views[i] = nullptr;
            else
                gpu_release(views[i]);
        }
    return result;
}

} // namespace maicord
