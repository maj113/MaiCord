// Startup attribution for MaiCordPerfProbe (see perf_startup.hpp), from process
// creation to the first presented frame: named main-thread points with CPU, DLL
// mappings (LdrRegisterDllNotification), system calls through this image's static
// and delay-load import tables, main-thread waits, memory at the first present,
// and the D3D11 device, DXGI factory, swap chain and DirectComposition methods
// reached through their vtables. Nothing here ships.
#include "perf_startup.hpp"

#include "maicord/core/base.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/crypto.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dcomp.hpp"
#include "maicord/sys/gdi.hpp"
#include "maicord/sys/ole.hpp"
#include "maicord/sys/process_info.hpp"
#include "maicord/sys/registry.hpp"
#include "maicord/sys/winrt.hpp"

namespace maicord::perf {
namespace {

struct StartupClock {
    u64 qpc0;
    u64 frequency;
    u64 create_offset_us; // process creation to qpc0
    u64 tsc0;
    DWORD main_thread;
    u64 image_base;
    bool ready;
};
StartupClock g_clock{};

u64 qpc() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<u64>(value.QuadPart);
}
u64 at_us(u64 at) noexcept {
    return g_clock.create_offset_us + (at - g_clock.qpc0) * 1'000'000ull / g_clock.frequency;
}
u64 span_us(u64 ticks) noexcept {
    return ticks * 1'000'000ull / g_clock.frequency;
}
bool on_main() noexcept {
    return GetCurrentThreadId() == g_clock.main_thread;
}
u64 thread_cycles() noexcept {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

// Attribution stops at the first presented frame.
volatile LONG g_window_closed = 0;
bool window_open() noexcept {
    return g_window_closed == 0;
}

struct Mark {
    const char* name;
    u64 at;
    u64 cycles;
};
constexpr u32 kMarkCapacity = 80;
Mark g_marks[kMarkCapacity];
u32 g_mark_count = 0;
u64 g_entry_cycles = 0;
u64 g_present_at = 0;
u64 g_present_cycles = 0;
u64 g_present_process_cycles = 0;
// Memory at the first presented frame (one call each, no address-space walk).
struct FirstFrameMemory {
    u64 private_bytes, working_set, private_working_set, peak_working_set, gpu_local, gpu_nonlocal;
    u32 handles, modules;
};
FirstFrameMemory g_first_frame_memory{};
ID3D11Device* g_first_device = nullptr; // not owned

struct ModuleName {
    wchar_t text[40];
};
void copy_name(ModuleName* out, const wchar_t* text, u32 units) noexcept {
    u32 i = 0;
    for (; i < units && i + 1 < 40; ++i)
        out->text[i] = text[i];
    out->text[i] = 0;
}
struct DllEvent {
    ModuleName name;
    u64 at;
    u32 kb;
    bool main;
    bool window;
};
constexpr u32 kDllCapacity = 192;
DllEvent g_dlls[kDllCapacity];
volatile LONG g_dll_count = 0;
volatile LONG g_dll_unloads = 0;
struct EntryModule {
    ModuleName name;
    u32 kb;
};
constexpr u32 kEntryModuleCapacity = 96;
EntryModule g_entry_modules[kEntryModuleCapacity];
u32 g_entry_module_count = 0;

struct LdrString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};
struct LdrLoaded {
    ULONG flags;
    const LdrString* full_name;
    const LdrString* base_name;
    void* base;
    ULONG size;
};
using LdrNotification = void(CALLBACK*)(ULONG, const LdrLoaded*, void*);
using LdrRegister = LONG(NTAPI*)(ULONG, LdrNotification, void*, void**);

// Runs under the loader lock: no allocation, no loads.
void CALLBACK dll_notification(ULONG reason, const LdrLoaded* data, void*) noexcept {
    if (reason == 2) {
        InterlockedIncrement(&g_dll_unloads);
        return;
    }
    if (reason != 1 || !data)
        return;
    const LONG slot = InterlockedIncrement(&g_dll_count) - 1;
    if (slot >= static_cast<LONG>(kDllCapacity))
        return;
    DllEvent& event = g_dlls[slot];
    event.at = qpc();
    event.kb = data->size / 1024;
    event.main = on_main();
    event.window = window_open();
    if (data->base_name && data->base_name->buffer)
        copy_name(&event.name, data->base_name->buffer, data->base_name->length / 2);
}

enum class Api : u32 {
    d3d11_create_device,
    dcomp_create_device,
    register_class,
    create_window,
    show_window,
    set_window_pos,
    load_library_ex,
    load_library,
    co_initialize,
    ole_initialize,
    co_create_instance,
    co_register_class_object,
    ro_initialize,
    ro_get_activation_factory,
    create_thread,
    create_file,
    read_file,
    get_font_data,
    reg_open_key,
    reg_query_value,
    reg_get_value,
    bcrypt_open_algorithm,
    wait_single,
    wait_single_ex,
    wait_multiple,
    msg_wait,
    count
};
const char* const kApiNames[] = {
    "D3D11CreateDevice", "DCompositionCreateDevice", "RegisterClassExW", "CreateWindowExW", "ShowWindow",
    "SetWindowPos", "LoadLibraryExW", "LoadLibraryW", "CoInitializeEx", "OleInitialize", "CoCreateInstance",
    "CoRegisterClassObject", "RoInitialize", "RoGetActivationFactory", "CreateThread", "CreateFileW", "ReadFile",
    "GetFontData", "RegOpenKeyExW", "RegQueryValueExW", "RegGetValueW", "BCryptOpenAlgorithmProvider",
    "WaitForSingleObject", "WaitForSingleObjectEx", "WaitForMultipleObjects", "MsgWaitForMultipleObjectsEx"};
static_assert(sizeof(kApiNames) / sizeof(kApiNames[0]) == static_cast<u32>(Api::count));

struct ApiStat {
    volatile LONG64 calls;
    volatile LONG64 ticks;
    volatile LONG64 main_calls;
    volatile LONG64 main_ticks;
    volatile LONG64 max_ticks;
    volatile LONG64 max_at; // start of the longest call (diagnostic; not atomic with max_ticks)
    volatile LONG64 first_at;
    volatile LONG64 later_calls;
};
ApiStat g_api[static_cast<u32>(Api::count)];

// Main-thread waits and module loads longer than this are listed one by one.
constexpr u64 kEventMinimumUs = 200;
struct TimedEvent {
    Api api;
    u64 at;
    u64 ticks;
    u64 caller_rva;
    ModuleName name;
};
constexpr u32 kEventCapacity = 64;
TimedEvent g_events[kEventCapacity];
volatile LONG g_event_count = 0;

u64 caller_rva(void* address) noexcept {
    const u64 value = reinterpret_cast<u64>(address);
    return value > g_clock.image_base ? value - g_clock.image_base : 0;
}

void record(Api api, u64 start, void* caller, const wchar_t* name = nullptr) noexcept {
    const u64 end = qpc();
    ApiStat& stat = g_api[static_cast<u32>(api)];
    if (!window_open()) {
        InterlockedIncrement64(&stat.later_calls);
        return;
    }
    const LONG64 ticks = static_cast<LONG64>(end - start);
    InterlockedIncrement64(&stat.calls);
    InterlockedAdd64(&stat.ticks, ticks);
    InterlockedCompareExchange64(&stat.first_at, static_cast<LONG64>(start), 0);
    LONG64 seen = stat.max_ticks;
    while (ticks > seen) {
        const LONG64 previous = InterlockedCompareExchange64(&stat.max_ticks, ticks, seen);
        if (previous == seen) {
            stat.max_at = static_cast<LONG64>(start);
            break;
        }
        seen = previous;
    }
    const bool main = on_main();
    if (main) {
        InterlockedIncrement64(&stat.main_calls);
        InterlockedAdd64(&stat.main_ticks, ticks);
    }
    const bool load = api == Api::load_library || api == Api::load_library_ex;
    if ((main || load) && span_us(static_cast<u64>(ticks)) >= kEventMinimumUs) {
        const LONG slot = InterlockedIncrement(&g_event_count) - 1;
        if (slot < static_cast<LONG>(kEventCapacity)) {
            TimedEvent& event = g_events[slot];
            event.api = api;
            event.at = start;
            event.ticks = static_cast<u64>(ticks);
            event.caller_rva = caller_rva(caller);
            if (name) {
                u32 units = 0;
                while (name[units] && units < 512)
                    ++units;
                u32 begin = units;
                while (begin && name[begin - 1] != L'\\' && name[begin - 1] != L'/')
                    --begin;
                copy_name(&event.name, name + begin, units - begin);
            }
        }
    }
}

// Vtable attribution: D3D11 device creation methods (d3d11.h / d3d11_1.h
// order), the DXGI factory's CreateSwapChainForComposition, swap chain presents
// and the DirectComposition device. All are x64 COM methods whose arguments are
// integers or pointers, so one forwarding stub per slot passes them through.
enum class Vt : u32 {
    create_buffer,
    create_texture1d,
    create_texture2d,
    create_texture3d,
    create_srv,
    create_uav,
    create_rtv,
    create_dsv,
    create_input_layout,
    create_vertex_shader,
    create_geometry_shader,
    create_geometry_shader_so,
    create_pixel_shader,
    create_hull_shader,
    create_domain_shader,
    create_compute_shader,
    create_class_linkage,
    create_blend_state,
    create_depth_stencil_state,
    create_rasterizer_state,
    create_sampler_state,
    create_query,
    create_predicate,
    create_counter,
    create_deferred_context,
    create_blend_state1,
    create_rasterizer_state1,
    swap_chain_for_composition,
    present,
    present1,
    dcomp_commit,
    dcomp_target_for_hwnd,
    dcomp_visual,
    count
};
const char* const kVtNames[] = {"CreateBuffer", "CreateTexture1D", "CreateTexture2D", "CreateTexture3D",
    "CreateShaderResourceView", "CreateUnorderedAccessView", "CreateRenderTargetView", "CreateDepthStencilView",
    "CreateInputLayout", "CreateVertexShader", "CreateGeometryShader", "CreateGeometryShaderWithStreamOutput",
    "CreatePixelShader", "CreateHullShader", "CreateDomainShader", "CreateComputeShader", "CreateClassLinkage",
    "CreateBlendState", "CreateDepthStencilState", "CreateRasterizerState", "CreateSamplerState", "CreateQuery",
    "CreatePredicate", "CreateCounter", "CreateDeferredContext", "CreateBlendState1", "CreateRasterizerState1",
    "CreateSwapChainForComposition", "Present", "Present1", "DCompCommit", "DCompCreateTargetForHwnd",
    "DCompCreateVisual"};
static_assert(sizeof(kVtNames) / sizeof(kVtNames[0]) == static_cast<u32>(Vt::count));
ApiStat g_vt[static_cast<u32>(Vt::count)];
void* g_vt_original[static_cast<u32>(Vt::count)]{};

struct PresentEvent {
    u64 at;
    u64 ticks;
    bool main;
};
constexpr u32 kPresentCapacity = 6;
PresentEvent g_presents[kPresentCapacity];
volatile LONG g_present_count = 0;

void record_vt(Vt slot, u64 start) noexcept {
    const u64 end = qpc();
    ApiStat& stat = g_vt[static_cast<u32>(slot)];
    const LONG64 ticks = static_cast<LONG64>(end - start);
    if (slot == Vt::present || slot == Vt::present1) {
        const LONG index = InterlockedIncrement(&g_present_count) - 1;
        if (index < static_cast<LONG>(kPresentCapacity))
            g_presents[index] = {start, static_cast<u64>(ticks), on_main()};
    }
    if (!window_open()) {
        InterlockedIncrement64(&stat.later_calls);
        return;
    }
    InterlockedIncrement64(&stat.calls);
    InterlockedAdd64(&stat.ticks, ticks);
    InterlockedCompareExchange64(&stat.first_at, static_cast<LONG64>(start), 0);
    if (ticks > stat.max_ticks) {
        stat.max_ticks = ticks;
        stat.max_at = static_cast<LONG64>(start);
    }
    if (on_main()) {
        InterlockedIncrement64(&stat.main_calls);
        InterlockedAdd64(&stat.main_ticks, ticks);
    }
}

using Forward = HRESULT(STDMETHODCALLTYPE*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
template <Vt Slot>
HRESULT STDMETHODCALLTYPE vt_stub(void* self, void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h,
                                  void* i) noexcept {
    const u64 start = qpc();
    const HRESULT result =
        reinterpret_cast<Forward>(g_vt_original[static_cast<u32>(Slot)])(self, a, b, c, d, e, f, g, h, i);
    record_vt(Slot, start);
    return result;
}

bool patch_slot(void* object, u32 index, Vt slot, void* replacement) noexcept {
    void** table = *static_cast<void***>(object);
    if (table[index] == replacement || g_vt_original[static_cast<u32>(slot)])
        return false;
    DWORD old = 0;
    if (!VirtualProtect(&table[index], sizeof(void*), PAGE_READWRITE, &old))
        return false;
    g_vt_original[static_cast<u32>(slot)] = table[index];
    table[index] = replacement;
    VirtualProtect(&table[index], sizeof(void*), old, &old);
    return true;
}

template <Vt First, u32 FirstIndex, u32 Count>
void patch_range(void* object) noexcept {
    if constexpr (Count > 0) {
        patch_slot(object, FirstIndex, First, reinterpret_cast<void*>(&vt_stub<First>));
        patch_range<static_cast<Vt>(static_cast<u32>(First) + 1), FirstIndex + 1, Count - 1>(object);
    }
}

HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain* self, UINT interval, UINT flags) noexcept {
    const u64 start = qpc();
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
    const HRESULT result = reinterpret_cast<Fn>(g_vt_original[static_cast<u32>(Vt::present)])(self, interval, flags);
    record_vt(Vt::present, start);
    return result;
}
HRESULT STDMETHODCALLTYPE hook_present1(IDXGISwapChain1* self, UINT interval, UINT flags,
                                        const DXGI_PRESENT_PARAMETERS* parameters) noexcept {
    const u64 start = qpc();
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    const HRESULT result =
        reinterpret_cast<Fn>(g_vt_original[static_cast<u32>(Vt::present1)])(self, interval, flags, parameters);
    record_vt(Vt::present1, start);
    return result;
}
HRESULT STDMETHODCALLTYPE hook_swap_chain_for_composition(IDXGIFactory2* self, IUnknown* device,
                                                          const DXGI_SWAP_CHAIN_DESC1* description,
                                                          IDXGIOutput* output, IDXGISwapChain1** swap_chain) noexcept {
    const u64 start = qpc();
    using Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                           IDXGISwapChain1**);
    const HRESULT result = reinterpret_cast<Fn>(g_vt_original[static_cast<u32>(Vt::swap_chain_for_composition)])(
        self, device, description, output, swap_chain);
    record_vt(Vt::swap_chain_for_composition, start);
    if (SUCCEEDED(result) && swap_chain && *swap_chain) {
        patch_slot(*swap_chain, 8, Vt::present, reinterpret_cast<void*>(&hook_present));
        patch_slot(*swap_chain, 22, Vt::present1, reinterpret_cast<void*>(&hook_present1));
    }
    return result;
}

