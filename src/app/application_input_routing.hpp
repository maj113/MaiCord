#pragma once

#include "maicord/ui/input_router.hpp"

namespace maicord::app_detail {

bool route_application_input(UiInputKind kind, u32 code, bool repeated = false) noexcept;

} // namespace maicord::app_detail
