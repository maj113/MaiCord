#include "maicord/render/d3d11_device.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dcomp.hpp"

namespace maicord {
namespace {

inline constexpr UINT kSwapChainFlags =
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

// A hardware device with its immediate context, protected for the video decode
// workers that share it (their API calls do not alter graphics/compute
// bindings, and their queued work is ordered with copies). Protection and the
// D3D11.1 context are set up where the device is created, so a prestart worker
// can use the context at once.
struct HardwareDevice {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11DeviceContext1* context1;
    HRESULT result;
    D3D11DeviceOwnerError error;
};

HardwareDevice create_hardware_device() noexcept {
    HardwareDevice hardware{};
    hardware.error = D3D11DeviceOwnerError::hardware_device;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    // Decoder owners query video capabilities on this device. VIDEO_SUPPORT
    // only adds a WDDM-version requirement; it does not enable video decoding.
    // Commands run on the calling thread: the driver's submission worker costs
    // more CPU, context switches and heap than it saves on these small frames.
    hardware.result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS, levels,
        static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION, &hardware.device, nullptr, &hardware.context);
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(hardware.result)) {
        hardware.result = hardware.context->QueryInterface(__uuidof(ID3D11Multithread),
                                                           reinterpret_cast<void**>(&multithread));
    }
    if (SUCCEEDED(hardware.result)) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
        hardware.error = D3D11DeviceOwnerError::context_11_1;
        hardware.result = hardware.context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                                           reinterpret_cast<void**>(&hardware.context1));
    }
    if (FAILED(hardware.result)) {
        com_release(hardware.context);
        com_release(hardware.device);
    }
    return hardware;
}

// Hardware device creation is dominated by user-mode driver initialization. A
// worker started at process entry overlaps it with window and model setup,
// then runs the caller's continuation beside the main thread, which builds the
// swap chain and composition tree.
struct DevicePrestart {
    HANDLE thread;
    HANDLE ready;
    HardwareDevice hardware;
    D3D11DevicePrestartContinuation continuation;
};
DevicePrestart g_prestart{};

DWORD WINAPI prestart_device(void*) noexcept {
    DevicePrestart& prestart = g_prestart;
    prestart.hardware = create_hardware_device();
    // The adopting owner may release the device (a failed swap chain) while the
    // continuation still uses it, so the worker keeps its own references.
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (SUCCEEDED(prestart.hardware.result) && prestart.continuation) {
        device = prestart.hardware.device;
        context = prestart.hardware.context;
        device->AddRef();
        context->AddRef();
    }
    SetEvent(prestart.ready);
    if (device) {
        prestart.continuation(device, context);
        context->Release();
        device->Release();
    }
    return 0;
}

// Takes the worker's device once it exists; its continuation may still run. A
// result of E_FAIL: no worker started.
HardwareDevice adopt_prestart() noexcept {
    if (!g_prestart.ready) {
        return HardwareDevice{nullptr, nullptr, nullptr, E_FAIL, D3D11DeviceOwnerError::hardware_device};
    }
    WaitForSingleObject(g_prestart.ready, INFINITE);
    const HardwareDevice hardware = g_prestart.hardware;
    g_prestart.hardware.device = nullptr;
    g_prestart.hardware.context = nullptr;
    g_prestart.hardware.context1 = nullptr;
    return hardware;
}

D3D11DeviceOwnerResult failure(D3D11DeviceOwner* owner,
                               D3D11DeviceOwnerError error,
                               HRESULT native_error) noexcept {
    d3d11_device_owner_release(owner);
    return D3D11DeviceOwnerResult{0, 0, 0, native_error, error};
}

HRESULT create_target(D3D11DeviceOwner* owner) noexcept {
    ID3D11Texture2D* back_buffer = nullptr;
    const HRESULT acquired = owner->swap_chain->GetBuffer(
        0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&back_buffer));
    if (FAILED(acquired)) {
        return acquired;
    }
    const HRESULT created = owner->device->CreateRenderTargetView(
        back_buffer, nullptr, &owner->target);
    com_release(back_buffer);
    return created;
}

} // namespace

