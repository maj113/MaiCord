#include "maicord/app/animation_visual_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
void test_animation_modal_pause(void (*check)(bool, const char*) noexcept) noexcept {
    AnimationVisualRuntime runtime{};
    if (!animation_visual_runtime_initialize(&runtime)) {
        check(false, "modal animation runtime initializes");
        return;
    }
    auto& timeline = runtime.timeline;
    const auto cached = media_asset_custom_emoji(901, true);
    const auto decoding = media_asset_custom_emoji(902, true);
    const auto hidden = media_asset_custom_emoji(903, true);
    check(animation_visual_runtime_publish(&runtime, {cached, 40000, 1, 3, 0, true}, 1000).ok() &&
              animation_visual_runtime_publish(&runtime, {decoding, 10000, 2, 3, 0, true}, 1000).ok() &&
              animation_visual_runtime_publish(&runtime, {hidden, 50000, 3, 3, 0, false}, 1000).ok(),
          "modal animation fixtures publish");
    auto collected = animation_timeline_collect_due(&timeline, 11000, {runtime.pending_advances, 1});
    runtime.pending_advance_count = collected.advances;
    check(collected.advances == 1 && runtime.pending_advances[0].frame_index == 1,
          "modal animation has a codec frame in flight");
    animation_visual_runtime_suspend(&runtime, 12000);
    animation_visual_runtime_suspend(&runtime, 9000000);
    AnimationFrameAdvance next[4]{};
    check(animation_timeline_collect_due(&timeline, 30012000, {next, 4}).advances == 0 &&
              animation_timeline_wait_timeout_ms(&timeline, 30012000) == INFINITE,
          "thirty-second titlebar hold creates no animation work or wakeups");
    animation_visual_runtime_resume(&runtime, 30012000);
    const auto cached_slot = animation_timeline_find(&timeline, cached).slot;
    const auto hidden_slot = animation_timeline_find(&timeline, hidden).slot;
    check(timeline.entries[cached_slot].due_at_us == 30041000 &&
              timeline.entries[hidden_slot].due_at_us == 0 &&
              timeline.entries[hidden_slot].paused_remaining_us == 50000,
          "resume preserves cached frame remainder and offscreen pause");
    check(runtime.pending_advances[0].scheduled_at_us == 30011000 &&
              animation_timeline_commit_advance(&timeline, runtime.pending_advances[0], 10000).ok(),
          "pre-drag codec completion retains its matching rebased token");
    runtime.pending_advance_count = 0;
    check(animation_timeline_collect_due(&timeline, 30012000, {next, 4}).advances == 0 &&
              animation_timeline_next_wake_at_us(&timeline) == 30021000,
          "resume does not rapidly replay thirty seconds of overdue frames");
    animation_visual_runtime_resume(&runtime, 40012000);
    check(animation_timeline_next_wake_at_us(&timeline) == 30021000,
          "duplicate resume does not shift deadlines twice");
    collected = animation_timeline_collect_due(&timeline, 30021000, {next, 4});
    check(collected.advances == 1 && next[0].frame_index == 2 &&
              animation_timeline_commit_advance(&timeline, next[0], 17000).ok() &&
              animation_timeline_next_wake_at_us(&timeline) == 30038000,
          "variable frame durations resume at their normal cadence");

    // Resize can re-enter presentation/visibility code while the outer pump
    // is stopped. New, revealed and restarted entries share the frozen time.
    animation_visual_runtime_suspend(&runtime, 30022000);
    const auto added = media_asset_custom_emoji(904, true);
    check(animation_visual_runtime_publish(&runtime, {added, 25000, 4, 2, 1, true}, 80022000).ok() &&
              animation_timeline_observe_visible(&timeline, hidden, 80022000) &&
              animation_timeline_restart(&timeline, cached, 99, 80022000),
          "modal reentrant publication and visibility remain valid");
    animation_visual_runtime_resume(&runtime, 90022000);
    check(timeline.entries[animation_timeline_find(&timeline, added).slot].due_at_us == 90047000 &&
              timeline.entries[hidden_slot].due_at_us == 90072000 &&
              timeline.entries[cached_slot].due_at_us == 90022000,
          "reentrant visual deadlines use the paused presentation clock");
    animation_visual_runtime_reset(&runtime);
    animation_visual_runtime_suspend(&runtime, 0);
    animation_visual_runtime_publish(&runtime, {cached, 10000, 5, 1, 1, true}, 5000000);
    animation_visual_runtime_resume(&runtime, 10000000);
    check(animation_timeline_next_wake_at_us(&timeline) == 10010000,
          "zero-time pause is distinct from an inactive pause");
    collected = animation_timeline_collect_due(&timeline, 10010000, {next, 4});
    check(collected.completed == 1 && collected.advances == 0,
          "finite animation completion survives pause and resume");
    animation_visual_runtime_suspend(&runtime, 10010000);
    animation_visual_runtime_resume(&runtime, 20010000);
    check(animation_timeline_next_wake_at_us(&timeline) == 0,
          "resume does not restart finished animations");

    // A visible entry whose pump stalls (a covering panel skipped collection)
    // must not replay every missed frame once collection resumes.
    animation_visual_runtime_reset(&runtime);
    const auto stalled = media_asset_custom_emoji(905, true);
    check(animation_visual_runtime_publish(&runtime, {stalled, 20000, 6, 4, 0, true}, 0).ok(),
          "stalled animation publishes visible");
    collected = animation_timeline_collect_due(&timeline, 20000, {next, 4});
    check(collected.advances == 1 && next[0].scheduled_at_us == 20000 && next[0].frame_index == 1 &&
              animation_timeline_commit_advance(&timeline, next[0], 20000).ok(),
          "stalled animation plays its first advance on time");
    const u64 late_before = timeline.counters.late_advances;
    collected = animation_timeline_collect_due(&timeline, 10040000, {next, 4});
    check(collected.advances == 1 && next[0].scheduled_at_us == 10040000 && next[0].frame_index == 2 &&
              timeline.counters.late_advances == late_before &&
              animation_timeline_commit_advance(&timeline, next[0], 20000).ok() &&
              animation_timeline_next_wake_at_us(&timeline) == 10060000,
          "ten-second visible stall resynchronizes to wall time at the next sequential frame");
    check(animation_timeline_collect_due(&timeline, 10040000, {next, 4}).advances == 0,
          "resynchronized stall has no immediately due backlog");
    u32 frames_in_second = 0;
    u16 expected_frame = 2;
    bool sequential = true;
    for (u64 now = 10040000; now <= 11040000; now += 16667) {
        collected = animation_timeline_collect_due(&timeline, now, {next, 4});
        for (u32 i = 0; i < collected.advances; ++i) {
            expected_frame = static_cast<u16>((expected_frame + 1) % 4);
            sequential = sequential && next[i].frame_index == expected_frame;
            ++frames_in_second;
            animation_timeline_commit_advance(&timeline, next[i], 20000);
        }
    }
    check(sequential && frames_in_second >= 48 && frames_in_second <= 51,
          "post-stall playback runs at its 50 fps cadence instead of a catch-up burst");
    // Ordinary decode jitter below the resync bound keeps the absolute cadence.
    const u64 jitter_due = animation_timeline_next_wake_at_us(&timeline);
    collected = animation_timeline_collect_due(&timeline, jitter_due + kAnimationLateResyncUs, {next, 4});
    check(collected.advances == 1 && next[0].scheduled_at_us == jitter_due &&
              animation_timeline_commit_advance(&timeline, next[0], 20000).ok() &&
              animation_timeline_next_wake_at_us(&timeline) == jitter_due + 20000,
          "lateness within the resync bound keeps drift-free deadlines");
    // A long offscreen pause resumes after the frame it paused on, once.
    const u64 paused_at = jitter_due + 10000;
    const auto& stalled_entry = timeline.entries[animation_timeline_find(&timeline, stalled).slot];
    const u16 paused_frame = stalled_entry.frame_index;
    animation_timeline_begin_visibility(&timeline);
    animation_timeline_end_visibility(&timeline, paused_at);
    check(!(stalled_entry.flags & animation_timeline_entry_visible) && stalled_entry.paused_remaining_us == 10000,
          "offscreen entry parks its remaining frame time");
    const u64 shown_at = paused_at + 3600000000ull;
    animation_timeline_begin_visibility(&timeline);
    check(animation_timeline_observe_visible(&timeline, stalled, shown_at),
          "hour-long offscreen entry is observed again");
    animation_timeline_end_visibility(&timeline, shown_at);
    collected = animation_timeline_collect_due(&timeline, shown_at + 10000, {next, 4});
    check(stalled_entry.frame_index == paused_frame && collected.advances == 1 &&
              next[0].frame_index == (paused_frame + 1u) % 4u && next[0].scheduled_at_us == shown_at + 10000 &&
              animation_timeline_commit_advance(&timeline, next[0], 20000).ok() &&
              animation_timeline_collect_due(&timeline, shown_at + 10000, {next, 4}).advances == 0,
          "hour-long hidden pause resumes on the next sequential frame without replay");
    animation_visual_runtime_release(&runtime);
}
} // namespace maicord
