#pragma once
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/navigation_view.hpp"

namespace maicord {
// Synthetic unread banner, jump bar and guild folder rail on the real native
// renderer. sync re-runs shell layout and content text; capture is optional.
bool navigation_gaps_ui_probe(D3D11Renderer&, NavigationView*, const ChatStateStore&,
                              bool (*sync)() noexcept, bool (*capture)(const wchar_t*) noexcept) noexcept;
} // namespace maicord
