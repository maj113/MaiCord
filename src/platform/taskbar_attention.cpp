#include "maicord/platform/taskbar_attention.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/gdi.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord {
namespace {
// 3x5 glyphs for 0-9 and '+', one bit per pixel, rows top to bottom.
constexpr u8 kGlyphs[11][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {0, 2, 7, 2, 0},
};
constexpr u32 kRed = 0xF23F43;
constexpr CLSID kTaskbarListClass = {0x56FDF344, 0xFD6D, 0x11D0, {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
constexpr IID kTaskbarList3 = {0xEA1AFB91, 0x9E28, 0x4B86, {0x90, 0xE9, 0x9E, 0x9F, 0x8A, 0x5E, 0xEF, 0xAF}};

float clamp01(float value) noexcept {
    return clamp_value(value, 0.f, 1.f);
}
float square_root(float value) noexcept {
    if (value <= 0)
        return 0;
    float x = value > 1 ? value : 1;
    for (u32 i = 0; i < 12; ++i)
        x = .5f * (x + value / x);
    return x;
}
u32 pixel(u32 rgb, float alpha) noexcept {
    return (static_cast<u32>(alpha * 255.f + .5f) << 24) | rgb;
}
void disc(Slice<u32> pixels, u32 extent, float radius) noexcept {
    const float center = static_cast<float>(extent) * .5f;
    for (u32 y = 0; y < extent; ++y)
        for (u32 x = 0; x < extent; ++x) {
            const float dx = static_cast<float>(x) + .5f - center, dy = static_cast<float>(y) + .5f - center;
            const float coverage = clamp01(radius - square_root(dx * dx + dy * dy) + .5f);
            pixels[y * extent + x] = coverage > 0 ? pixel(kRed, coverage) : 0;
        }
}
// Opaque white glyph blocks over the disc, centered on whole pixels.
void text(Slice<u32> pixels, u32 extent, const u8* glyphs, u32 count) noexcept {
    const u32 width_units = count * 3 + (count - 1);
    u32 scale = 1;
    while ((scale + 1) * width_units * 10 <= extent * 7 && (scale + 1) * 5 * 10 <= extent * 7)
        ++scale;
    const u32 width = width_units * scale, height = 5 * scale;
    if (width > extent || height > extent)
        return;
    const u32 left = (extent - width) / 2, top = (extent - height + 1) / 2;
    for (u32 g = 0; g < count; ++g)
        for (u32 row = 0; row < 5; ++row)
            for (u32 column = 0; column < 3; ++column) {
                if (!(kGlyphs[glyphs[g]][row] & (4u >> column)))
                    continue;
                for (u32 sy = 0; sy < scale; ++sy)
                    for (u32 sx = 0; sx < scale; ++sx) {
                        const u32 x = left + (g * 4 + column) * scale + sx, y = top + row * scale + sy;
                        pixels[y * extent + x] = 0xFFFFFFFFu;
                    }
            }
}
HICON create_icon(u8 badge, u32 extent) noexcept {
    struct Scratch {
        u32 pixels[kTaskbarBadgeMaximumExtent * kTaskbarBadgeMaximumExtent];
        u8 resource[sizeof(BITMAPINFOHEADER) + sizeof(pixels) + kTaskbarBadgeMaximumExtent * 8];
    };
    ScratchPages<Scratch> scratch;
    if (!scratch.data)
        return nullptr;
    u32* pixels = scratch.data->pixels;
    u8* resource = scratch.data->resource;
    if (!taskbar_attention_render_badge(badge, extent, {pixels, extent * extent}))
        return nullptr;
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(extent);
    header.biHeight = static_cast<LONG>(extent * 2); // XOR image plus AND mask
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    // Fresh scratch pages are zero, so the AND mask is already clear.
    copy_bytes(resource, &header, sizeof(header));
    u8* rows = resource + sizeof(header);
    for (u32 y = 0; y < extent; ++y) // bottom-up
        copy_bytes(rows + (extent - 1 - y) * extent * 4, pixels + y * extent, extent * 4);
    const u32 mask_bytes = extent * ((extent + 31) / 32 * 4);
    const u32 total = static_cast<u32>(sizeof(header)) + extent * extent * 4 + mask_bytes;
    return CreateIconFromResourceEx(resource, total, TRUE, 0x00030000, static_cast<int>(extent),
                                    static_cast<int>(extent), LR_DEFAULTCOLOR);
}
void flash(HWND window, bool start) noexcept {
    FLASHWINFO info{sizeof(info), window, start ? static_cast<DWORD>(FLASHW_TRAY | FLASHW_TIMERNOFG)
                                                : static_cast<DWORD>(FLASHW_STOP),
                    start ? 3u : 0u, 0};
    FlashWindowEx(&info);
}
} // namespace

u8 taskbar_attention_badge(u32 mentions, bool unread, bool enabled) noexcept {
    if (!enabled)
        return kTaskbarBadgeNone;
    if (mentions)
        return mentions > 9 ? kTaskbarBadgeOverflow : static_cast<u8>(mentions);
    return unread ? kTaskbarBadgeDot : kTaskbarBadgeNone;
}

TaskbarAttentionPlan taskbar_attention_plan(TaskbarAttentionState* state, const TaskbarAttentionInput& input) noexcept {
    TaskbarAttentionPlan plan{};
    if (!state)
        return plan;
    plan.badge = taskbar_attention_badge(input.mentions, input.unread, input.badge_enabled);
    plan.set_badge = !state->badge_applied || plan.badge != state->badge;
    state->badge = plan.badge;
    state->badge_applied = true;
    if (input.focused || !input.flash_enabled) {
        // Focus acknowledges pending notifications; they never flash later.
        plan.stop_flash = state->flashing;
        state->flashing = false;
        state->flashed_serial = input.notify_serial;
    } else if (input.notify_serial != state->flashed_serial) {
        plan.start_flash = true;
        state->flashing = true;
        state->flashed_serial = input.notify_serial;
    }
    return plan;
}

bool taskbar_attention_render_badge(u8 badge, u32 extent, Slice<u32> pixels) noexcept {
    if (!badge || badge > kTaskbarBadgeDot || extent < 8 || extent > kTaskbarBadgeMaximumExtent || !pixels.data ||
        pixels.count < extent * extent)
        return false;
    const float full = static_cast<float>(extent) * .5f - .5f;
    disc(pixels, extent, badge == kTaskbarBadgeDot ? full * .6f : full);
    if (badge == kTaskbarBadgeDot)
        return true;
    const u8 overflow[2] = {9, 10};
    const u8 single[1] = {badge};
    if (badge == kTaskbarBadgeOverflow)
        text(pixels, extent, overflow, 2);
    else
        text(pixels, extent, single, 1);
    return true;
}

bool taskbar_attention_initialize(TaskbarAttention* attention) noexcept {
    if (!attention)
        return false;
    *attention = {};
    attention->created_message = RegisterWindowMessageW(L"TaskbarButtonCreated");
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    attention->com_initialized = SUCCEEDED(apartment);
    void* list = nullptr;
    if (FAILED(CoCreateInstance(kTaskbarListClass, nullptr, CLSCTX_INPROC_SERVER, kTaskbarList3, &list)) || !list)
        return attention->created_message != 0;
    attention->list = static_cast<ITaskbarList3*>(list);
    if (FAILED(attention->list->HrInit())) {
        attention->list->Release();
        attention->list = nullptr;
    }
    return true;
}

bool taskbar_attention_window_message(TaskbarAttention* attention, UINT message) noexcept {
    if (!attention || !attention->created_message || message != attention->created_message)
        return false;
    // Explorer restarts recreate the button; the overlay must be reapplied.
    attention->button_ready = true;
    attention->state.badge_applied = false;
    return true;
}

void taskbar_attention_apply(TaskbarAttention* attention, HWND window, const TaskbarAttentionPlan& plan,
                             u32 dpi) noexcept {
    if (!attention || !window)
        return;
    if (plan.stop_flash)
        flash(window, false);
    if (plan.start_flash)
        flash(window, true);
    if (!plan.set_badge)
        return;
    if (!attention->list || !attention->button_ready) {
        attention->state.badge_applied = false;
        return;
    }
    HICON icon = nullptr;
    if (plan.badge) {
        if (attention->icon && attention->icon_badge == plan.badge) {
            icon = static_cast<HICON>(attention->icon);
        } else {
            const int extent = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : 96);
            icon = create_icon(plan.badge, static_cast<u32>(extent < 8 ? 16 : extent > 64 ? 64 : extent));
        }
    }
    const wchar_t* label = !plan.badge ? L""
                           : plan.badge == kTaskbarBadgeDot ? L"Unread messages"
                                                            : L"Unread mentions";
    if (FAILED(attention->list->SetOverlayIcon(window, icon, label))) {
        // Retry once Explorer announces the button instead of every tick.
        attention->state.badge_applied = false;
        attention->button_ready = false;
        if (icon && icon != attention->icon)
            DestroyIcon(icon);
        return;
    }
    if (attention->icon && attention->icon != icon)
        DestroyIcon(static_cast<HICON>(attention->icon));
    attention->icon = icon;
    attention->icon_badge = icon ? plan.badge : 0;
}

void taskbar_attention_release(TaskbarAttention* attention, HWND window) noexcept {
    if (!attention)
        return;
    if (attention->list) {
        if (window && attention->button_ready)
            attention->list->SetOverlayIcon(window, nullptr, L"");
        attention->list->Release();
    }
    if (window && attention->state.flashing)
        flash(window, false);
    if (attention->icon)
        DestroyIcon(static_cast<HICON>(attention->icon));
    if (attention->com_initialized)
        CoUninitialize();
    *attention = {};
}

} // namespace maicord
