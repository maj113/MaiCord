#include "maicord/ui/spinner.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
void test_spinner(void (*check)(bool, const char*) noexcept) noexcept {
    check(ui_spinner_phase(0, false) == 0 && ui_spinner_phase(31, false) == 0 &&
              ui_spinner_phase(32, false) == 1 && ui_spinner_phase(1024, false) == 0 &&
              ui_spinner_phase(512, true) == 0,
          "spinner: deterministic bounded phase respects reduced motion");
    check(ui_spinner_next_wake(31, true, false) == 32 && ui_spinner_next_wake(32, true, false) == 64 &&
              !ui_spinner_next_wake(32, false, false) && !ui_spinner_next_wake(32, true, true) &&
              !ui_spinner_next_wake(~u64{0}, true, false),
          "spinner: visible-only frame deadlines cannot wrap or create idle polling");
    PaintStream paint{};
    if (!paint_stream_initialize(&paint)) {
        check(false, "spinner: native paint stream initialization");
        return;
    }
    const UiRect bounds{10, 20, 42, 52};
    bool ok = paint_stream_reset(&paint, 80, 80) && paint_stream_push_clip(&paint, 10, 20, 42, 52) &&
              ui_spinner_paint(&paint, bounds, kDefaultTheme.text_primary, 0) &&
              paint_stream_pop_clip(&paint);
    check(ok && paint.count == 24 && paint.batch_count == 1 && paint.clip_depth == 1,
          "spinner: native round-capped arc uses one clipped batch and 24 existing paint instances");
    const u64 first = hash64_bytes(kHash64Seed, paint.quads, paint.count * sizeof(PaintQuad));
    bool clips = true;
    for (u32 i = 0; i < paint.batch_count; ++i) {
        const auto& clip = paint.clips[paint.batches[i].clip_index];
        clips = clips && clip.left == 10 && clip.top == 20 && clip.right == 42 && clip.bottom == 52;
    }
    check(clips, "spinner: every arc segment obeys the supplied native clip");
    ok = paint_stream_reset(&paint, 80, 80) && paint_stream_push_clip(&paint, 10, 20, 42, 52) &&
         ui_spinner_paint(&paint, bounds, kDefaultTheme.text_primary, 1) && paint_stream_pop_clip(&paint);
    check(ok && paint.count == 24 &&
              first != hash64_bytes(kHash64Seed, paint.quads, paint.count * sizeof(PaintQuad)),
          "spinner: the next frame rotates original geometry without text or bitmap assets");
    paint_stream_reset(&paint, 80, 80);
    check(!ui_spinner_paint(&paint, {0, 0, 1, 1}, kDefaultTheme.text_primary, 0) && !paint.count &&
              !ui_spinner_paint(&paint, bounds, kDefaultTheme.text_primary, 0, {-1, .18f}) && !paint.count,
          "spinner: invalid geometry and metrics reject before emitting paint");
    paint_stream_release(&paint);
}
} // namespace maicord
