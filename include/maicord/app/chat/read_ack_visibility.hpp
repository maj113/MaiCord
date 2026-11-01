#pragma once

namespace maicord {
// The platform/runtime owner computes these observations. This policy owns
// neither clocks nor scenes and never turns a resident history edge into a
// verified live edge.
struct ReadAckVisibility {
    bool foreground;
    bool chat_visible;
    bool following_bottom;
    bool current_history;
    bool loading_more;
    bool newer_history_required;
    bool catchup_required;
    bool resync_required;
    bool idle;
    bool manual_suppressed;
};
constexpr bool read_ack_automatic_eligible(const ReadAckVisibility& view) noexcept {
    return view.foreground && view.chat_visible && view.following_bottom && view.current_history &&
        !view.loading_more && !view.newer_history_required && !view.catchup_required &&
        !view.resync_required && !view.idle && !view.manual_suppressed;
}
} // namespace maicord
