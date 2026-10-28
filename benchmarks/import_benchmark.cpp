#include "maicord/core/module.hpp"
#include "maicord/core/time.hpp"
#include "benchmark_output.hpp"
#include <unknwn.h>
#include <shellapi.h>

namespace maicord {
namespace {

constexpr u32 kIterations = 1'000'000;
volatile u64 g_checksum;
using ModuleFunction = HINSTANCE (*)() noexcept;
using GuidFunction = bool (*)(REFGUID, REFGUID) noexcept;
using DropFunction = void (*)(HWND, bool) noexcept;

__declspec(noinline) void original_drop(HWND window, bool enabled) noexcept {
    const bool registered = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) != 0;
    if (registered != enabled) DragAcceptFiles(window, enabled ? TRUE : FALSE);
}

__declspec(noinline) void direct_drop(HWND window, bool enabled) noexcept {
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (((style & WS_EX_ACCEPTFILES) != 0) != enabled)
        SetWindowLongPtrW(window, GWL_EXSTYLE,
                         enabled ? style | WS_EX_ACCEPTFILES : style & ~LONG_PTR{WS_EX_ACCEPTFILES});
}

__declspec(noinline) HINSTANCE lookup_module() noexcept {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&lookup_module), &module);
    return module;
}

__declspec(noinline) HINSTANCE linked_module() noexcept {
    return module_instance();
}

__declspec(noinline) bool byte_guid(REFGUID left, REFGUID right) noexcept {
    const auto* a = reinterpret_cast<const volatile u8*>(&left);
    const auto* b = reinterpret_cast<const volatile u8*>(&right);
    bool equal = true;
    for (u32 i = 0; i < sizeof(GUID); ++i) equal = equal && a[i] == b[i];
    return equal;
}

__declspec(noinline) bool sdk_guid(REFGUID left, REFGUID right) noexcept {
    return InlineIsEqualGUID(left, right) != 0;
}

u64 measure_module(ModuleFunction function, const Clock& clock) noexcept {
    // Reload the callback each iteration so LTCG cannot hoist the constant
    // image address. Both paths pay the same indirect-call overhead.
    ModuleFunction volatile invoke = function;
    u64 checksum = 0;
    const i64 begin = clock_now();
    for (u32 i = 0; i < kIterations; ++i)
        checksum += reinterpret_cast<usize>(invoke());
    const i64 end = clock_now();
    g_checksum = checksum;
    return clock_microseconds(clock, begin, end);
}

u64 measure_guid(GuidFunction function, const Clock& clock) noexcept {
    GuidFunction volatile invoke = function;
    const GUID reference = __uuidof(IUnknown);
    GUID candidates[4] = {reference, reference, reference, reference};
    candidates[1].Data1 ^= 1;
    candidates[2].Data4[0] ^= 1;
    candidates[3].Data4[7] ^= 1;
    u64 checksum = 0;
    const i64 begin = clock_now();
    for (u32 i = 0; i < kIterations; ++i)
        checksum += invoke(reference, candidates[i & 3]);
    const i64 end = clock_now();
    g_checksum = checksum;
    return checksum == kIterations / 4 ? clock_microseconds(clock, begin, end) : 0;
}

u64 measure_drop(DropFunction function, HWND window, const Clock& clock) noexcept {
    const i64 begin = clock_now();
    for (u32 i = 0; i < 10'000; ++i) function(window, (i & 1) == 0);
    return clock_microseconds(clock, begin, clock_now());
}

} // namespace
} // namespace maicord

extern "C" void maicord_import_benchmark_entry() noexcept {
    using namespace maicord;
    if (!lookup_module() || lookup_module() != module_instance() ||
        GetModuleHandleW(nullptr) != module_instance()) ExitProcess(1);
    const GUID reference = __uuidof(IUnknown);
    if (!sdk_guid(reference, reference)) ExitProcess(2);
    for (u32 bit = 0; bit < 128; ++bit) {
        GUID different = reference;
        reinterpret_cast<u8*>(&different)[bit / 8] ^= static_cast<u8>(1u << (bit % 8));
        if (sdk_guid(reference, different) || byte_guid(reference, different)) ExitProcess(3);
    }
    const Clock clock = clock_initialize();
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"MaiCord import benchmark",
        WS_OVERLAPPED, 0, 0, 32, 32, nullptr, nullptr, module_instance(), nullptr);
    if (!window) ExitProcess(4);
    const LONG_PTR initial_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    direct_drop(window, true);
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) != (initial_style | WS_EX_ACCEPTFILES)) ExitProcess(5);
    direct_drop(window, false);
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) != initial_style) ExitProcess(6);
    // Resolve the original wrapper before warm measurements.
    original_drop(window, true);
    original_drop(window, false);
    write_value("iterations", kIterations);
    for (u32 sample = 0; sample < 15; ++sample) {
        write_value("sample", sample + 1);
        // Alternate order to distribute warmup and frequency drift.
        if (sample & 1) {
            write_value("module_linked_us", measure_module(linked_module, clock));
            write_value("module_lookup_us", measure_module(lookup_module, clock));
            write_value("guid_sdk_us", measure_guid(sdk_guid, clock));
            write_value("guid_bytes_us", measure_guid(byte_guid, clock));
            write_value("drop_direct_us", measure_drop(direct_drop, window, clock));
            write_value("drop_original_us", measure_drop(original_drop, window, clock));
        } else {
            write_value("module_lookup_us", measure_module(lookup_module, clock));
            write_value("module_linked_us", measure_module(linked_module, clock));
            write_value("guid_bytes_us", measure_guid(byte_guid, clock));
            write_value("guid_sdk_us", measure_guid(sdk_guid, clock));
            write_value("drop_original_us", measure_drop(original_drop, window, clock));
            write_value("drop_direct_us", measure_drop(direct_drop, window, clock));
        }
    }
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) != initial_style) ExitProcess(7);
    DestroyWindow(window);
    ExitProcess(0);
}
