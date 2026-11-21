#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// The Gateway connection indicator of the window caption. While the session is not live, the caption's title gives
// way to "Connecting…" with a spinner, to "Disconnected" with the countdown to the next attempt, or to "Connection
// failed" once retries stop. A session that comes back after the indicator showed says "Connected" for a moment, then
// the title returns. The host reports what its sources say and ticks the indicator from frames. It has no timer of
// its own: the host wakes at ui_connection_indicator_next_wake, which is zero while the session is up and nothing
// shows.

// What the sources report.
enum class UiConnectionLink : u8 {
    // READY or RESUMED.
    up,
    // Not live, and the client retries on its own: an attempt runs, or the next one starts at retry_at_ms.
    down,
    // Retries stopped. Only the user's Reconnect starts the session again.
    failed,
    // The account needs the user (a sign-in or an account step) before it can reconnect.
    stopped,
};

// What the caption shows.
enum class UiConnectionStatus : u8 { hidden, connecting, connected, disconnected, failed, stopped };

// A loss shorter than this shows nothing: a reconnect that Discord asks for resumes within it.
inline constexpr u64 kUiConnectionGraceMs = 1'000;
// How long "Connected" shows after a session came back.
inline constexpr u64 kUiConnectionConfirmMs = 2'000;
// Room for the longest line, "Disconnected · retrying in " and a countdown.
inline constexpr u32 kUiConnectionTextBytes = 64;

// Zero-initialized storage is a live session with nothing shown.
struct UiConnectionIndicator {
    // When the link left up, zero while it is up.
    u64 down_ms;
    // The next attempt (monotonic) while the link is down, zero while one runs.
    u64 retry_at_ms;
    // When `shown` last changed: the start of the confirmation.
    u64 shown_ms;
    // The whole seconds to the next attempt while disconnected shows, else zero.
    u32 seconds;
    UiConnectionLink link;
    UiConnectionStatus shown;
};

// Records what the sources report and ticks. Both return true when the shown state or its countdown changed: the
// host then draws again.
bool ui_connection_indicator_set(UiConnectionIndicator*, UiConnectionLink, u64 retry_at_ms, u64 now_ms) noexcept;
bool ui_connection_indicator_tick(UiConnectionIndicator*, u64 now_ms) noexcept;
// When the shown state or its countdown changes next without a new report: the end of the grace, of the
// confirmation or of a countdown second. Zero when nothing changes on its own.
u64 ui_connection_indicator_next_wake(const UiConnectionIndicator&) noexcept;
// The shown line, empty while hidden: "Connecting…", "Connected", "Disconnected · retrying in 12s" (m:ss from a
// minute), "Connection failed" or "Disconnected".
Utf8View ui_connection_indicator_text(const UiConnectionIndicator&, Slice<char> output) noexcept;

} // namespace maicord