void patch_device(ID3D11Device* device) noexcept {
    // CreateBuffer (3) through CreateDeferredContext (27), then the 11.1 states.
    patch_range<Vt::create_buffer, 3, 25>(device);
    patch_slot(device, 45, Vt::create_blend_state1, reinterpret_cast<void*>(&vt_stub<Vt::create_blend_state1>));
    patch_slot(device, 46, Vt::create_rasterizer_state1,
               reinterpret_cast<void*>(&vt_stub<Vt::create_rasterizer_state1>));
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory))))
        patch_slot(factory, 24, Vt::swap_chain_for_composition,
                   reinterpret_cast<void*>(&hook_swap_chain_for_composition));
    if (factory)
        factory->Release();
    if (adapter)
        adapter->Release();
    if (dxgi)
        dxgi->Release();
}

// Import hooks
#define MC_HOOK(api, ret, function, params, args)                                                                 \
    using function##_type = ret(WINAPI*) params;                                                                  \
    function##_type g_##function = nullptr;                                                                       \
    ret WINAPI hook_##function params noexcept {                                                                  \
        const u64 start = qpc();                                                                                  \
        const ret result = g_##function args;                                                                     \
        record(Api::api, start, _ReturnAddress());                                                                \
        return result;                                                                                            \
    }
