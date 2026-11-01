#pragma once
// QA only: sizes of the glyph owner's GPU resources. The shipping client never reads them back.
#include "maicord/core/memory.hpp"
#include "../../src/render/text/d3d11_color_glyph_owner.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
// Sizes of the descriptor payloads, not the driver's allocation size or actual memory use.
struct D3D11GlyphGpuFootprint {
    u64 texture_bytes,buffer_bytes,mono_pixels,mono_commands;
    u32 mono_dispatches,color_dispatches; // These count raster jobs, not batched Dispatch calls.
};
inline bool d3d11_glyph_text_gpu_footprint(const D3D11GlyphTextOwner& owner, D3D11GlyphGpuFootprint* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    const auto texture = [&](ID3D11Texture2D* resource, DXGI_FORMAT format, u32 bytes) noexcept {
        if (!resource)
            return true;
        D3D11_TEXTURE2D_DESC d{};
        resource->GetDesc(&d);
        if (d.Format != format || d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1)
            return false;
        output->texture_bytes += static_cast<u64>(d.Width) * d.Height * bytes;
        return true;
    };
    const auto buffer = [&](ID3D11Buffer* resource) noexcept {
        if (resource) {
            D3D11_BUFFER_DESC d{};
            resource->GetDesc(&d);
            output->buffer_bytes += d.ByteWidth;
        }
    };
    if (!texture(owner.gpu.texture, DXGI_FORMAT_R8_UNORM, 1))
        return false;
    buffer(owner.gpu.command_buffer);
    output->mono_pixels = owner.gpu.dispatched_pixels;
    output->mono_commands = owner.gpu.dispatched_commands;
    output->mono_dispatches = owner.gpu.raster_count;
    if (owner.color) {
        const auto& g = owner.color->gpu;
        if (!texture(g.texture, DXGI_FORMAT_R8G8B8A8_UNORM, 4))
            return false;
        buffer(g.program);
        buffer(g.constants);
        output->color_dispatches = g.dispatches;
    }
    return true;
}
} // namespace maicord
