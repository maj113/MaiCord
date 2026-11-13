#include "maicord/render/d3d11/device.hpp"
#include "maicord/gpu/api.hpp"

namespace maicord {

D3D11DeviceOwnerResult d3d11_device_owner_initialize(D3D11DeviceOwner* owner, const GpuSurfaceDesc& surface) noexcept {
    const GpuStatus created = gpu_device_create(&owner->device, &owner->context);
    if (!gpu_ok(created)) {
        return D3D11DeviceOwnerResult{created, D3D11DeviceOwnerError::hardware_device};
    }
    owner->context1 = gpu_context_1(owner->context);
    const bool prestarted = gpu_device_prestarted(owner->device);
    const u64 adapter_key = gpu_adapter_key(owner->device);
    const GpuStatus presentable = gpu_surface_create(owner->device, surface, &owner->surface);
    if (!gpu_ok(presentable)) {
        d3d11_device_owner_release(owner);
        return D3D11DeviceOwnerResult{presentable, D3D11DeviceOwnerError::surface};
    }
    return D3D11DeviceOwnerResult{kGpuOk, D3D11DeviceOwnerError::none, prestarted, adapter_key};
}

void d3d11_device_owner_release(D3D11DeviceOwner* owner) noexcept {
    gpu_surface_release(&owner->surface);
    owner->context1 = nullptr;
    gpu_device_destroy(&owner->device, &owner->context);
}

} // namespace maicord
