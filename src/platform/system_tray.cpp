#include "maicord/platform/system_tray.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/platform/taskbar_attention.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/sys/gdi.hpp"
#include "maicord/sys/shell.hpp"

namespace maicord {
namespace {
constexpr UINT kTrayId = 1;

float clamp01(float v) noexcept {
    return clamp_value(v, 0.f, 1.f);
}
float root(float v) noexcept {
    if (v <= 0)
        return 0;
    float x = v > 1 ? v : 1;
    for (u32 i = 0; i < 14; ++i)
        x = .5f * (x + v / x);
    return x;
}
float channel(u32 color, u32 shift) noexcept {
    return static_cast<float>((color >> shift) & 255u) / 255.f;
}
u32 pack(float a, float r, float g, float b) noexcept {
    const auto byte = [](float v) noexcept { return static_cast<u32>(clamp01(v) * 255.f + .5f); };
    return (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
}
// Straight-alpha source-over.
u32 over(u32 destination, u32 source, float coverage) noexcept {
    const float sa = channel(source, 24) * clamp01(coverage), da = channel(destination, 24);
    const float a = sa + da * (1 - sa);
    if (a <= 0)
        return 0;
    const auto mix = [&](u32 shift) noexcept {
        return (channel(source, shift) * sa + channel(destination, shift) * da * (1 - sa)) / a;
    };
    return pack(a, mix(16), mix(8), mix(0));
}
u32 lerp_color(u32 a, u32 b, float t) noexcept {
    const auto mix = [&](u32 shift) noexcept { return channel(a, shift) + (channel(b, shift) - channel(a, shift)) * t; };
    return pack(mix(24), mix(16), mix(8), mix(0));
}
float segment_distance(float px, float py, float ax, float ay, float bx, float by) noexcept {
    const float dx = bx - ax, dy = by - ay;
    const float length = dx * dx + dy * dy;
    const float t = length > 0 ? clamp01(((px - ax) * dx + (py - ay) * dy) / length) : 0;
    const float ex = px - (ax + dx * t), ey = py - (ay + dy * t);
    return root(ex * ex + ey * ey);
}
// Signed distance to a rounded square centred at c with half extent h.
float rounded_distance(float px, float py, float c, float h, float r) noexcept {
    const float qx = absolute_value(px - c) - (h - r), qy = absolute_value(py - c) - (h - r);
    const float ox = maximum(qx, 0.f), oy = maximum(qy, 0.f);
    return root(ox * ox + oy * oy) + minimum(maximum(qx, qy), 0.f) - r;
}

void render_mark(const TrayIconPalette& palette, u32 extent, Slice<u32> pixels) noexcept {
    const float e = static_cast<float>(extent), c = e * .5f;
    // "M": two posts and a centre valley, round caps.
    const float points[5][2] = {{.28f, .71f}, {.28f, .31f}, {.5f, .55f}, {.72f, .31f}, {.72f, .71f}};
    const float half_stroke = maximum(e * .072f, .85f);
    for (u32 y = 0; y < extent; ++y)
        for (u32 x = 0; x < extent; ++x) {
            const float px = static_cast<float>(x) + .5f, py = static_cast<float>(y) + .5f;
            const float tile = clamp01(.5f - rounded_distance(px, py, c, c, e * .3f));
            if (tile <= 0) {
                pixels[y * extent + x] = 0;
                continue;
            }
            const u32 base = lerp_color(palette.brand, palette.brand_shade, py / e);
            float d = 1e9f;
            for (u32 i = 0; i < 4; ++i)
                d = minimum(d, segment_distance(px, py, points[i][0] * e, points[i][1] * e, points[i + 1][0] * e,
                                                points[i + 1][1] * e));
            const u32 shaded = over(base | 0xFF000000u, palette.glyph | 0xFF000000u, half_stroke - d + .5f);
            pixels[y * extent + x] = (static_cast<u32>(tile * 255.f + .5f) << 24) | (shaded & 0x00FFFFFFu);
        }
}
// Clears a ring around the overlay so it reads against the tile, like the official status cutout.
void cut(Slice<u32> pixels, u32 extent, float cx, float cy, float radius) noexcept {
    for (u32 y = 0; y < extent; ++y)
        for (u32 x = 0; x < extent; ++x) {
            const float dx = static_cast<float>(x) + .5f - cx, dy = static_cast<float>(y) + .5f - cy;
            const float keep = clamp01(root(dx * dx + dy * dy) - radius + .5f);
            u32& p = pixels[y * extent + x];
            p = (static_cast<u32>(channel(p, 24) * keep * 255.f + .5f) << 24) | (p & 0x00FFFFFFu);
        }
}
void disc(Slice<u32> pixels, u32 extent, float cx, float cy, float radius, u32 color) noexcept {
    for (u32 y = 0; y < extent; ++y)
        for (u32 x = 0; x < extent; ++x) {
            const float dx = static_cast<float>(x) + .5f - cx, dy = static_cast<float>(y) + .5f - cy;
            const float coverage = clamp01(radius - root(dx * dx + dy * dy) + .5f);
            if (coverage > 0)
                pixels[y * extent + x] = over(pixels[y * extent + x], color | 0xFF000000u, coverage);
        }
}
void stroke(Slice<u32> pixels, u32 extent, float ax, float ay, float bx, float by, float half, u32 color) noexcept {
    for (u32 y = 0; y < extent; ++y)
        for (u32 x = 0; x < extent; ++x) {
            const float d = segment_distance(static_cast<float>(x) + .5f, static_cast<float>(y) + .5f, ax, ay, bx, by);
            const float coverage = clamp01(half - d + .5f);
            if (coverage > 0)
                pixels[y * extent + x] = over(pixels[y * extent + x], color | 0xFF000000u, coverage);
        }
}

bool palette_equal(const TrayIconPalette& a, const TrayIconPalette& b) noexcept {
    return a.brand == b.brand && a.brand_shade == b.brand_shade && a.success == b.success && a.danger == b.danger &&
           a.glyph == b.glyph;
}
u32 append(wchar_t* text, u32 at, u32 capacity, const wchar_t* value) noexcept {
    while (*value && at + 1 < capacity)
        text[at++] = *value++;
    text[at] = 0;
    return at;
}
} // namespace

TrayIconState tray_icon_state(const TrayInput& input) noexcept {
    if (input.connected) {
        if (input.self_deaf || input.server_deaf)
            return TrayIconState::deafened;
        if (input.self_mute || input.server_mute || input.suppressed)
            return TrayIconState::muted;
        return input.speaking ? TrayIconState::speaking : TrayIconState::connected;
    }
    return input.unread || input.mentions ? TrayIconState::unread : TrayIconState::normal;
}

TrayPlan tray_plan(TrayModel* model, const TrayInput& input, bool available) noexcept {
    TrayPlan plan{};
    if (!model)
        return plan;
    if (!available) {
        plan.remove = model->shown;
        *model = {};
        return plan;
    }
    plan.state = tray_icon_state(input);
    plan.badge = plan.state == TrayIconState::unread ? taskbar_attention_badge(input.mentions, true, true) : 0;
    plan.add = !model->shown;
    plan.update = model->shown && (plan.state != model->state || plan.badge != model->badge);
    *model = {plan.state, plan.badge, true};
    return plan;
}

void tray_model_reset(TrayModel* model) noexcept {
    if (model)
        *model = {};
}

u32 tray_menu_build(const TrayInput& input, TrayMenuItem* items, u32 capacity) noexcept {
    if (!items || capacity < kTrayMenuItems)
        return 0;
    items[0] = {L"Open MaiCord", tray_command_open, false, false};
    items[1] = {L"Mute", tray_command_mute, input.self_mute || input.self_deaf, true};
    items[2] = {L"Deafen", tray_command_deafen, input.self_deaf, false};
    items[3] = {L"Quit MaiCord", tray_command_quit, false, true};
    return kTrayMenuItems;
}

u32 tray_tooltip(const TrayInput& input, wchar_t* text, u32 capacity) noexcept {
    if (!text || !capacity)
        return 0;
    u32 at = append(text, 0, capacity, L"MaiCord");
    const wchar_t* detail = nullptr;
    switch (tray_icon_state(input)) {
    case TrayIconState::deafened:
        detail = L" \x2014 Deafened";
        break;
    case TrayIconState::muted:
        detail = L" \x2014 Muted";
        break;
    case TrayIconState::connected:
    case TrayIconState::speaking:
        detail = L" \x2014 Voice connected";
        break;
    case TrayIconState::unread:
        detail = input.mentions ? L" \x2014 Unread mentions" : L" \x2014 Unread messages";
        break;
    default:
        break;
    }
    return detail ? append(text, at, capacity, detail) : at;
}

bool tray_icon_render(TrayIconState state, u8 badge, const TrayIconPalette& palette, u32 extent,
                      Slice<u32> pixels) noexcept {
    if (extent < 16 || extent > kTrayIconMaximumExtent || !pixels.data || pixels.count < extent * extent)
        return false;
    render_mark(palette, extent, pixels);
    const float e = static_cast<float>(extent);
    if (state == TrayIconState::normal || (state == TrayIconState::unread && !badge))
        return true;
    if (state == TrayIconState::unread && badge != kTaskbarBadgeDot) {
        // Mention count: the taskbar's badge in the lower-right corner.
        const u32 side = static_cast<u32>(e * .62f + .5f);
        u32 badge_pixels[kTrayIconMaximumExtent * kTrayIconMaximumExtent];
        if (side < 8 || !taskbar_attention_render_badge(badge, side, {badge_pixels, side * side}))
            return false;
        const float radius = static_cast<float>(side) * .5f;
        cut(pixels, extent, e - radius, e - radius, radius + e * .06f);
        for (u32 y = 0; y < side; ++y)
            for (u32 x = 0; x < side; ++x) {
                u32& p = pixels[(extent - side + y) * extent + extent - side + x];
                p = over(p, badge_pixels[y * side + x], 1);
            }
        return true;
    }
    const float radius = e * (state == TrayIconState::unread ? .2f : .27f);
    const float cx = e - radius - e * .02f, cy = cx;
    cut(pixels, extent, cx, cy, radius + e * .07f);
    const u32 fill = state == TrayIconState::connected || state == TrayIconState::speaking ? palette.success
                                                                                             : palette.danger;
    disc(pixels, extent, cx, cy, radius, fill);
    const float half = maximum(e * .045f, .7f), arm = radius * .5f;
    switch (state) {
    case TrayIconState::speaking:
        disc(pixels, extent, cx, cy, radius * .42f, palette.glyph);
        break;
    case TrayIconState::muted:
        stroke(pixels, extent, cx - arm, cy + arm, cx + arm, cy - arm, half, palette.glyph);
        break;
    case TrayIconState::deafened:
        stroke(pixels, extent, cx - arm, cy + arm, cx + arm, cy - arm, half, palette.glyph);
        stroke(pixels, extent, cx - arm, cy - arm, cx + arm, cy + arm, half, palette.glyph);
        break;
    default:
        break;
    }
    return true;
}

HICON system_tray_create_icon(TrayIconState state, u8 badge, const TrayIconPalette& palette, u32 extent) noexcept {
    static_assert(sizeof(BITMAPINFOHEADER) == 40);
    ScratchPages<u8[sizeof(BITMAPINFOHEADER) + kTrayIconMaximumExtent * kTrayIconMaximumExtent * sizeof(u32) +
                    kTrayIconMaximumExtent * 8]>
        scratch;
    if (!scratch.data)
        return nullptr;
    u8* resource = *scratch.data;
    auto* pixels = reinterpret_cast<u32*>(resource + sizeof(BITMAPINFOHEADER));
    if (!tray_icon_render(state, badge, palette, extent,
                          {pixels, kTrayIconMaximumExtent * kTrayIconMaximumExtent}))
        return nullptr;
    // Reverse only active rows. The center row of an odd extent stays in place.
    for (u32 y = 0; y < extent / 2; ++y) {
        u32* top = pixels + y * extent;
        u32* bottom = pixels + (extent - 1 - y) * extent;
        for (u32 x = 0; x < extent; ++x) {
            const u32 pixel = top[x];
            top[x] = bottom[x];
            bottom[x] = pixel;
        }
    }
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(extent);
    header.biHeight = static_cast<LONG>(extent * 2); // XOR image plus AND mask
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    copy_bytes(resource, &header, sizeof(header));
    const u32 pixel_bytes = extent * extent * static_cast<u32>(sizeof(u32));
    const u32 mask_bytes = extent * ((extent + 31) / 32 * 4);
    clear_bytes(resource + sizeof(header) + pixel_bytes, mask_bytes);
    const u32 total = static_cast<u32>(sizeof(header)) + pixel_bytes + mask_bytes;
    return CreateIconFromResourceEx(resource, total, TRUE, 0x00030000, static_cast<int>(extent),
                                    static_cast<int>(extent), LR_DEFAULTCOLOR);
}

bool system_tray_initialize(SystemTray* tray, HWND window, u32 callback_message) noexcept {
    if (!tray)
        return false;
    *tray = {};
    tray->window = window;
    tray->callback_message = callback_message;
    tray->created_message = RegisterWindowMessageW(L"TaskbarCreated");
    return window != nullptr;
}

bool system_tray_apply(SystemTray* tray, const TrayPlan& plan, const TrayIconPalette& palette, const wchar_t* tooltip,
                       u32 dpi) noexcept {
    if (!tray || !tray->window)
        return false;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = tray->window;
    data.uID = kTrayId;
    if (plan.remove) {
        if (tray->added)
            Shell_NotifyIconW(NIM_DELETE, &data);
        tray->added = false;
        if (tray->icon)
            DestroyIcon(static_cast<HICON>(tray->icon));
        tray->icon = nullptr;
        return true;
    }
    if (!plan.add && !plan.update && tray->added && palette_equal(palette, tray->palette))
        return true;
    const int metric = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : 96);
    const u32 extent = static_cast<u32>(metric < 16 ? 16 : metric > 64 ? 64 : metric);
    const HICON icon = system_tray_create_icon(plan.state, plan.badge, palette, extent);
    if (!icon)
        return false;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = tray->callback_message;
    data.hIcon = icon;
    append(data.szTip, 0, sizeof(data.szTip) / sizeof(data.szTip[0]), tooltip ? tooltip : L"MaiCord");
    bool applied = false;
    if (!tray->added || plan.add) {
        applied = Shell_NotifyIconW(NIM_ADD, &data) != FALSE || Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
        if (applied) {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
    } else {
        applied = Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
    }
    if (!applied) {
        DestroyIcon(icon);
        tray->added = false;
        tray_model_reset(&tray->model);
        return false;
    }
    if (tray->icon)
        DestroyIcon(static_cast<HICON>(tray->icon));
    tray->icon = icon;
    tray->added = true;
    tray->palette = palette;
    return true;
}

bool system_tray_shell_restarted(SystemTray* tray, UINT message) noexcept {
    if (!tray || !tray->created_message || message != tray->created_message)
        return false;
    tray->added = false;
    tray_model_reset(&tray->model);
    return true;
}

u32 system_tray_menu(SystemTray* tray, const TrayMenuItem* items, u32 count) noexcept {
    if (!tray || !tray->window || !items)
        return tray_command_none;
    const HMENU menu = CreatePopupMenu();
    if (!menu)
        return tray_command_none;
    for (u32 i = 0; i < count; ++i) {
        if (items[i].separator_before)
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (items[i].checked ? MF_CHECKED : MF_UNCHECKED), items[i].command,
                    items[i].label);
    }
    SetMenuDefaultItem(menu, tray_command_open, FALSE);
    POINT cursor{};
    GetCursorPos(&cursor);
    // The owner must be foreground or the menu won't close on an outside click.
    SetForegroundWindow(tray->window);
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const u32 command = static_cast<u32>(TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, cursor.x, cursor.y,
        tray->window, nullptr));
    PostMessageW(tray->window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    return command;
}

void system_tray_release(SystemTray* tray) noexcept {
    if (!tray)
        return;
    if (tray->added && tray->window) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = tray->window;
        data.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    if (tray->icon)
        DestroyIcon(static_cast<HICON>(tray->icon));
    *tray = {};
}

} // namespace maicord
