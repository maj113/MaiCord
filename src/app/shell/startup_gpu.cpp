// The GPU line of the startup record (startup_record.hpp), apart so the rest of the record builds without a GPU backend.
#include "startup_record.hpp"
#include "maicord/core/log_event.hpp"
#if defined(_WIN32)
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dxgi.hpp"
#endif

namespace maicord::app_detail {

void application_log_start_gpu(GpuDevice* device) noexcept {
    if (!device)
        return;
    LogEvent line("gpu");
#if defined(_WIN32)
    line.field("backend", "d3d11");
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC description{};
    // Every Direct3D 11 device is an IDXGIDevice.
    device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
    if (SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&description))) {
        // The adapter name is ASCII in practice; anything else becomes '?'.
        char name[128];
        u32 used = 0;
        for (; description.Description[used] && used + 1 < sizeof(name); ++used)
            name[used] = description.Description[used] < 0x80 ? static_cast<char>(description.Description[used]) : '?';
        name[used] = 0;
        line.quoted("adapter", name)
            .hex("vendor", description.VendorId)
            .hex("device", description.DeviceId)
            .field("vram_mib", description.DedicatedVideoMemory >> 20);
        LARGE_INTEGER driver{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver))) {
            // The user-mode driver's version, a.b.c.d in its four 16-bit parts.
            char version[24];
            used = 0;
            const u64 value = static_cast<u64>(driver.QuadPart);
            for (u32 part = 0; part < 4; ++part) {
                if (part)
                    version[used++] = '.';
                used += decimal_write(version + used, (value >> (48 - part * 16)) & 0xFFFFu);
            }
            version[used] = 0;
            line.field("driver", version);
        }
    }
    com_release(adapter);
    com_release(dxgi);
    // D3D_FEATURE_LEVEL_11_1 is 0xb100: the major version in the top nibbles, the minor in the next.
    const u32 level = static_cast<u32>(device->GetFeatureLevel());
    char feature[8];
    u32 at = decimal_write(feature, level >> 12);
    feature[at++] = '_';
    at += decimal_write(feature + at, (level >> 8) & 0xFu);
    feature[at] = 0;
    line.field("feature_level", feature);
#else
    // Vendor, renderer and GL ES version in one string; under WSL the renderer names Mesa's d3d12 driver.
    const char* description = gpu_device_description(device);
    line.field("backend", "gles").quoted("description", description);
    bool d3d12 = false;
    for (const char* at = description; at && *at && !d3d12; ++at)
        d3d12 = at[0] == 'D' && at[1] == '3' && at[2] == 'D' && at[3] == '1' && at[4] == '2';
    if (d3d12)
        line.field("wsl_d3d12", u64{1});
#endif
    line.write(LogLevel::info, "start");
}

} // namespace maicord::app_detail
