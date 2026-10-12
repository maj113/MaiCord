#pragma once
#include "application_search.hpp"
#include "maicord/ui/message_text_presentation.hpp"

namespace maicord::app_detail {
bool application_search_content_activate(const ApplicationSearchSourceTicket&, UiSearchTarget) noexcept;
bool application_search_content_link(const ApplicationSearchSourceTicket&, const MessageTextHit&) noexcept;
void application_search_content_pump() noexcept;
void application_search_content_release() noexcept;
// Presentation owner confirms the exact currently admitted visible semantic,
// including spoiler reveal state. Native confirmation dispatches messages, so
// the content owner asks again immediately before OS activation.
bool application_search_source_link_current(const ApplicationSearchSourceTicket&,
                                            const MessageTextHit&) noexcept;
} // namespace maicord::app_detail
