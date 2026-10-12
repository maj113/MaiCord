#pragma once

namespace maicord {
// Native focused-view fixture for shipping composer layout/input/controller.
// Compact viewports exercise the reusable API; the shipping 632-DIP minimum
// window is unchanged. Transport, dialogs, picker presentation and scheduling
// are fixture boundaries; no live account, network or clipboard is used.
bool composer_ui_probe() noexcept;
} // namespace maicord
