#include "perf_probe_internal.hpp"
#include "perf_imports.hpp"
#include "../../src/render/d3d11/renderer_draw.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/process_info.hpp"
#include "maicord/core/win32_exception.hpp"

namespace maicord::perf {
namespace perf_probe_detail {

Out g_out;

// Originals of the hooked allocators, assigned in probe_initialize: this image
// has no dynamic initializers.
VirtualAllocFunction g_virtual_alloc = nullptr;
VirtualFreeFunction g_virtual_free = nullptr;
HeapAllocFunction g_heap_alloc = nullptr;
HeapReAllocFunction g_heap_realloc = nullptr;
HeapFreeFunction g_heap_free = nullptr;

void* probe_alloc(u64 bytes) noexcept {
    return g_virtual_alloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

AllocCounters g_alloc{};

} // namespace perf_probe_detail

namespace {

D3D11_RECT g_last_scissor{};

} // namespace

namespace perf_probe_detail {

GpuApiCounters g_gpu_api{};

} // namespace perf_probe_detail

namespace {

constexpr u32 kContextSlots = 133;
void* g_context_original[kContextSlots]{};

template <class F> F context_original(u32 slot) noexcept {
    return reinterpret_cast<F>(g_context_original[slot]);
}
#define MC_CTX_BIND(slot, counter, ...)                                                                            \
    void STDMETHODCALLTYPE hook_slot_##slot(ID3D11DeviceContext* c, UINT a, UINT b, __VA_ARGS__ p) noexcept {      \
        InterlockedIncrement64(&g_gpu_api.counter);                                                                \
        context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, __VA_ARGS__)>(slot)(c, a, b, p); \
    }

MC_CTX_BIND(7, constant_binds, ID3D11Buffer* const*)
MC_CTX_BIND(8, resource_binds, ID3D11ShaderResourceView* const*)
MC_CTX_BIND(10, sampler_binds, ID3D11SamplerState* const*)
MC_CTX_BIND(16, constant_binds, ID3D11Buffer* const*)
MC_CTX_BIND(67, compute_binds, ID3D11ShaderResourceView* const*)
MC_CTX_BIND(71, compute_binds, ID3D11Buffer* const*)
#undef MC_CTX_BIND
template <u32 Slot, class Shader>
void STDMETHODCALLTYPE hook_shader(ID3D11DeviceContext* c, Shader* s, ID3D11ClassInstance* const* i, UINT n) noexcept {
    InterlockedIncrement64(Slot == 69 ? &g_gpu_api.compute_binds : &g_gpu_api.shader_binds);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, Shader*, ID3D11ClassInstance* const*, UINT)>(
        Slot)(c, s, i, n);
}
HRESULT STDMETHODCALLTYPE hook_map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, D3D11_MAP type, UINT flags,
                                   D3D11_MAPPED_SUBRESOURCE* mapped) noexcept {
    InterlockedIncrement64(&g_gpu_api.maps);
    if (type == D3D11_MAP_WRITE_DISCARD)
        InterlockedIncrement64(&g_gpu_api.map_discards);
    else if (type == D3D11_MAP_WRITE_NO_OVERWRITE)
        InterlockedIncrement64(&g_gpu_api.map_no_overwrites);
    owners_set_tag(kStartupTagMap);
    const HRESULT result =
        context_original<HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT,
                                                     D3D11_MAPPED_SUBRESOURCE*)>(14)(c, r, s, type, flags, mapped);
    owners_set_tag(0);
    return result;
}
void STDMETHODCALLTYPE hook_update(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, const D3D11_BOX* box,
                                   const void* data, UINT row, UINT depth) noexcept {
    owners_set_tag(kStartupTagUpdate);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*,
                                              const void*, UINT, UINT)>(48)(c, r, s, box, data, row, depth);
    owners_set_tag(0);
}
void STDMETHODCALLTYPE hook_input_layout(ID3D11DeviceContext* c, ID3D11InputLayout* layout) noexcept {
    InterlockedIncrement64(&g_gpu_api.input_binds);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11InputLayout*)>(17)(c, layout);
}
void STDMETHODCALLTYPE hook_vertex_buffers(ID3D11DeviceContext* c, UINT a, UINT n, ID3D11Buffer* const* b,
                                           const UINT* strides, const UINT* offsets) noexcept {
    InterlockedIncrement64(&g_gpu_api.input_binds);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*, const UINT*,
                                              const UINT*)>(18)(c, a, n, b, strides, offsets);
}
void STDMETHODCALLTYPE hook_topology(ID3D11DeviceContext* c, D3D11_PRIMITIVE_TOPOLOGY topology) noexcept {
    InterlockedIncrement64(&g_gpu_api.input_binds);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, D3D11_PRIMITIVE_TOPOLOGY)>(24)(c, topology);
}
void STDMETHODCALLTYPE hook_targets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* t,
                                    ID3D11DepthStencilView* d) noexcept {
    InterlockedIncrement64(&g_gpu_api.output_binds);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                              ID3D11DepthStencilView*)>(33)(c, n, t, d);
}
void STDMETHODCALLTYPE hook_blend(ID3D11DeviceContext* c, ID3D11BlendState* s, const FLOAT f[4], UINT m) noexcept {
    InterlockedIncrement64(&g_gpu_api.output_binds);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT*, UINT)>(35)(
        c, s, f, m);
}
void STDMETHODCALLTYPE hook_dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) noexcept {
    InterlockedIncrement64(&g_gpu_api.dispatches);
    InterlockedAdd64(&g_gpu_api.dispatch_groups, static_cast<LONG64>(x) * y * z);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT)>(41)(c, x, y, z);
}
void STDMETHODCALLTYPE hook_raster_state(ID3D11DeviceContext* c, ID3D11RasterizerState* s) noexcept {
    InterlockedIncrement64(&g_gpu_api.raster_binds);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RasterizerState*)>(43)(c, s);
}
void STDMETHODCALLTYPE hook_viewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) noexcept {
    InterlockedIncrement64(&g_gpu_api.raster_binds);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*)>(44)(c, n, v);
}
void STDMETHODCALLTYPE hook_scissors(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r) noexcept {
    InterlockedIncrement64(&g_gpu_api.scissors);
    if (n == 1 && r) {
        if (r->left == g_last_scissor.left && r->top == g_last_scissor.top && r->right == g_last_scissor.right &&
            r->bottom == g_last_scissor.bottom)
            InterlockedIncrement64(&g_gpu_api.repeated_scissors);
        g_last_scissor = *r;
    }
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*)>(45)(c, n, r);
}
void STDMETHODCALLTYPE hook_copy_region(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z,
                                        ID3D11Resource* s, UINT ss, const D3D11_BOX* box) noexcept {
    InterlockedIncrement64(&g_gpu_api.copies);
    if (box && box->right > box->left && box->bottom > box->top)
        InterlockedAdd64(&g_gpu_api.copy_pixels, static_cast<LONG64>(box->right - box->left) * (box->bottom - box->top));
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
                                              ID3D11Resource*, UINT, const D3D11_BOX*)>(46)(c, d, ds, x, y, z, s, ss,
                                                                                            box);
}
void STDMETHODCALLTYPE hook_copy_resource(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s) noexcept {
    InterlockedIncrement64(&g_gpu_api.copies);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*)>(47)(c, d, s);
}
void STDMETHODCALLTYPE hook_clear_target(ID3D11DeviceContext* c, ID3D11RenderTargetView* t, const FLOAT f[4]) noexcept {
    InterlockedIncrement64(&g_gpu_api.clears);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*)>(50)(c, t,
                                                                                                               f);
}
void STDMETHODCALLTYPE hook_uavs(ID3D11DeviceContext* c, UINT a, UINT n, ID3D11UnorderedAccessView* const* u,
                                 const UINT* counts) noexcept {
    InterlockedIncrement64(&g_gpu_api.compute_binds);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*,
                                              const UINT*)>(68)(c, a, n, u, counts);
}
void STDMETHODCALLTYPE hook_flush(ID3D11DeviceContext* c) noexcept {
    InterlockedIncrement64(&g_gpu_api.flushes);
    owners_set_tag(kStartupTagFlush);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*)>(111)(c);
    owners_set_tag(0);
}
void STDMETHODCALLTYPE hook_clear_view(ID3D11DeviceContext* c, ID3D11View* v, const FLOAT f[4], const D3D11_RECT* r,
                                       UINT n) noexcept {
    InterlockedIncrement64(&g_gpu_api.clears);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11View*, const FLOAT*, const D3D11_RECT*,
                                              UINT)>(132)(c, v, f, r, n);
}

} // namespace

