#pragma once

#include "maicord/render/glyph_raster.hpp"
#include "maicord/core/win32_types.hpp"

struct ID3D11Buffer;
struct ID3D11ComputeShader;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;

namespace maicord {

struct GlyphAtlasAllocation;

struct GlyphAtlasGpu {
    ID3D11Texture2D* texture;
    ID3D11ShaderResourceView* texture_view;
    ID3D11UnorderedAccessView* texture_uav;
    ID3D11Buffer* command_buffer;
    ID3D11ShaderResourceView* command_view;
    ID3D11ComputeShader* raster_shader;
    u64 dispatched_pixels;
    u64 dispatched_commands;
    u32 raster_count;
    u32 dispatch_count;
    u32 rejected;
    u16 command_capacity; // Configured maximum.
    u16 command_committed; // Current buffer elements; grows up to command_capacity.
    u16 width;
    u16 height;
};
static_assert(sizeof(GlyphAtlasGpu)==88);

enum class GlyphAtlasGpuError : u8 {
    none,
    invalid_input,
    unsupported_format,
    command_capacity,
    create_resource,
    upload,
};

struct GlyphAtlasGpuResult {
    HRESULT native_error;
    GlyphAtlasGpuError error;

    constexpr bool ok() const noexcept {
        return error == GlyphAtlasGpuError::none;
    }
};

constexpr u32 glyph_raster_batch_limit=8;
constexpr u32 glyph_raster_header_bytes=glyph_raster_batch_limit*sizeof(GlyphPathCommand);
// Finish before atlas reads; canceled jobs require caller cache invalidation.
struct GlyphRasterBatch {
    GlyphAtlasGpu* owner;ID3D11DeviceContext* context;
    u8* mapped=nullptr;
    u32 jobs[glyph_raster_batch_limit][7];
    u32 count=0,cursor=0,groups_x=0,groups_y=0;
    bool failed=false;
    GlyphRasterBatch(GlyphAtlasGpu* gpu,ID3D11DeviceContext* value) noexcept:owner(gpu),context(value){}
    GlyphRasterBatch(const GlyphRasterBatch&)=delete;
    GlyphRasterBatch& operator=(const GlyphRasterBatch&)=delete;
    ~GlyphRasterBatch() noexcept {cancel();}
    // A positive stroke half-width (pixels, < 128) also covers every contour's
    // centreline with round caps and joins; zero keeps text coverage unchanged.
    GlyphAtlasGpuResult add(const GlyphOutline&,const GlyphRasterBounds&,const GlyphAtlasAllocation&,float,u16,
                            float stroke_half_width=0) noexcept;
    bool finish() noexcept;
    void cancel() noexcept;
    bool pending() const noexcept {return count||mapped||failed;}
};

GlyphAtlasGpuResult glyph_atlas_gpu_initialize(GlyphAtlasGpu* gpu, ID3D11Device* device,
                                               u16 width, u16 height,
                                               u32 command_capacity) noexcept;
void glyph_atlas_gpu_release(GlyphAtlasGpu* gpu) noexcept;

} // namespace maicord
