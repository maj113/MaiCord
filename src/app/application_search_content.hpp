#pragma once
#include "application_search.hpp"

namespace maicord::app_detail {
bool application_search_content_activate(const ApplicationSearchSourceTicket&, UiSearchTarget) noexcept;
void application_search_content_pump() noexcept;
void application_search_content_release() noexcept;
} // namespace maicord::app_detail