namespace perf_probe_detail {

// Patches one context vtable; a slot already holding a hook stays as is.
u32 install_context_hooks(void* context, bool context1) noexcept {
    if (!context)
        return 0;
    struct Slot {
        u32 index;
        void* replacement;
    };
    const Slot slots[] = {
        {7, reinterpret_cast<void*>(hook_slot_7)},
        {8, reinterpret_cast<void*>(hook_slot_8)},
        {9, reinterpret_cast<void*>(hook_shader<9, ID3D11PixelShader>)},
        {10, reinterpret_cast<void*>(hook_slot_10)},
        {11, reinterpret_cast<void*>(hook_shader<11, ID3D11VertexShader>)},
        {14, reinterpret_cast<void*>(hook_map)},
        {16, reinterpret_cast<void*>(hook_slot_16)},
        {17, reinterpret_cast<void*>(hook_input_layout)},
        {18, reinterpret_cast<void*>(hook_vertex_buffers)},
        {24, reinterpret_cast<void*>(hook_topology)},
        {33, reinterpret_cast<void*>(hook_targets)},
        {35, reinterpret_cast<void*>(hook_blend)},
        {41, reinterpret_cast<void*>(hook_dispatch)},
        {43, reinterpret_cast<void*>(hook_raster_state)},
        {44, reinterpret_cast<void*>(hook_viewports)},
        {45, reinterpret_cast<void*>(hook_scissors)},
        {46, reinterpret_cast<void*>(hook_copy_region)},
        {47, reinterpret_cast<void*>(hook_copy_resource)},
        {48, reinterpret_cast<void*>(hook_update)},
        {50, reinterpret_cast<void*>(hook_clear_target)},
        {67, reinterpret_cast<void*>(hook_slot_67)},
        {68, reinterpret_cast<void*>(hook_uavs)},
        {69, reinterpret_cast<void*>(hook_shader<69, ID3D11ComputeShader>)},
        {71, reinterpret_cast<void*>(hook_slot_71)},
        {111, reinterpret_cast<void*>(hook_flush)},
        {132, reinterpret_cast<void*>(hook_clear_view)},
    };
    void** table = *static_cast<void***>(context);
    u32 installed = 0;
    for (const Slot& slot : slots) {
        if ((slot.index >= 112 && !context1) || table[slot.index] == slot.replacement)
            continue;
        DWORD old = 0;
        if (!VirtualProtect(&table[slot.index], sizeof(void*), PAGE_READWRITE, &old))
            continue;
        g_context_original[slot.index] = table[slot.index];
        table[slot.index] = slot.replacement;
        VirtualProtect(&table[slot.index], sizeof(void*), old, &old);
        ++installed;
    }
    return installed;
}

GpuReplay g_replay{};

void gpu_replay_collect(ID3D11DeviceContext* context) noexcept {
    if (!context)
        return;
    for (GpuReplayFrame& frame : g_replay.frames) {
        if (!frame.pending)
            continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        u64 begin = 0, end = 0;
        if (context->GetData(frame.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(frame.begin, &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(frame.end, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        frame.pending = false;
        if (disjoint.Disjoint || !disjoint.Frequency || end < begin) {
            ++g_replay.failures;
            continue;
        }
        const u64 us = (end - begin) * 1'000'000ull / disjoint.Frequency;
        if (frame.full) {
            g_replay.full_us += us;
            ++g_replay.full_samples;
        } else {
            g_replay.partial_us += us;
            ++g_replay.partial_samples;
        }
    }
}

} // namespace perf_probe_detail

namespace {

bool gpu_replay_target(D3D11Renderer& renderer) noexcept {
    if (g_replay.target && g_replay.width == renderer.width && g_replay.height == renderer.height)
        return true;
    if (g_replay.target)
        g_replay.target->Release();
    if (g_replay.texture)
        g_replay.texture->Release();
    g_replay.target = nullptr;
    g_replay.texture = nullptr;
    ID3D11Resource* source = nullptr;
    renderer.graphics.target->GetResource(&source);
    ID3D11Texture2D* back_buffer = nullptr;
    D3D11_TEXTURE2D_DESC description{};
    if (source && SUCCEEDED(source->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back_buffer)))) {
        back_buffer->GetDesc(&description);
        back_buffer->Release();
    }
    if (source)
        source->Release();
    if (!description.Width)
        return false;
    description.Width = renderer.width;
    description.Height = renderer.height;
    description.MipLevels = description.ArraySize = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET;
    description.CPUAccessFlags = description.MiscFlags = 0;
    startup_gpu_ignore(true);
    const HRESULT created = renderer.graphics.device->CreateTexture2D(&description, nullptr, &g_replay.texture);
    startup_gpu_ignore(false);
    if (FAILED(created) ||
        FAILED(renderer.graphics.device->CreateRenderTargetView(g_replay.texture, nullptr, &g_replay.target)))
        return false;
    g_replay.width = renderer.width;
    g_replay.height = renderer.height;
    return true;
}

} // namespace

namespace perf_probe_detail {

void gpu_replay(bool full) noexcept {
    ULONG64 start = 0;
    QueryThreadCycleTime(GetCurrentThread(), &start);
    D3D11Renderer& renderer = g_app.renderer;
    ID3D11DeviceContext* context = renderer.graphics.context;
    gpu_replay_collect(context);
    GpuReplayFrame& frame = g_replay.frames[g_replay.cursor];
    if (frame.pending || !renderer.graphics.target || !gpu_replay_target(renderer)) {
        ++g_replay.skipped;
    } else {
        const D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
        if (!frame.disjoint && (FAILED(renderer.graphics.device->CreateQuery(&disjoint, &frame.disjoint)) ||
                                FAILED(renderer.graphics.device->CreateQuery(&stamp, &frame.begin)) ||
                                FAILED(renderer.graphics.device->CreateQuery(&stamp, &frame.end)))) {
            ++g_replay.failures;
        } else {
            const RECT damage = *renderer.present_dirty_rect.native();
            ID3D11RenderTargetView* presented = renderer.graphics.target;
            const u32 draws = renderer.draw_calls_last_frame, media = renderer.media_draws_last_frame;
            const u32 media_total = renderer.media_draws_total;
            const u64 effect_next = renderer.text_effect_next_ms;
            // The replay's own context calls stay out of the per-frame counts.
            const GpuApiCounters api = g_gpu_api;
            const bool timer = renderer.gpu_timer.enabled;
            renderer.gpu_timer.enabled = false;
            renderer.graphics.target = g_replay.target;
            const u32 origin = renderer.target_origin_y;
            renderer_bind_target_origin(&renderer, 0);
            context->Begin(frame.disjoint);
            context->End(frame.begin);
            const bool drawn =
                d3d11_renderer_draw_stream(&renderer, app_detail::application_theme(), false, false,
                                           full ? nullptr : &damage);
            context->End(frame.end);
            context->End(frame.disjoint);
            renderer.graphics.target = presented;
            renderer_bind_target_origin(&renderer, origin);
            context->OMSetRenderTargets(1, &renderer.graphics.target, nullptr);
            renderer.gpu_timer.enabled = timer;
            renderer.text_effect_next_ms = effect_next;
            g_gpu_api = api;
            ++g_replay.replays;
            renderer.draw_calls_last_frame = draws;
            renderer.media_draws_last_frame = media;
            renderer.media_draws_total = media_total;
            if (drawn) {
                frame.pending = true;
                frame.full = full;
                if (!full)
                    g_replay.partial_pixels += static_cast<u64>(damage.right - damage.left) * (damage.bottom - damage.top);
                g_replay.cursor = (g_replay.cursor + 1) % kGpuReplayFrames;
            } else {
                ++g_replay.failures;
            }
        }
    }
    ULONG64 stop = 0;
    QueryThreadCycleTime(GetCurrentThread(), &stop);
    InterlockedAdd64(&g_replay.cycles, static_cast<LONG64>(stop - start));
}

TrackedRegion* g_tracked = nullptr; // sorted by base
u32 g_tracked_count = 0;
u32 g_tracked_overflow = 0;
SRWLOCK g_tracked_lock = SRWLOCK_INIT;
u64 g_image_base = 0;

u32 tracked_lower_bound(u64 base) noexcept {
    u32 low = 0, high = g_tracked_count;
    while (low < high) {
        const u32 mid = (low + high) / 2;
        if (g_tracked[mid].base < base)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}

} // namespace perf_probe_detail

namespace {

void track_reserve(void* address, u64 bytes, void* const* frames) noexcept {
    if (!g_tracked || !address)
        return;
    AcquireSRWLockExclusive(&g_tracked_lock);
    if (g_tracked_count == kTrackedCapacity) {
        ++g_tracked_overflow;
    } else {
        const u64 base = reinterpret_cast<u64>(address);
        const u32 at = tracked_lower_bound(base);
        if (at < g_tracked_count && g_tracked[at].base == base) {
            g_tracked[at].bytes = bytes;
        } else {
            memmove(g_tracked + at + 1, g_tracked + at, sizeof(TrackedRegion) * (g_tracked_count - at));
            TrackedRegion& region = g_tracked[at];
            region.base = base;
            region.bytes = bytes;
            for (u32 i = 0; i < kSiteFrames; ++i)
                region.frames[i] = reinterpret_cast<u64>(frames[i]);
            ++g_tracked_count;
        }
    }
    ReleaseSRWLockExclusive(&g_tracked_lock);
}

void track_release(void* address) noexcept {
    if (!g_tracked || !address)
        return;
    AcquireSRWLockExclusive(&g_tracked_lock);
    const u64 base = reinterpret_cast<u64>(address);
    const u32 at = tracked_lower_bound(base);
    if (at < g_tracked_count && g_tracked[at].base == base) {
        memmove(g_tracked + at, g_tracked + at + 1, sizeof(TrackedRegion) * (g_tracked_count - at - 1));
        --g_tracked_count;
    }
    ReleaseSRWLockExclusive(&g_tracked_lock);
}

LPVOID WINAPI hook_virtual_alloc(LPVOID address, SIZE_T bytes, DWORD type, DWORD protect) noexcept {
    LPVOID result = g_virtual_alloc(address, bytes, type, protect);
    InterlockedIncrement64(&g_alloc.virtual_alloc_calls);
    if (result) {
        if (type & MEM_COMMIT) {
            const u64 rounded = (bytes + 4095) & ~4095ull;
            InterlockedAdd64(&g_alloc.virtual_commit_bytes, static_cast<LONG64>(rounded));
            InterlockedAdd64(&g_alloc.virtual_commit_waste, static_cast<LONG64>(rounded - bytes));
        }
        if ((type & MEM_RESERVE) && !address) {
            InterlockedAdd64(&g_alloc.virtual_reserve_bytes, static_cast<LONG64>((bytes + 65535) & ~65535ull));
            void* frames[kSiteFrames + 1]{};
            RtlCaptureStackBackTrace(1, kSiteFrames, frames, nullptr);
            track_reserve(result, bytes, frames);
        }
    }
    return result;
}
BOOL WINAPI hook_virtual_free(LPVOID address, SIZE_T bytes, DWORD type) noexcept {
    InterlockedIncrement64(&g_alloc.virtual_free_calls);
    if (type & MEM_RELEASE) {
        InterlockedIncrement64(&g_alloc.virtual_release_calls);
        track_release(address);
    }
    return g_virtual_free(address, bytes, type);
}
LPVOID WINAPI hook_heap_alloc(HANDLE heap, DWORD flags, SIZE_T bytes) noexcept {
    InterlockedIncrement64(&g_alloc.heap_alloc_calls);
    InterlockedAdd64(&g_alloc.heap_alloc_bytes, static_cast<LONG64>(bytes));
    return g_heap_alloc(heap, flags, bytes);
}
LPVOID WINAPI hook_heap_realloc(HANDLE heap, DWORD flags, LPVOID memory, SIZE_T bytes) noexcept {
    InterlockedIncrement64(&g_alloc.heap_realloc_calls);
    InterlockedAdd64(&g_alloc.heap_alloc_bytes, static_cast<LONG64>(bytes));
    return g_heap_realloc(heap, flags, memory, bytes);
}
BOOL WINAPI hook_heap_free(HANDLE heap, DWORD flags, LPVOID memory) noexcept {
    InterlockedIncrement64(&g_alloc.heap_free_calls);
    return g_heap_free(heap, flags, memory);
}

} // namespace

namespace perf_probe_detail {

u32 install_import_hooks() noexcept {
    auto* base = reinterpret_cast<u8*>(GetModuleHandleW(nullptr));
    g_image_base = reinterpret_cast<u64>(base);
    struct Hook {
        const char* name;
        void* replacement;
        void** original;
    };
    Hook hooks[] = {
        {"VirtualAlloc", reinterpret_cast<void*>(hook_virtual_alloc), reinterpret_cast<void**>(&g_virtual_alloc)},
        {"VirtualFree", reinterpret_cast<void*>(hook_virtual_free), reinterpret_cast<void**>(&g_virtual_free)},
        {"HeapAlloc", reinterpret_cast<void*>(hook_heap_alloc), reinterpret_cast<void**>(&g_heap_alloc)},
        {"HeapReAlloc", reinterpret_cast<void*>(hook_heap_realloc), reinterpret_cast<void**>(&g_heap_realloc)},
        {"HeapFree", reinterpret_cast<void*>(hook_heap_free), reinterpret_cast<void**>(&g_heap_free)},
    };
    u32 installed = 0;
    for_each_import_slot(
        base, static_cast<u32>(sizeof(hooks) / sizeof(hooks[0])), [&](u32 i) noexcept { return hooks[i].name; },
        false, [&](u32 i, IMAGE_THUNK_DATA64* slot) noexcept {
            Hook& hook = hooks[i];
            DWORD old = 0;
            if (!VirtualProtect(&slot->u1.Function, sizeof(u64), PAGE_READWRITE, &old))
                return false;
            *hook.original = reinterpret_cast<void*>(slot->u1.Function);
            slot->u1.Function = reinterpret_cast<u64>(hook.replacement);
            VirtualProtect(&slot->u1.Function, sizeof(u64), old, &old);
            ++installed;
            return true;
        });
    return installed;
}

} // namespace perf_probe_detail

namespace {

static_assert(sizeof(SpiProcess) == 256);

static_assert(sizeof(SpiThread) == 80);

} // namespace

namespace perf_probe_detail {

NtQuerySystemInformationFunction g_query_system = nullptr;
NtQueryInformationThreadFunction g_query_thread = nullptr;
u8* g_spi_buffer = nullptr;

DWORD g_main_thread_id = 0;

bool sample_process(ProcessSample* out) noexcept {
    *out = {};
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    out->qpc = static_cast<u64>(qpc.QuadPart);
    ULONG64 cycles = 0;
    const u64 replay = static_cast<u64>(g_replay.cycles);
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    out->main_cycles = cycles - replay;
    QueryProcessCycleTime(GetCurrentProcess(), &cycles);
    out->process_cycles = cycles - replay;
    if (g_query_system && g_spi_buffer) {
        ULONG needed = 0;
        if (g_query_system(5, g_spi_buffer, kSpiBytes, &needed) >= 0) {
            const u64 self = GetCurrentProcessId();
            for (u8* at = g_spi_buffer;;) {
                const auto* process = reinterpret_cast<const SpiProcess*>(at);
                if (reinterpret_cast<u64>(process->process_id) == self) {
                    out->private_working_set = static_cast<u64>(process->private_working_set.QuadPart);
                    out->private_bytes = process->private_pages;
                    out->working_set = process->working_set;
                    out->page_faults = process->page_faults;
                    out->threads = process->threads;
                    out->handles = process->handle_count;
                    out->user_100ns = static_cast<u64>(process->user_time.QuadPart);
                    out->kernel_100ns = static_cast<u64>(process->kernel_time.QuadPart);
                    const auto* threads = reinterpret_cast<const SpiThread*>(process + 1);
                    for (u32 t = 0; t < process->threads; ++t) {
                        out->context_switches += threads[t].context_switches;
                        if (reinterpret_cast<u64>(threads[t].thread_id) == g_main_thread_id)
                            out->main_context_switches = threads[t].context_switches;
                    }
                    break;
                }
                if (!process->next)
                    break;
                at += process->next;
            }
        }
    }
    out->gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    out->user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    return true;
}

} // namespace perf_probe_detail

namespace {

void sample_thread_stack(HANDLE thread, ThreadRow& row) noexcept {
    struct {
        LONG exit_status;
        PVOID teb;
        void* client[2];
        ULONG_PTR affinity;
        LONG priority, base_priority;
    } basic{};
    if (g_query_thread(thread, 0, &basic, sizeof(basic), nullptr) < 0 || !basic.teb)
        return;
    const u64 base = reinterpret_cast<u64>(static_cast<const NT_TIB*>(basic.teb)->StackBase);
    const u64 deallocation = *reinterpret_cast<const u64*>(static_cast<const u8*>(basic.teb) + 0x1478);
    if (base <= deallocation)
        return;
    u64 committed = 0;
    for (u64 at = deallocation; at < base;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) || !info.RegionSize)
            break;
        const u64 end = minimum<u64>(base, reinterpret_cast<u64>(info.BaseAddress) + info.RegionSize);
        if (info.State == MEM_COMMIT)
            committed += end - at;
        at = end;
    }
    row.stack_reserved = base - deallocation;
    row.stack_committed = committed;
}

} // namespace

namespace perf_probe_detail {

ThreadRow g_threads[kThreadCapacity];
u32 g_thread_count = 0;

void collect_threads(bool idle_baseline) noexcept {
    if (!g_query_system || !g_query_thread)
        return;
    ULONG needed = 0;
    if (g_query_system(5, g_spi_buffer, kSpiBytes, &needed) < 0)
        return;
    const u64 self = GetCurrentProcessId();
    for (u8* at = g_spi_buffer;;) {
        const auto* process = reinterpret_cast<const SpiProcess*>(at);
        if (reinterpret_cast<u64>(process->process_id) == self) {
            const auto* threads = reinterpret_cast<const SpiThread*>(process + 1);
            for (u32 t = 0; t < process->threads; ++t) {
                const u64 id = reinterpret_cast<u64>(threads[t].thread_id);
                u32 slot = 0;
                while (slot < g_thread_count && g_threads[slot].id != id)
                    ++slot;
                if (slot == g_thread_count) {
                    if (g_thread_count == kThreadCapacity)
                        continue;
                    g_threads[g_thread_count++] = {id};
                }
                ThreadRow& row = g_threads[slot];
                row.context_switches = threads[t].context_switches;
                if (idle_baseline) {
                    row.idle_baseline = row.context_switches;
                    row.seen_at_idle = true;
                } else if (row.seen_at_idle) {
                    row.idle_switches = row.context_switches - row.idle_baseline;
                }
                HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, static_cast<DWORD>(id));
                if (thread) {
                    if (!row.start) {
                        g_query_thread(thread, 9, &row.start, sizeof(row.start), nullptr);
                        // Resolve now: driver DLLs unload before the report prints.
                        HMODULE module = nullptr;
                        wchar_t path[MAX_PATH]{};
                        if (row.start &&
                            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               reinterpret_cast<LPCWSTR>(row.start), &module) &&
                            K32GetModuleBaseNameW(GetCurrentProcess(), module, path, MAX_PATH)) {
                            row.module_base = reinterpret_cast<u64>(module);
                            for (u32 c = 0; c < 39 && path[c]; ++c)
                                row.module[c] = path[c];
                        }
                    }
                    ULONG64 cycles = 0;
                    if (QueryThreadCycleTime(thread, &cycles))
                        row.cycles = cycles;
                    sample_thread_stack(thread, row);
                    CloseHandle(thread);
                }
            }
            break;
        }
        if (!process->next)
            break;
        at += process->next;
    }
}

} // namespace perf_probe_detail
} // namespace maicord::perf
