#pragma once
#include "../../src/app/voice/go_live.hpp"

// These fixtures exercise a different host surface with no Go Live screen share dialog open.
namespace maicord::app_detail {
bool application_go_live_modal_input(UiInputKind, u32, bool) noexcept { return false; }
}