D3D11DeviceOwnerResult d3d11_device_owner_initialize(
    D3D11DeviceOwner* owner, HWND window) noexcept {
    if (owner->device || owner->context || owner->swap_chain || owner->target || owner->frame_latency_waitable) {
        return D3D11DeviceOwnerResult{
            0, 0, 0, E_INVALIDARG, D3D11DeviceOwnerError::invalid_input};
    }

    // MaiCord.exe delay-loads these, so the loader does not map them before the
    // entry point (which the device worker waits for). Map them here, in parallel
    // with the worker: GPU user-mode drivers import shell32, ole32 and advapi32
    // themselves, which would otherwise load on the device's critical path, and
    // composition needs dcomp right after it. The references stay for the process
    // lifetime.
    static constexpr const wchar_t* kParallelLibraries[] = {L"dcomp.dll", L"shell32.dll", L"ole32.dll",
                                                            L"advapi32.dll"};
    for (const wchar_t* library : kParallelLibraries)
        LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HardwareDevice hardware = adopt_prestart();
    const bool prestarted = SUCCEEDED(hardware.result);
    if (!prestarted) {
        // No worker, or it failed: retry on this thread; driver failures can be transient.
        hardware = create_hardware_device();
    }
    if (FAILED(hardware.result)) {
        return D3D11DeviceOwnerResult{0, 0, 0, hardware.result, hardware.error};
    }
    owner->device = hardware.device;
    owner->context = hardware.context;
    owner->context1 = hardware.context1;
    HRESULT result = S_OK;

    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    result = owner->device->QueryInterface(
        __uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device));
    if (SUCCEEDED(result)) {
        result = dxgi_device->GetAdapter(&adapter);
    }
    if (SUCCEEDED(result)) {
        result = adapter->GetParent(__uuidof(IDXGIFactory2),
                                    reinterpret_cast<void**>(&factory));
    }
    if (FAILED(result)) {
        com_release(factory);
        com_release(adapter);
        com_release(dxgi_device);
        return failure(owner, D3D11DeviceOwnerError::dxgi_factory, result);
    }

    RECT client{};
    GetClientRect(window, &client);
    const u32 width =
        static_cast<u32>(maximum<LONG>(1, client.right - client.left));
    const u32 height =
        static_cast<u32>(maximum<LONG>(1, client.bottom - client.top));

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width;
    description.Height = height;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    // Composition avoids the HWND redirection bitmap and stale strips during
    // interactive resizing.
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    description.Flags = kSwapChainFlags;

    result = factory->CreateSwapChainForComposition(
        owner->device, &description, nullptr, &owner->swap_chain);
    if (SUCCEEDED(result)) {
        result = DCompositionCreateDevice(
            dxgi_device, __uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(&owner->composition_device));
    }
    if (SUCCEEDED(result)) {
        result = owner->composition_device->CreateTargetForHwnd(
            window, TRUE, &owner->composition_target);
    }
    if (SUCCEEDED(result)) {
        result = owner->composition_device->CreateVisual(
            &owner->composition_visual);
    }
    if (SUCCEEDED(result)) {
        result = owner->composition_visual->SetContent(owner->swap_chain);
    }
    if (SUCCEEDED(result)) {
        result = owner->composition_target->SetRoot(owner->composition_visual);
    }
    if (SUCCEEDED(result)) {
        result = owner->composition_device->Commit();
    }
    com_release(factory);
    com_release(adapter);
    com_release(dxgi_device);
    if (FAILED(result)) {
        return failure(owner, D3D11DeviceOwnerError::composition_swap_chain,
                       result);
    }

    IDXGISwapChain2* latency = nullptr;
    result = owner->swap_chain->QueryInterface(
        __uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&latency));
    if (SUCCEEDED(result)) {
        // The maximum frame latency of a waitable swap chain is 1 by default.
        owner->frame_latency_waitable = latency->GetFrameLatencyWaitableObject();
        latency->Release();
        if (!owner->frame_latency_waitable) {
            result = E_FAIL;
        }
    }
    if (FAILED(result)) {
        return failure(owner, D3D11DeviceOwnerError::frame_latency, result);
    }

    owner->buffer_width = width;
    owner->buffer_height = height;
    result = create_target(owner);
    if (FAILED(result)) {
        return failure(owner, D3D11DeviceOwnerError::render_target, result);
    }
    u32 dpi = GetDpiForWindow(window);
    if (dpi == 0) {
        dpi = 96;
    }
    return D3D11DeviceOwnerResult{
        width, height, dpi, S_OK, D3D11DeviceOwnerError::none, prestarted};
}

