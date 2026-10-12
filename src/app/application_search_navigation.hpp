#pragma once
#include "application_search.hpp"
#include "application_message_navigation.hpp"
#include "maicord/app/message_history_sync.hpp"
#include "maicord/ui/message_markdown.hpp"

namespace maicord::app_detail {
// Private access waits on the shared thread DETAILS service. Retiring this
// navigation drops its waiter; it does not cancel a coalesced membership fetch.
// Public and archived readable threads never require Join or Unarchive here.
bool application_search_navigation_begin(const ApplicationSearchSourceTicket&) noexcept;
bool application_search_navigation_link(const ApplicationSearchSourceTicket&, const MessageLink&) noexcept;
void application_search_navigation_synchronize() noexcept;
void application_search_navigation_release() noexcept;
bool application_search_navigation_pending(u64 now_ms) noexcept;
bool application_search_navigation_submit(u64 request_id, u64 now_ms) noexcept;
bool application_search_navigation_response(const RestClientResponse&, Utf8View body) noexcept;
u64 application_search_navigation_next_wake() noexcept;
void application_search_navigation_history_submitted(u64 channel_id, u64 message_id,
                                                      u64 request_id) noexcept;
void application_search_navigation_history_response(const RestClientResponse&,
                                                     const MessageHistoryRestResult&) noexcept;
bool application_search_navigation_history_admission_failed(u64 channel_id, u64 message_id,
                                                            bool busy = false) noexcept;
} // namespace maicord::app_detail