MC_HOOK(register_class, ATOM, RegisterClassExW, (const WNDCLASSEXW* c), (c))
MC_HOOK(create_window, HWND, CreateWindowExW,
        (DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
         HINSTANCE instance, LPVOID param),
        (ex, cls, name, style, x, y, w, h, parent, menu, instance, param))
MC_HOOK(show_window, BOOL, ShowWindow, (HWND window, int show), (window, show))
MC_HOOK(set_window_pos, BOOL, SetWindowPos, (HWND window, HWND after, int x, int y, int w, int h, UINT flags),
        (window, after, x, y, w, h, flags))
MC_HOOK(co_initialize, HRESULT, CoInitializeEx, (LPVOID reserved, DWORD model), (reserved, model))
MC_HOOK(ole_initialize, HRESULT, OleInitialize, (LPVOID reserved), (reserved))
MC_HOOK(co_create_instance, HRESULT, CoCreateInstance,
        (REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* out), (clsid, outer, context, iid, out))
MC_HOOK(co_register_class_object, HRESULT, CoRegisterClassObject,
        (REFCLSID clsid, LPUNKNOWN unknown, DWORD context, DWORD flags, LPDWORD cookie),
        (clsid, unknown, context, flags, cookie))
MC_HOOK(ro_initialize, HRESULT, RoInitialize, (RO_INIT_TYPE type), (type))
MC_HOOK(ro_get_activation_factory, HRESULT, RoGetActivationFactory, (HSTRING id, REFIID iid, void** out),
        (id, iid, out))