D3D11DeviceOwnerResult d3d11_device_owner_resize(
    D3D11DeviceOwner* owner, u32 width, u32 height) noexcept {
    owner->context->OMSetRenderTargets(0, nullptr, nullptr);
    com_release(owner->target);
    HRESULT result = owner->swap_chain->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
    if (FAILED(result)) {
        // ResizeBuffers leaves the old buffers intact on failure. Recreate the
        // released view so a transient resize error cannot leave the owner
        // without a render target.
        const HRESULT restored = create_target(owner);
        if (FAILED(restored)) {
            return D3D11DeviceOwnerResult{
                0, 0, 0, restored, D3D11DeviceOwnerError::render_target};
        }
        return D3D11DeviceOwnerResult{
            0, 0, 0, result, D3D11DeviceOwnerError::resize};
    }
    owner->buffer_width = width;
    owner->buffer_height = height;
    result = create_target(owner);
    if (FAILED(result)) {
        return D3D11DeviceOwnerResult{
            0, 0, 0, result, D3D11DeviceOwnerError::render_target};
    }
    ++owner->resize_buffer_reallocations;
    return D3D11DeviceOwnerResult{
        width, height, 0, S_OK, D3D11DeviceOwnerError::none};
}

DWORD d3d11_device_owner_wait(const D3D11DeviceOwner& owner,
                              u32 timeout_ms) noexcept {
    if (!owner.frame_latency_waitable) {
        return WAIT_OBJECT_0;
    }
    return WaitForSingleObject(owner.frame_latency_waitable, timeout_ms);
}

HRESULT d3d11_device_owner_present(D3D11DeviceOwner* owner,
                                   u32 sync_interval,
                                   const RECT* dirty_rectangles,
                                   u32 dirty_count) noexcept {
    if (!dirty_rectangles || !dirty_count) {
        return owner->swap_chain->Present(sync_interval, 0);
    }
    DXGI_PRESENT_PARAMETERS parameters{};
    parameters.DirtyRectsCount = dirty_count;
    parameters.pDirtyRects = const_cast<RECT*>(dirty_rectangles);
    return owner->swap_chain->Present1(sync_interval, 0, &parameters);
}

void d3d11_device_prestart(D3D11DevicePrestartContinuation continuation) noexcept {
    if (g_prestart.thread) {
        return;
    }
    g_prestart.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_prestart.ready) {
        return;
    }
    g_prestart.continuation = continuation;
    // Driver initialization may use more stack than the small worker default.
    g_prestart.thread = CreateThread(nullptr, 1024u * 1024u, prestart_device, nullptr,
                                     STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!g_prestart.thread) {
        CloseHandle(g_prestart.ready);
        reset_value<DevicePrestart>(&g_prestart);
    }
}

void d3d11_device_prestart_finish() noexcept {
    if (!g_prestart.thread) {
        return;
    }
    WaitForSingleObject(g_prestart.thread, INFINITE);
    CloseHandle(g_prestart.thread);
    CloseHandle(g_prestart.ready);
    com_release(g_prestart.hardware.context1);
    com_release(g_prestart.hardware.context);
    com_release(g_prestart.hardware.device);
    reset_value<DevicePrestart>(&g_prestart);
}

HRESULT d3d11_device_owner_set_offset(D3D11DeviceOwner* owner, float x, float y) noexcept {
    if (!owner || !owner->composition_visual || !owner->composition_device)
        return E_POINTER;
    HRESULT result = owner->composition_visual->SetOffsetX(x);
    if (SUCCEEDED(result))
        result = owner->composition_visual->SetOffsetY(y);
    return SUCCEEDED(result) ? owner->composition_device->Commit() : result;
}

void d3d11_device_owner_release(D3D11DeviceOwner* owner) noexcept {
    com_release(owner->target);
    if (owner->composition_visual) {
        owner->composition_visual->SetContent(nullptr);
    }
    if (owner->composition_target) {
        owner->composition_target->SetRoot(nullptr);
    }
    if (owner->composition_device) {
        owner->composition_device->Commit();
    }
    com_release(owner->composition_visual);
    com_release(owner->composition_target);
    com_release(owner->composition_device);
    if (owner->frame_latency_waitable) {
        CloseHandle(owner->frame_latency_waitable);
        owner->frame_latency_waitable = nullptr;
    }
    com_release(owner->swap_chain);
    com_release(owner->context1);
    com_release(owner->context);
    com_release(owner->device);
    owner->buffer_width = 0;
    owner->buffer_height = 0;
    owner->resize_buffer_reallocations = 0;
}

} // namespace maicord
