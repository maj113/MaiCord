#include "maicord/platform/desktop/taskbar_attention.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/memory.hpp"
#if defined(_WIN32)
#include "maicord/platform/window/win32.hpp"

#include "maicord/sys/shell.hpp"
#else
#include "maicord/platform/dbus/launcher_entry.hpp"
#endif

namespace maicord {
namespace {
// 3x5 glyphs for 0-9 and '+', one bit per pixel, rows top to bottom.
constexpr u8 kGlyphs[11][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}, {0, 2, 7, 2, 0},
};
constexpr u32 kRed = 0xF23F43;
#if defined(_WIN32)
constexpr CLSID kTaskbarListClass = {0x56FDF344, 0xFD6D, 0x11D0, {0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90}};
constexpr IID kTaskbarList3 = {0xEA1AFB91, 0x9E28, 0x4B86, {0x90, 0xE9, 0x9E, 0x9F, 0x8A, 0x5E, 0xEF, 0xAF}};
#endif

u32 pixel(u32 rgb, float alpha) noexcept {
    return (static_cast<u32>(alpha * 255.f + .5f) << 24) | rgb;
}
void disc(Slice<u32> pixels, u32 extent, float radius) noexcept {
    const float center = static_cast<float>(extent) * .5f;
    for (u32 y = 0; y < extent; ++y)
        for (u32 x = 0; x < extent; ++x) {
            const float dx = static_cast<float>(x) + .5f - center, dy = static_cast<float>(y) + .5f - center;
            const float coverage = clamp_unit(radius - taskbar_attention_root(dx * dx + dy * dy) + .5f);
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
#if defined(_WIN32)
HICON create_icon(u8 badge, u32 extent) noexcept {
    ScratchPages<u32[kTaskbarBadgeMaximumExtent * kTaskbarBadgeMaximumExtent]> pixels;
    if (!pixels.data || !taskbar_attention_render_badge(badge, extent, {*pixels.data, extent * extent}))
        return nullptr;
    return win32_create_icon({*pixels.data, extent * extent}, extent);
}
// The shell's taskbar object is activated when the first badge is shown; a
// session that never shows one never pays for it. One attempt per session.
bool acquire_list(TaskbarAttention* attention) noexcept {
    if (attention->list)
        return true;
    if (attention->list_attempted)
        return false;
    attention->list_attempted = true;
    void* list = nullptr;
    if (FAILED(CoCreateInstance(kTaskbarListClass, nullptr, CLSCTX_INPROC_SERVER, kTaskbarList3, &list)) || !list)
        return false;
    auto* taskbar = static_cast<ITaskbarList3*>(list);
    if (FAILED(taskbar->HrInit())) {
        taskbar->Release();
        return false;
    }
    attention->list = taskbar;
    return true;
}
#endif
} // namespace

u8 taskbar_attention_badge(u32 mentions, bool unread, bool enabled) noexcept {
    if (!enabled)
        return kTaskbarBadgeNone;
    if (mentions)
        return mentions > 9 ? kTaskbarBadgeOverflow : static_cast<u8>(mentions);
    return unread ? kTaskbarBadgeDot : kTaskbarBadgeNone;
}

float taskbar_attention_root(float value) noexcept {
    return value > 0 ? __builtin_elementwise_sqrt(value) : 0;
}

TaskbarAttentionPlan taskbar_attention_plan(TaskbarAttentionState* state, const TaskbarAttentionInput& input) noexcept {
    TaskbarAttentionPlan plan{};
    if (!state)
        return plan;
    plan.badge = taskbar_attention_badge(input.mentions, input.unread, input.badge_enabled);
    plan.set_badge = !state->badge_applied || plan.badge != state->badge;
    plan.mentions = input.mentions;
#if !defined(_WIN32)
    // A dock shows the count as it is, so a change within "9+" is news too.
    plan.set_badge = plan.set_badge || (plan.badge == kTaskbarBadgeOverflow && input.mentions != state->mentions);
    state->mentions = input.mentions;
#endif
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

#if defined(_WIN32)
bool taskbar_attention_initialize(TaskbarAttention* attention) noexcept {
    if (!attention)
        return false;
    reset_value(attention);
    attention->created_message = RegisterWindowMessageW(L"TaskbarButtonCreated");
    attention->com_initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    return attention->created_message != 0;
}

bool taskbar_attention_window_message(TaskbarAttention* attention, UINT message) noexcept {
    if (!attention || !attention->created_message || message != attention->created_message)
        return false;
    // Explorer restarts recreate the button; the overlay must be reapplied.
    attention->button_ready = true;
    attention->state.badge_applied = false;
    return true;
}

void taskbar_attention_apply(TaskbarAttention* attention, Window owner, const TaskbarAttentionPlan& plan,
                             u32 dpi) noexcept {
    const HWND window = win32_hwnd(owner);
    if (!attention || !window)
        return;
    if (plan.stop_flash)
        window_request_attention(owner, Attention::stop);
    if (plan.start_flash)
        window_request_attention(owner, Attention::flash);
    if (!plan.set_badge)
        return;
    if (!attention->button_ready) {
        attention->state.badge_applied = false;
        return;
    }
    // Without a taskbar object nothing was shown, so there is no overlay to clear.
    if ((!plan.badge && !attention->list) || !acquire_list(attention))
        return;
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

void taskbar_attention_release(TaskbarAttention* attention, Window owner) noexcept {
    const HWND window = win32_hwnd(owner);
    if (!attention)
        return;
    if (attention->list) {
        if (window && attention->button_ready)
            attention->list->SetOverlayIcon(window, nullptr, L"");
        attention->list->Release();
    }
    if (window && attention->state.flashing)
        window_request_attention(owner, Attention::stop);
    if (attention->icon)
        DestroyIcon(static_cast<HICON>(attention->icon));
    if (attention->com_initialized)
        CoUninitialize();
    reset_value(attention);
}
#else
namespace {
// The desktop file of the application (packaging/linux), which the dock matches the entry to.
constexpr char kDesktopId[] = "io.github.maj113.MaiCord.desktop";
} // namespace

bool taskbar_attention_initialize(TaskbarAttention* attention) noexcept {
    if (!attention)
        return false;
    reset_value(attention);
    return true;
}

void taskbar_attention_apply(TaskbarAttention* attention, Window window, const TaskbarAttentionPlan& plan,
                             u32) noexcept {
    if (!attention || !window)
        return;
    if (plan.stop_flash)
        window_request_attention(window, Attention::stop);
    if (plan.start_flash)
        window_request_attention(window, Attention::flash);
    // The state already reflects the plan. A dot is urgent without a count; flashing is urgent too.
    const u8 badge = attention->state.badge;
    dbus::LauncherEntryState entry{};
    entry.count = badge == kTaskbarBadgeNone || badge == kTaskbarBadgeDot ? 0 : plan.mentions ? plan.mentions : badge;
    entry.count_visible = entry.count > 0;
    entry.urgent = attention->state.flashing || badge == kTaskbarBadgeDot;
    dbus::launcher_entry_update(kDesktopId, entry);
}

void taskbar_attention_release(TaskbarAttention* attention, Window window) noexcept {
    if (!attention)
        return;
    if (window && attention->state.flashing)
        window_request_attention(window, Attention::stop);
    dbus::launcher_entry_update(kDesktopId, dbus::LauncherEntryState{});
    reset_value(attention);
}
#endif

} // namespace maicord
