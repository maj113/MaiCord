#pragma once

#include "maicord/core/base.hpp"
#include "maicord/gpu/gpu.hpp"

namespace maicord {

enum class D3D11DeviceOwnerError : u8 {
    none,
    hardware_device,
    // The surface could not be created; the backend logged which step failed.
    surface,
};

struct D3D11DeviceOwnerResult {
    GpuStatus native_error;
    D3D11DeviceOwnerError error;
    // Initialization adopted the gpu_device_prestart worker's device.
    bool prestarted;
    // Identifies the adapter (vendor, device, revision and LUID): what a per-GPU record is keyed by.
    u64 adapter_key;

    constexpr bool ok() const noexcept {
        return error == D3D11DeviceOwnerError::none;
    }
};

// Owns the device, its immediate context and the surface frames are presented on. Logical UI dimensions, paint
// policy and shader resources remain renderer concerns.
struct D3D11DeviceOwner {
    GpuDevice* device;
    GpuContext* context;
    GpuContext1* context1;
    GpuSurface surface;
};

#if defined(_WIN32)
static_assert(sizeof(D3D11DeviceOwner) == 88);
#endif

// Makes the device (adopting the prestart worker's when there is one), then the surface for `surface`'s window.
D3D11DeviceOwnerResult d3d11_device_owner_initialize(D3D11DeviceOwner* owner, const GpuSurfaceDesc& surface) noexcept;
// Releases the surface, then the context and device. An empty owner is left empty.
void d3d11_device_owner_release(D3D11DeviceOwner* owner) noexcept;

} // namespace maicord
