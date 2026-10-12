#pragma once

#include "maicord/render/d3d11_renderer.hpp"

namespace maicord {
// A separate native executable links shipping content input, the shared link
// owner, thread membership and mutation notices. Only account transport,
// account presentation scheduling, and window bootstrap are fixture boundaries.
// No authenticated account, network, clipboard or external URL is accessed.
bool message_link_ui_probe() noexcept;
} // namespace maicord