MC_HOOK(create_thread, HANDLE, CreateThread,
        (LPSECURITY_ATTRIBUTES a, SIZE_T stack, LPTHREAD_START_ROUTINE routine, LPVOID p, DWORD flags, LPDWORD id),
        (a, stack, routine, p, flags, id))
MC_HOOK(create_file, HANDLE, CreateFileW,
        (LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD attributes,
         HANDLE templ),
        (path, access, share, sa, disposition, attributes, templ))
MC_HOOK(read_file, BOOL, ReadFile, (HANDLE file, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped),
        (file, buffer, bytes, read, overlapped))
MC_HOOK(get_font_data, DWORD, GetFontData, (HDC dc, DWORD table, DWORD offset, PVOID buffer, DWORD bytes),
        (dc, table, offset, buffer, bytes))
MC_HOOK(reg_open_key, LSTATUS, RegOpenKeyExW, (HKEY key, LPCWSTR sub, DWORD options, REGSAM sam, PHKEY out),
        (key, sub, options, sam, out))
MC_HOOK(reg_query_value, LSTATUS, RegQueryValueExW,
        (HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD bytes),
        (key, name, reserved, type, data, bytes))
MC_HOOK(reg_get_value, LSTATUS, RegGetValueW,
        (HKEY key, LPCWSTR sub, LPCWSTR value, DWORD flags, LPDWORD type, PVOID data, LPDWORD bytes),
        (key, sub, value, flags, type, data, bytes))
