#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// A requested join gets this long to show up in the voice session.
inline constexpr u64 kCallViewJoinGraceMs = 15'000;

enum class CallViewStep : u8 {
    keep,
    follow, // the call moved to another channel: the view follows it
    close,
};

// Per opened call view. Once the view's call has been joined, the call ending
// (a local disconnect, a VOICE_STATE_UPDATE without a channel, a kick) closes
// the view at once, as the official client returns to the text channel. Before
// that a join still in flight keeps the view for the grace period. A connection
// that failed for good closes it too.
struct CallViewLifecycle {
    bool joined;
};

inline CallViewStep call_view_lifecycle_step(CallViewLifecycle* life, u64 view_channel, u64 call_channel,
                                             bool failed, u64 elapsed_ms) noexcept {
    if (call_channel && failed)
        return CallViewStep::close;
    if (call_channel && call_channel == view_channel) {
        life->joined = true;
        return CallViewStep::keep;
    }
    const bool settled = life->joined || elapsed_ms >= kCallViewJoinGraceMs;
    if (call_channel)
        return settled ? CallViewStep::follow : CallViewStep::keep;
    return settled ? CallViewStep::close : CallViewStep::keep;
}

// When a view still waiting for its join must be reconsidered (zero: none).
inline u64 call_view_lifecycle_deadline(const CallViewLifecycle& life, u64 opened_ms) noexcept {
    return life.joined ? 0 : opened_ms + kCallViewJoinGraceMs;
}

} // namespace maicord
