#include "effect_inputs.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_kernel.hpp"

#include "maicord/sys/d3d11.hpp"

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

HRESULT make_view(ID3D11Device* device, u32 width, u32 height, DXGI_FORMAT format, const void* pixels,
                  u32 pitch, ID3D11ShaderResourceView** view) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{pixels, pitch, 0};
    ID3D11Texture2D* texture = nullptr;
    HRESULT result = device->CreateTexture2D(&description, &data, &texture);
    if (SUCCEEDED(result))
        result = device->CreateShaderResourceView(texture, nullptr, view);
    com_release(texture);
    return result;
}

HRESULT make_noise(ID3D11Device* device, ID3D11ShaderResourceView** view) noexcept {
    constexpr u32 count = kNoiseExtent * kNoiseExtent;
    auto* pixels = commit_pages<u32>(count * sizeof(u32));
    if (!pixels)
        return E_OUTOFMEMORY;
    for (u32 i = 0; i < count; ++i)
        pixels[i] = pcg(i);
    const HRESULT result =
        make_view(device, kNoiseExtent, kNoiseExtent, DXGI_FORMAT_R8G8B8A8_UNORM, pixels, kNoiseExtent * 4, view);
    VirtualFree(pixels, 0, MEM_RELEASE);
    return result;
}

HRESULT make_package_texture(ID3D11Device* device, const EffectTexturePixels& texture,
                             ID3D11ShaderResourceView** view) noexcept {
    // A declared texture the package did not ship reads as opaque black.
    constexpr u32 black = 0xFF000000u;
    const bool shipped = texture.pixels && texture.width && texture.height && texture.width <= kEffectTextureMaximumExtent &&
                         texture.height <= kEffectTextureMaximumExtent;
    if (!shipped)
        return make_view(device, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, &black, 4, view);
    return make_view(device, texture.width, texture.height,
                     texture.rgba32f ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM, texture.pixels,
                     texture.width * (texture.rgba32f ? 16u : 4u), view);
}

} // namespace

HRESULT effect_inputs_create(ID3D11Device* device, const BackgroundEffectSpec& spec,
                             ID3D11ShaderResourceView* glyph,
                             ID3D11ShaderResourceView* (&views)[kD3D11EffectInputs]) noexcept {
    HRESULT result = S_OK;
    if (spec.uses & effect_use_noise)
        result = make_noise(device, &views[0]);
    if (SUCCEEDED(result) && (spec.uses & effect_use_glyphs)) {
        if (!glyph)
            result = E_INVALIDARG;
        else
            views[1] = glyph;
    }
    if (SUCCEEDED(result) && (spec.uses & effect_use_blue_noise))
        result = make_view(device, kBlueExtent, kBlueExtent, DXGI_FORMAT_R8_UNORM, kBlueNoise, kBlueExtent,
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
                com_release(views[i]);
        }
    return result;
}

} // namespace maicord
