// Compares top-level frame strategies for a custom title bar on one desktop:
//   native  WS_OVERLAPPEDWINDOW with the DWM caption (reference)
//   ncframe WM_NCCALCSIZE extends the client over the caption, DWM side and
//           bottom frame kept (resize borders, shadow, corners, animations)
//   extend  ncframe plus DwmExtendFrameIntoClientArea over the caption height
//   glass   client covers the whole window, DwmExtendFrameIntoClientArea(1px)
//           keeps the shadow; every resize border is hit-tested manually
//   popup   WS_POPUP, manual resize hit-testing, extended 1px frame + rounded
//           corner preference
// Each window is a DirectComposition flip swap chain, as in the client, and
// opens behind every other window without activation. Reports medians of
// creation, first present, first composition, WM_NCHITTEST / WM_NCMOUSEMOVE
// dispatch cost, steady frames and live-resize steps, with the CPU time
// other processes (DWM) spent meanwhile.
#include "maicord/core/time.hpp"
#include "benchmark_output.hpp"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <dwmapi.h>

namespace maicord {
namespace {

enum Mode : u32 {
    kNative,
    kNcFrame,
    kExtend,
    kGlass,
    kPopup,
    kModeCount
};
constexpr const char* kModeNames[kModeCount]{"native", "ncframe", "extend", "glass", "popup"};
constexpr u32 kSamples = 9;
constexpr u32 kHitCalls = 4096;
constexpr u32 kFrames = 60;
constexpr u32 kResizeSteps = 24;
constexpr int kClientWidth = 960, kClientHeight = 600, kCaptionDips = 32, kButtonDips = 32;

struct State {
    Mode mode;
    UINT dpi;
    int frame;   // resize border thickness in pixels
    int caption; // caption height in pixels
    u32 calcsize_calls;
} g;

void metric(const char* mode, const char* name, u64 value) noexcept {
    write_text(mode);
    write_text(".");
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text("\n");
}

u64 median(u64* values, u32 count) noexcept {
    for (u32 i = 1; i < count; ++i)
        for (u32 j = i; j > 0 && values[j - 1] > values[j]; --j) {
            const u64 swap = values[j];
            values[j] = values[j - 1];
            values[j - 1] = swap;
        }
    return count ? values[count / 2] : 0;
}

u64 thread_cycles() noexcept {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

// CPU time every other process (DWM, the shell) spent, in 100 ns units:
// system busy time less this process. Noisy, so read only as medians.
u64 other_cpu() noexcept {
    FILETIME idle{}, kernel{}, user{}, created{}, exited{}, own_kernel{}, own_user{};
    if (!GetSystemTimes(&idle, &kernel, &user) ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &own_kernel, &own_user))
        return 0;
    const auto value = [](const FILETIME& time) noexcept {
        return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    const u64 busy = value(kernel) + value(user) - value(idle);
    const u64 own = value(own_kernel) + value(own_user);
    return busy > own ? busy - own : 0;
}

bool custom_frame() noexcept {
    return g.mode == kNcFrame || g.mode == kExtend;
}

// Pure arithmetic over the window rectangle, as the client does.
LRESULT hit_test(HWND window, LPARAM lparam) noexcept {
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const int x = static_cast<short>(LOWORD(lparam)) - bounds.left;
    const int y = static_cast<short>(HIWORD(lparam)) - bounds.top;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (x < 0 || y < 0 || x >= width || y >= height)
        return HTNOWHERE;
    const bool zoomed = IsZoomed(window) != FALSE;
    // ncframe/extend keep native side and bottom borders outside the client.
    const int side = custom_frame() ? g.frame : 0;
    if (!zoomed) {
        const int border = g.frame;
        const bool left = x < border, right = x >= width - border;
        if (y < border)
            return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (!custom_frame()) {
            const bool bottom = y >= height - border;
            if (bottom)
                return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
            if (left)
                return HTLEFT;
            if (right)
                return HTRIGHT;
        } else if (x < side || x >= width - side || y >= height - side) {
            return DefWindowProcW(window, WM_NCHITTEST, 0, lparam);
        }
    }
    const int top = zoomed ? g.frame : 0;
    if (y - top >= g.caption)
        return HTCLIENT;
    const int button = MulDiv(kButtonDips, static_cast<int>(g.dpi), 96);
    const int from_right = width - side - (zoomed ? g.frame : 0) - x;
    if (from_right <= 0)
        return HTCLIENT;
    const int index = (from_right - 1) / button;
    return index == 0 ? HTCLOSE : index == 1 ? HTMAXBUTTON : index == 2 ? HTMINBUTTON : HTCAPTION;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_NCCALCSIZE:
        ++g.calcsize_calls;
        if (custom_frame()) {
            RECT* client = reinterpret_cast<RECT*>(lparam);
            const bool zoomed = IsZoomed(window) != FALSE;
            client->left += g.frame;
            client->right -= g.frame;
            client->bottom -= g.frame;
            if (zoomed)
                client->top += g.frame;
            return 0;
        }
        if (g.mode == kGlass)
            return 0;
        break;
    case WM_NCHITTEST:
        if (g.mode != kNative)
            return hit_test(window, lparam);
        break;
    case WM_NCACTIVATE:
        if (g.mode != kNative)
            return TRUE;
        break;
    case WM_NCMOUSEMOVE:
        if (g.mode != kNative) {
            // Custom buttons: a hover state change plus leave tracking.
            static WPARAM hovered;
            if (hovered != wparam) {
                hovered = wparam;
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE | TME_NONCLIENT, window, 0};
                TrackMouseEvent(&track);
            }
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(window, nullptr);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct Graphics {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGIFactory2* factory = nullptr;
    IDCompositionDevice* composition = nullptr;
};

struct Surface {
    IDXGISwapChain1* swap_chain = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual* visual = nullptr;
    ID3D11RenderTargetView* view = nullptr;
    void release() noexcept {
        if (view)
            view->Release();
        if (visual)
            visual->Release();
        if (target)
            target->Release();
        if (swap_chain)
            swap_chain->Release();
        *this = {};
    }
};

bool create_graphics(Graphics* graphics) noexcept {
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION,
                                 &graphics->device, nullptr, &graphics->context)))
        return false;
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    bool ok = SUCCEEDED(graphics->device->QueryInterface(__uuidof(IDXGIDevice),
                                                         reinterpret_cast<void**>(&dxgi))) &&
              SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
              SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                           reinterpret_cast<void**>(&graphics->factory))) &&
              SUCCEEDED(DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice),
                                                 reinterpret_cast<void**>(&graphics->composition)));
    if (adapter)
        adapter->Release();
    if (dxgi)
        dxgi->Release();
    return ok;
}

