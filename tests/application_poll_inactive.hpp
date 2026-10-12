#pragma once

#include "../src/app/application_poll.hpp"

// These fixtures exercise other host owners without a selected poll session.
namespace maicord::app_detail {
bool application_poll_state(u64, Slice<UiPollAnswer>, UiPollState*, ApplicationPollSource*) noexcept {
    return false;
}
bool application_poll_search_state(u64, u32, Slice<UiPollAnswer>, UiPollState*,
                                   ApplicationPollSource*) noexcept {
    return false;
}
bool application_poll_voters_active() noexcept {
    return false;
}
bool application_poll_input(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_poll_consume_key_tail(UiInputKind, u32, bool) noexcept {
    return false;
}
bool application_poll_synchronize() noexcept {
    return true;
}
u64 application_poll_next_wake() noexcept {
    return 0;
}
bool application_poll_pending(u64) noexcept {
    return false;
}
bool application_poll_submit(u64, u64) noexcept {
    return false;
}
bool application_poll_response(const RestClientResponse&, Utf8View) noexcept {
    return false;
}
void application_poll_observe(const GatewayMessagePollVoteEvent&, bool) noexcept {
}
void application_poll_message_observe(const GatewayMessageEvent&, bool) noexcept {
}
void application_poll_message_deleted(u64, u64) noexcept {
}
void application_poll_gateway_reset() noexcept {
}
void application_poll_release() noexcept {
}
} // namespace maicord::app_detail
