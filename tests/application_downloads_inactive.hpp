#pragma once
#include "../src/app/application_downloads.hpp"

// These fixtures exercise a different host surface with no Downloads owner.
// The download host has its own source/chooser/transfer runtime fixture.
namespace maicord::app_detail {
bool application_downloads_active() noexcept { return false; }
bool application_downloads_filter(const MSG&) noexcept { return false; }
bool application_downloads_input(UiInputKind, u32, bool) noexcept { return false; }
bool application_downloads_open() noexcept { return false; }
u64 application_downloads_next_wake() noexcept { return 0; }
}
