#pragma once
#include "../src/app/application_inbox.hpp"

// These fixtures exercise a different host surface with no inbox owner.
namespace maicord::app_detail {
bool application_inbox_input(UiInputKind, u32, bool) noexcept { return false; }
u64 application_inbox_opens() noexcept { return 0; }
}
