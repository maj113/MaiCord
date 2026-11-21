// Draw submission bookkeeping: pipelines bind once per switch, shared state
// once per stream, scissors only when they change, and consecutive instances
// of one pass under the same scissor extend one draw.
#include "maicord/render/d3d11/draw_submission.hpp"
#include "maicord/render/media/renderer_media_draw_storage.hpp"
#include "../../support/test_output.hpp"

namespace maicord::draw_submission_test {
namespace {
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}

void test_runs() noexcept {
    const RasterClipRect a{0, 0, 100, 100}, b{100, 0, 200, 100};
    DrawRun run{a, 4, 10};
    check(draw_run_extend(&run, a, 10, 5) && run.first == 4 && run.end == 15, "run: contiguous same scissor extends");
    check(!draw_run_extend(&run, b, 15, 5) && run.end == 15, "run: another scissor starts a new draw");
    check(!draw_run_extend(&run, a, 16, 5) && run.end == 15, "run: a gap in the instances starts a new draw");
    DrawRun empty{};
    check(!draw_run_extend(&empty, {}, 0, 3), "run: an empty run never extends");
    DrawRun top{a, 1, ~0u - 1};
    check(!draw_run_extend(&top, a, ~0u - 1, 4) && top.end == ~0u - 1, "run: an extension never wraps");
}

void test_state() noexcept {
    DrawSubmissionState state{};
    draw_submission_begin(&state);
    check(draw_submission_bind_shared(&state) && !draw_submission_bind_shared(&state),
          "state: shared state binds once per stream");
    check(draw_submission_select(&state, DrawPipeline::paint) && !draw_submission_select(&state, DrawPipeline::paint) &&
              draw_submission_select(&state, DrawPipeline::glyph) && draw_submission_select(&state, DrawPipeline::paint),
          "state: a pipeline binds only when it changes");
    const RasterClipRect clip{1, 2, 3, 4};
    check(draw_submission_scissor(&state, clip) && !draw_submission_scissor(&state, clip) &&
              draw_submission_scissor(&state, {1, 2, 3, 5}),
          "state: a repeated scissor is not set again");
    draw_submission_reset(&state);
    check(draw_submission_bind_shared(&state) &&
              draw_submission_select(&state, DrawPipeline::paint) && draw_submission_scissor(&state, clip),
          "state: after another user binds, everything binds again");
    check(!draw_submission_rebind_blend(&state), "state: a full rebind restores the blend state");
    // The background effect: only the pipeline and the blend state bind again.
    draw_submission_reset_pipeline(&state, false);
    check(!draw_submission_bind_shared(&state) && draw_submission_rebind_blend(&state) &&
              !draw_submission_rebind_blend(&state) && draw_submission_select(&state, DrawPipeline::paint) &&
              !draw_submission_scissor(&state, clip),
          "state: after the effect, the pipeline and blend bind again, not the shared state or scissor");
    draw_submission_reset_pipeline(&state, false);
    draw_submission_reset(&state);
    check(draw_submission_bind_shared(&state) && !draw_submission_rebind_blend(&state),
          "state: a full reset after the effect binds the blend state once");
    draw_submission_begin(&state);
    draw_submission_reset_pipeline(&state, true);
    check(!draw_submission_rebind_blend(&state), "state: nothing bound, nothing to restore");
    check(draw_submission_bind_shared(&state), "state: a new stream assumes nothing bound");
    // Pixel constants: paint needs slot 0 (frame) and slot 1, glyphs only slot 1.
    int palette = 0, effects = 0;
    draw_submission_begin(&state);
    check(draw_submission_pixel_constants(&state, true, nullptr) && !draw_submission_pixel_constants(&state, true, nullptr) &&
              !draw_submission_pixel_constants(&state, false, nullptr),
          "constants: an unchanged binding is not repeated, whichever pipeline asks");
    check(draw_submission_pixel_constants(&state, false, &effects) &&
              draw_submission_pixel_constants(&state, true, &palette) &&
              !draw_submission_pixel_constants(&state, true, &palette) &&
              draw_submission_pixel_constants(&state, false, &effects),
          "constants: slot 1 rebinds when the other user took it");
    draw_submission_begin(&state);
    check(draw_submission_pixel_constants(&state, false, nullptr) && draw_submission_pixel_constants(&state, true, nullptr),
          "constants: glyphs binding slot 1 do not provide the frame constants");
    draw_submission_reset_pipeline(&state, false);
    check(draw_submission_pixel_constants(&state, true, nullptr) && !draw_submission_pixel_constants(&state, true, nullptr),
          "constants: the background effect's slot 0 is rebound once");
    draw_submission_reset_pipeline(&state, false);
    check(!draw_submission_pixel_constants(&state, false, nullptr),
          "constants: the background effect leaves slot 1 alone");
    check(draw_submission_input(&state, DrawPipeline::paint) && !draw_submission_input(&state, DrawPipeline::paint) &&
              draw_submission_input(&state, DrawPipeline::glyph),
          "input: layout and buffer bind once per owner");
    check(draw_submission_views(&state, DrawPipeline::glyph) && !draw_submission_views(&state, DrawPipeline::glyph) &&
              draw_submission_views(&state, DrawPipeline::media),
          "views: bound once per owner");
    draw_submission_reset_pipeline(&state, false);
    check(!draw_submission_input(&state, DrawPipeline::glyph) && !draw_submission_views(&state, DrawPipeline::media),
          "input: a built-in effect leaves the layout, buffer and views bound");
    draw_submission_reset_pipeline(&state, true);
    check(!draw_submission_input(&state, DrawPipeline::glyph) && draw_submission_views(&state, DrawPipeline::media),
          "views: a plugin effect clears them");
    draw_submission_reset(&state);
    check(draw_submission_input(&state, DrawPipeline::glyph) && draw_submission_views(&state, DrawPipeline::glyph),
          "input: another user's binds are rebound");
    draw_submission_reset(&state);
    check(draw_submission_pixel_constants(&state, false, nullptr), "constants: another user's binds are rebound");
}

// Media rows: whether a batch's quads all lie inside its clip, which lets the
// renderer draw neighbouring rows under the target scissor as one call.
void test_quads_within() noexcept {
    const RasterClipRect row_clip[]{{312, 60, 1040, 100}, {312, 100, 1040, 140}, {312, 140, 1040, 180},
                                    {312, 180, 1040, 220}};
    const MediaQuad quads[]{
        {320, 64, 360, 96},  {400, 64, 440, 96},  {320, 104, 360, 136}, {400, 104, 440, 136},
        {320, 144, 360, 176}, {400, 144, 440, 176}, {320, 170, 360, 230},
    };
    const auto inside = [&](const MediaQuad* q, u32 count, const RasterClipRect& clip, float scale = 1) noexcept {
        return draw_quads_within(q, sizeof(MediaQuad), count, clip, scale, 1280, 800, kDrawQuadNoSign);
    };
    check(inside(quads, 2, row_clip[0]) && inside(quads + 4, 2, row_clip[2]) && !inside(quads + 6, 1, row_clip[3]) &&
              !inside(quads, 2, row_clip[1]) && !inside(quads, 0, row_clip[0]),
          "rows: a batch is inside its clip only when every quad is");
    // Pixels rounded outward from a fractional clip admit a quad the DIP clip
    // misses by less than the rounding; a clip on the target's edge bounds nothing there.
    const MediaQuad edge[]{{-20, 0, 300, 30}};
    check(inside(quads, 2, {320, 64, 360, 96}, 1.25f) == false && inside(quads, 2, {399, 79, 551, 121}, 1.25f) &&
              inside(edge, 1, {0, 0, 400, 100}) && !inside(edge, 1, {0, 10, 400, 100}) &&
              !inside(edge, 1, {5, 0, 400, 100}),
          "rows: scissors reach the target edge, and rounded clips admit what they already draw");
    // A quad whose shader moves it beyond its rectangle (a glyph effect, its
    // red channel negative) is never taken as inside.
    MediaQuad moving[]{{320, 64, 360, 96}, {400, 64, 440, 96}};
    moving[1].u0 = -1;
    const u32 u0 = __builtin_offsetof(MediaQuad, u0);
    check(draw_quads_within(moving, sizeof(MediaQuad), 1, row_clip[0], 1.f, 1280, 800, u0) &&
              !draw_quads_within(moving, sizeof(MediaQuad), 2, row_clip[0], 1.f, 1280, 800, u0),
          "rows: a quad with a negative gate field keeps its clip scissor");
}
} // namespace
} // namespace maicord::draw_submission_test

extern "C" __declspec(noreturn) void WINAPI maicord_draw_submission_entry() noexcept {
    using namespace maicord::draw_submission_test;
    test_runs();
    test_state();
    test_quads_within();
    maicord::test_exit(failures, "draw submission");
}