bool create_view(Graphics& graphics, Surface* surface) noexcept {
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(surface->swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                              reinterpret_cast<void**>(&buffer))))
        return false;
    const bool ok = SUCCEEDED(graphics.device->CreateRenderTargetView(buffer, nullptr, &surface->view));
    buffer->Release();
    return ok;
}

bool create_surface(Graphics& graphics, HWND window, Surface* surface) noexcept {
    RECT client{};
    GetClientRect(window, &client);
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = static_cast<UINT>(client.right);
    description.Height = static_cast<UINT>(client.bottom);
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    return SUCCEEDED(graphics.factory->CreateSwapChainForComposition(graphics.device, &description, nullptr,
                                                                     &surface->swap_chain)) &&
           SUCCEEDED(graphics.composition->CreateTargetForHwnd(window, TRUE, &surface->target)) &&
           SUCCEEDED(graphics.composition->CreateVisual(&surface->visual)) &&
           SUCCEEDED(surface->visual->SetContent(surface->swap_chain)) &&
           SUCCEEDED(surface->target->SetRoot(surface->visual)) &&
           SUCCEEDED(graphics.composition->Commit()) && create_view(graphics, surface);
}

void draw(Graphics& graphics, Surface& surface, u32 frame) noexcept {
    const float shade = static_cast<float>(frame & 7) / 64.0f;
    const float color[4]{0.06f + shade, 0.06f, 0.07f, 1};
    graphics.context->OMSetRenderTargets(1, &surface.view, nullptr);
    graphics.context->ClearRenderTargetView(surface.view, color);
}

DWORD style_for(Mode mode) noexcept {
    return mode == kPopup ? WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX : WS_OVERLAPPEDWINDOW;
}

