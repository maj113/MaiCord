#include "maicord/render/d3d11_device.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dcomp.hpp"

namespace maicord {
namespace {

inline constexpr UINT kSwapChainFlags =
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

// Hardware device creation is dominated by user-mode driver initialization. A
// worker started at process entry overlaps it with window and model setup, then
// runs the caller's continuation while the main thread builds the swap chain.
struct DevicePrestart {
    HANDLE thread;
    HANDLE ready;
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    D3D_FEATURE_LEVEL feature_level;
    HRESULT result;
    D3D11DevicePrestartContinuation continuation;
    void* continuation_context;
};
DevicePrestart g_prestart{};

HRESULT create_hardware_device(ID3D11Device** device, D3D_FEATURE_LEVEL* level,
                               ID3D11DeviceContext** context) noexcept {
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };
    // Decoder owners query video capabilities on this device. VIDEO_SUPPORT
    // only adds a WDDM-version requirement; it does not enable video decoding.
    return D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
        static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
        D3D11_SDK_VERSION, device, level, context);
}

DWORD WINAPI prestart_device(void*) noexcept {
    DevicePrestart& prestart = g_prestart;
    prestart.result = create_hardware_device(
        &prestart.device, &prestart.feature_level, &prestart.context);
    // The adopting owner may release the device (a failed swap chain) while
    // the continuation still creates objects on it. Device object creation is
    // free-threaded; the continuation never touches the immediate context.
    ID3D11Device* device = SUCCEEDED(prestart.result) && prestart.continuation
                               ? prestart.device
                               : nullptr;
    if (device) {
        device->AddRef();
    }
    SetEvent(prestart.ready);
    if (device) {
        prestart.continuation(device, prestart.continuation_context);
        device->Release();
    }
    return 0;
}

// Takes the worker's device once it exists; the continuation may still run.
HRESULT adopt_prestart(D3D11DeviceOwner* owner) noexcept {
    if (!g_prestart.ready) {
        return E_FAIL;
    }
    WaitForSingleObject(g_prestart.ready, INFINITE);
    if (FAILED(g_prestart.result) || !g_prestart.device || !g_prestart.context) {
        return FAILED(g_prestart.result) ? g_prestart.result : E_FAIL;
    }
    owner->device = g_prestart.device;
    owner->context = g_prestart.context;
    owner->feature_level = static_cast<u32>(g_prestart.feature_level);
    g_prestart.device = nullptr;
    g_prestart.context = nullptr;
    return S_OK;
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
    if (!owner || !window || owner->device || owner->context ||
        owner->swap_chain || owner->target || owner->frame_latency_waitable) {
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
    HRESULT result = adopt_prestart(owner);
    const bool prestarted = SUCCEEDED(result);
    if (FAILED(result)) {
        // No worker, or it failed: retry on this thread; driver failures can be transient.
        D3D_FEATURE_LEVEL level{};
        result = create_hardware_device(&owner->device, &level, &owner->context);
        owner->feature_level = static_cast<u32>(level);
    }
    if (FAILED(result)) {
        return failure(owner, D3D11DeviceOwnerError::hardware_device, result);
    }

    // Video decode workers and presentation share this device. Protect the
    // immediate context before publishing it; video API calls do not alter
    // graphics/compute bindings, and their queued work is ordered with copies.
    ID3D11Multithread* multithread = nullptr;
    result = owner->context->QueryInterface(__uuidof(ID3D11Multithread),
        reinterpret_cast<void**>(&multithread));
    if (FAILED(result)) {
        return failure(owner, D3D11DeviceOwnerError::hardware_device, result);
    }
    multithread->SetMultithreadProtected(TRUE);
    multithread->Release();

    result = owner->context->QueryInterface(
        __uuidof(ID3D11DeviceContext1),
        reinterpret_cast<void**>(&owner->context1));
    if (FAILED(result)) {
        return failure(owner, D3D11DeviceOwnerError::context_11_1, result);
    }

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
    description.Stereo = FALSE;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.Scaling = DXGI_SCALING_STRETCH;
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

    result = owner->swap_chain->QueryInterface(
        __uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&owner->swap_chain2));
    if (SUCCEEDED(result)) {
        result = owner->swap_chain2->SetMaximumFrameLatency(1);
    }
    if (SUCCEEDED(result)) {
        owner->frame_latency_waitable =
            owner->swap_chain2->GetFrameLatencyWaitableObject();
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
    if (!owner || !owner->swap_chain || !owner->swap_chain2 ||
        !owner->context || width == 0 || height == 0) {
        return D3D11DeviceOwnerResult{
            0, 0, 0, E_INVALIDARG, D3D11DeviceOwnerError::invalid_input};
    }
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
                                   const RECT* dirty_rectangle) noexcept {
    if (!owner || !owner->swap_chain) {
        return E_INVALIDARG;
    }
    if (!dirty_rectangle) {
        return owner->swap_chain->Present(sync_interval, 0);
    }
    RECT dirty = *dirty_rectangle;
    DXGI_PRESENT_PARAMETERS parameters{};
    parameters.DirtyRectsCount = 1;
    parameters.pDirtyRects = &dirty;
    return owner->swap_chain->Present1(sync_interval, 0, &parameters);
}

void d3d11_device_prestart(D3D11DevicePrestartContinuation continuation,
                           void* context) noexcept {
    if (g_prestart.thread) {
        return;
    }
    g_prestart.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_prestart.ready) {
        return;
    }
    g_prestart.continuation = continuation;
    g_prestart.continuation_context = context;
    // Driver initialization may use more stack than the small worker default.
    g_prestart.thread = CreateThread(nullptr, 1024u * 1024u, prestart_device, nullptr,
                                     STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!g_prestart.thread) {
        CloseHandle(g_prestart.ready);
        g_prestart = DevicePrestart{};
    }
}

void d3d11_device_prestart_finish() noexcept {
    if (!g_prestart.thread) {
        return;
    }
    WaitForSingleObject(g_prestart.thread, INFINITE);
    CloseHandle(g_prestart.thread);
    CloseHandle(g_prestart.ready);
    com_release(g_prestart.context);
    com_release(g_prestart.device);
    g_prestart = DevicePrestart{};
}

void d3d11_device_owner_release(D3D11DeviceOwner* owner) noexcept {
    if (!owner) {
        return;
    }
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
    com_release(owner->swap_chain2);
    com_release(owner->swap_chain);
    com_release(owner->context1);
    com_release(owner->context);
    com_release(owner->device);
    owner->buffer_width = 0;
    owner->buffer_height = 0;
    owner->resize_buffer_reallocations = 0;
    owner->feature_level = 0;
}

} // namespace maicord
