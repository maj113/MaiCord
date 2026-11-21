#include "maicord/ui/shell/connection_status.hpp"

#include "maicord/core/text_bank.hpp"

namespace maicord {
namespace {

// The state the indicator shows at `now_ms`, and its countdown in `seconds`.
UiConnectionStatus target(const UiConnectionIndicator& c, u64 now_ms, u32* seconds) noexcept {
    *seconds = 0;
    const bool confirming = c.shown == UiConnectionStatus::connected && now_ms - c.shown_ms < kUiConnectionConfirmMs;
    switch (c.link) {
    case UiConnectionLink::up:
        // Only a session that came back after the indicator showed is confirmed.
        return c.shown == UiConnectionStatus::hidden || (c.shown == UiConnectionStatus::connected && !confirming)
                   ? UiConnectionStatus::hidden
                   : UiConnectionStatus::connected;
    case UiConnectionLink::failed:
        return UiConnectionStatus::failed;
    case UiConnectionLink::stopped:
        return UiConnectionStatus::stopped;
    case UiConnectionLink::down:
        break;
    }
    // A short loss shows nothing new; a running confirmation ends first.
    if ((c.shown == UiConnectionStatus::hidden || c.shown == UiConnectionStatus::connected) &&
        now_ms - c.down_ms < kUiConnectionGraceMs)
        return confirming ? UiConnectionStatus::connected : UiConnectionStatus::hidden;
    if (c.retry_at_ms <= now_ms)
        return UiConnectionStatus::connecting;
    const u64 remaining = (c.retry_at_ms - now_ms + 999) / 1000;
    *seconds = remaining < 0xFFFF'FFFFull ? static_cast<u32>(remaining) : 0xFFFF'FFFFu;
    return UiConnectionStatus::disconnected;
}

} // namespace

bool ui_connection_indicator_set(UiConnectionIndicator* c, UiConnectionLink link, u64 retry_at_ms,
                                 u64 now_ms) noexcept {
    if (!c)
        return false;
    if (link == UiConnectionLink::up)
        c->down_ms = 0;
    else if (c->link == UiConnectionLink::up)
        c->down_ms = now_ms;
    c->link = link;
    c->retry_at_ms = link == UiConnectionLink::down ? retry_at_ms : 0;
    return ui_connection_indicator_tick(c, now_ms);
}

bool ui_connection_indicator_tick(UiConnectionIndicator* c, u64 now_ms) noexcept {
    if (!c)
        return false;
    u32 seconds = 0;
    const UiConnectionStatus shown = target(*c, now_ms, &seconds);
    if (shown == c->shown && seconds == c->seconds)
        return false;
    if (shown != c->shown)
        c->shown_ms = now_ms;
    c->shown = shown;
    c->seconds = seconds;
    return true;
}

u64 ui_connection_indicator_next_wake(const UiConnectionIndicator& c) noexcept {
    const u64 confirm_end = c.shown == UiConnectionStatus::connected ? c.shown_ms + kUiConnectionConfirmMs : 0;
    switch (c.link) {
    case UiConnectionLink::up:
        return confirm_end;
    case UiConnectionLink::down:
        break;
    default:
        return 0;
    }
    if (c.shown == UiConnectionStatus::hidden || c.shown == UiConnectionStatus::connected) {
        const u64 grace_end = c.down_ms + kUiConnectionGraceMs;
        return confirm_end && confirm_end < grace_end ? confirm_end : grace_end;
    }
    // The countdown shows one less at each whole second before the attempt, which shows Connecting.
    if (c.shown == UiConnectionStatus::disconnected && c.seconds)
        return c.retry_at_ms - u64{c.seconds - 1} * 1000;
    return 0;
}

Utf8View ui_connection_indicator_text(const UiConnectionIndicator& c, Slice<char> output) noexcept {
    switch (c.shown) {
    case UiConnectionStatus::connecting:
        return utf8_literal("Connecting\xE2\x80\xA6");
    case UiConnectionStatus::connected:
        return utf8_literal("Connected");
    case UiConnectionStatus::failed:
        return utf8_literal("Connection failed");
    case UiConnectionStatus::stopped:
        return utf8_literal("Disconnected");
    case UiConnectionStatus::disconnected:
        break;
    case UiConnectionStatus::hidden:
        return {};
    }
    TextWriter writer{output, 0, output.data != nullptr};
    writer.add("Disconnected \xC2\xB7 retrying in ");
    if (c.seconds < 60) {
        writer.number(c.seconds);
        writer.add("s");
    } else {
        writer.number(c.seconds / 60);
        const char digits[3]{':', static_cast<char>('0' + c.seconds % 60 / 10), static_cast<char>('0' + c.seconds % 10)};
        writer.add(digits, 3);
    }
    // Without room for the countdown the state still reads.
    const u32 bytes = writer.finish();
    return bytes ? Utf8View{output.data, bytes} : utf8_literal("Disconnected");
}

} // namespace maicord