void apply_frame(HWND window, Mode mode) noexcept {
    if (mode == kExtend) {
        const MARGINS margins{0, 0, g.caption, 0};
        DwmExtendFrameIntoClientArea(window, &margins);
    } else if (mode == kGlass || mode == kPopup) {
        const MARGINS margins{0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(window, &margins);
    }
    if (mode == kPopup) {
        const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }
}

struct Sample {
    u64 create_us, present_us, composed_us, create_cycles, present_cycles;
    u64 hit_caption_cycles, hit_button_cycles, hit_border_cycles, hit_client_cycles, move_cycles;
    u64 frame_cycles, frame_other_us, frame_us, resize_cycles, resize_other_us, resize_us, calcsize_calls;
};

u64 hit_cost(HWND window, POINT point) noexcept {
    const LPARAM lparam = MAKELPARAM(point.x, point.y);
    SendMessageW(window, WM_NCHITTEST, 0, lparam);
    const u64 begin = thread_cycles();
    for (u32 i = 0; i < kHitCalls; ++i)
        SendMessageW(window, WM_NCHITTEST, 0, lparam);
    return (thread_cycles() - begin) / kHitCalls;
}

bool run(Graphics& graphics, Mode mode, const Clock& clock, Sample* out) noexcept {
    g.mode = mode;
    g.calcsize_calls = 0;
    const DWORD ex_style = WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    // Same client content height in every mode: the custom frames carry the
    // caption inside the client.
    RECT bounds{0, 0, kClientWidth, kClientHeight};
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, FALSE, ex_style, g.dpi);
    int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (mode == kPopup || mode == kGlass) {
        width = kClientWidth + 2 * g.frame;
        height = kClientHeight + g.caption + 2 * g.frame;
    }
    const i64 t0 = clock_now();
    const u64 c0 = thread_cycles();
    const HWND window = CreateWindowExW(ex_style, L"MaiCordWindowFrameBenchmark",
                                        L"MaiCord frame benchmark", style_for(mode), 64, 64, width, height,
                                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window)
        return false;
    apply_frame(window, mode);
    const i64 t1 = clock_now();
    const u64 c1 = thread_cycles();
    Surface surface{};
    bool ok = create_surface(graphics, window, &surface);
    if (ok) {
        draw(graphics, surface, 0);
        SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        ok = SUCCEEDED(surface.swap_chain->Present(0, 0));
    }
    const i64 t2 = clock_now();
    const u64 c2 = thread_cycles();
    DwmFlush();
    const i64 t3 = clock_now();
    out->create_us = clock_microseconds(clock, t0, t1);
    out->present_us = clock_microseconds(clock, t1, t2);
    out->composed_us = clock_microseconds(clock, t0, t3);
    out->create_cycles = c1 - c0;
    out->present_cycles = c2 - c1;

    RECT window_rect{};
    GetWindowRect(window, &window_rect);
    const int top = window_rect.top, right = window_rect.right;
    const int button = MulDiv(kButtonDips, static_cast<int>(g.dpi), 96);
    // Native caption metrics come from the system; probe the same screen points.
    const int caption_y = top + g.frame + g.caption / 2;
    out->hit_caption_cycles = hit_cost(window, {(window_rect.left + right) / 2, caption_y});
    out->hit_button_cycles = hit_cost(window, {right - g.frame - button / 2, caption_y});
    out->hit_border_cycles = hit_cost(window, {window_rect.left + 2, (top + window_rect.bottom) / 2});
    out->hit_client_cycles =
        hit_cost(window, {(window_rect.left + right) / 2, (top + window_rect.bottom) / 2});
    {
        const LPARAM point = MAKELPARAM(right - g.frame - button / 2, caption_y);
        const u64 begin = thread_cycles();
        for (u32 i = 0; i < kHitCalls; ++i) {
            const LRESULT hit = SendMessageW(window, WM_NCHITTEST, 0, point);
            SendMessageW(window, WM_NCMOUSEMOVE, static_cast<WPARAM>((i & 1) ? hit : HTCAPTION), point);
        }
        out->move_cycles = (thread_cycles() - begin) / kHitCalls;
    }

    // Steady frames paced by DWM, one small dirty rectangle each.
    if (ok) {
        surface.swap_chain->Present(1, 0);
        const u64 other_begin = other_cpu();
        const u64 begin = thread_cycles();
        const i64 frames_begin = clock_now();
        for (u32 frame = 1; ok && frame <= kFrames; ++frame) {
            draw(graphics, surface, frame);
            RECT dirty{0, 0, 64, 64};
            DXGI_PRESENT_PARAMETERS parameters{1, &dirty, nullptr, nullptr};
            ok = SUCCEEDED(surface.swap_chain->Present1(1, 0, &parameters));
        }
        DwmFlush();
        out->frame_cycles = (thread_cycles() - begin) / kFrames;
        out->frame_us = clock_microseconds(clock, frames_begin, clock_now()) / kFrames;
        out->frame_other_us = (other_cpu() - other_begin) / 10 / kFrames;
    }

    // Live-resize steps: window resize, buffer resize, present, composition.
    if (ok) {
        const u32 calcsize_before = g.calcsize_calls;
        const u64 other_begin = other_cpu();
        const u64 begin = thread_cycles();
        const i64 resize_begin = clock_now();
        for (u32 step = 0; ok && step < kResizeSteps; ++step) {
            const int delta = static_cast<int>((step % 12) * 8);
            SetWindowPos(window, nullptr, 0, 0, width + delta, height + delta / 2,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client{};
            GetClientRect(window, &client);
            surface.view->Release();
            surface.view = nullptr;
            graphics.context->OMSetRenderTargets(0, nullptr, nullptr);
            ok = SUCCEEDED(surface.swap_chain->ResizeBuffers(0, static_cast<UINT>(client.right),
                                                             static_cast<UINT>(client.bottom),
                                                             DXGI_FORMAT_UNKNOWN, 0)) &&
                 create_view(graphics, &surface);
            if (ok) {
                draw(graphics, surface, step);
                ok = SUCCEEDED(surface.swap_chain->Present(1, 0));
            }
        }
        DwmFlush();
        out->resize_us = clock_microseconds(clock, resize_begin, clock_now()) / kResizeSteps;
        out->resize_cycles = (thread_cycles() - begin) / kResizeSteps;
        out->resize_other_us = (other_cpu() - other_begin) / 10 / kResizeSteps;
        out->calcsize_calls = g.calcsize_calls - calcsize_before;
    }
    graphics.context->OMSetRenderTargets(0, nullptr, nullptr);
    surface.release();
    graphics.composition->Commit();
    DestroyWindow(window);
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        DispatchMessageW(&message);
    return ok;
}

} // namespace
} // namespace maicord

