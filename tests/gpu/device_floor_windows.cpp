// The Direct3D 11 device floor of gpu_device_create (src/gpu/d3d11/device.cpp): a driver that cannot make a Direct3D 11
// fence (below WDDM 2.0) is refused with DXGI_ERROR_UNSUPPORTED, which the shell reports as "Direct3D 11 unavailable"
// and a request to update the graphics driver; a driver that can is accepted. The refusal is made real by patching
// ID3D11Device5::CreateFence (slot 68 in the Windows SDK's order) in the runtime's device vtable, which every device of
// this process shares, and restoring it afterwards.
#include "maicord/gpu/api.hpp"
#include "d3d_harness.hpp"

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool condition, const char* label) noexcept {
    test_check(&g_failures, condition, label);
}

constexpr u32 kCreateFenceSlot = 68;

HRESULT STDMETHODCALLTYPE refuse_fence(ID3D11Device5*, UINT64, D3D11_FENCE_FLAG, REFIID, void** fence) noexcept {
    *fence = nullptr;
    return E_NOTIMPL;
}

// Swaps the CreateFence slot of `table` for `function`; returns the previous one, or null when the page stays locked.
void* swap_slot(void** table, void* function) noexcept {
    DWORD old = 0;
    if (!VirtualProtect(&table[kCreateFenceSlot], sizeof(void*), PAGE_READWRITE, &old))
        return nullptr;
    void* previous = table[kCreateFenceSlot];
    table[kCreateFenceSlot] = function;
    VirtualProtect(&table[kCreateFenceSlot], sizeof(void*), old, &old);
    return previous;
}

void run() noexcept {
    // A device of the class gpu_device_create makes, to reach the runtime's vtable.
    ID3D11Device* probe = nullptr;
    ID3D11DeviceContext* probe_context = nullptr;
    check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                      &probe, nullptr, &probe_context)),
          "a hardware device");
    if (!probe)
        return;
    ID3D11Device5* probe5 = nullptr;
    check(SUCCEEDED(probe->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&probe5))),
          "the device has ID3D11Device5");
    if (!probe5) {
        com_release(probe_context);
        com_release(probe);
        return;
    }
    void** table = *reinterpret_cast<void***>(probe5);

    GpuDevice* device = nullptr;
    GpuContext* context = nullptr;
    void* original = swap_slot(table, reinterpret_cast<void*>(&refuse_fence));
    check(original != nullptr, "CreateFence is replaced");
    if (original) {
        const GpuStatus refused = gpu_device_create(&device, &context);
        check(refused == DXGI_ERROR_UNSUPPORTED && !device && !context,
              "a driver without Direct3D 11 fences is refused as unsupported");
        if (device)
            gpu_device_destroy(&device, &context);
        check(swap_slot(table, original) == reinterpret_cast<void*>(&refuse_fence), "CreateFence is restored");
    }

    const GpuStatus accepted = gpu_device_create(&device, &context);
    check(gpu_ok(accepted) && device && context, "a driver with Direct3D 11 fences is accepted");
    GpuContext4* signaller = nullptr;
    GpuFence* fence = nullptr;
    if (device) {
        check(gpu_ok(gpu_fence_create(device, &signaller, &fence)) && signaller && fence,
              "the accepted device makes fences");
        com_release(fence);
        com_release(signaller);
        gpu_device_destroy(&device, &context);
    }
    probe5->Release();
    com_release(probe_context);
    com_release(probe);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_gpu_device_floor_entry) {
    maicord::run();
    maicord::test_exit(maicord::g_failures, "maicord_gpu_device_floor");
}