MC_HOOK(bcrypt_open_algorithm, LONG, BCryptOpenAlgorithmProvider,
        (void** algorithm, LPCWSTR id, LPCWSTR implementation, ULONG flags), (algorithm, id, implementation, flags))
MC_HOOK(wait_single, DWORD, WaitForSingleObject, (HANDLE handle, DWORD timeout), (handle, timeout))
MC_HOOK(wait_single_ex, DWORD, WaitForSingleObjectEx, (HANDLE handle, DWORD timeout, BOOL alertable),
        (handle, timeout, alertable))
MC_HOOK(wait_multiple, DWORD, WaitForMultipleObjects, (DWORD count, const HANDLE* handles, BOOL all, DWORD timeout),
        (count, handles, all, timeout))
MC_HOOK(msg_wait, DWORD, MsgWaitForMultipleObjectsEx,
        (DWORD count, const HANDLE* handles, DWORD timeout, DWORD mask, DWORD flags),
        (count, handles, timeout, mask, flags))
#undef MC_HOOK

using LoadLibraryExWType = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
LoadLibraryExWType g_LoadLibraryExW = nullptr;
HMODULE WINAPI hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) noexcept {
    const u64 start = qpc();
    const HMODULE result = g_LoadLibraryExW(name, file, flags);
    record(Api::load_library_ex, start, _ReturnAddress(), name);
    return result;
}
using LoadLibraryWType = HMODULE(WINAPI*)(LPCWSTR);
LoadLibraryWType g_LoadLibraryW = nullptr;
HMODULE WINAPI hook_LoadLibraryW(LPCWSTR name) noexcept {
    const u64 start = qpc();
    const HMODULE result = g_LoadLibraryW(name);
    record(Api::load_library, start, _ReturnAddress(), name);
    return result;
}

using D3D11CreateDeviceType = decltype(&D3D11CreateDevice);
D3D11CreateDeviceType g_D3D11CreateDevice = nullptr;
HRESULT WINAPI hook_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
                                      const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk,
                                      ID3D11Device** device, D3D_FEATURE_LEVEL* level,
                                      ID3D11DeviceContext** context) noexcept {
    const u64 start = qpc();
    const HRESULT result =
        g_D3D11CreateDevice(adapter, type, software, flags, levels, level_count, sdk, device, level, context);
    record(Api::d3d11_create_device, start, _ReturnAddress());
    if (SUCCEEDED(result) && device && *device) {
        if (!g_first_device)
            g_first_device = *device;
        patch_device(*device);
    }
    return result;
}
using DCompositionCreateDeviceType = HRESULT(WINAPI*)(IDXGIDevice*, REFIID, void**);
DCompositionCreateDeviceType g_DCompositionCreateDevice = nullptr;
HRESULT WINAPI hook_DCompositionCreateDevice(IDXGIDevice* device, REFIID iid, void** out) noexcept {
    const u64 start = qpc();
    const HRESULT result = g_DCompositionCreateDevice(device, iid, out);
    record(Api::dcomp_create_device, start, _ReturnAddress());
    if (SUCCEEDED(result) && out && *out) {
        patch_slot(*out, 3, Vt::dcomp_commit, reinterpret_cast<void*>(&vt_stub<Vt::dcomp_commit>));
        patch_slot(*out, 6, Vt::dcomp_target_for_hwnd, reinterpret_cast<void*>(&vt_stub<Vt::dcomp_target_for_hwnd>));
        patch_slot(*out, 7, Vt::dcomp_visual, reinterpret_cast<void*>(&vt_stub<Vt::dcomp_visual>));
    }
    return result;
}