extern "C" void maicord_window_frame_benchmark_entry() noexcept {
    using namespace maicord;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"MaiCordWindowFrameBenchmark";
    if (!RegisterClassExW(&window_class))
        ExitProcess(1);
    Graphics graphics{};
    if (!create_graphics(&graphics))
        ExitProcess(2);
    g.dpi = GetDpiForSystem();
    g.frame = GetSystemMetricsForDpi(SM_CXFRAME, g.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g.dpi);
    g.caption = MulDiv(kCaptionDips, static_cast<int>(g.dpi), 96);
    const Clock clock = clock_initialize();
    metric("system", "dpi", g.dpi);
    metric("system", "frame_px", static_cast<u64>(g.frame));
    static Sample samples[kModeCount][kSamples];
    // Warm the driver, DComp and the class once per mode before sampling.
    for (u32 mode = 0; mode < kModeCount; ++mode) {
        Sample warm{};
        if (!run(graphics, static_cast<Mode>(mode), clock, &warm))
            ExitProcess(10 + mode);
    }
    for (u32 sample = 0; sample < kSamples; ++sample)
        for (u32 step = 0; step < kModeCount; ++step) {
            // Rotate the order so drift and warm-up spread over every mode.
            const u32 mode = (step + sample) % kModeCount;
            if (!run(graphics, static_cast<Mode>(mode), clock, &samples[mode][sample]))
                ExitProcess(20 + mode);
        }
    for (u32 mode = 0; mode < kModeCount; ++mode) {
        u64 values[kSamples];
        const auto report = [&](const char* name, u64 Sample::* field) noexcept {
            for (u32 i = 0; i < kSamples; ++i)
                values[i] = samples[mode][i].*field;
            metric(kModeNames[mode], name, median(values, kSamples));
        };
        report("create_us", &Sample::create_us);
        report("create_cycles", &Sample::create_cycles);
        report("show_present_us", &Sample::present_us);
        report("show_present_cycles", &Sample::present_cycles);
        report("first_composed_us", &Sample::composed_us);
        report("nchittest_caption_cycles", &Sample::hit_caption_cycles);
        report("nchittest_button_cycles", &Sample::hit_button_cycles);
        report("nchittest_border_cycles", &Sample::hit_border_cycles);
        report("nchittest_client_cycles", &Sample::hit_client_cycles);
        report("ncmousemove_cycles", &Sample::move_cycles);
        report("frame_cycles", &Sample::frame_cycles);
        report("frame_other_cpu_us", &Sample::frame_other_us);
        report("frame_us", &Sample::frame_us);
        report("resize_step_us", &Sample::resize_us);
        report("resize_step_cycles", &Sample::resize_cycles);
        report("resize_step_other_cpu_us", &Sample::resize_other_us);
        report("resize_calcsize_calls", &Sample::calcsize_calls);
    }
    ExitProcess(0);
}
