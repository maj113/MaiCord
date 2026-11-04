// Icons on Windows: the window's own (window_set_icon) and the one stored in the program (window_app_icon_png).
#include "maicord/platform/window/win32.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
namespace {

// The icons last set on the window, destroyed when replaced.
HICON g_window_icons[2];

} // namespace

void window_set_icon(Window window, const IconImage* images, u32 count) noexcept {
    if (!images || !count)
        return;
    const IconImage* const slots[2] = {&images[0], &images[count - 1]};
    for (u32 i = 0; i < 2; ++i) {
        const HICON icon = win32_create_icon({slots[i]->pixels, slots[i]->extent * slots[i]->extent}, slots[i]->extent);
        if (!icon)
            continue;
        SendMessageW(win32_hwnd(window), WM_SETICON, i ? ICON_BIG : ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        if (g_window_icons[i])
            DestroyIcon(g_window_icons[i]);
        g_window_icons[i] = icon;
    }
}

// The program's icon (resource group 1): the stored image closest above the wanted size.
ConstSlice<u8> window_app_icon_png(u32 extent, u32 largest, u32* side) noexcept {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC group = module ? FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14)) : nullptr;
    const HGLOBAL group_data = group ? LoadResource(module, group) : nullptr;
    const auto* directory = group_data ? static_cast<const u8*>(LockResource(group_data)) : nullptr;
    const u32 directory_bytes = group ? SizeofResource(module, group) : 0;
    if (!directory || directory_bytes < 6 || !side)
        return {};
    // GRPICONDIR: three WORDs, then 14-byte entries ending in the RT_ICON id.
    const u32 count = minimum<u32>(directory[4] | directory[5] << 8, (directory_bytes - 6) / 14);
    u32 best = 0, best_id = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* entry = directory + 6 + i * 14;
        const u32 size = entry[0] ? entry[0] : 256;
        if (size >= extent && size <= largest && (!best || size < best))
            best = size, best_id = static_cast<u32>(entry[12] | entry[13] << 8);
    }
    const HRSRC image = best ? FindResourceW(module, MAKEINTRESOURCEW(best_id), MAKEINTRESOURCEW(3)) : nullptr;
    const HGLOBAL image_data = image ? LoadResource(module, image) : nullptr;
    const auto* encoded = image_data ? static_cast<const u8*>(LockResource(image_data)) : nullptr;
    if (!encoded)
        return {};
    *side = best;
    return {encoded, SizeofResource(module, image)};
}

} // namespace maicord
