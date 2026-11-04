#pragma once

#include "maicord/render/text/glyph_raster.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/render/text/glyph_atlas.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {

struct GlyphAtlasAllocation;

struct GlyphAtlasGpu {
    GpuTexture* texture;
    GpuSrv* texture_view;
    GpuUav* texture_uav;
    GpuBuffer* command_buffer;
    GpuSrv* command_view;
    GpuComputeShader* raster_shader;
    GpuComputeShader* culled_shader; // Unstroked text; null if its creation failed.
    u64 dispatched_pixels;
    u64 dispatched_commands;
    u32 raster_count;
    u32 dispatch_count;
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
    GpuStatus native_error;
    GlyphAtlasGpuError error;

    constexpr bool ok() const noexcept {
        return error == GlyphAtlasGpuError::none;
    }
};

// Jobs per dispatch; GLYPH_JOBS in glyph_atlas.hlsl. A raster dispatch is
// latency-bound, so a larger batch costs about the same as a small one. The
// header plus the initial 128 commands must stay under 4 KB: a dynamic buffer
// past that size made every Map(DISCARD) about 10x dearer on the CPU.
constexpr u32 glyph_raster_batch_limit=16;
constexpr u32 glyph_raster_header_bytes=glyph_raster_batch_limit*sizeof(GlyphPathCommand);
static_assert(glyph_raster_header_bytes+128*sizeof(GlyphPathCommand)<=4096);
// The tile-culling shader skips every command that cannot reach a thread group's
// tile, which shortens each thread's serial command chain. It pays for rasters at
// least this wide or tall, and for any raster of at least glyph_raster_culling_commands
// commands; below both, its per-command test costs more than it skips.
constexpr u32 glyph_raster_culling_extent=64,glyph_raster_culling_commands=32;
// Finish before atlas reads; canceled jobs require caller cache invalidation.
struct GlyphRasterBatch {
    GlyphAtlasGpu* owner;GpuContext* context;
    u8* mapped=nullptr;
    u32 jobs[glyph_raster_batch_limit][7];
    u32 count=0,cursor=0,groups_x=0,groups_y=0,peak_commands=0;
    // `large`: some job reaches glyph_raster_culling_extent. `stroked`: some job strokes; the
    // culling shader serves only unstroked text, whose output the reference probe pins exactly.
    bool failed=false,large=false,stroked=false;
    GlyphRasterBatch(GlyphAtlasGpu* gpu,GpuContext* value) noexcept:owner(gpu),context(value){}
    GlyphRasterBatch(const GlyphRasterBatch&)=delete;
    GlyphRasterBatch& operator=(const GlyphRasterBatch&)=delete;
    ~GlyphRasterBatch() noexcept {cancel();}
    // A positive stroke half-width (pixels, < 128) also covers every contour's
    // centreline with round caps and joins; zero keeps text coverage unchanged.
    // `pen_offset` (pixels, [0, 1)) draws the outline that far right of the tile's
    // integer origin; `bounds` must already enclose the shifted outline.
    GlyphAtlasGpuResult add(const GlyphOutline&,const GlyphRasterBounds&,const GlyphAtlasAllocation&,float,u16,
                            float stroke_half_width=0,float pen_offset=0) noexcept;
    bool finish() noexcept;
    void cancel() noexcept;
    bool pending() const noexcept {return count||mapped||failed;}
};

GlyphAtlasGpuResult glyph_atlas_gpu_initialize(GlyphAtlasGpu* gpu, GpuDevice* device,
                                               u16 width, u16 height,
                                               u32 command_capacity) noexcept;
void glyph_atlas_gpu_release(GlyphAtlasGpu* gpu) noexcept;
// Recreates an atlas texture (the glyph or COLR atlas, with its shader
// resource and unordered access views) `height` rows tall: copies the rows
// below min(old, new) and `moves` from the old texture, then releases the old
// resources. Unchanged on failure.
GpuStatus glyph_atlas_texture_resize(GpuDevice* device, GpuContext* context, GpuTexture** texture,
                                   GpuSrv** view, GpuUav** uav, u16 height,
                                   ConstSlice<GlyphAtlasRowMove> moves) noexcept;

} // namespace maicord