bool same_name(const char* a, const char* b) noexcept {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

void install_hooks() noexcept {
    auto* base = reinterpret_cast<u8*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return;
    struct Hook {
        const char* name;
        void* replacement;
        void** original;
    };
#define MC_ENTRY(function)                                                                                        \
    {#function, reinterpret_cast<void*>(&hook_##function), reinterpret_cast<void**>(&g_##function)}
    Hook hooks[] = {MC_ENTRY(D3D11CreateDevice),     MC_ENTRY(DCompositionCreateDevice),
                    MC_ENTRY(RegisterClassExW),      MC_ENTRY(CreateWindowExW),
                    MC_ENTRY(ShowWindow),            MC_ENTRY(SetWindowPos),
                    MC_ENTRY(LoadLibraryExW),        MC_ENTRY(LoadLibraryW),
                    MC_ENTRY(CoInitializeEx),        MC_ENTRY(OleInitialize),
                    MC_ENTRY(CoCreateInstance),      MC_ENTRY(CoRegisterClassObject),
                    MC_ENTRY(RoInitialize),          MC_ENTRY(RoGetActivationFactory),
                    MC_ENTRY(CreateThread),          MC_ENTRY(CreateFileW),
                    MC_ENTRY(ReadFile),              MC_ENTRY(GetFontData),
                    MC_ENTRY(RegOpenKeyExW),         MC_ENTRY(RegQueryValueExW),
                    MC_ENTRY(RegGetValueW),          MC_ENTRY(BCryptOpenAlgorithmProvider),
                    MC_ENTRY(WaitForSingleObject),   MC_ENTRY(WaitForSingleObjectEx),
                    MC_ENTRY(WaitForMultipleObjects), MC_ENTRY(MsgWaitForMultipleObjectsEx)};
#undef MC_ENTRY
    const auto patch = [&](const IMAGE_THUNK_DATA64* names, IMAGE_THUNK_DATA64* slots) noexcept {
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            for (Hook& hook : hooks) {
                if (!same_name(reinterpret_cast<const char*>(by_name->Name), hook.name))
                    continue;
                DWORD old = 0;
                if (!VirtualProtect(&slots->u1.Function, sizeof(u64), PAGE_READWRITE, &old))
                    continue;
                if (!*hook.original)
                    *hook.original = reinterpret_cast<void*>(slots->u1.Function);
                slots->u1.Function = reinterpret_cast<u64>(hook.replacement);
                VirtualProtect(&slots->u1.Function, sizeof(u64), old, &old);
            }
        }
    };
    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         import->Name; ++import)
        patch(reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                  base + (import->OriginalFirstThunk ? import->OriginalFirstThunk : import->FirstThunk)),
              reinterpret_cast<IMAGE_THUNK_DATA64*>(base + import->FirstThunk));
    // Delay-loaded imports: the slot still holds the image's resolver thunk.
    // Calling it as the "original" resolves the import (overwriting this hook
    // with the real address), so only the first call of each is timed, which
    // includes mapping the DLL when nothing loaded it before.
    const auto& delayed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed.VirtualAddress)
        for (auto* entry = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delayed.VirtualAddress);
             entry->DllNameRVA; ++entry)
            if (entry->Attributes.RvaBased)
                patch(reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + entry->ImportNameTableRVA),
                      reinterpret_cast<IMAGE_THUNK_DATA64*>(base + entry->ImportAddressTableRVA));
}

struct Line {
    char text[512];
    u32 used;
    void s(const char* value) noexcept {
        while (*value && used + 1 < sizeof(text))
            text[used++] = *value++;
    }
    void w(const wchar_t* value) noexcept {
        while (*value && used + 1 < sizeof(text)) {
            const wchar_t c = *value++;
            text[used++] = c > 32 && c < 127 ? static_cast<char>(c) : '_';
        }
    }
    void n(u64 value) noexcept {
        char digits[24];
        u32 count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count && used + 1 < sizeof(text))
            text[used++] = digits[--count];
    }
    void kv(const char* key, u64 value) noexcept {
        s(" ");
        s(key);
        s("=");
        n(value);
    }
    void hex(const char* key, u64 value) noexcept {
        s(" ");
        s(key);
        s("=0x");
        char digits[17];
        u32 count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value & 15];
            value >>= 4;
        } while (value);
        while (count && used + 1 < sizeof(text))
            text[used++] = digits[--count];
    }
};
char g_report[64 * 1024];
u32 g_report_used = 0;
void emit(Line& line) noexcept {
    line.s("\n");
    if (g_report_used + line.used <= sizeof(g_report)) {
        for (u32 i = 0; i < line.used; ++i)
            g_report[g_report_used + i] = line.text[i];
        g_report_used += line.used;
    }
    line.used = 0;
}

} // namespace

