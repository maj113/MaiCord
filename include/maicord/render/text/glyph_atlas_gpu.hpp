#pragma once

#include "maicord/render/text/glyph_raster.hpp"
#include "maicord/render/text/glyph_atlas.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {

struct GlyphAtlasAllocation;

struct GlyphAtlasGpu {
    GpuTexture* texture;
    GpuSrv* texture_view;
    GpuUav* texture_uav;
    // The renderer's glyph atlas is a paged surface (glyph_atlas_gpu_initialize_paged): rasters write its pages and
    // texture_view is its view; texture and texture_uav stay null.
    GpuPagedTexture* surface;
    GpuBuffer* command_buffer;
    GpuSrv* command_view;
    // Culls every raster's commands by tile (glyph_atlas.hlsl).
    GpuComputeShader* raster_shader;
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
// Finish before atlas reads; canceled jobs require caller cache invalidation. One dispatch writes one page, so a
// job on another page dispatches the queued ones first.
struct GlyphRasterBatch {
    GlyphAtlasGpu* owner;GpuContext* context;
    u8* mapped=nullptr;
    GpuUav* target=nullptr; // the queued jobs' page
    u32 jobs[glyph_raster_batch_limit][7];
    u32 count=0,cursor=0,groups_x=0,groups_y=0;
    bool failed=false;
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
// An atlas whose texture is a paged surface (R8) of `width` x `pages` pages of `page_rows` rows; its rows are backed
// with gpu_paged_texture_commit as the allocator commits them.
GlyphAtlasGpuResult glyph_atlas_gpu_initialize_paged(GlyphAtlasGpu* gpu, GpuDevice* device, u16 width, u16 page_rows,
                                                     u8 pages, u32 command_capacity) noexcept;
void glyph_atlas_gpu_release(GlyphAtlasGpu* gpu) noexcept;

} // namespace maicord
