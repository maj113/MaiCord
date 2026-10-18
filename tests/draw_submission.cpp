// Draw submission bookkeeping: pipelines bind once per switch, shared state
// once per stream, scissors only when they change, and consecutive batches of
// one pass under the same scissor draw as one call.
#include "maicord/render/draw_submission.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::draw_submission_test {
namespace {
u32 failures{};
void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}

struct Batch {
    u32 first, count;
    RasterClipRect clip;
};

// The renderer's pass loop over synthetic batches: a batch whose clip misses
// the damage is skipped, the rest are clipped to it and merged into runs.
struct Pass {
    u32 draws, instances, scissor_calls;
};
Pass submit_pass(DrawSubmissionState* state, DrawPipeline pipeline, const Batch* batches, u32 count,
                 RasterClipRect damage, u32* pipeline_calls) noexcept {
    Pass pass{};
    DrawRun run{};
    const auto draw = [&](const DrawRun& next) noexcept {
        if (next.end <= next.first)
            return;
        draw_submission_bind_shared(state);
        if (draw_submission_select(state, pipeline))
            ++*pipeline_calls;
        if (draw_submission_scissor(state, next.scissor))
            ++pass.scissor_calls;
        ++pass.draws;
        pass.instances += next.end - next.first;
    };
    for (u32 i = 0; i < count; ++i) {
        const RasterClipRect clip{maximum(batches[i].clip.left, damage.left), maximum(batches[i].clip.top, damage.top),
                                  minimum(batches[i].clip.right, damage.right),
                                  minimum(batches[i].clip.bottom, damage.bottom)};
        if (clip.left >= clip.right || clip.top >= clip.bottom || !batches[i].count)
            continue;
        if (!draw_run_extend(&run, clip, batches[i].first, batches[i].count)) {
            draw(run);
            run = {clip, batches[i].first, batches[i].first + batches[i].count};
        }
    }
    draw(run);
    return pass;
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
    check(draw_submission_bind_shared(&state) && !draw_submission_bind_shared(&state) && state.shared_binds == 1,
          "state: shared state binds once per stream");
    check(draw_submission_select(&state, DrawPipeline::paint) && !draw_submission_select(&state, DrawPipeline::paint) &&
              draw_submission_select(&state, DrawPipeline::glyph) &&
              draw_submission_select(&state, DrawPipeline::paint) && state.pipeline_binds == 3,
          "state: a pipeline binds only when it changes");
    const RasterClipRect clip{1, 2, 3, 4};
    check(draw_submission_scissor(&state, clip) && !draw_submission_scissor(&state, clip) &&
              draw_submission_scissor(&state, {1, 2, 3, 5}) && state.scissor_sets == 2 && state.scissor_skips == 1,
          "state: a repeated scissor is not set again");
    draw_submission_reset(&state);
    check(state.pipeline == DrawPipeline::none && draw_submission_bind_shared(&state) &&
              draw_submission_select(&state, DrawPipeline::paint) && draw_submission_scissor(&state, clip) &&
              state.shared_binds == 2 && state.pipeline_binds == 4,
          "state: after another user binds, everything binds again");
    check(!draw_submission_rebind_blend(&state), "state: a full rebind restores the blend state");
    // The background effect: only the pipeline and the blend state bind again.
    draw_submission_reset_pipeline(&state);
    check(
        !draw_submission_bind_shared(&state) && draw_submission_rebind_blend(&state) &&
            !draw_submission_rebind_blend(&state) && draw_submission_select(&state, DrawPipeline::paint) &&
            !draw_submission_scissor(&state, clip) && state.shared_binds == 2 && state.pipeline_binds == 5,
        "state: after the effect, the pipeline and blend bind again, not the shared state or scissor");
    draw_submission_reset_pipeline(&state);
    draw_submission_reset(&state);
    check(draw_submission_bind_shared(&state) && !draw_submission_rebind_blend(&state),
          "state: a full reset after the effect binds the blend state once");
    draw_submission_begin(&state);
    draw_submission_reset_pipeline(&state);
    check(!draw_submission_rebind_blend(&state), "state: nothing bound, nothing to restore");
    check(!state.pipeline_binds && !state.shared_binds && !state.scissor_sets && !state.shared_bound,
          "state: a new stream restarts its counters");
}