void startup_trace_initialize() noexcept {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    g_clock.qpc0 = qpc();
    g_clock.tsc0 = __rdtsc();
    g_clock.frequency = static_cast<u64>(frequency.QuadPart);
    const u64 create = (static_cast<u64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    const u64 current = (static_cast<u64>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    g_clock.create_offset_us = current > create ? (current - create) / 10 : 0;
    g_clock.main_thread = GetCurrentThreadId();
    g_clock.image_base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
    g_entry_cycles = thread_cycles();
    // Static imports are mapped by now.
    HMODULE modules[kEntryModuleCapacity]{};
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        const u32 count = minimum<u32>(static_cast<u32>(needed / sizeof(HMODULE)), kEntryModuleCapacity);
        for (u32 i = 0; i < count; ++i) {
            wchar_t name[64]{};
            const DWORD units = K32GetModuleBaseNameW(GetCurrentProcess(), modules[i], name, 63);
            MODULEINFO info{};
            K32GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info));
            copy_name(&g_entry_modules[g_entry_module_count].name, name, units);
            g_entry_modules[g_entry_module_count].kb = info.SizeOfImage / 1024;
            ++g_entry_module_count;
        }
    }
    const auto register_notification = reinterpret_cast<LdrRegister>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    void* cookie = nullptr;
    if (register_notification)
        register_notification(0, dll_notification, nullptr, &cookie);
    install_hooks();
    g_clock.ready = true;
    startup_mark("entry");
}

void startup_mark(const char* name) noexcept {
    if (!g_clock.ready || !window_open() || !on_main() || g_mark_count == kMarkCapacity)
        return;
    for (u32 i = 0; i < g_mark_count; ++i)
        if (g_marks[i].name == name || same_name(g_marks[i].name, name))
            return;
    g_marks[g_mark_count++] = {name, qpc(), thread_cycles()};
}

void startup_first_present() noexcept {
    if (!g_clock.ready || !window_open())
        return;
    g_present_at = qpc();
    g_present_cycles = thread_cycles();
    ULONG64 process = 0;
    QueryProcessCycleTime(GetCurrentProcess(), &process);
    g_present_process_cycles = process;
    startup_mark("first_present");
    PROCESS_MEMORY_COUNTERS_EX2 memory{};
    memory.cb = sizeof(memory);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                                sizeof(memory))) {
        g_first_frame_memory.private_bytes = memory.PrivateUsage;
        g_first_frame_memory.working_set = memory.WorkingSetSize;
        g_first_frame_memory.private_working_set = memory.PrivateWorkingSetSize;
        g_first_frame_memory.peak_working_set = memory.PeakWorkingSetSize;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        g_first_frame_memory.handles = handles;
    HMODULE modules[256];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        g_first_frame_memory.modules = static_cast<u32>(needed / sizeof(HMODULE));
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIAdapter3* adapter3 = nullptr;
    if (g_first_device &&
        SUCCEEDED(g_first_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
            g_first_frame_memory.gpu_local = local.CurrentUsage;
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal)))
            g_first_frame_memory.gpu_nonlocal = nonlocal.CurrentUsage;
    }
    if (adapter3)
        adapter3->Release();
    if (adapter)
        adapter->Release();
    if (dxgi)
        dxgi->Release();
    InterlockedExchange(&g_window_closed, 1);
}

