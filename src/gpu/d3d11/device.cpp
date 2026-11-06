#include "maicord/gpu/api.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace {

// A hardware device with its immediate context, protected for the video decode workers that share it (their API
// calls do not alter graphics/compute bindings, and their queued work is ordered with copies). Protection is set up
// where the device is created, so a prestart worker can use the context at once.
struct HardwareDevice {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    HRESULT result;
};

HardwareDevice create_hardware_device() noexcept {
    HardwareDevice hardware{};
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
    }
    // Atlases are tiled textures (gpu_paged_texture_*): the device floor is Tiled Resources Tier 1. A device below
    // it is refused with DXGI_ERROR_UNSUPPORTED, which the renderer logs and the shell names to the user.
    if (SUCCEEDED(hardware.result)) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS1 options{};
        if (FAILED(hardware.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &options, sizeof(options))) ||
            options.TiledResourcesTier < D3D11_TILED_RESOURCES_TIER_1) {
            log_write(LogLevel::error, "gpu", "d3d11_device_lacks_tiled_resources_tier_1");
            hardware.result = DXGI_ERROR_UNSUPPORTED;
        }
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
    GpuDevicePrestartContinuation continuation;
    // The device gpu_device_create handed out of the worker's.
    const ID3D11Device* adopted;
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
        return HardwareDevice{nullptr, nullptr, E_FAIL};
    }
    WaitForSingleObject(g_prestart.ready, INFINITE);
    const HardwareDevice hardware = g_prestart.hardware;
    g_prestart.hardware.device = nullptr;
    g_prestart.hardware.context = nullptr;
    // A device created later (after a device loss) is made on the calling thread.
    g_prestart.hardware.result = E_FAIL;
    g_prestart.adopted = hardware.device;
    return hardware;
}

} // namespace

GpuStatus gpu_device_create(GpuDevice** device, GpuContext** context) {
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
    if (FAILED(hardware.result)) {
        // No worker, or it failed: retry on this thread; driver failures can be transient.
        hardware = create_hardware_device();
    }
    if (FAILED(hardware.result))
        return hardware.result;
    *device = hardware.device;
    *context = hardware.context;
    return S_OK;
}

void gpu_device_destroy(GpuDevice** device, GpuContext** context) {
    com_release(*context);
    com_release(*device);
}

const GpuCaps& gpu_caps(const GpuDevice* device) {
    // One device at a time: the limits of Direct3D 11 feature level 11, and the one optional feature in use.
    static GpuCaps caps;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    const bool options_known =
        SUCCEEDED(const_cast<GpuDevice*>(device)->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options,
                                                                      sizeof(options)));
    caps = GpuCaps{16384, 65536, 1024, 32768, 8, true, true, true, true, true, true,
                   options_known && options.MapNoOverwriteOnDynamicBufferSRV, false, true, false};
    return caps;
}

const char* gpu_device_description(const GpuDevice*) {
    return "Direct3D 11";
}

u64 gpu_adapter_key(const GpuDevice* device) {
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    u64 key = 0;
    HRESULT result = const_cast<GpuDevice*>(device)->QueryInterface(__uuidof(IDXGIDevice),
                                                                    reinterpret_cast<void**>(&dxgi_device));
    if (SUCCEEDED(result))
        result = dxgi_device->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC description{};
    if (SUCCEEDED(result) && SUCCEEDED(adapter->GetDesc(&description))) {
        const u32 parts[]{description.VendorId, description.DeviceId, description.SubSysId,
                          description.Revision, description.AdapterLuid.LowPart,
                          static_cast<u32>(description.AdapterLuid.HighPart)};
        key = kHash64Seed;
        for (const u32 part : parts)
            key = hash64_mix(key, part);
    }
    com_release(adapter);
    com_release(dxgi_device);
    return key;
}

bool gpu_device_lost(GpuDevice* device) {
    return device && FAILED(device->GetDeviceRemovedReason());
}

void gpu_device_set_native_display(void*) {}

void gpu_device_prestart(GpuDevicePrestartContinuation continuation) {
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

void gpu_device_prestart_wait() {
    if (g_prestart.thread) {
        WaitForSingleObject(g_prestart.thread, INFINITE);
    }
}

void gpu_device_prestart_finish() {
    if (!g_prestart.thread) {
        return;
    }
    WaitForSingleObject(g_prestart.thread, INFINITE);
    CloseHandle(g_prestart.thread);
    CloseHandle(g_prestart.ready);
    com_release(g_prestart.hardware.context);
    com_release(g_prestart.hardware.device);
    reset_value<DevicePrestart>(&g_prestart);
}

bool gpu_device_prestarted(const GpuDevice* device) {
    return device && device == g_prestart.adopted;
}

GpuContext1* gpu_context_1(GpuContext* context) {
    ID3D11DeviceContext1* extended = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&extended))))
        extended->Release();
    return extended;
}

} // namespace maicord
