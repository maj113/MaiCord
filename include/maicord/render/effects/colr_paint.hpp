#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/render/effects/colr_program.hpp"
#include "maicord/render/text/glyph_atlas_gpu.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {

// The colour atlas is a paged surface (gpu_paged_texture_*): the renderer's shares the glyph allocator's pages
// (colr_paint_attach_surface, backed as the allocator commits); otherwise the first raster makes a one-page atlas of
// the size it names, backed whole.
struct ColrPaintGpu {
    GpuPagedTexture* surface;
    GpuSrv* view; // the surface's view, owned by it
    GpuBuffer* program;
    GpuSrv* program_view;
    GpuBuffer* constants;
    GpuComputeShader* shader;
    GpuComputeShader* wide_shader;
    GpuComputeShader* layers_shader;
    u32 capacity, dispatches, upload_cursor;
    u32 spatial_jobs;
    u16 width, height; // The atlas surface.
    bool append_upload;
};

// One compute job: where its glyph goes in the atlas, its outline origin and
// scale, and where its program sits in the upload buffer. 48 bytes, the
// shader's ColrJob.
struct ColrPaintJob {
    u32 x, y, width, height;
    float left, top, scale;
    u32 first, padding, nodes;
    u32 program_base, reserved;
};
static_assert(sizeof(ColrPaintJob) == 48);
// Matches COLR_JOB_CAPACITY in colr_paint.hlsl (the shader's constant buffer holds this many jobs).
inline constexpr u32 kColrPaintBatchJobs = 64;

// One immediate-context compute interval. Glyph jobs queue here and run as one
// dispatch (a group slice per job) when the queue fills, the kernel, owner or
// upload buffer changes, a job overlaps a queued one, or at flush/reset;
// reset also unbinds the compute resources.
struct ColrPaintBatch {
    GpuContext* context;
    ColrPaintGpu* gpu=nullptr;
    GpuComputeShader* shader=nullptr;
    GpuUav* output=nullptr; // the queued jobs' page
    u32 pending=0;
    bool bound=false;
    ColrPaintJob jobs[kColrPaintBatchJobs];
    explicit ColrPaintBatch(GpuContext* value) noexcept : context(value) {}
    ColrPaintBatch(const ColrPaintBatch&)=delete;
    ColrPaintBatch& operator=(const ColrPaintBatch&)=delete;
    ~ColrPaintBatch() noexcept {reset();}
    bool active() const noexcept {return gpu!=nullptr;}
    // Dispatches the queued jobs; false when the submission failed.
    bool flush() noexcept;
    void reset() noexcept;
};

// The colour atlas takes `atlas`'s pages (the glyph allocator's); its rows are backed with gpu_paged_texture_commit.
bool colr_paint_attach_surface(ColrPaintGpu* gpu, GpuDevice* device, const GlyphAtlasAllocator& atlas) noexcept;

bool colr_paint_rasterize(ColrPaintGpu* gpu, GpuContext* context, const ColrPaintProgram& program,
                          const GlyphRasterBounds& bounds, const GlyphAtlasAllocation& allocation,
                          float pixels_per_em, u16 atlas_width, u16 atlas_height, u16 padding,
                          ColrPaintBatch* batch=nullptr) noexcept;
#if !defined(_WIN32)
// Writes the premultiplied RGBA8 pixels of a decoded colour bitmap glyph (bitmap_color_glyph_build) into the colour
// atlas at `allocation`, its padding gutter transparent, as a paint program would have left it. `scratch` supplies the
// padded copy and is rewound; queued paint jobs are dispatched first. The same size rules as colr_paint_rasterize.
bool colr_paint_upload_pixels(ColrPaintGpu* gpu, GpuContext* context, const GlyphAtlasAllocation& allocation,
                              u16 atlas_width, u16 atlas_height, u16 padding, const u32* pixels, Arena* scratch,
                              ColrPaintBatch* batch=nullptr) noexcept;
#endif
void colr_paint_gpu_release(ColrPaintGpu* gpu) noexcept;
// Bakes immediately into a caller-provided RGBA UAV; the program, shader and
// upload buffers are shared with color text. The renderer's owned target remains unchanged.
bool colr_paint_rasterize_target(ColrPaintGpu* gpu, GpuContext* context,
    const ColrPaintProgram& program, const GlyphRasterBounds& bounds,
    const GlyphAtlasAllocation& allocation, float pixels_per_unit,
    u16 target_width, u16 target_height, u16 padding, GpuUav* target) noexcept;

} // namespace maicord