// A frame shaped like the shell: paint under the root, rail, channel list,
// header, chat (with its message rows nested inside) and member clips, glyphs
// under four of them, media under three, then a popup's paint.
void test_frame() noexcept {
    const RasterClipRect rail{0, 0, 72, 800}, channels{72, 0, 312, 800}, chat{312, 48, 1040, 760},
        rows{312, 60, 1040, 740}, header{312, 0, 1040, 48}, members{1040, 0, 1280, 800}, popup{700, 400, 900, 600};
    const Batch paint[]{{0, 1, {0, 0, 1280, 800}}, {1, 30, rail},   {31, 80, channels}, {111, 4, header},
                        {115, 200, chat},           {315, 50, rows}, {365, 90, members}};
    const Batch glyphs[]{{0, 400, channels}, {400, 50, header}, {450, 2000, chat}, {2450, 300, members}};
    const Batch media[]{{0, 20, rail}, {20, 6, chat}, {26, 40, members}};
    const Batch overlay[]{{455, 12, popup}, {467, 3, popup}};
    const RasterClipRect full{0, 0, 1280, 800};
    DrawSubmissionState state{};
    draw_submission_begin(&state);
    u32 pipelines = 0;
    Pass p = submit_pass(&state, DrawPipeline::paint, paint, 7, full, &pipelines);
    check(p.draws == 7 && p.instances == 455 && p.scissor_calls == 7, "frame: full paint draws each clip");
    p = submit_pass(&state, DrawPipeline::media, media, 3, full, &pipelines);
    check(p.draws == 3 && p.instances == 66 && p.scissor_calls == 3, "frame: full media draws each clip");
    p = submit_pass(&state, DrawPipeline::glyph, glyphs, 4, full, &pipelines);
    check(p.draws == 4 && p.instances == 2750 && p.scissor_calls == 4, "frame: full glyphs draw each clip");
    p = submit_pass(&state, DrawPipeline::paint, overlay, 2, full, &pipelines);
    check(p.draws == 1 && p.instances == 15 && p.scissor_calls == 1, "frame: two batches under one popup clip draw once");
    check(pipelines == 4 && state.pipeline_binds == 4 && state.shared_binds == 1,
          "frame: four passes bind four pipelines and the shared state once");

    // An animated emoji in the chat: every batch overlapping the damage clips
    // to the same rectangle. The chat background and its rows are contiguous
    // and draw as one call; the root fill is separated by skipped panels.
    const RasterClipRect emoji{600, 300, 622, 322};
    draw_submission_begin(&state);
    pipelines = 0;
    p = submit_pass(&state, DrawPipeline::paint, paint, 7, emoji, &pipelines);
    check(p.draws == 2 && p.instances == 251 && p.scissor_calls == 1,
          "damage: the chat background and its rows draw as one call");
    p = submit_pass(&state, DrawPipeline::media, media, 3, emoji, &pipelines);
    check(p.draws == 1 && p.instances == 6 && p.scissor_calls == 0, "damage: media keeps the damage scissor");
    p = submit_pass(&state, DrawPipeline::glyph, glyphs, 4, emoji, &pipelines);
    check(p.draws == 1 && p.instances == 2000 && p.scissor_calls == 0, "damage: glyphs keep the damage scissor");
    p = submit_pass(&state, DrawPipeline::paint, overlay, 2, emoji, &pipelines);
    check(p.draws == 0 && pipelines == 3 && state.pipeline_binds == 3,
          "damage: a pass with nothing under the damage binds nothing");
    check(state.scissor_sets == 1 && state.scissor_skips == 3, "damage: one scissor serves the whole frame");

    // Damage across the chat and member list: each keeps its own scissor.
    const RasterClipRect split{1000, 100, 1100, 140};
    draw_submission_begin(&state);
    pipelines = 0;
    p = submit_pass(&state, DrawPipeline::paint, paint, 7, split, &pipelines);
    check(p.draws == 3 && p.instances == 341 && p.scissor_calls == 3,
          "split damage: the chat and the member list keep their own scissors");
}
} // namespace
} // namespace maicord::draw_submission_test

extern "C" __declspec(noreturn) void WINAPI maicord_draw_submission_entry() noexcept {
    using namespace maicord::draw_submission_test;
    test_runs();
    test_state();
    test_frame();
    const bool failed = failures != 0;
    constexpr char pass[] = "PASS draw submission\n", fail[] = "FAIL draw submission\n";
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? fail : pass, sizeof(pass) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
