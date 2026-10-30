#pragma once

#include "maicord/render/raster_clip.hpp"
#include "maicord/render/renderer_media_draw_storage.hpp"
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
    // The pipeline whose input layout and instance buffer, and whose
    // pixel-stage views, are bound.
    DrawPipeline input_pipeline, views_pipeline;
    bool shared_bound;
    bool scissor_valid;
    bool blend_stale;
    // Pixel constant slot 0 holds the frame constants; slot 1 holds
    // `ps_slot1` (the background palette or the text effects, else null).
    bool ps_frame_bound;
    bool ps_slot1_valid;
    const void* ps_slot1;
    u32 pipeline_binds, shared_binds, scissor_sets, scissor_skips;
};

static_assert(sizeof(DrawSubmissionState) == 48);

// A new stream: nothing is assumed bound and its counters restart.
inline void draw_submission_begin(DrawSubmissionState* state) noexcept {
    reset_value<DrawSubmissionState>(state);
}

// Another user bound over the context: rebind everything at the next draw.
inline void draw_submission_reset(DrawSubmissionState* state) noexcept {
    state->pipeline = DrawPipeline::none;
    state->input_pipeline = DrawPipeline::none;
    state->views_pipeline = DrawPipeline::none;
    state->shared_bound = false;
    state->scissor_valid = false;
    state->blend_stale = false;
    state->ps_frame_bound = false;
    state->ps_slot1_valid = false;
}

// The background effect bound its shaders, pixel constant slot 0 and blend
// state, and cleared the pixel views when `clears_views` (a plugin shader gets
// none). It left the input layout, instance buffer and constant slot 1 alone:
// the next pass binds only what changed.
inline void draw_submission_reset_pipeline(DrawSubmissionState* state, bool clears_views) noexcept {
    state->pipeline = DrawPipeline::none;
    state->blend_stale = state->shared_bound;
    state->ps_frame_bound = false;
    if (clears_views)
        state->views_pipeline = DrawPipeline::none;
}

// True when a pipeline needing the frame constants in slot 0 (`frame`) and
// `slot1` in slot 1 must bind them; they are then recorded as bound.
inline bool draw_submission_pixel_constants(DrawSubmissionState* state, bool frame, const void* slot1) noexcept {
    const bool stale = (frame && !state->ps_frame_bound) || !state->ps_slot1_valid || state->ps_slot1 != slot1;
    if (stale) {
        state->ps_frame_bound |= frame;
        state->ps_slot1_valid = true;
        state->ps_slot1 = slot1;
    }
    return stale;
}

// True when `pipeline`'s input layout and instance buffer are not the bound
// ones; they then are.
inline bool draw_submission_input(DrawSubmissionState* state, DrawPipeline pipeline) noexcept {
    if (state->input_pipeline == pipeline)
        return false;
    state->input_pipeline = pipeline;
    return true;
}

// True when `pipeline`'s pixel-stage views are not the bound ones; they then are.
inline bool draw_submission_views(DrawSubmissionState* state, DrawPipeline pipeline) noexcept {
    if (state->views_pipeline == pipeline)
        return false;
    state->views_pipeline = pipeline;
    return true;
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

// Value of `sign_offset` for quads without a sign-gated field.
inline constexpr u32 kDrawQuadNoSign = ~0u;

// True when `count` > 0 quads (`stride` bytes apart, each starting with
// left, top, right, bottom in DIPs) all lie inside `clip`, a scissor in target
// pixels at `scale` pixels per DIP. The scissor of such a batch only
// duplicates what the geometry already bounds, so it may use the whole target
// (and merge with its neighbours) without changing a pixel. A clip edge on
// the target's own edge bounds nothing: the target clips there. The float at
// byte `sign_offset` of a quad (a glyph's red channel) must not be negative:
// a negative one selects a shader that draws beyond the rectangle.
inline bool draw_quads_within(const void* quads, u32 stride, u32 count, const RasterClipRect& clip, float scale,
                              u32 width, u32 height, u32 sign_offset) noexcept {
    if (!count)
        return false;
    constexpr float kUnbounded = 1.0e30f;
    const float left = clip.left <= 0 ? -kUnbounded : static_cast<float>(clip.left);
    const float top = clip.top <= 0 ? -kUnbounded : static_cast<float>(clip.top);
    const float right = clip.right >= static_cast<i32>(width) ? kUnbounded : static_cast<float>(clip.right);
    const float bottom = clip.bottom >= static_cast<i32>(height) ? kUnbounded : static_cast<float>(clip.bottom);
    const u8* quad = static_cast<const u8*>(quads);
    for (u32 i = 0; i < count; ++i, quad += stride) {
        const float* rectangle = reinterpret_cast<const float*>(quad);
        if (!(rectangle[0] * scale >= left && rectangle[1] * scale >= top && rectangle[2] * scale <= right &&
              rectangle[3] * scale <= bottom))
            return false;
        if (sign_offset != kDrawQuadNoSign && !(*reinterpret_cast<const float*>(quad + sign_offset) >= 0))
            return false;
    }
    return true;
}

} // namespace maicord
