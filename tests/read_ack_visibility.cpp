#include "maicord/app/read_ack_visibility.hpp"

namespace maicord {
void test_read_ack_visibility(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr ReadAckVisibility visible{true, true, true, true, false, false, false, false, false, false};
    static_assert(read_ack_automatic_eligible(visible));
    check(read_ack_automatic_eligible(visible),
        "automatic ACK: foreground visible live history following bottom admits newly caught-up metadata");
    bool ReadAckVisibility::* const required[]{&ReadAckVisibility::foreground, &ReadAckVisibility::chat_visible,
        &ReadAckVisibility::following_bottom, &ReadAckVisibility::current_history};
    for (const auto gate : required) {
        auto view = visible;
        view.*gate = false;
        check(!read_ack_automatic_eligible(view),
            "automatic ACK: foreground, visible chat, bottom and current history are independently required");
    }
    bool ReadAckVisibility::* const blockers[]{&ReadAckVisibility::loading_more, &ReadAckVisibility::newer_history_required,
        &ReadAckVisibility::catchup_required, &ReadAckVisibility::resync_required,
        &ReadAckVisibility::idle, &ReadAckVisibility::manual_suppressed};
    for (const auto gate : blockers) {
        auto view = visible;
        view.*gate = true;
        check(!read_ack_automatic_eligible(view),
            "automatic ACK: loading, stale history, catchup, resync, idle and manual suppression independently block clearing");
    }
    auto modal = visible;
    modal.chat_visible = false;
    check(!read_ack_automatic_eligible(modal),
        "automatic ACK: focused foreground window cannot acknowledge chat covered by settings, Friends, a viewer or modal");
    auto history = visible;
    history.current_history = false;
    history.newer_history_required = true;
    history.catchup_required = true;
    check(!read_ack_automatic_eligible(history),
        "automatic ACK: bottom of a partial resident page is unread until live history catches up");
    history.current_history = true;
    history.newer_history_required = false;
    check(!read_ack_automatic_eligible(history),
        "automatic ACK: current metadata does not bypass an outstanding catchup requirement");
    history.catchup_required = false;
    check(read_ack_automatic_eligible(history),
        "automatic ACK: completed catchup at a visible live bottom restores eligibility");
    auto manual = visible;
    manual.manual_suppressed = true;
    manual.idle = true;
    check(!read_ack_automatic_eligible(manual),
        "automatic ACK: idle manually unread channel stays unread while selected");
    manual.idle = false;
    check(!read_ack_automatic_eligible(manual),
        "automatic ACK: renewed user activity does not undo manual unread suppression");
    manual.manual_suppressed = false;
    check(read_ack_automatic_eligible(manual),
        "automatic ACK: owner clearing suppression after leaving the channel allows a later visible visit");
    auto idle = visible;
    idle.idle = true;
    check(!read_ack_automatic_eligible(idle),
        "automatic ACK: owner-observed ten-minute inactivity blocks visible live chat");
    idle.idle = false;
    check(read_ack_automatic_eligible(idle),
        "automatic ACK: activity restores eligibility when all history and visibility gates remain satisfied");
}
} // namespace maicord
