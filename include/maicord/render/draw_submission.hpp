#pragma once

#include "maicord/render/raster_clip.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {

// Bookkeeping for one draw stream on the immediate context. The paint, glyph
// and media pipelines share the render target, rasterizer and blend state, the
// frame constants (VS b0) and the sampler: those bind once per stream, and a
// pass that switches pipelines binds only its own topology, input layout,
// vertex stream, shaders, pixel constants and views, and only when it draws.
// Anything else that binds over the shared state (the settings backdrop)
// resets the state, so the next pass binds everything again; the background
// effect replaces only the pipeline and the blend state.
enum class DrawPipeline : u8 {
    none,
    paint,
    glyph,
    media,
};

struct DrawSubmissionState {
    RasterClipRect scissor;
    DrawPipeline pipeline;
    bool shared_bound;
    bool scissor_valid;
    bool blend_stale;
    u32 pipeline_binds, shared_binds, scissor_sets, scissor_skips;
};

static_assert(sizeof(DrawSubmissionState) == 36);

// A new stream: nothing is assumed bound and its counters restart.
inline void draw_submission_begin(DrawSubmissionState* state) noexcept {
    reset_value<DrawSubmissionState>(state);
}

// Another user bound over the context: rebind everything at the next draw.
inline void draw_submission_reset(DrawSubmissionState* state) noexcept {
    state->pipeline = DrawPipeline::none;
    state->shared_bound = false;
    state->scissor_valid = false;
    state->blend_stale = false;
}

// The background effect bound its shaders, input, pixel constants, view and
// blend state: the next pass binds its pipeline and the blend state again.
inline void draw_submission_reset_pipeline(DrawSubmissionState* state) noexcept {
    state->pipeline = DrawPipeline::none;
    state->blend_stale = state->shared_bound;
}

// True when only the blend state must be bound again before drawing.
inline bool draw_submission_rebind_blend(DrawSubmissionState* state) noexcept {
    const bool stale = state->blend_stale;
    state->blend_stale = false;
    return stale;
}

// True when the shared state must be bound before drawing.
inline bool draw_submission_bind_shared(DrawSubmissionState* state) noexcept {
    if (state->shared_bound)
        return false;
    state->shared_bound = true;
    state->blend_stale = false;
    ++state->shared_binds;
    return true;
}

// True when `pipeline` is not the bound one and must be bound; it then is.
inline bool draw_submission_select(DrawSubmissionState* state, DrawPipeline pipeline) noexcept {
    if (state->pipeline == pipeline)
        return false;
    state->pipeline = pipeline;
    ++state->pipeline_binds;
    return true;
}

inline bool raster_clip_equal(const RasterClipRect& a, const RasterClipRect& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

// True when `scissor` differs from the bound rectangle and must be set.
inline bool draw_submission_scissor(DrawSubmissionState* state, const RasterClipRect& scissor) noexcept {
    if (state->scissor_valid && raster_clip_equal(state->scissor, scissor)) {
        ++state->scissor_skips;
        return false;
    }
    state->scissor = scissor;
    state->scissor_valid = true;
    ++state->scissor_sets;
    return true;
}

// Consecutive instance ranges of one pass under the same scissor draw as one
// call: the instances, their order and every pixel they touch are unchanged.
struct DrawRun {
    RasterClipRect scissor;
    u32 first, end;
};

// Extends a non-empty `run` by [first, first + count) when that continues it
// under the same scissor. Otherwise the run is unchanged and the caller draws
// it before starting the next one.
inline bool draw_run_extend(DrawRun* run, const RasterClipRect& scissor, u32 first, u32 count) noexcept {
    if (run->end <= run->first || run->end != first || count > ~0u - first ||
        !raster_clip_equal(run->scissor, scissor))
        return false;
    run->end = first + count;
    return true;
}

} // namespace maicord
