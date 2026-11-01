#pragma once
// Fixture uploads for the UI probes. The shipping renderer uploads straight RGBA8 (the GPU premultiplies);
// the probe fixtures are premultiplied, so each upload un-premultiplies into a scratch copy first.
// Opaque and fully transparent pixels pass through unchanged.
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_error.hpp"
#include "maicord/render/d3d11/renderer.hpp"

namespace maicord {
// Successful fixture uploads through renderer_upload_media_rgba8.
inline u32 g_probe_rgba8_uploads;

inline MediaAtlasGpuResult renderer_upload_media_rgba8(D3D11Renderer* renderer, MediaAssetKey key, u16 width,
                                                       u16 height, u32 row_pitch,
                                                       ConstSlice<u8> premultiplied_rgba8) noexcept {
    const u64 minimum_pitch = static_cast<u64>(width) * 4;
    const u64 required = height == 0 ? 0 : static_cast<u64>(height - 1) * row_pitch + minimum_pitch;
    u8* straight = nullptr;
    ConstSlice<u8> input = premultiplied_rgba8;
    // Invalid shapes go to the shipping upload unchanged so it rejects them.
    if (width != 0 && height != 0 && row_pitch >= minimum_pitch && required <= premultiplied_rgba8.count &&
        premultiplied_rgba8.data) {
        straight = static_cast<u8*>(
            VirtualAlloc(nullptr, static_cast<SIZE_T>(required), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!straight)
            return MediaAtlasGpuResult{{}, E_OUTOFMEMORY, MediaAtlasGpuError::device_failure};
        for (u32 row = 0; row < height; ++row) {
            const u8* source = premultiplied_rgba8.data + static_cast<u64>(row) * row_pitch;
            u8* target = straight + static_cast<u64>(row) * row_pitch;
            for (u32 x = 0; x < width; ++x, source += 4, target += 4) {
                const u32 alpha = source[3];
                for (u32 c = 0; c < 3; ++c)
                    target[c] = alpha == 0 ? 0
                                           : alpha == 255 ? source[c]
                                                          : static_cast<u8>(minimum<u32>(
                                                                255, (source[c] * 255u + alpha / 2) / alpha));
                target[3] = static_cast<u8>(alpha);
            }
        }
        input = ConstSlice<u8>{straight, static_cast<u32>(required)};
    }
    const MediaAtlasGpuResult result =
        renderer_upload_media_straight_rgba8(renderer, key, width, height, row_pitch, input);
    if (straight)
        VirtualFree(straight, 0, MEM_RELEASE);
    g_probe_rgba8_uploads += result.ok();
    return result;
}
} // namespace maicord