void startup_trace_report() noexcept {
    if (!g_clock.ready)
        return;
    const u64 elapsed_us = span_us(qpc() - g_clock.qpc0);
    const double cycles_per_us =
        elapsed_us ? static_cast<double>(__rdtsc() - g_clock.tsc0) / static_cast<double>(elapsed_us) : 1.0;
    const auto cpu_us = [cycles_per_us](u64 cycles) noexcept {
        return static_cast<u64>(static_cast<double>(cycles) / cycles_per_us);
    };
    Line line{};
    line.s("st_entry");
    line.kv("at_us", g_clock.create_offset_us);
    line.kv("main_cpu_us", cpu_us(g_entry_cycles));
    line.kv("modules", g_entry_module_count);
    emit(line);
    line.s("st_present");
    line.kv("at_us", g_present_at ? at_us(g_present_at) : 0);
    line.kv("main_cpu_us", cpu_us(g_present_cycles));
    line.kv("process_cpu_us", cpu_us(g_present_process_cycles));
    line.kv("dll_unloads", static_cast<u64>(g_dll_unloads));
    emit(line);
    line.s("st_memory");
    line.kv("private_kb", g_first_frame_memory.private_bytes / 1024);
    line.kv("ws_kb", g_first_frame_memory.working_set / 1024);
    line.kv("pws_kb", g_first_frame_memory.private_working_set / 1024);
    line.kv("peak_ws_kb", g_first_frame_memory.peak_working_set / 1024);
    line.kv("gpu_local_kb", g_first_frame_memory.gpu_local / 1024);
    line.kv("gpu_nonlocal_kb", g_first_frame_memory.gpu_nonlocal / 1024);
    line.kv("handles", g_first_frame_memory.handles);
    line.kv("modules", g_first_frame_memory.modules);
    emit(line);
    for (u32 i = 0; i < g_mark_count; ++i) {
        line.s("st_mark name=");
        line.s(g_marks[i].name);
        line.kv("at_us", at_us(g_marks[i].at));
        line.kv("cpu_us", cpu_us(g_marks[i].cycles));
        emit(line);
    }
    for (u32 i = 0; i < g_entry_module_count; ++i) {
        line.s("st_module name=");
        line.w(g_entry_modules[i].name.text);
        line.kv("kb", g_entry_modules[i].kb);
        emit(line);
    }
    const u32 dlls = minimum<u32>(static_cast<u32>(g_dll_count), kDllCapacity);
    u32 later = 0;
    for (u32 i = 0; i < dlls; ++i) {
        const DllEvent& event = g_dlls[i];
        if (!event.window) {
            ++later;
            continue;
        }
        line.s("st_dll name=");
        line.w(event.name.text);
        line.kv("at_us", at_us(event.at));
        line.kv("kb", event.kb);
        line.kv("main", event.main ? 1 : 0);
        emit(line);
    }
    line.s("st_dll_later");
    line.kv("count", later);
    emit(line);
    for (u32 i = 0; i < static_cast<u32>(Api::count); ++i) {
        const ApiStat& stat = g_api[i];
        if (!stat.calls && !stat.later_calls)
            continue;
        line.s("st_api name=");
        line.s(kApiNames[i]);
        line.kv("calls", static_cast<u64>(stat.calls));
        line.kv("dur_us", span_us(static_cast<u64>(stat.ticks)));
        line.kv("main_calls", static_cast<u64>(stat.main_calls));
        line.kv("main_us", span_us(static_cast<u64>(stat.main_ticks)));
        line.kv("max_us", span_us(static_cast<u64>(stat.max_ticks)));
        line.kv("max_at_us", stat.max_at ? at_us(static_cast<u64>(stat.max_at)) : 0);
        line.kv("first_us", stat.first_at ? at_us(static_cast<u64>(stat.first_at)) : 0);
        line.kv("later", static_cast<u64>(stat.later_calls));
        emit(line);
    }
    for (u32 i = 0; i < static_cast<u32>(Vt::count); ++i) {
        const ApiStat& stat = g_vt[i];
        if (!stat.calls && !stat.later_calls)
            continue;
        line.s("st_d3d name=");
        line.s(kVtNames[i]);
        line.kv("calls", static_cast<u64>(stat.calls));
        line.kv("dur_us", span_us(static_cast<u64>(stat.ticks)));
        line.kv("main_calls", static_cast<u64>(stat.main_calls));
        line.kv("main_us", span_us(static_cast<u64>(stat.main_ticks)));
        line.kv("max_us", span_us(static_cast<u64>(stat.max_ticks)));
        line.kv("max_at_us", stat.max_at ? at_us(static_cast<u64>(stat.max_at)) : 0);
        line.kv("first_us", stat.first_at ? at_us(static_cast<u64>(stat.first_at)) : 0);
        line.kv("later", static_cast<u64>(stat.later_calls));
        emit(line);
    }
    const u32 presents = minimum<u32>(static_cast<u32>(g_present_count), kPresentCapacity);
    for (u32 i = 0; i < presents; ++i) {
        line.s("st_present_call");
        line.kv("index", i);
        line.kv("at_us", at_us(g_presents[i].at));
        line.kv("dur_us", span_us(g_presents[i].ticks));
        line.kv("main", g_presents[i].main ? 1 : 0);
        emit(line);
    }
    const u32 events = minimum<u32>(static_cast<u32>(g_event_count), kEventCapacity);
    for (u32 i = 0; i < events; ++i) {
        const TimedEvent& event = g_events[i];
        line.s("st_event api=");
        line.s(kApiNames[static_cast<u32>(event.api)]);
        line.kv("at_us", at_us(event.at));
        line.kv("dur_us", span_us(event.ticks));
        line.hex("rva", event.caller_rva);
        if (event.name.text[0]) {
            line.s(" module=");
            line.w(event.name.text);
        }
        emit(line);
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), g_report, g_report_used, &written, nullptr);
}

} // namespace maicord::perf
