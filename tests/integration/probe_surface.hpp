#pragma once
#include "maicord/core/win32_user.hpp"
#include "maicord/render/d3d11/renderer.hpp"

namespace maicord {
// The surface a probe's own window needs: its handle, client size (at least one pixel each way) and DPI, the way the
// application's window service reports them (renderer_window_surface).
inline GpuSurfaceDesc probe_window_surface(HWND window) noexcept {
    RECT client{};
    GetClientRect(window, &client);
    const u32 dpi = GetDpiForWindow(window);
    return GpuSurfaceDesc{window, static_cast<u32>(maximum<LONG>(1, client.right - client.left)),
                          static_cast<u32>(maximum<LONG>(1, client.bottom - client.top)), dpi == 0 ? 96 : dpi};
}
} // namespace maicord
