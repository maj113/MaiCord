#pragma once

namespace maicord {
// Native frame assertions remain in memory; failures report to inherited
// console pipes without application file logs or capture artifacts.
bool scoped_read_ui_probe() noexcept;
} // namespace maicord
