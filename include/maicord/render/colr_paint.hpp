#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/render/colr_program.hpp"
#include "maicord/render/glyph_atlas_gpu.hpp"

namespace maicord {

struct ColrPaintGpu {
    ID3D11Texture2D* texture;
    ID3D11ShaderResourceView* view;
    ID3D11UnorderedAccessView* uav;
    ID3D11Buffer* program;
    ID3D11ShaderResourceView* program_view;
    ID3D11Buffer* constants;
    ID3D11ComputeShader* shader;
    ID3D11ComputeShader* wide_shader;
    ID3D11ComputeShader* layers_shader;
    u32 capacity, dispatches, upload_cursor;
    u32 spatial_jobs;
    bool append_upload;
};

// One immediate-context compute interval. Deferred callers must finish before
// drawing and propagate failure. Cancellation/destruction drops queued jobs;
// their caller must invalidate pending cache entries. Reset finishes and unbinds.
struct ColrPaintBatch {
    ID3D11DeviceContext* context;
    ID3D11ShaderResourceView* program_view=nullptr;
    ColrPaintGpu* gpu=nullptr;
    explicit ColrPaintBatch(ID3D11DeviceContext* value) noexcept : context(value) {}
    ColrPaintBatch(const ColrPaintBatch&)=delete;
    ColrPaintBatch& operator=(const ColrPaintBatch&)=delete;
    ~ColrPaintBatch() noexcept {cancel();}
    bool active() const noexcept {return gpu!=nullptr;}
    void reset() noexcept;
    bool finish() noexcept;
    void cancel() noexcept;
};

bool colr_paint_rasterize(ColrPaintGpu* gpu, ID3D11DeviceContext* context, const ColrPaintProgram& program,
                          const GlyphRasterBounds& bounds, const GlyphAtlasAllocation& allocation,
                          float pixels_per_em, u16 atlas_extent, u16 padding, ColrPaintBatch* batch=nullptr,
                          bool external_target=false) noexcept;
void colr_paint_gpu_release(ColrPaintGpu* gpu) noexcept;
// Bakes immediately into a caller-provided RGBA UAV; the program, shader and
// upload buffers are shared with color text. Restores the renderer's target before returning.
bool colr_paint_rasterize_target(ColrPaintGpu* gpu, ID3D11DeviceContext* context,
    const ColrPaintProgram& program, const GlyphRasterBounds& bounds,
    const GlyphAtlasAllocation& allocation, float pixels_per_unit, u16 owner_extent,
    u16 target_width, u16 target_height, u16 padding, ID3D11UnorderedAccessView* target) noexcept;

} // namespace maicord
