#include "maicord/gpu/api.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dcomp.hpp"

namespace maicord {
namespace {

inline constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

// A failure during creation releases what was made; the stage is logged here, where it is known.
GpuStatus failure(GpuSurface* surface, const char* stage, HRESULT native_error) noexcept {
    log_write(LogLevel::error, "render", stage);
    gpu_surface_release(surface);
    return native_error;
}

HRESULT create_target(GpuDevice* device, GpuSurface* surface) noexcept {
    ID3D11Texture2D* back_buffer = nullptr;
    const HRESULT acquired = surface->swap_chain->GetBuffer(
        0, __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&back_buffer));
    if (FAILED(acquired)) {
        return acquired;
    }
    const HRESULT created = device->CreateRenderTargetView(
        back_buffer, nullptr, &surface->target);
    com_release(back_buffer);
    return created;
}

} // namespace

GpuStatus gpu_surface_create(GpuDevice* device, const GpuSurfaceDesc& desc, GpuSurface* surface) {
    if (!device || !desc.window || surface->swap_chain || surface->target || surface->frame_latency_waitable) {
        return E_INVALIDARG;
    }
    HRESULT result = S_OK;

    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    result = device->QueryInterface(
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
        return failure(surface, "dxgi_factory_unavailable", result);
    }

    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = desc.width;
    description.Height = desc.height;
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
        device, &description, nullptr, &surface->swap_chain);
    if (SUCCEEDED(result)) {
        result = DCompositionCreateDevice(
            dxgi_device, __uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(&surface->composition_device));
    }
    if (SUCCEEDED(result)) {
        result = surface->composition_device->CreateTargetForHwnd(
            static_cast<HWND>(desc.window), TRUE, &surface->composition_target);
    }
    if (SUCCEEDED(result)) {
        result = surface->composition_device->CreateVisual(
            &surface->composition_visual);
    }
    if (SUCCEEDED(result)) {
        result = surface->composition_visual->SetContent(surface->swap_chain);
    }
    if (SUCCEEDED(result)) {
        result = surface->composition_target->SetRoot(surface->composition_visual);
    }
    if (SUCCEEDED(result)) {
        result = surface->composition_device->Commit();
    }
    com_release(factory);
    com_release(adapter);
    com_release(dxgi_device);
    if (FAILED(result)) {
        return failure(surface, "direct_composition_swap_chain_creation_failed", result);
    }

    IDXGISwapChain2* latency = nullptr;
    result = surface->swap_chain->QueryInterface(
        __uuidof(IDXGISwapChain2), reinterpret_cast<void**>(&latency));
    if (SUCCEEDED(result)) {
        // The maximum frame latency of a waitable swap chain is 1 by default.
        surface->frame_latency_waitable = latency->GetFrameLatencyWaitableObject();
        latency->Release();
        if (!surface->frame_latency_waitable) {
            result = E_FAIL;
        }
    }
    if (FAILED(result)) {
        return failure(surface, "dxgi_1_3_resize_surface_unavailable", result);
    }

    surface->buffer_width = desc.width;
    surface->buffer_height = desc.height;
    result = create_target(device, surface);
    if (FAILED(result)) {
        return failure(surface, "create_render_target_failed", result);
    }
    return S_OK;
}

GpuStatus gpu_surface_resize(GpuSurface* surface, u32 width, u32 height) {
    ID3D11Device* device = nullptr;
    surface->swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    com_release(surface->target);
    HRESULT result = surface->swap_chain->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
    if (FAILED(result)) {
        // ResizeBuffers leaves the old buffers intact on failure. Recreate the
        // released view so a transient resize error cannot leave the surface
        // without a render target.
        const HRESULT restored = create_target(device, surface);
        com_release(context);
        com_release(device);
        if (FAILED(restored)) {
            log_write(LogLevel::error, "render", "create_render_target_failed");
            return restored;
        }
        log_write(LogLevel::error, "render", "swap_chain_resize_failed");
        return result;
    }
    surface->buffer_width = width;
    surface->buffer_height = height;
    result = create_target(device, surface);
    com_release(context);
    com_release(device);
    if (FAILED(result)) {
        log_write(LogLevel::error, "render", "create_render_target_failed");
        return result;
    }
    ++surface->resize_buffer_reallocations;
    return S_OK;
}

bool gpu_frame_try_acquire(GpuSurface* surface) {
    return gpu_frame_wait(surface, 0);
}

bool gpu_frame_wait(GpuSurface* surface, u32 timeout_ms) {
    if (!surface->frame_latency_waitable) {
        return true;
    }
    return WaitForSingleObject(surface->frame_latency_waitable, timeout_ms) == WAIT_OBJECT_0;
}

GpuStatus gpu_surface_present(GpuSurface* surface, u32 sync_interval, const GpuRect* dirty, u32 dirty_count) {
    if (!dirty || !dirty_count) {
        return surface->swap_chain->Present(sync_interval, 0);
    }
    DXGI_PRESENT_PARAMETERS parameters{};
    parameters.DirtyRectsCount = dirty_count;
    parameters.pDirtyRects = const_cast<RECT*>(reinterpret_cast<const RECT*>(dirty));
    return surface->swap_chain->Present1(sync_interval, 0, &parameters);
}

GpuStatus gpu_surface_set_offset(GpuSurface* surface, float x, float y, bool* needs_present) {
    if (needs_present)
        *needs_present = false;
    if (!surface || !surface->composition_visual || !surface->composition_device)
        return E_POINTER;
    HRESULT result = surface->composition_visual->SetOffsetX(x);
    if (SUCCEEDED(result))
        result = surface->composition_visual->SetOffsetY(y);
    return SUCCEEDED(result) ? surface->composition_device->Commit() : result;
}

void gpu_surface_frame_callback(GpuSurface*) {}

void gpu_surface_release(GpuSurface* surface) {
    com_release(surface->target);
    if (surface->composition_visual) {
        surface->composition_visual->SetContent(nullptr);
    }
    if (surface->composition_target) {
        surface->composition_target->SetRoot(nullptr);
    }
    if (surface->composition_device) {
        surface->composition_device->Commit();
    }
    com_release(surface->composition_visual);
    com_release(surface->composition_target);
    com_release(surface->composition_device);
    if (surface->frame_latency_waitable) {
        CloseHandle(surface->frame_latency_waitable);
        surface->frame_latency_waitable = nullptr;
    }
    com_release(surface->swap_chain);
    surface->buffer_width = 0;
    surface->buffer_height = 0;
    surface->resize_buffer_reallocations = 0;
}

} // namespace maicord
