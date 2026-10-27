#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11DeviceContext1;
struct ID3D11RenderTargetView;
struct IDCompositionDevice;
struct IDCompositionTarget;
struct IDCompositionVisual;
struct IDXGISwapChain1;
struct IDXGISwapChain2;

namespace maicord {

enum class D3D11DeviceOwnerError : u8 {
    none,
    invalid_input,
    hardware_device,
    context_11_1,
    dxgi_factory,
    composition_swap_chain,
    frame_latency,
    render_target,
    resize,
};

struct D3D11DeviceOwnerResult {
    u32 width;
    u32 height;
    u32 dpi;
    HRESULT native_error;
    D3D11DeviceOwnerError error;
    // Initialization adopted the d3d11_device_prestart worker's device.
    bool prestarted;

    constexpr bool ok() const noexcept {
        return error == D3D11DeviceOwnerError::none;
    }
};

// Owns the platform D3D11/DXGI/DirectComposition lifetime graph. Logical UI
// dimensions, paint policy and shader resources remain renderer concerns.
struct D3D11DeviceOwner {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11DeviceContext1* context1;
    IDXGISwapChain1* swap_chain;
    IDXGISwapChain2* swap_chain2;
    IDCompositionDevice* composition_device;
    IDCompositionTarget* composition_target;
    IDCompositionVisual* composition_visual;
    ID3D11RenderTargetView* target;
    HANDLE frame_latency_waitable;
    u32 buffer_width;
    u32 buffer_height;
    u32 resize_buffer_reallocations;
    u32 feature_level; // D3D_FEATURE_LEVEL
};

static_assert(sizeof(D3D11DeviceOwner) == 96);

// Runs on the prestart worker with its new device; it may create device
// objects but must not use the immediate context.
using D3D11DevicePrestartContinuation = void (*)(ID3D11Device* device) noexcept;
// Starts hardware device creation on a worker thread so user-mode driver
// initialization overlaps the rest of startup. The next owner initialization
// adopts that device as soon as it exists, or creates one itself when none
// was started or the worker failed. Main thread only.
void d3d11_device_prestart(D3D11DevicePrestartContinuation continuation) noexcept;
// Waits for the worker and its continuation; releases a device no owner adopted.
void d3d11_device_prestart_finish() noexcept;
D3D11DeviceOwnerResult d3d11_device_owner_initialize(
    D3D11DeviceOwner* owner, HWND window) noexcept;
D3D11DeviceOwnerResult d3d11_device_owner_resize(
    D3D11DeviceOwner* owner, u32 width, u32 height) noexcept;
DWORD d3d11_device_owner_wait(const D3D11DeviceOwner& owner,
                              u32 timeout_ms) noexcept;
// A null dirty_rectangles presents the whole buffer.
HRESULT d3d11_device_owner_present(D3D11DeviceOwner* owner,
                                   u32 sync_interval,
                                   const RECT* dirty_rectangles,
                                   u32 dirty_count = 1) noexcept;
void d3d11_device_owner_release(D3D11DeviceOwner* owner) noexcept;

} // namespace maicord
