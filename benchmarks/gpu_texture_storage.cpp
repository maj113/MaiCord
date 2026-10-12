#include "maicord/core/time.hpp"
#include "maicord/core/trace_record.hpp"
#include <d3d11_2.h>
#include <dxgi1_4.h>
#include <psapi.h>

namespace maicord {
namespace {
struct Probe {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Device2* device2 = nullptr;
    ID3D11DeviceContext2* context2 = nullptr;
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIAdapter3* adapter3 = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11Buffer* pool = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    ~Probe() noexcept {
        if (context) { context->ClearState(); context->Flush(); }
        release(end); release(begin); release(disjoint); release(uav); release(view); release(target);
        release(staging); release(texture); release(pool); release(adapter3); release(adapter); release(dxgi);
        release(context2); release(device2); release(context); release(device);
    }
    template<class T> static void release(T*& p) noexcept { if (p) { p->Release(); p = nullptr; } }
};
void emit(TraceRecord& line) noexcept {
    DWORD written; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line.text, line.used, &written, nullptr);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\r\n", 2, &written, nullptr);
}
u32 setting(const wchar_t* name, u32 fallback) noexcept {
    wchar_t value[16]; const DWORD count = GetEnvironmentVariableW(name, value, 16);
    if (!count || count >= 16) return fallback;
    u32 number = 0; for (u32 i = 0; i < count; ++i) {
        if (value[i] < L'0' || value[i] > L'9') return fallback;
        number = number * 10 + static_cast<u32>(value[i] - L'0');
    }
    return number;
}
void memory(Probe& p, const char* stage) noexcept {
    PROCESS_MEMORY_COUNTERS_EX info{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), sizeof(info));
    u64 wc = 0; usize address = 0;
    MEMORY_BASIC_INFORMATION region{};
    while (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) {
        if (region.State == MEM_COMMIT && region.Type == MEM_PRIVATE && (region.Protect & PAGE_WRITECOMBINE)) wc += region.RegionSize;
        const usize next = reinterpret_cast<usize>(region.BaseAddress) + region.RegionSize;
        if (next <= address) break;
        address = next;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    if (p.adapter3) {
        p.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        p.adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
    }
    TraceRecord line("texture_memory"); line.field("stage", stage);
    line.field("private", info.PrivateUsage); line.field("writecombine", wc);
    line.field("amd_encoder_loaded", GetModuleHandleW(L"amdenc64.dll") ? 1u : 0u);
    line.field("gpu_local", local.CurrentUsage); line.field("gpu_nonlocal", nonlocal.CurrentUsage); emit(line);
}
bool query(Probe& p, ID3D11Query* q, void* value, u32 bytes) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + 10000;
    HRESULT result;
    while ((result = p.context->GetData(q, value, bytes, 0)) == S_FALSE) {
        if (clock_monotonic_milliseconds() >= deadline) return false;
        SwitchToThread();
    }
    return SUCCEEDED(result);
}
bool run() noexcept {
    const u32 tiles = setting(L"MAICORD_TEXTURE_TILES", 0);
    const u32 video = setting(L"MAICORD_TEXTURE_VIDEO", 1);
    const u32 no_overwrite = setting(L"MAICORD_TEXTURE_NO_OVERWRITE", 1);
    if (tiles > 1024 || video > 1) return false;
    const Clock clock = clock_initialize(); Probe p;
    memory(p, "before_device");
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    const i64 start = clock_now();
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | (video ? D3D11_CREATE_DEVICE_VIDEO_SUPPORT : 0), levels, 2,
        D3D11_SDK_VERSION, &p.device, &level, &p.context))) return false;
    const u64 device_us = clock_microseconds(clock, start, clock_now());
    if (FAILED(p.device->QueryInterface(__uuidof(ID3D11Device2), reinterpret_cast<void**>(&p.device2))) ||
        FAILED(p.context->QueryInterface(__uuidof(ID3D11DeviceContext2), reinterpret_cast<void**>(&p.context2))) ||
        FAILED(p.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&p.dxgi))) ||
        FAILED(p.dxgi->GetAdapter(&p.adapter)) ||
        FAILED(p.adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&p.adapter3)))) return false;
    D3D11_FEATURE_DATA_D3D11_OPTIONS1 options{};
    p.device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS1, &options, sizeof(options));
    memory(p, "before_video_query");
    ID3D11VideoDevice* video_device = nullptr;
    const HRESULT video_result = p.device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&video_device));
    const u32 profiles = video_device ? video_device->GetVideoDecoderProfileCount() : 0;
    Probe::release(video_device);
    TraceRecord configuration("texture_configuration"); configuration.field("tiles", tiles);
    configuration.field("video_flag", video); configuration.field("device_us", device_us);
    configuration.field("no_overwrite", no_overwrite);
    configuration.field("tier", static_cast<u32>(options.TiledResourcesTier));
    configuration.field("video_hr", static_cast<u32>(video_result)); configuration.field("video_profiles", profiles);
    emit(configuration); memory(p, "device_ready");
    if (tiles && options.TiledResourcesTier < D3D11_TILED_RESOURCES_TIER_2) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 4096; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = tiles ? D3D11_RESOURCE_MISC_TILED : 0;
    const i64 create_start = clock_now();
    if (FAILED(p.device->CreateTexture2D(&desc, nullptr, &p.texture))) return false;
    const i64 texture_created = clock_now();
    i64 pool_created = texture_created, mapped_at = texture_created;
    if (tiles) {
        UINT total = 0, count = 1; D3D11_PACKED_MIP_DESC packed{};
        D3D11_TILE_SHAPE shape{}; D3D11_SUBRESOURCE_TILING tiling{};
        p.device2->GetResourceTiling(p.texture, &total, &packed, &shape, &count, 0, &tiling);
        if (total != 1024 || shape.WidthInTexels != 128 || shape.HeightInTexels != 128) return false;
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = tiles * 65536;
        buffer.Usage = D3D11_USAGE_DEFAULT; buffer.MiscFlags = D3D11_RESOURCE_MISC_TILE_POOL;
        if (FAILED(p.device->CreateBuffer(&buffer, nullptr, &p.pool))) return false;
        pool_created = clock_now();
        D3D11_TILED_RESOURCE_COORDINATE coordinate{};
        D3D11_TILE_REGION_SIZE region{tiles, FALSE, 0, 0, 0};
        const UINT flags = 0, offset = 0;
        if (FAILED(p.context2->UpdateTileMappings(p.texture, 1, &coordinate, &region, p.pool,
            1, &flags, &offset, &tiles, no_overwrite ? D3D11_TILE_MAPPING_NO_OVERWRITE : 0))) return false;
        mapped_at = clock_now();
    }
    if (FAILED(p.device->CreateShaderResourceView(p.texture, nullptr, &p.view)) ||
        FAILED(p.device->CreateRenderTargetView(p.texture, nullptr, &p.target)) ||
        FAILED(p.device->CreateUnorderedAccessView(p.texture, nullptr, &p.uav))) return false;
    TraceRecord creation("texture_create"); creation.field("us", clock_microseconds(clock, create_start, clock_now()));
    creation.field("texture_us", clock_microseconds(clock, create_start, texture_created));
    creation.field("pool_us", clock_microseconds(clock, texture_created, pool_created));
    creation.field("mapping_us", clock_microseconds(clock, pool_created, mapped_at)); emit(creation);
    memory(p, "texture_ready");
    desc.Width = desc.Height = 128; desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(p.device->CreateTexture2D(&desc, nullptr, &p.staging))) return false;
    D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (FAILED(p.device->CreateQuery(&q, &p.disjoint))) return false;
    q.Query = D3D11_QUERY_TIMESTAMP;
    if (FAILED(p.device->CreateQuery(&q, &p.begin)) || FAILED(p.device->CreateQuery(&q, &p.end))) return false;
    const D3D11_RECT rect{0, 0, 128, 128};
    const D3D11_BOX box{0, 0, 0, 128, 128, 1};
    for (u32 round = 0; round < 5; ++round) {
        p.context->Begin(p.disjoint); p.context->End(p.begin);
        const i64 issue = clock_now();
        constexpr u32 calls = 2048;
        for (u32 i = 0; i < calls; ++i) {
            const float color[]{static_cast<float>(i & 1), 1, 0, 1};
            p.context2->ClearView(p.target, color, &rect, 1);
        }
        p.context->End(p.end); p.context->End(p.disjoint); p.context->Flush();
        const u64 cpu_us = clock_microseconds(clock, issue, clock_now());
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{}; u64 begin = 0, end = 0;
        if (!query(p, p.disjoint, &timing, sizeof(timing)) || !query(p, p.begin, &begin, sizeof(begin)) ||
            !query(p, p.end, &end, sizeof(end)) || timing.Disjoint || !timing.Frequency) return false;
        p.context->CopySubresourceRegion(p.staging, 0, 0, 0, 0, p.texture, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(p.context->Map(p.staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
        bool valid = true;
        for (u32 y = 0; y < 128; ++y) for (u32 x = 0; x < 128; ++x) {
            const auto* pixel = static_cast<const u8*>(mapped.pData) + y * mapped.RowPitch + x * 4;
            valid = valid && pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255;
        }
        p.context->Unmap(p.staging, 0);
        if (!valid) return false;
        TraceRecord line("texture_clear"); line.field("round", round); line.field("calls", calls);
        line.field("cpu_us", cpu_us); line.field("gpu_ns", (end - begin) * 1000000000ull / timing.Frequency);
        emit(line);
    }
    memory(p, "after_work");
    return true;
}
} // namespace
} // namespace maicord
extern "C" void maicord_gpu_texture_storage_entry() noexcept { ExitProcess(maicord::run() ? 0 : 1); }
