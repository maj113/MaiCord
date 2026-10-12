// MaiCordPerfProbe: the shipping client sources built with MAICORD_PERF_PROBE. The
// client loads MaiCordPerfAccount.dll (synthetic account, no network); this file
// drives a deterministic scenario through the real main loop and reports startup to
// first present, main-thread CPU per frame, wakes, context switches, client
// allocation calls, private working set and committed memory by owner on standard
// output. tools/perf/run_perf.ps1 aggregates several runs.
#include "perf_probe.hpp"
#include "../../src/app/application_state.hpp"
#include "../../src/app/application_account_presentation.hpp"
#include "../../src/app/application_channel_session.hpp"
#include "../../src/app/application_content_input.hpp"
#include "../../src/app/application_friends.hpp"
#include "../../src/app/application_message_history.hpp"
#include "../../src/app/application_settings.hpp"
#include "../../src/app/application_account_pump.hpp"
#include "../../src/app/application_background.hpp"
#include "../../src/app/application_frame_presentation.hpp"
#include "../../src/app/application_surface_motion.hpp"
#include "../../src/app/application_tooltip.hpp"
#include "../../src/app/application_video.hpp"
#include "../../src/app/application_scheduler.hpp"
#include "../../src/app/application_activity.hpp"
#include "../../src/app/application_invite_cards.hpp"
#include "../../src/app/application_message_components.hpp"
#include "../../src/app/application_action_captcha.hpp"
#include "../../src/app/application_emoji_preferences.hpp"
#include "../../src/app/application_downloads.hpp"
#include "../../src/app/application_message_feedback.hpp"
#include "../../src/app/application_commands.hpp"
#include "../../src/app/application_stream.hpp"
#include "../../src/app/application_stage.hpp"
#include "../../src/app/application_soundboard.hpp"
#include "../../src/app/application_account_text_retry.hpp"
#include "../../src/app/application_profile.hpp"
#include "../../src/app/application_read_state.hpp"
#include "../../src/app/application_scoped_read.hpp"
#include "../../src/app/application_notifications.hpp"
#include "../../src/app/application_search.hpp"
#include "../../src/app/application_pinned_messages.hpp"
#include "../../src/app/application_media_picker.hpp"
#include "../../src/app/application_quick_switcher.hpp"
#include "../../src/app/application_message_forward.hpp"
#include "../../src/app/application_message_reaction.hpp"
#include "../../src/app/application_self_status.hpp"
#include "../../src/app/application_channel_overview.hpp"
#include "../../src/app/application_channel_snapshots.hpp"
#include "../../src/app/application_thread_actions.hpp"
#include "../../src/app/application_thread_browser.hpp"
#include "../../src/app/application_thread_creation.hpp"
#include "../../src/app/application_forum_post.hpp"
#include "../../src/app/application_forum_form.hpp"
#include "../../src/app/application_forum_preview.hpp"
#include "../../src/app/application_composer_preview.hpp"
#include "../../src/app/application_poll.hpp"
#include "../../src/app/application_member_moderation.hpp"
#include "../../src/app/application_user_settings.hpp"
#include "../../src/app/application_guild_folders.hpp"
#include "../../src/app/application_account_settings.hpp"
#include "../../src/app/application_notification_controls.hpp"
#include "../../src/app/application_custom_status.hpp"
#include "../../src/app/application_inbox.hpp"
#include "../../src/app/application_account_module.hpp"
#include "../../src/app/application_account_logout.hpp"
#include "../../src/app/application_login_auth.hpp"
#include "../../src/app/application_captcha_auth.hpp"
#include "../../src/app/application_text_form.hpp"
#include "../../src/app/application_voice.hpp"
#include "../../src/app/account_ui_runtime.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "maicord/app/account_paths.hpp"
#include "maicord/core/memory.hpp"
#include <intrin.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_4.h>

namespace maicord::perf {
namespace {
using app_detail::g_app;

struct Out {
    char text[256 * 1024];
    u32 used;
    void s(const char* value) noexcept {
        while (*value && used + 1 < sizeof(text))
            text[used++] = *value++;
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
    // Fixed point with one decimal.
    void f1(double value) noexcept {
        if (value < 0)
            value = 0;
        const u64 scaled = static_cast<u64>(value * 10.0 + 0.5);
        n(scaled / 10);
        s(".");
        n(scaled % 10);
    }
    void kv(const char* key, u64 value) noexcept {
        s(" ");
        s(key);
        s("=");
        n(value);
    }
    void kvf(const char* key, double value) noexcept {
        s(" ");
        s(key);
        s("=");
        f1(value);
    }
    void hex(u64 value) noexcept {
        char digits[17];
        u32 count = 0;
        do {
            digits[count++] = "0123456789abcdef"[value & 15];
            value >>= 4;
        } while (value);
        s("0x");
        while (count && used + 1 < sizeof(text))
            text[used++] = digits[--count];
    }
    void wide(const wchar_t* value) noexcept {
        while (*value && used + 1 < sizeof(text)) {
            const wchar_t c = *value++;
            text[used++] = c < 128 && c > 32 ? static_cast<char>(c) : '_';
        }
    }
};
Out g_out;

using VirtualAllocFunction = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFreeFunction = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
using HeapAllocFunction = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFunction = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using HeapFreeFunction = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);
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

struct AllocCounters {
    volatile LONG64 virtual_alloc_calls;
    volatile LONG64 virtual_commit_bytes;
    volatile LONG64 virtual_reserve_bytes;
    volatile LONG64 virtual_commit_waste;
    volatile LONG64 virtual_free_calls;
    volatile LONG64 virtual_release_calls;
    volatile LONG64 heap_alloc_calls;
    volatile LONG64 heap_alloc_bytes;
    volatile LONG64 heap_free_calls;
    volatile LONG64 heap_realloc_calls;
};
AllocCounters g_alloc{};

// Immediate-context calls counted by patching the context's vtable (probe
// image only; slot indices follow the d3d11.h/d3d11_1.h vtable order). Media
// decoder threads that share the device context are counted too. Draw and
// UpdateSubresource are not hooked: the runtime rewrites those slots itself.
// Scissors that repeat the previous rectangle are counted separately.
struct GpuApiCounters {
    volatile LONG64 shader_binds, resource_binds, sampler_binds, constant_binds, input_binds, output_binds,
        raster_binds, scissors, repeated_scissors, maps, map_discards, map_no_overwrites, copies, copy_pixels,
        dispatches, dispatch_groups, compute_binds, clears, flushes;
};
D3D11_RECT g_last_scissor{};
GpuApiCounters g_gpu_api{};
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
    return context_original<HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP,
                                                        UINT, D3D11_MAPPED_SUBRESOURCE*)>(14)(c, r, s, type, flags,
                                                                                              mapped);
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
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*)>(111)(c);
}
void STDMETHODCALLTYPE hook_clear_view(ID3D11DeviceContext* c, ID3D11View* v, const FLOAT f[4], const D3D11_RECT* r,
                                       UINT n) noexcept {
    InterlockedIncrement64(&g_gpu_api.clears);
    context_original<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11View*, const FLOAT*, const D3D11_RECT*,
                                              UINT)>(132)(c, v, f, r, n);
}

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

// GPU frame replay. Timestamps around a presented frame include the wait for its
// back buffer, which DWM releases only periodically while the display sleeps. So
// after each present the probe draws the same retained stream again, with the
// presented damage, into an offscreen target of the back buffer's size and format
// and times only that. Nothing is uploaded; the replay's main-thread cycles are
// excluded from the CPU columns.
struct GpuReplayFrame {
    ID3D11Query* disjoint;
    ID3D11Query* begin;
    ID3D11Query* end;
    bool pending, full;
};
constexpr u32 kGpuReplayFrames = 8;
struct GpuReplay {
    ID3D11Texture2D* texture;
    ID3D11RenderTargetView* target;
    u32 width, height;
    GpuReplayFrame frames[kGpuReplayFrames];
    u32 cursor;
    u64 full_presents;
    volatile LONG64 cycles; // main-thread cycles spent replaying
    u64 full_us, full_samples, partial_us, partial_samples, partial_pixels, skipped, failures, replays;
};
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
    if (FAILED(renderer.graphics.device->CreateTexture2D(&description, nullptr, &g_replay.texture)) ||
        FAILED(renderer.graphics.device->CreateRenderTargetView(g_replay.texture, nullptr, &g_replay.target)))
        return false;
    g_replay.width = renderer.width;
    g_replay.height = renderer.height;
    return true;
}

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
            const u32 draws_max = renderer.draw_calls_max, media_total = renderer.media_draws_total;
            const u64 effect_next = renderer.text_effect_next_ms;
            // The replay's own context calls stay out of the per-frame counts.
            const GpuApiCounters api = g_gpu_api;
            const bool timer = renderer.gpu_timer.enabled;
            renderer.gpu_timer.enabled = false;
            renderer.graphics.target = g_replay.target;
            context->Begin(frame.disjoint);
            context->End(frame.begin);
            const bool drawn =
                d3d11_renderer_draw_stream(&renderer, app_detail::application_theme(), false, false,
                                           full ? nullptr : &damage);
            context->End(frame.end);
            context->End(frame.disjoint);
            renderer.graphics.target = presented;
            context->OMSetRenderTargets(1, &renderer.graphics.target, nullptr);
            renderer.gpu_timer.enabled = timer;
            renderer.text_effect_next_ms = effect_next;
            g_gpu_api = api;
            ++g_replay.replays;
            renderer.draw_calls_last_frame = draws;
            renderer.media_draws_last_frame = media;
            renderer.draw_calls_max = draws_max;
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

constexpr u32 kSiteFrames = 4;
struct TrackedRegion {
    u64 base;
    u64 bytes;
    u64 frames[kSiteFrames];
};
constexpr u32 kTrackedCapacity = 32'768;
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

bool name_equals(const char* a, const char* b) noexcept {
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z')
            x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = static_cast<char>(y + 32);
        if (x != y)
            return false;
    }
    return *a == *b;
}

u32 install_import_hooks() noexcept {
    auto* base = reinterpret_cast<u8*>(GetModuleHandleW(nullptr));
    g_image_base = reinterpret_cast<u64>(base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return 0;
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
    for (auto* import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         import->Name; ++import) {
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            base + (import->OriginalFirstThunk ? import->OriginalFirstThunk : import->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + import->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            for (Hook& hook : hooks) {
                if (!name_equals(reinterpret_cast<const char*>(by_name->Name), hook.name))
                    continue;
                DWORD old = 0;
                if (!VirtualProtect(&slots->u1.Function, sizeof(u64), PAGE_READWRITE, &old))
                    continue;
                *hook.original = reinterpret_cast<void*>(slots->u1.Function);
                slots->u1.Function = reinterpret_cast<u64>(hook.replacement);
                VirtualProtect(&slots->u1.Function, sizeof(u64), old, &old);
                ++installed;
            }
        }
    }
    return installed;
}

struct NtUnicodeString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};
struct NtClientId {
    HANDLE process;
    HANDLE thread;
};
struct SpiProcess {
    ULONG next;
    ULONG threads;
    LARGE_INTEGER private_working_set;
    ULONG hard_faults;
    ULONG thread_high_watermark;
    ULONGLONG cycle_time;
    LARGE_INTEGER create_time, user_time, kernel_time;
    NtUnicodeString image_name;
    LONG base_priority;
    HANDLE process_id;
    HANDLE parent_id;
    ULONG handle_count;
    ULONG session_id;
    ULONG_PTR key;
    SIZE_T peak_virtual, virtual_size;
    ULONG page_faults;
    SIZE_T peak_working_set, working_set;
    SIZE_T quota_peak_paged, quota_paged, quota_peak_nonpaged, quota_nonpaged;
    SIZE_T pagefile, peak_pagefile, private_pages;
    LARGE_INTEGER io[6];
};
static_assert(sizeof(SpiProcess) == 256);
struct SpiThread {
    LARGE_INTEGER kernel_time, user_time, create_time;
    ULONG wait_time;
    PVOID start_address;
    HANDLE process_id;
    HANDLE thread_id;
    LONG priority;
    LONG base_priority;
    ULONG context_switches;
    ULONG state;
    ULONG wait_reason;
};
static_assert(sizeof(SpiThread) == 80);
using NtQuerySystemInformationFunction = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationThreadFunction = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
NtQuerySystemInformationFunction g_query_system = nullptr;
NtQueryInformationThreadFunction g_query_thread = nullptr;
u8* g_spi_buffer = nullptr;
constexpr u32 kSpiBytes = 8 * 1024 * 1024;

struct ProcessSample {
    u64 qpc;
    u64 main_cycles;
    u64 process_cycles;
    u64 user_100ns, kernel_100ns;
    u64 context_switches;
    u64 main_context_switches;
    u64 private_working_set;
    u64 private_bytes;
    u64 working_set;
    u64 page_faults;
    u32 threads;
    u32 handles;
    u32 gdi, user;
};
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

// Per-thread owner (Win32 start address), CPU cycles and context switches,
// with the context switches of the final idle phase for idle attribution, and
// the stack reservation and its committed pages (latest sample).
struct ThreadRow {
    u64 id;
    u64 start;
    u64 cycles;
    u32 context_switches;
    u32 idle_baseline;
    u32 idle_switches;
    bool seen_at_idle;
    u64 module_base;
    wchar_t module[40];
    u64 stack_reserved, stack_committed;
};
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
constexpr u32 kThreadCapacity = 96;
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

enum class Phase : u32 {
    startup,
    history_wait,
    idle_static,
    scroll_timeline,
    channel_switch,
    member_list,
    churn,
    settings,
    idle_animated,
    idle_final,
    idle_hidden,
    report,
    done,
    count
};
const char* const kPhaseNames[] = {"startup",  "history",       "idle_static",  "scroll",
                                   "switch",   "member_list",   "churn",        "settings",
                                   "idle_anim", "idle_final",   "idle_hidden",  "report",       "done"};

constexpr u32 kFrameSampleCapacity = 8192;
// Retained text and paint work: parser runs against cache hits, content text
// streams rebuilt (and how many kept the shell's glyphs), paint streams rebuilt,
// GPU instance bytes uploaded, member rows resolved for media and shell
// segments replayed instead of built.
struct TextWork {
    u64 markdown_parses, markdown_hits, text_builds, shell_reuses, paint_builds;
    u64 paint_upload_bytes, text_upload_bytes, media_upload_bytes, member_media_resolves, shell_replays;
};
// GPU work: immediate-context calls, draw-stream GPU time (timestamp
// queries), renderer paint CPU time and texture traffic.
struct GpuWork {
    GpuApiCounters api;
    u64 full_us, full_samples, partial_us, partial_samples, replay_pixels, replay_failures, replays, replay_cycles,
        painted,
        partial_pixels, media_upload_bytes, animation_copy_pixels, glyph_pixels, instance_uploads;
};
struct PhaseStats {
    ProcessSample begin, end;
    AllocCounters alloc_begin, alloc_end;
    TextWork text_begin, text_end;
    GpuWork gpu_begin, gpu_end;
    // Animation timeline at the end of the phase: entries, playing entries,
    // and whether presentation was suspended (inactive window, reduced motion).
    u32 anim_entries, anim_visible;
    bool anim_suspended;
    u64 wakes, wake_timeouts, wake_frames, wake_messages, wake_handles;
    u64 presents;
    u64 draw_calls;
    u32 frame_count;
    u32 frame_cycles_offset; // into g_frame_cycles
    u64 zero_timeouts;
    // After each present: what keeps requesting frames (deadline scheduled).
    u64 next_pending, next_timeline, next_effect, next_spinner, next_video, next_tooltip, next_surface,
        next_background, next_smooth, next_caret, next_interaction, next_timestamp;
    bool measured;
    bool spin_dumped;
};
PhaseStats g_phases[static_cast<u32>(Phase::count)];
u64* g_frame_cycles = nullptr; // per present, main-thread cycles since the previous present
u32 g_frame_cycle_count = 0;

struct Probe {
    Phase phase;
    u64 phase_start_qpc;
    u64 qpc_frequency;
    u64 process_create_100ns;
    u64 first_present_100ns;
    u64 ready_100ns;
    u64 history_100ns;
    u64 last_present_cycles;
    u64 present_counter;
    u32 step_counter;
    u32 actions;
    u64 next_action_qpc;
    u64 media_channel;
    u64 text_channel;
    u64 lab_guild;
    bool initialized;
    bool failed;
    bool shell_seen;
    const char* failure;
    PerfAccountControlFunction control;
    PerfAccountCountersFunction counters;
    PerfAccountCounters module_counters;
    u32 hooks;
    u32 context_hooks;
    u32 client_width, client_height;
    u32 zero_streak;
    u32 final_messages, final_guilds, final_channels, final_members;
};
Probe g_probe{};
// Phase tick (see arm_phase_tick).
HANDLE g_tick_timer = nullptr;
HANDLE g_tick_thread = nullptr;
volatile LONG g_tick_stop = 0;
DWORD WINAPI probe_ticker(void*) noexcept {
    while (WaitForSingleObject(g_tick_timer, INFINITE) == WAIT_OBJECT_0 && !g_tick_stop)
        PostMessageW(g_app.window, WM_NULL, 0, 0);
    return 0;
}

u64 now_100ns() noexcept {
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
u64 qpc_now() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return static_cast<u64>(value.QuadPart);
}
u64 elapsed_ms(u64 since_qpc) noexcept {
    return (qpc_now() - since_qpc) * 1000 / g_probe.qpc_frequency;
}
void fail(const char* reason) noexcept {
    if (!g_probe.failed) {
        g_probe.failed = true;
        g_probe.failure = reason;
    }
}

void write_stdout(const char* text, u32 bytes) noexcept;
void trace(const char* text) noexcept {
    char line[160];
    u32 used = 0;
    for (const char* prefix = "trace "; *prefix; ++prefix)
        line[used++] = *prefix;
    while (*text && used < sizeof(line) - 2)
        line[used++] = *text++;
    line[used++] = '\n';
    write_stdout(line, used);
}

TextWork sample_text_work() noexcept {
    TextWork work{};
    if (g_app.account_ui) {
        work.markdown_parses = g_app.account_ui->message_text.markdown_parses;
        work.markdown_hits = g_app.account_ui->message_text.markdown_cache_hits;
    }
    work.text_builds = g_app.renderer.content_text_prefix.base_builds + g_app.renderer.content_text_prefix.overlay_builds;
    work.shell_reuses = g_app.renderer.content_text_prefix.shell.reuses;
    work.paint_builds = g_app.renderer.paint_stream_builds;
    work.paint_upload_bytes = g_app.renderer.paint_upload_bytes;
    work.text_upload_bytes = g_app.renderer.text_upload_bytes;
    work.media_upload_bytes = g_app.renderer.media_upload_bytes;
    if (g_app.account_ui)
        work.member_media_resolves = g_app.account_ui->member_media_cache.resolves;
    work.shell_replays = g_app.renderer.shell_segment.replays;
    return work;
}

GpuWork sample_gpu_work() noexcept {
    GpuWork work{};
    work.api = g_gpu_api;
    const auto& r = g_app.renderer;
    gpu_replay_collect(r.graphics.context);
    work.full_us = g_replay.full_us;
    work.full_samples = g_replay.full_samples;
    work.partial_us = g_replay.partial_us;
    work.partial_samples = g_replay.partial_samples;
    work.replay_pixels = g_replay.partial_pixels;
    work.replay_failures = g_replay.failures + g_replay.skipped;
    work.replays = g_replay.replays;
    work.replay_cycles = static_cast<u64>(g_replay.cycles);
    work.painted = r.painted_frames;
    work.partial_pixels = r.partial_present_pixels;
    work.media_upload_bytes = r.media_gpu.uploaded_bytes;
    work.animation_copy_pixels = r.animation_gpu.copied_pixels + r.preview_animation_gpu.copied_pixels;
    work.glyph_pixels = r.glyph_text.gpu.dispatched_pixels;
    work.instance_uploads =
        static_cast<u64>(r.paint_instance_uploads) + r.text_instance_uploads + r.media_instance_uploads;
    return work;
}

void begin_phase(Phase phase) noexcept {
    PhaseStats& stats = g_phases[static_cast<u32>(phase)];
    sample_process(&stats.begin);
    stats.alloc_begin = g_alloc;
    stats.frame_cycles_offset = g_frame_cycle_count;
    stats.measured = true;
    g_probe.phase = phase;
    trace(kPhaseNames[static_cast<u32>(phase)]);
    g_probe.phase_start_qpc = stats.begin.qpc;
    stats.text_begin = sample_text_work();
    stats.gpu_begin = sample_gpu_work();
    g_probe.actions = 0;
    g_probe.next_action_qpc = stats.begin.qpc;
}
void end_phase() noexcept {
    PhaseStats& stats = g_phases[static_cast<u32>(g_probe.phase)];
    sample_process(&stats.end);
    stats.alloc_end = g_alloc;
    stats.text_end = sample_text_work();
    stats.gpu_end = sample_gpu_work();
    const auto& timeline = g_app.animations.timeline;
    stats.anim_entries = timeline.entry_count;
    stats.anim_visible = 0;
    for (u32 i = 0; timeline.entries && i < timeline.capacity; ++i)
        stats.anim_visible += (timeline.entries[i].flags & animation_timeline_entry_occupied) &&
                              (timeline.entries[i].flags & animation_timeline_entry_visible);
    stats.anim_suspended = timeline.suspended;
}
void next_phase(Phase phase) noexcept {
    end_phase();
    begin_phase(phase);
}

bool select_channel(u64 guild, u64 channel) noexcept {
    if (!g_app.account_ui)
        return false;
    auto& state = g_app.account_ui->chat_state;
    if (!chat_state_store_find_channel(state, channel))
        return false;
    if (guild && !app_detail::select_content_guild(guild))
        return false;
    if (!chat_state_store_select_channel(&state, channel).ok())
        return false;
    app_detail::application_friends_hide();
    if (!app_detail::synchronize_navigation_view() || !app_detail::synchronize_selected_channel() ||
        !app_detail::refresh_account_text())
        return false;
    app_detail::pump_message_history();
    app_detail::application_request_frame();
    return true;
}

// Wheel input posted to this process's own window (synthetic data only).
void post_wheel(float x_dip, float y_dip, i32 delta) noexcept {
    const float scale = static_cast<float>(maximum(g_app.renderer.dpi, 1u)) / 96.0f;
    POINT point{static_cast<LONG>(x_dip * scale), static_cast<LONG>(y_dip * scale)};
    ClientToScreen(g_app.window, &point);
    PostMessageW(g_app.window, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<u16>(static_cast<i16>(delta))),
                 MAKELPARAM(static_cast<u16>(point.x), static_cast<u16>(point.y)));
}
float client_width_dip() noexcept {
    RECT client{};
    GetClientRect(g_app.window, &client);
    return static_cast<float>(client.right) * 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
}
float client_height_dip() noexcept {
    RECT client{};
    GetClientRect(g_app.window, &client);
    return static_cast<float>(client.bottom) * 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
}
float message_x() noexcept {
    const float left = 72.0f + static_cast<float>(g_app.channel_pane_width);
    const float right = client_width_dip() - static_cast<float>(g_app.member_pane_width);
    return (left + right) * 0.5f;
}
float member_x() noexcept {
    return client_width_dip() - static_cast<float>(g_app.member_pane_width) * 0.5f;
}

bool due(u64 interval_ms) noexcept {
    const u64 now = qpc_now();
    if (now < g_probe.next_action_qpc)
        return false;
    g_probe.next_action_qpc = now + interval_ms * g_probe.qpc_frequency / 1000;
    ++g_probe.actions;
    return true;
}

bool shell_ready() noexcept {
    return g_app.account_ui && !g_app.login.visible &&
           g_app.account_ui->chat_state.guild_count >= kPerfGuilds;
}

enum MemoryCategory : u32 {
    cat_app_virtual,
    cat_heap,
    cat_stack,
    cat_synthetic,
    cat_probe,
    cat_write_combined,
    cat_other_private,
    cat_image_exe,
    cat_image_graphics,
    cat_image_system,
    cat_image_synthetic,
    cat_mapped_file,
    cat_mapped_section,
    cat_count
};
const char* const kCategoryNames[] = {"app_virtualalloc", "heaps",         "stacks",       "synthetic_module",
                                      "probe",            "write_combined", "other_private", "image_exe",
                                      "image_graphics",   "image_system",   "image_synthetic", "mapped_files",
                                      "mapped_sections"};
struct CategoryTotals {
    u64 committed, resident, private_resident, regions;
};
struct ModuleTotals {
    u64 base, size, resident, private_resident;
    wchar_t name[48];
};
constexpr u32 kModuleCapacity = 160;
struct SiteTotals {
    u64 frames[kSiteFrames];
    u64 committed, resident, regions;
};
constexpr u32 kSiteCapacity = 2048;
struct MemorySnapshot {
    CategoryTotals categories[cat_count];
    ModuleTotals modules[kModuleCapacity];
    u32 module_count;
    SiteTotals sites[kSiteCapacity];
    u32 site_count;
    u64 heap_committed, heap_busy, heap_blocks, heap_count;
    u64 stack_reserved, stack_committed, stack_threads;
    u64 app_regions, app_reserved, app_small_regions;
    u64 gpu_local, gpu_nonlocal;
    bool valid;
};
MemorySnapshot* g_snapshots[3] = {};
const char* const kSnapshotNames[] = {"shell", "after_scroll", "end"};

struct Range {
    u64 begin, end;
};
constexpr u32 kRangeCapacity = 4096;
Range* g_heap_ranges = nullptr;
u32 g_heap_range_count = 0;
Range* g_stack_ranges = nullptr;
u32 g_stack_range_count = 0;
Range g_probe_ranges[8];
u32 g_probe_range_count = 0;

bool in_ranges(const Range* ranges, u32 count, u64 address) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (address >= ranges[i].begin && address < ranges[i].end)
            return true;
    return false;
}

void collect_heaps(MemorySnapshot& snapshot) noexcept {
    g_heap_range_count = 0;
    HANDLE heaps[128];
    const DWORD count = GetProcessHeaps(128, heaps);
    snapshot.heap_count = count;
    for (DWORD h = 0; h < count && h < 128; ++h) {
        if (!HeapLock(heaps[h]))
            continue;
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heaps[h], &entry)) {
            if (entry.wFlags & PROCESS_HEAP_REGION) {
                if (g_heap_range_count < kRangeCapacity) {
                    const u64 begin = reinterpret_cast<u64>(entry.Region.lpFirstBlock);
                    g_heap_ranges[g_heap_range_count++] = {begin & ~0xFFFFull,
                                                           reinterpret_cast<u64>(entry.Region.lpLastBlock)};
                }
                snapshot.heap_committed += entry.Region.dwCommittedSize;
            } else if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                snapshot.heap_busy += entry.cbData;
                ++snapshot.heap_blocks;
            }
        }
        HeapUnlock(heaps[h]);
    }
}

void collect_stacks(MemorySnapshot& snapshot) noexcept {
    g_stack_range_count = 0;
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
                HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE,
                                           static_cast<DWORD>(reinterpret_cast<u64>(threads[t].thread_id)));
                if (!thread)
                    continue;
                struct {
                    LONG exit_status;
                    PVOID teb;
                    NtClientId client;
                    ULONG_PTR affinity;
                    LONG priority, base_priority;
                } basic{};
                if (g_query_thread(thread, 0, &basic, sizeof(basic), nullptr) >= 0 && basic.teb) {
                    const auto* tib = static_cast<const NT_TIB*>(basic.teb);
                    const u64 base = reinterpret_cast<u64>(tib->StackBase);
                    const u64 deallocation = *reinterpret_cast<const u64*>(static_cast<const u8*>(basic.teb) + 0x1478);
                    if (base > deallocation && g_stack_range_count < kRangeCapacity) {
                        g_stack_ranges[g_stack_range_count++] = {deallocation, base};
                        snapshot.stack_reserved += base - deallocation;
                        ++snapshot.stack_threads;
                    }
                }
                CloseHandle(thread);
            }
            break;
        }
        if (!process->next)
            break;
        at += process->next;
    }
}

u32 module_index(MemorySnapshot& snapshot, u64 allocation_base, MemoryCategory* category) noexcept {
    for (u32 i = 0; i < snapshot.module_count; ++i)
        if (snapshot.modules[i].base == allocation_base) {
            const wchar_t* name = snapshot.modules[i].name;
            *category = cat_image_system;
            auto has = [&](const wchar_t* needle) noexcept {
                for (const wchar_t* at = name; *at; ++at) {
                    u32 k = 0;
                    while (needle[k] && at[k] && ((at[k] | 32) == (needle[k] | 32)))
                        ++k;
                    if (!needle[k])
                        return true;
                }
                return false;
            };
            if (allocation_base == g_image_base)
                *category = cat_image_exe;
            else if (has(L"MaiCordPerfAccount"))
                *category = cat_image_synthetic;
            else if (has(L"d3d") || has(L"dxgi") || has(L"dcomp") || has(L"nvw") || has(L"nvl") ||
                     has(L"amd") || has(L"atidx") || has(L"igd") || has(L"igc") || has(L"intel") ||
                     has(L"dxcore") || has(L"gdi32full") || has(L"nvapi") || has(L"nvgpu") || has(L"ati"))
                *category = cat_image_graphics;
            return i;
        }
    if (snapshot.module_count == kModuleCapacity)
        return ~0u;
    ModuleTotals& module = snapshot.modules[snapshot.module_count];
    module = {};
    module.base = allocation_base;
    wchar_t path[MAX_PATH]{};
    if (!K32GetModuleFileNameExW(GetCurrentProcess(), reinterpret_cast<HMODULE>(allocation_base), path, MAX_PATH))
        K32GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(allocation_base), path, MAX_PATH);
    const wchar_t* file = path;
    for (const wchar_t* at = path; *at; ++at)
        if (*at == L'\\' || *at == L'/')
            file = at + 1;
    for (u32 i = 0; i < 47 && file[i]; ++i)
        module.name[i] = file[i];
    ++snapshot.module_count;
    return module_index(snapshot, allocation_base, category);
}

bool take_snapshot(u32 index) noexcept {
    MemorySnapshot* snapshot = g_snapshots[index];
    if (!snapshot)
        return false;
    *snapshot = {};
    collect_heaps(*snapshot);
    collect_stacks(*snapshot);
    // GPU memory as DXGI accounts it for this process.
    if (g_app.renderer.graphics.device) {
        IDXGIDevice* dxgi = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIAdapter3* adapter3 = nullptr;
        if (SUCCEEDED(g_app.renderer.graphics.device->QueryInterface(__uuidof(IDXGIDevice),
                                                                     reinterpret_cast<void**>(&dxgi))) &&
            SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
                snapshot->gpu_local = local.CurrentUsage;
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal)))
                snapshot->gpu_nonlocal = nonlocal.CurrentUsage;
        }
        com_release(adapter3);
        com_release(adapter);
        com_release(dxgi);
    }
    PerfAccountCounters module{};
    if (g_probe.counters)
        g_probe.counters(&module);
    AcquireSRWLockShared(&g_tracked_lock);
    snapshot->app_regions = g_tracked_count;
    for (u32 i = 0; i < g_tracked_count; ++i) {
        snapshot->app_reserved += g_tracked[i].bytes;
        if (g_tracked[i].bytes <= 16 * 1024)
            ++snapshot->app_small_regions;
    }
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const u64 limit = reinterpret_cast<u64>(info.lpMaximumApplicationAddress);
    PSAPI_WORKING_SET_EX_INFORMATION pages[512];
    u64 at = 0;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region)) != sizeof(region))
            break;
        const u64 begin = reinterpret_cast<u64>(region.BaseAddress);
        const u64 end = begin + region.RegionSize;
        if (end <= at)
            break;
        at = end;
        if (region.State != MEM_COMMIT)
            continue;
        MemoryCategory category = cat_other_private;
        u32 module_slot = ~0u;
        i32 site = -1;
        if (region.Type == MEM_IMAGE) {
            module_slot = module_index(*snapshot, reinterpret_cast<u64>(region.AllocationBase), &category);
        } else if (region.Type == MEM_MAPPED) {
            wchar_t path[8];
            category = K32GetMappedFileNameW(GetCurrentProcess(), region.BaseAddress, path, 8) ? cat_mapped_file
                                                                                                 : cat_mapped_section;
        } else {
            const u64 allocation = reinterpret_cast<u64>(region.AllocationBase);
            if (module.module_base && begin >= module.module_base &&
                begin < module.module_base + module.module_reserved_bytes)
                category = cat_synthetic;
            else if (in_ranges(g_probe_ranges, g_probe_range_count, begin))
                category = cat_probe;
            else if (in_ranges(g_stack_ranges, g_stack_range_count, begin))
                category = cat_stack;
            else {
                const u32 tracked = tracked_lower_bound(allocation);
                if (tracked < g_tracked_count && g_tracked[tracked].base == allocation) {
                    category = cat_app_virtual;
                    // Aggregate by allocation call stack.
                    const TrackedRegion& owner = g_tracked[tracked];
                    for (u32 s = 0; s < snapshot->site_count; ++s)
                        if (bytes_equal(snapshot->sites[s].frames, owner.frames, sizeof(owner.frames))) {
                            site = static_cast<i32>(s);
                            break;
                        }
                    if (site < 0 && snapshot->site_count < kSiteCapacity) {
                        site = static_cast<i32>(snapshot->site_count++);
                        copy_bytes(snapshot->sites[site].frames, owner.frames, sizeof(owner.frames));
                    }
                    if (site >= 0 && begin == allocation)
                        ++snapshot->sites[site].regions;
                } else if (in_ranges(g_heap_ranges, g_heap_range_count, begin))
                    category = cat_heap;
                else if (region.Protect & PAGE_WRITECOMBINE)
                    category = cat_write_combined;
            }
        }
        u64 resident = 0, private_resident = 0;
        for (u64 page = begin; page < end;) {
            u32 count = 0;
            while (count < 512 && page < end) {
                pages[count++].VirtualAddress = reinterpret_cast<void*>(page);
                page += 4096;
            }
            if (!K32QueryWorkingSetEx(GetCurrentProcess(), pages, static_cast<DWORD>(count * sizeof(pages[0]))))
                break;
            for (u32 i = 0; i < count; ++i)
                if (pages[i].VirtualAttributes.Valid) {
                    resident += 4096;
                    if (!pages[i].VirtualAttributes.Shared)
                        private_resident += 4096;
                }
        }
        CategoryTotals& totals = snapshot->categories[category];
        totals.committed += region.RegionSize;
        totals.resident += resident;
        totals.private_resident += private_resident;
        ++totals.regions;
        if (category == cat_stack)
            snapshot->stack_committed += region.RegionSize;
        if (module_slot != ~0u) {
            snapshot->modules[module_slot].size += region.RegionSize;
            snapshot->modules[module_slot].resident += resident;
            snapshot->modules[module_slot].private_resident += private_resident;
        }
        if (site >= 0) {
            snapshot->sites[site].committed += region.RegionSize;
            snapshot->sites[site].resident += private_resident;
        }
    }
    ReleaseSRWLockShared(&g_tracked_lock);
    snapshot->valid = true;
    return true;
}

double cycles_per_us = 0;

void calibrate_cycles() noexcept {
    const u64 qpc0 = qpc_now();
    const u64 tsc0 = __rdtsc();
    Sleep(30);
    const u64 qpc1 = qpc_now();
    const u64 tsc1 = __rdtsc();
    const double us = static_cast<double>(qpc1 - qpc0) * 1e6 / static_cast<double>(g_probe.qpc_frequency);
    cycles_per_us = us > 0 ? static_cast<double>(tsc1 - tsc0) / us : 1.0;
}
double cycles_to_us(u64 cycles) noexcept {
    return cycles_per_us > 0 ? static_cast<double>(cycles) / cycles_per_us : 0;
}

void sort_u64(u64* values, u32 count) noexcept {
    // Heap sort: no CRT qsort.
    auto sift = [&](u32 start, u32 end) noexcept {
        u32 root = start;
        while (root * 2 + 1 < end) {
            u32 child = root * 2 + 1;
            if (child + 1 < end && values[child] < values[child + 1])
                ++child;
            if (values[root] >= values[child])
                return;
            const u64 t = values[root];
            values[root] = values[child];
            values[child] = t;
            root = child;
        }
    };
    if (count < 2)
        return;
    for (u32 start = count / 2; start-- > 0;)
        sift(start, count);
    for (u32 end = count - 1; end > 0; --end) {
        const u64 t = values[0];
        values[0] = values[end];
        values[end] = t;
        sift(0, end);
    }
}
u64 percentile(const u64* sorted, u32 count, u32 permille) noexcept {
    if (!count)
        return 0;
    const u32 index = minimum<u32>(count - 1, static_cast<u32>((static_cast<u64>(count) * permille + 999) / 1000) - 1);
    return sorted[index];
}

void report_phase(Phase phase) noexcept {
    PhaseStats& stats = g_phases[static_cast<u32>(phase)];
    if (!stats.measured)
        return;
    Out& o = g_out;
    const double freq = static_cast<double>(g_probe.qpc_frequency);
    o.s("phase name=");
    o.s(kPhaseNames[static_cast<u32>(phase)]);
    o.kv("ms", static_cast<u64>((stats.end.qpc - stats.begin.qpc) * 1000.0 / freq));
    o.kv("frames", stats.frame_count);
    o.kvf("main_cpu_ms", cycles_to_us(stats.end.main_cycles - stats.begin.main_cycles) / 1000.0);
    o.kvf("proc_cpu_ms", cycles_to_us(stats.end.process_cycles - stats.begin.process_cycles) / 1000.0);
    o.kvf("proc_time_ms", static_cast<double>((stats.end.user_100ns + stats.end.kernel_100ns) -
                                              (stats.begin.user_100ns + stats.begin.kernel_100ns)) / 10'000.0);
    u64* frames = g_frame_cycles + stats.frame_cycles_offset;
    sort_u64(frames, stats.frame_count);
    o.kvf("frame_p50_us", cycles_to_us(percentile(frames, stats.frame_count, 500)));
    o.kvf("frame_p95_us", cycles_to_us(percentile(frames, stats.frame_count, 950)));
    o.kvf("frame_p99_us", cycles_to_us(percentile(frames, stats.frame_count, 990)));
    o.kvf("frame_max_us", cycles_to_us(stats.frame_count ? frames[stats.frame_count - 1] : 0));
    o.kv("wakes", stats.wakes);
    o.kv("wake_timeouts", stats.wake_timeouts);
    o.kv("wake_frames", stats.wake_frames);
    o.kv("wake_messages", stats.wake_messages);
    o.kv("wake_handles", stats.wake_handles);
    o.kv("zero_timeouts", stats.zero_timeouts);
    o.kv("next_pending", stats.next_pending);
    o.kv("next_timeline", stats.next_timeline);
    o.kv("next_effect", stats.next_effect);
    o.kv("next_spinner", stats.next_spinner);
    o.kv("next_video", stats.next_video);
    o.kv("next_tooltip", stats.next_tooltip);
    o.kv("next_surface", stats.next_surface);
    o.kv("next_background", stats.next_background);
    o.kv("next_smooth", stats.next_smooth);
    o.kv("next_caret", stats.next_caret);
    o.kv("next_interaction", stats.next_interaction);
    o.kv("next_timestamp", stats.next_timestamp);
    const auto delta = [](u64 end, u64 begin) noexcept { return end >= begin ? end - begin : end; };
    o.kv("md_parses", delta(stats.text_end.markdown_parses, stats.text_begin.markdown_parses));
    o.kv("md_hits", delta(stats.text_end.markdown_hits, stats.text_begin.markdown_hits));
    o.kv("text_builds", delta(stats.text_end.text_builds, stats.text_begin.text_builds));
    o.kv("shell_reuses", delta(stats.text_end.shell_reuses, stats.text_begin.shell_reuses));
    o.kv("paint_builds", delta(stats.text_end.paint_builds, stats.text_begin.paint_builds));
    o.kv("up_paint_b", delta(stats.text_end.paint_upload_bytes, stats.text_begin.paint_upload_bytes));
    o.kv("up_text_b", delta(stats.text_end.text_upload_bytes, stats.text_begin.text_upload_bytes));
    o.kv("up_media_b", delta(stats.text_end.media_upload_bytes, stats.text_begin.media_upload_bytes));
    o.kv("member_resolves", delta(stats.text_end.member_media_resolves, stats.text_begin.member_media_resolves));
    o.kv("shell_replays", delta(stats.text_end.shell_replays, stats.text_begin.shell_replays));
    o.kv("anim_entries", stats.anim_entries);
    o.kv("anim_visible", stats.anim_visible);
    o.kv("anim_suspended", stats.anim_suspended);
    o.kv("ctxsw", stats.end.context_switches - stats.begin.context_switches);
    o.kv("ctxsw_main", stats.end.main_context_switches - stats.begin.main_context_switches);
    o.kv("draws", stats.draw_calls);
    {
        const GpuApiCounters& a = stats.gpu_end.api;
        const GpuApiCounters& b = stats.gpu_begin.api;
        const auto d = [](LONG64 end, LONG64 begin) noexcept { return static_cast<u64>(end - begin); };
        o.kv("g_shader", d(a.shader_binds, b.shader_binds));
        o.kv("g_srv", d(a.resource_binds, b.resource_binds));
        o.kv("g_sampler", d(a.sampler_binds, b.sampler_binds));
        o.kv("g_cb", d(a.constant_binds, b.constant_binds));
        o.kv("g_ia", d(a.input_binds, b.input_binds));
        o.kv("g_om", d(a.output_binds, b.output_binds));
        o.kv("g_rs", d(a.raster_binds, b.raster_binds));
        o.kv("g_scissor", d(a.scissors, b.scissors));
        o.kv("g_scissor_same", d(a.repeated_scissors, b.repeated_scissors));
        o.kv("g_maps", d(a.maps, b.maps));
        o.kv("g_discard", d(a.map_discards, b.map_discards));
        o.kv("g_nooverwrite", d(a.map_no_overwrites, b.map_no_overwrites));
        o.kv("g_copies", d(a.copies, b.copies));
        o.kv("g_copy_px", d(a.copy_pixels, b.copy_pixels));
        o.kv("g_dispatch", d(a.dispatches, b.dispatches));
        o.kv("g_groups", d(a.dispatch_groups, b.dispatch_groups));
        o.kv("g_cs", d(a.compute_binds, b.compute_binds));
        o.kv("g_clears", d(a.clears, b.clears));
        o.kv("g_flushes", d(a.flushes, b.flushes));
        const GpuWork& e = stats.gpu_end;
        const GpuWork& w = stats.gpu_begin;
        o.kv("gpu_full_us", e.full_us - w.full_us);
        o.kv("gpu_full_n", e.full_samples - w.full_samples);
        o.kv("gpu_partial_us", e.partial_us - w.partial_us);
        o.kv("gpu_partial_n", e.partial_samples - w.partial_samples);
        o.kv("gpu_partial_px", e.replay_pixels - w.replay_pixels);
        o.kv("gpu_replay_fail", e.replay_failures - w.replay_failures);
        // Main-thread CPU of the replayed draw streams: submission cost alone.
        o.kv("replays", e.replays - w.replays);
        o.kvf("replay_cpu_us", cycles_to_us(e.replay_cycles - w.replay_cycles));
        o.kv("painted", e.painted - w.painted);
        o.kv("partial_px", e.partial_pixels - w.partial_pixels);
        o.kv("media_up_b", e.media_upload_bytes - w.media_upload_bytes);
        o.kv("anim_copy_px", e.animation_copy_pixels - w.animation_copy_pixels);
        o.kv("glyph_px", e.glyph_pixels - w.glyph_pixels);
        o.kv("inst_uploads", e.instance_uploads - w.instance_uploads);
    }
    o.kv("va_calls", static_cast<u64>(stats.alloc_end.virtual_alloc_calls - stats.alloc_begin.virtual_alloc_calls));
    o.kv("va_commit_kb",
         static_cast<u64>(stats.alloc_end.virtual_commit_bytes - stats.alloc_begin.virtual_commit_bytes) / 1024);
    o.kv("vf_calls", static_cast<u64>(stats.alloc_end.virtual_free_calls - stats.alloc_begin.virtual_free_calls));
    o.kv("heap_calls", static_cast<u64>((stats.alloc_end.heap_alloc_calls - stats.alloc_begin.heap_alloc_calls) +
                                        (stats.alloc_end.heap_realloc_calls - stats.alloc_begin.heap_realloc_calls)));
    o.kv("heap_kb", static_cast<u64>(stats.alloc_end.heap_alloc_bytes - stats.alloc_begin.heap_alloc_bytes) / 1024);
    o.kv("faults", stats.end.page_faults - stats.begin.page_faults);
    o.kv("pws_kb", stats.end.private_working_set / 1024);
    o.kv("private_kb", stats.end.private_bytes / 1024);
    o.kv("threads", stats.end.threads);
    o.kv("handles", stats.end.handles);
    o.s("\n");
}

void report_snapshot(u32 index) noexcept {
    const MemorySnapshot* snapshot = g_snapshots[index];
    if (!snapshot || !snapshot->valid)
        return;
    Out& o = g_out;
    for (u32 c = 0; c < cat_count; ++c) {
        const CategoryTotals& totals = snapshot->categories[c];
        o.s("memory snapshot=");
        o.s(kSnapshotNames[index]);
        o.s(" category=");
        o.s(kCategoryNames[c]);
        o.kv("commit_kb", totals.committed / 1024);
        o.kv("resident_kb", totals.resident / 1024);
        o.kv("pws_kb", totals.private_resident / 1024);
        o.kv("regions", totals.regions);
        o.s("\n");
    }
    o.s("memory_detail snapshot=");
    o.s(kSnapshotNames[index]);
    o.kv("heap_count", snapshot->heap_count);
    o.kv("heap_committed_kb", snapshot->heap_committed / 1024);
    o.kv("heap_busy_kb", snapshot->heap_busy / 1024);
    o.kv("heap_blocks", snapshot->heap_blocks);
    o.kv("stack_threads", snapshot->stack_threads);
    o.kv("stack_reserved_kb", snapshot->stack_reserved / 1024);
    o.kv("stack_committed_kb", snapshot->stack_committed / 1024);
    o.kv("app_regions", snapshot->app_regions);
    o.kv("app_small_regions", snapshot->app_small_regions);
    o.kv("app_reserved_kb", snapshot->app_reserved / 1024);
    o.kv("gpu_local_kb", snapshot->gpu_local / 1024);
    o.kv("gpu_nonlocal_kb", snapshot->gpu_nonlocal / 1024);
    o.s("\n");
}

void report_modules(const MemorySnapshot& snapshot) noexcept {
    // Largest resident modules first (selection by repeated maximum).
    bool used[kModuleCapacity]{};
    for (u32 rank = 0; rank < 24 && rank < snapshot.module_count; ++rank) {
        u32 best = ~0u;
        for (u32 i = 0; i < snapshot.module_count; ++i)
            if (!used[i] && (best == ~0u || snapshot.modules[i].resident > snapshot.modules[best].resident))
                best = i;
        if (best == ~0u)
            break;
        used[best] = true;
        const ModuleTotals& module = snapshot.modules[best];
        g_out.s("module name=");
        g_out.wide(module.name);
        g_out.kv("commit_kb", module.size / 1024);
        g_out.kv("resident_kb", module.resident / 1024);
        g_out.kv("pws_kb", module.private_resident / 1024);
        g_out.s("\n");
    }
    g_out.kv("modules_loaded", snapshot.module_count);
    g_out.s("\n");
}

void report_sites(const MemorySnapshot& snapshot) noexcept {
    bool used[kSiteCapacity]{};
    for (u32 rank = 0; rank < 40 && rank < snapshot.site_count; ++rank) {
        u32 best = ~0u;
        for (u32 i = 0; i < snapshot.site_count; ++i)
            if (!used[i] && (best == ~0u || snapshot.sites[i].committed > snapshot.sites[best].committed))
                best = i;
        if (best == ~0u)
            break;
        used[best] = true;
        const SiteTotals& site = snapshot.sites[best];
        g_out.s("site frames=");
        for (u32 f = 0; f < kSiteFrames; ++f) {
            if (f)
                g_out.s("/");
            const u64 frame = site.frames[f];
            g_out.hex(frame >= g_image_base && frame < g_image_base + 0x10000000ull ? frame - g_image_base : 0);
        }
        g_out.kv("commit_kb", site.committed / 1024);
        g_out.kv("pws_kb", site.resident / 1024);
        g_out.kv("regions", site.regions);
        g_out.s("\n");
    }
}

// Sampling profiler, opt-in (MAICORD_PERF_PROFILE=1): a 1 kHz sampler suspends
// the main thread, unwinds it with .pdata (no symbols, no locks: a module table
// built while the main thread runs) and counts function-start RVAs per phase.
// Self time in another module is charged to that module and to the nearest
// probe-image caller; run_perf.ps1 resolves RVAs through the link map.
struct ProfileModule {
    u64 base, end;
    const RUNTIME_FUNCTION* functions;
    u32 function_count;
    u32 stable_id; // index into g_profile_names; module order changes as DLLs load
};
struct ProfileName {
    u64 base;
    wchar_t name[32];
};
constexpr u32 kProfileNameCapacity = 256;
ProfileName g_profile_names[kProfileNameCapacity];
u32 g_profile_name_count = 0;
constexpr u32 kProfileModuleCapacity = 96;
ProfileModule g_profile_modules[kProfileModuleCapacity];
u32 g_profile_module_count = 0;
struct ProfileEntry {
    u32 key; // function begin RVA in the probe image, or 0x80000000 | module index
    u32 self;
    u32 inclusive;
    u32 caller_self; // self time of callees in other modules charged here
};
constexpr u32 kProfileEntries = 4096;
struct PhaseProfile {
    ProfileEntry entries[kProfileEntries];
    u32 samples, unattributed, in_syscall;
};
PhaseProfile* g_profiles = nullptr; // one per phase
HANDLE g_sampler = nullptr;
HANDLE g_main_thread = nullptr;
volatile LONG g_sampler_stop = 0;

void profile_refresh_modules() noexcept {
    HMODULE modules[kProfileModuleCapacity];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return;
    u32 count = minimum<u32>(bytes / sizeof(HMODULE), kProfileModuleCapacity);
    ProfileModule fresh[kProfileModuleCapacity];
    u32 used = 0;
    for (u32 i = 0; i < count; ++i) {
        auto* base = reinterpret_cast<const u8*>(modules[i]);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto& pdata = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        ProfileModule& m = fresh[used++];
        m = {};
        m.base = reinterpret_cast<u64>(base);
        m.end = m.base + nt->OptionalHeader.SizeOfImage;
        m.functions = reinterpret_cast<const RUNTIME_FUNCTION*>(base + pdata.VirtualAddress);
        m.function_count = pdata.Size / sizeof(RUNTIME_FUNCTION);
        u32 id = 0;
        while (id < g_profile_name_count && g_profile_names[id].base != m.base)
            ++id;
        if (id == g_profile_name_count && id < kProfileNameCapacity) {
            ProfileName& name = g_profile_names[g_profile_name_count++];
            name = {};
            name.base = m.base;
            wchar_t path[MAX_PATH]{};
            K32GetModuleBaseNameW(GetCurrentProcess(), modules[i], path, MAX_PATH);
            for (u32 c = 0; c < 31 && path[c]; ++c)
                name.name[c] = path[c];
        }
        m.stable_id = id;
    }
    // Sort by base (insertion sort; under a hundred modules).
    for (u32 i = 1; i < used; ++i)
        for (u32 j = i; j > 0 && fresh[j - 1].base > fresh[j].base; --j) {
            const ProfileModule t = fresh[j];
            fresh[j] = fresh[j - 1];
            fresh[j - 1] = t;
        }
    copy_bytes(g_profile_modules, fresh, used * static_cast<u32>(sizeof(ProfileModule)));
    g_profile_module_count = used;
}

i32 profile_module_of(u64 address) noexcept {
    u32 low = 0, high = g_profile_module_count;
    while (low < high) {
        const u32 mid = (low + high) / 2;
        if (g_profile_modules[mid].end <= address)
            low = mid + 1;
        else
            high = mid;
    }
    return low < g_profile_module_count && address >= g_profile_modules[low].base ? static_cast<i32>(low) : -1;
}

const RUNTIME_FUNCTION* profile_function_of(const ProfileModule& module, u64 address) noexcept {
    const u32 rva = static_cast<u32>(address - module.base);
    u32 low = 0, high = module.function_count;
    while (low < high) {
        const u32 mid = (low + high) / 2;
        if (module.functions[mid].EndAddress <= rva)
            low = mid + 1;
        else
            high = mid;
    }
    if (low < module.function_count && rva >= module.functions[low].BeginAddress)
        return &module.functions[low];
    return nullptr;
}

// The primary entry of a chained (split) function, for attribution.
u32 profile_primary_begin(const ProfileModule& module, const RUNTIME_FUNCTION* function) noexcept {
    for (u32 depth = 0; depth < 8 && function; ++depth) {
        if (function->UnwindData & 1)
            return function->BeginAddress;
        const u8* info = reinterpret_cast<const u8*>(module.base + function->UnwindData);
        if (!((info[0] >> 3) & 0x4)) // UNW_FLAG_CHAININFO
            return function->BeginAddress;
        const u32 codes = info[2];
        const u32 offset = 4 + ((codes + 1) & ~1u) * 2;
        function = reinterpret_cast<const RUNTIME_FUNCTION*>(info + offset);
    }
    return function ? function->BeginAddress : 0;
}

void profile_count(PhaseProfile& profile, u32 key, u32 self, u32 inclusive, u32 caller_self) noexcept {
    u32 slot = (key * 2654435761u) & (kProfileEntries - 1);
    for (u32 probe = 0; probe < kProfileEntries; ++probe, slot = (slot + 1) & (kProfileEntries - 1)) {
        ProfileEntry& entry = profile.entries[slot];
        if (entry.key == key || (!entry.key && !entry.self && !entry.inclusive)) {
            entry.key = key;
            entry.self += self;
            entry.inclusive += inclusive;
            entry.caller_self += caller_self;
            return;
        }
    }
}

u64 g_profile_last_cycles = 0;

void profile_sample() noexcept {
    PhaseProfile& profile = g_profiles[static_cast<u32>(g_probe.phase)];
    // CPU-weighted: a thread that barely ran since the last tick was waiting.
    ULONG64 cycles = 0;
    QueryThreadCycleTime(g_main_thread, &cycles);
    const u64 ran = cycles - g_profile_last_cycles;
    g_profile_last_cycles = cycles;
    // Startup is sampled by wall clock (waits included) to show where the
    // main thread blocks; every other phase is CPU weighted.
    const bool wall = g_probe.phase == Phase::startup;
    if (ran < 200'000 && !wall)
        return;
    if (SuspendThread(g_main_thread) == static_cast<DWORD>(-1))
        return;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    u32 keys[24];
    u32 key_count = 0;
    bool top_external = false;
    bool charged_caller = false;
    if (GetThreadContext(g_main_thread, &context)) {
        ++profile.samples;
        for (u32 depth = 0; depth < 24 && context.Rip; ++depth) {
            const i32 module_index = profile_module_of(context.Rip);
            if (module_index < 0)
                break;
            const ProfileModule& module = g_profile_modules[module_index];
            const RUNTIME_FUNCTION* function = profile_function_of(module, context.Rip);
            u32 key;
            if (module.base == g_image_base)
                key = function ? profile_primary_begin(module, function) : static_cast<u32>(context.Rip - module.base);
            else
                key = 0x80000000u | module.stable_id;
            if (depth == 0) {
                // Parked right after a syscall: blocked in a wait or in the kernel.
                const u8* code = reinterpret_cast<const u8*>(context.Rip);
                if (code[-2] == 0x0F && code[-1] == 0x05) {
                    ++profile.in_syscall;
                    if (!wall)
                        break;
                }
                profile_count(profile, key, 1, 0, 0);
                top_external = module.base != g_image_base;
            } else if (top_external && !charged_caller && module.base == g_image_base) {
                profile_count(profile, key, 0, 0, 1);
                charged_caller = true;
            }
            bool seen = false;
            for (u32 k = 0; k < key_count; ++k)
                seen = seen || keys[k] == key;
            if (!seen && key_count < 24)
                keys[key_count++] = key;
            if (!function) {
                // Leaf: the return address is at the stack pointer.
                context.Rip = *reinterpret_cast<const u64*>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            void* handler_data = nullptr;
            u64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, module.base, context.Rip, const_cast<RUNTIME_FUNCTION*>(function),
                             &context, &handler_data, &establisher, nullptr);
        }
    } else {
        ++profile.unattributed;
    }
    ResumeThread(g_main_thread);
    for (u32 k = 0; k < key_count; ++k)
        profile_count(profile, keys[k], 0, 1, 0);
}

DWORD WINAPI sampler_thread(void*) noexcept {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        return 1;
    LARGE_INTEGER due{};
    due.QuadPart = -10'000;
    SetWaitableTimer(timer, &due, 1, nullptr, nullptr, FALSE);
    u32 ticks = 0;
    while (!g_sampler_stop) {
        WaitForSingleObject(timer, INFINITE);
        if ((ticks++ & 255) == 0)
            profile_refresh_modules();
        if (g_probe.phase < Phase::report)
            profile_sample();
    }
    CloseHandle(timer);
    return 0;
}

u32 profile_weight(const ProfileEntry& e, bool self) noexcept {
    return self ? e.self + e.caller_self : e.inclusive;
}

void report_profile(Phase phase) noexcept {
    if (!g_profiles)
        return;
    const PhaseProfile& profile = g_profiles[static_cast<u32>(phase)];
    if (!profile.samples)
        return;
    Out& o = g_out;
    o.s("profile phase=");
    o.s(kPhaseNames[static_cast<u32>(phase)]);
    o.kv("samples", profile.samples);
    o.kv("in_syscall", profile.in_syscall);
    o.s("\n");
    for (u32 pass = 0; pass < 2; ++pass) {
        static bool used[kProfileEntries];
        clear_bytes(used, sizeof(used));
        for (u32 rank = 0; rank < 30; ++rank) {
            u32 best = ~0u;
            for (u32 i = 0; i < kProfileEntries; ++i) {
                const ProfileEntry& e = profile.entries[i];
                if (used[i] || !e.key || !profile_weight(e, pass == 0))
                    continue;
                if (best == ~0u || profile_weight(e, pass == 0) > profile_weight(profile.entries[best], pass == 0))
                    best = i;
            }
            if (best == ~0u)
                break;
            used[best] = true;
            const ProfileEntry& e = profile.entries[best];
            o.s("hot phase=");
            o.s(kPhaseNames[static_cast<u32>(phase)]);
            o.s(pass == 0 ? " kind=self" : " kind=inclusive");
            o.kv("weight", profile_weight(e, pass == 0));
            o.kv("own", e.self);
            o.kv("callees", e.caller_self);
            o.kv("incl", e.inclusive);
            if (e.key & 0x80000000u) {
                o.s(" module=");
                const u32 index = e.key & 0x7FFFFFFFu;
                o.wide(index < g_profile_name_count ? g_profile_names[index].name : L"?");
            } else {
                o.s(" rva=");
                o.hex(e.key);
            }
            o.s("\n");
        }
    }
}

void report_threads() noexcept {
    for (u32 i = 0; i < g_thread_count; ++i) {
        const ThreadRow& row = g_threads[i];
        g_out.s("thread start=");
        if (row.module[0]) {
            g_out.wide(row.module);
            g_out.s("+");
            g_out.hex(row.start - row.module_base);
        } else {
            g_out.hex(row.start);
        }
        g_out.kvf("cpu_ms", cycles_to_us(row.cycles) / 1000.0);
        g_out.kv("ctxsw", row.context_switches);
        g_out.kv("idle_ctxsw", row.idle_switches);
        g_out.kv("alive_at_idle", row.seen_at_idle);
        g_out.kv("stack_reserve_kb", row.stack_reserved / 1024);
        g_out.kv("stack_commit_kb", row.stack_committed / 1024);
        g_out.s("\n");
    }
}

// AccountUiRuntime members by residency (private working set per member).
Out g_fields;
void report_account_ui_fields_text() noexcept {
    g_out.s(g_fields.text);
}
struct FieldSpan {
    const char* name;
    u64 offset, bytes;
};
const FieldSpan kAccountUiFields[] = {
    {"author_hydration", offsetof(app_detail::AccountUiRuntime, author_hydration), sizeof(app_detail::AccountUiRuntime::author_hydration)},
    {"private_conversation", offsetof(app_detail::AccountUiRuntime, private_conversation), sizeof(app_detail::AccountUiRuntime::private_conversation)},
    {"chat_state", offsetof(app_detail::AccountUiRuntime, chat_state), sizeof(app_detail::AccountUiRuntime::chat_state)},
    {"read_state_store", offsetof(app_detail::AccountUiRuntime, read_state_store), sizeof(app_detail::AccountUiRuntime::read_state_store)},
    {"read_state_bank", offsetof(app_detail::AccountUiRuntime, read_state_bank), sizeof(app_detail::AccountUiRuntime::read_state_bank)},
    {"role_storage", offsetof(app_detail::AccountUiRuntime, role_storage), sizeof(app_detail::AccountUiRuntime::role_storage)},
    {"overwrite_storage", offsetof(app_detail::AccountUiRuntime, overwrite_storage), sizeof(app_detail::AccountUiRuntime::overwrite_storage)},
    {"private_channel_store", offsetof(app_detail::AccountUiRuntime, private_channel_store), sizeof(app_detail::AccountUiRuntime::private_channel_store)},
    {"user_store", offsetof(app_detail::AccountUiRuntime, user_store), sizeof(app_detail::AccountUiRuntime::user_store)},
    {"guild_member_store", offsetof(app_detail::AccountUiRuntime, guild_member_store), sizeof(app_detail::AccountUiRuntime::guild_member_store)},
    {"thread_member_store", offsetof(app_detail::AccountUiRuntime, thread_member_store), sizeof(app_detail::AccountUiRuntime::thread_member_store)},
    {"thread_member_banks", offsetof(app_detail::AccountUiRuntime, thread_member_banks), sizeof(app_detail::AccountUiRuntime::thread_member_banks)},
    {"typing_store", offsetof(app_detail::AccountUiRuntime, typing_store), sizeof(app_detail::AccountUiRuntime::typing_store)},
    {"voice_states", offsetof(app_detail::AccountUiRuntime, voice_states), sizeof(app_detail::AccountUiRuntime::voice_states)},
    {"voice_session", offsetof(app_detail::AccountUiRuntime, voice_session), sizeof(app_detail::AccountUiRuntime::voice_session)},
    {"member_list", offsetof(app_detail::AccountUiRuntime, member_list), sizeof(app_detail::AccountUiRuntime::member_list)},
    {"navigation_view", offsetof(app_detail::AccountUiRuntime, navigation_view), sizeof(app_detail::AccountUiRuntime::navigation_view)},
    {"message_text", offsetof(app_detail::AccountUiRuntime, message_text), sizeof(app_detail::AccountUiRuntime::message_text)},
    {"text_measure_cache", offsetof(app_detail::AccountUiRuntime, text_measure_cache), sizeof(app_detail::AccountUiRuntime::text_measure_cache)},
    {"renderer_text", offsetof(app_detail::AccountUiRuntime, renderer_text), sizeof(app_detail::AccountUiRuntime::renderer_text)},
    {"message_rich_store", offsetof(app_detail::AccountUiRuntime, message_rich_store), sizeof(app_detail::AccountUiRuntime::message_rich_store)},
    {"message_poll_store", offsetof(app_detail::AccountUiRuntime, message_poll_store), sizeof(app_detail::AccountUiRuntime::message_poll_store)},
    {"message_poll_binding", offsetof(app_detail::AccountUiRuntime, message_poll_binding), sizeof(app_detail::AccountUiRuntime::message_poll_binding)},
    {"poll_gateway_serial", offsetof(app_detail::AccountUiRuntime, poll_gateway_serial), sizeof(app_detail::AccountUiRuntime::poll_gateway_serial)},
    {"message_history_poll_serial", offsetof(app_detail::AccountUiRuntime, message_history_poll_serial), sizeof(app_detail::AccountUiRuntime::message_history_poll_serial)},
    {"poll_source_sequences", offsetof(app_detail::AccountUiRuntime, poll_source_sequences), sizeof(app_detail::AccountUiRuntime::poll_source_sequences)},
    {"poll_source_sequence_mask", offsetof(app_detail::AccountUiRuntime, poll_source_sequence_mask), sizeof(app_detail::AccountUiRuntime::poll_source_sequence_mask)},
    {"media_atlas", offsetof(app_detail::AccountUiRuntime, media_atlas), sizeof(app_detail::AccountUiRuntime::media_atlas)},
    {"preview_media_atlas", offsetof(app_detail::AccountUiRuntime, preview_media_atlas), sizeof(app_detail::AccountUiRuntime::preview_media_atlas)},
    {"message_drafts", offsetof(app_detail::AccountUiRuntime, message_drafts), sizeof(app_detail::AccountUiRuntime::message_drafts)},
    {"reaction_preferences", offsetof(app_detail::AccountUiRuntime, reaction_preferences), sizeof(app_detail::AccountUiRuntime::reaction_preferences)},
    {"message_composer_controller", offsetof(app_detail::AccountUiRuntime, message_composer_controller), sizeof(app_detail::AccountUiRuntime::message_composer_controller)},
    {"guild_record_storage", offsetof(app_detail::AccountUiRuntime, guild_record_storage), sizeof(app_detail::AccountUiRuntime::guild_record_storage)},
    {"channel_record_storage", offsetof(app_detail::AccountUiRuntime, channel_record_storage), sizeof(app_detail::AccountUiRuntime::channel_record_storage)},
    {"emoji_record_storage", offsetof(app_detail::AccountUiRuntime, emoji_record_storage), sizeof(app_detail::AccountUiRuntime::emoji_record_storage)},
    {"emoji_role_storage", offsetof(app_detail::AccountUiRuntime, emoji_role_storage), sizeof(app_detail::AccountUiRuntime::emoji_role_storage)},
    {"user_record_storage", offsetof(app_detail::AccountUiRuntime, user_record_storage), sizeof(app_detail::AccountUiRuntime::user_record_storage)},
    {"guild_member_record_storage", offsetof(app_detail::AccountUiRuntime, guild_member_record_storage), sizeof(app_detail::AccountUiRuntime::guild_member_record_storage)},
    {"typing_record_storage", offsetof(app_detail::AccountUiRuntime, typing_record_storage), sizeof(app_detail::AccountUiRuntime::typing_record_storage)},
    {"voice_state_records", offsetof(app_detail::AccountUiRuntime, voice_state_records), sizeof(app_detail::AccountUiRuntime::voice_state_records)},
    {"guild_order_storage", offsetof(app_detail::AccountUiRuntime, guild_order_storage), sizeof(app_detail::AccountUiRuntime::guild_order_storage)},
    {"channel_order_storage", offsetof(app_detail::AccountUiRuntime, channel_order_storage), sizeof(app_detail::AccountUiRuntime::channel_order_storage)},
    {"user_order_storage", offsetof(app_detail::AccountUiRuntime, user_order_storage), sizeof(app_detail::AccountUiRuntime::user_order_storage)},
    {"guild_member_order_storage", offsetof(app_detail::AccountUiRuntime, guild_member_order_storage), sizeof(app_detail::AccountUiRuntime::guild_member_order_storage)},
    {"member_list_rows", offsetof(app_detail::AccountUiRuntime, member_list_rows), sizeof(app_detail::AccountUiRuntime::member_list_rows)},
    {"navigation_guild_rows", offsetof(app_detail::AccountUiRuntime, navigation_guild_rows), sizeof(app_detail::AccountUiRuntime::navigation_guild_rows)},
    {"navigation_channel_rows", offsetof(app_detail::AccountUiRuntime, navigation_channel_rows), sizeof(app_detail::AccountUiRuntime::navigation_channel_rows)},
    {"renderer_text_quads", offsetof(app_detail::AccountUiRuntime, renderer_text_quads), sizeof(app_detail::AccountUiRuntime::renderer_text_quads)},
    {"renderer_text_batches", offsetof(app_detail::AccountUiRuntime, renderer_text_batches), sizeof(app_detail::AccountUiRuntime::renderer_text_batches)},
    {"message_text_commands", offsetof(app_detail::AccountUiRuntime, message_text_commands), sizeof(app_detail::AccountUiRuntime::message_text_commands)},
    {"message_text_utf8", offsetof(app_detail::AccountUiRuntime, message_text_utf8), sizeof(app_detail::AccountUiRuntime::message_text_utf8)},
    {"message_text_styles", offsetof(app_detail::AccountUiRuntime, message_text_styles), sizeof(app_detail::AccountUiRuntime::message_text_styles)},
    {"message_text_semantics", offsetof(app_detail::AccountUiRuntime, message_text_semantics), sizeof(app_detail::AccountUiRuntime::message_text_semantics)},
    {"message_text_measurements", offsetof(app_detail::AccountUiRuntime, message_text_measurements), sizeof(app_detail::AccountUiRuntime::message_text_measurements)},
    {"message_text_measure_cache_records", offsetof(app_detail::AccountUiRuntime, message_text_measure_cache_records), sizeof(app_detail::AccountUiRuntime::message_text_measure_cache_records)},
    {"message_text_spoiler_reveals", offsetof(app_detail::AccountUiRuntime, message_text_spoiler_reveals), sizeof(app_detail::AccountUiRuntime::message_text_spoiler_reveals)},
    {"renderer_content_semantics", offsetof(app_detail::AccountUiRuntime, renderer_content_semantics), sizeof(app_detail::AccountUiRuntime::renderer_content_semantics)},
    {"renderer_content_hit_regions", offsetof(app_detail::AccountUiRuntime, renderer_content_hit_regions), sizeof(app_detail::AccountUiRuntime::renderer_content_hit_regions)},
    {"chat_state_name_storage", offsetof(app_detail::AccountUiRuntime, chat_state_name_storage), sizeof(app_detail::AccountUiRuntime::chat_state_name_storage)},
    {"user_name_storage", offsetof(app_detail::AccountUiRuntime, user_name_storage), sizeof(app_detail::AccountUiRuntime::user_name_storage)},
    {"event_payload_scratch", offsetof(app_detail::AccountUiRuntime, event_payload_scratch), sizeof(app_detail::AccountUiRuntime::event_payload_scratch)},
    {"guild_id_scratch", offsetof(app_detail::AccountUiRuntime, guild_id_scratch), sizeof(app_detail::AccountUiRuntime::guild_id_scratch)},
    {"message_bulk_ids", offsetof(app_detail::AccountUiRuntime, message_bulk_ids), sizeof(app_detail::AccountUiRuntime::message_bulk_ids)},
    {"response_shape_scratch", offsetof(app_detail::AccountUiRuntime, response_shape_scratch), sizeof(app_detail::AccountUiRuntime::response_shape_scratch)},
    {"navigation_sort_keys", offsetof(app_detail::AccountUiRuntime, navigation_sort_keys), sizeof(app_detail::AccountUiRuntime::navigation_sort_keys)},
    {"message_history_page_structurals", offsetof(app_detail::AccountUiRuntime, message_history_page_structurals), sizeof(app_detail::AccountUiRuntime::message_history_page_structurals)},
    {"profile_decode", offsetof(app_detail::AccountUiRuntime, profile_decode), sizeof(app_detail::AccountUiRuntime::profile_decode)},
    {"projection_scratch", offsetof(app_detail::AccountUiRuntime, projection_scratch), sizeof(app_detail::AccountUiRuntime::projection_scratch)},
    {"message_history_object_structurals", offsetof(app_detail::AccountUiRuntime, message_history_object_structurals), sizeof(app_detail::AccountUiRuntime::message_history_object_structurals)},
    {"history_decode_storage", offsetof(app_detail::AccountUiRuntime, history_decode_storage), sizeof(app_detail::AccountUiRuntime::history_decode_storage)},
    {"message_rich_record_storage", offsetof(app_detail::AccountUiRuntime, message_rich_record_storage), sizeof(app_detail::AccountUiRuntime::message_rich_record_storage)},
    {"message_rich_order", offsetof(app_detail::AccountUiRuntime, message_rich_order), sizeof(app_detail::AccountUiRuntime::message_rich_order)},
    {"message_rich_text", offsetof(app_detail::AccountUiRuntime, message_rich_text), sizeof(app_detail::AccountUiRuntime::message_rich_text)},
    {"media_atlas_entries", offsetof(app_detail::AccountUiRuntime, media_atlas_entries), sizeof(app_detail::AccountUiRuntime::media_atlas_entries)},
    {"media_atlas_buckets", offsetof(app_detail::AccountUiRuntime, media_atlas_buckets), sizeof(app_detail::AccountUiRuntime::media_atlas_buckets)},
    {"media_atlas_allocations", offsetof(app_detail::AccountUiRuntime, media_atlas_allocations), sizeof(app_detail::AccountUiRuntime::media_atlas_allocations)},
    {"media_atlas_rows", offsetof(app_detail::AccountUiRuntime, media_atlas_rows), sizeof(app_detail::AccountUiRuntime::media_atlas_rows)},
    {"preview_media_atlas_entries", offsetof(app_detail::AccountUiRuntime, preview_media_atlas_entries), sizeof(app_detail::AccountUiRuntime::preview_media_atlas_entries)},
    {"preview_media_atlas_buckets", offsetof(app_detail::AccountUiRuntime, preview_media_atlas_buckets), sizeof(app_detail::AccountUiRuntime::preview_media_atlas_buckets)},
    {"preview_media_atlas_allocations", offsetof(app_detail::AccountUiRuntime, preview_media_atlas_allocations), sizeof(app_detail::AccountUiRuntime::preview_media_atlas_allocations)},
    {"preview_media_atlas_rows", offsetof(app_detail::AccountUiRuntime, preview_media_atlas_rows), sizeof(app_detail::AccountUiRuntime::preview_media_atlas_rows)},
    {"message_draft_records", offsetof(app_detail::AccountUiRuntime, message_draft_records), sizeof(app_detail::AccountUiRuntime::message_draft_records)},
    {"message_draft_text", offsetof(app_detail::AccountUiRuntime, message_draft_text), sizeof(app_detail::AccountUiRuntime::message_draft_text)},
    {"message_request_text", offsetof(app_detail::AccountUiRuntime, message_request_text), sizeof(app_detail::AccountUiRuntime::message_request_text)},
    {"message_draft_session", offsetof(app_detail::AccountUiRuntime, message_draft_session), sizeof(app_detail::AccountUiRuntime::message_draft_session)},
    {"active_channel_id", offsetof(app_detail::AccountUiRuntime, active_channel_id), sizeof(app_detail::AccountUiRuntime::active_channel_id)},
    {"message_history_request_id", offsetof(app_detail::AccountUiRuntime, message_history_request_id), sizeof(app_detail::AccountUiRuntime::message_history_request_id)},
    {"observed_selection_revision", offsetof(app_detail::AccountUiRuntime, observed_selection_revision), sizeof(app_detail::AccountUiRuntime::observed_selection_revision)},
    {"rendered_navigation_revision", offsetof(app_detail::AccountUiRuntime, rendered_navigation_revision), sizeof(app_detail::AccountUiRuntime::rendered_navigation_revision)},
    {"rendered_message_created", offsetof(app_detail::AccountUiRuntime, rendered_message_created), sizeof(app_detail::AccountUiRuntime::rendered_message_created)},
    {"rendered_message_updated", offsetof(app_detail::AccountUiRuntime, rendered_message_updated), sizeof(app_detail::AccountUiRuntime::rendered_message_updated)},
    {"rendered_message_deleted", offsetof(app_detail::AccountUiRuntime, rendered_message_deleted), sizeof(app_detail::AccountUiRuntime::rendered_message_deleted)},
    {"rendered_user_revision", offsetof(app_detail::AccountUiRuntime, rendered_user_revision), sizeof(app_detail::AccountUiRuntime::rendered_user_revision)},
    {"rendered_member_revision", offsetof(app_detail::AccountUiRuntime, rendered_member_revision), sizeof(app_detail::AccountUiRuntime::rendered_member_revision)},
    {"rendered_private_revision", offsetof(app_detail::AccountUiRuntime, rendered_private_revision), sizeof(app_detail::AccountUiRuntime::rendered_private_revision)},
    {"rendered_emoji_mutations", offsetof(app_detail::AccountUiRuntime, rendered_emoji_mutations), sizeof(app_detail::AccountUiRuntime::rendered_emoji_mutations)},
    {"rendered_typing_revision", offsetof(app_detail::AccountUiRuntime, rendered_typing_revision), sizeof(app_detail::AccountUiRuntime::rendered_typing_revision)},
    {"rendered_permission_mutations", offsetof(app_detail::AccountUiRuntime, rendered_permission_mutations), sizeof(app_detail::AccountUiRuntime::rendered_permission_mutations)},
    {"rendered_composer_revision", offsetof(app_detail::AccountUiRuntime, rendered_composer_revision), sizeof(app_detail::AccountUiRuntime::rendered_composer_revision)},
    {"rendered_channel_id", offsetof(app_detail::AccountUiRuntime, rendered_channel_id), sizeof(app_detail::AccountUiRuntime::rendered_channel_id)},
    {"text_rebuild_retry_at_ms", offsetof(app_detail::AccountUiRuntime, text_rebuild_retry_at_ms), sizeof(app_detail::AccountUiRuntime::text_rebuild_retry_at_ms)},
    {"text_rebuild_failures", offsetof(app_detail::AccountUiRuntime, text_rebuild_failures), sizeof(app_detail::AccountUiRuntime::text_rebuild_failures)},
    {"rendered_poll_revision", offsetof(app_detail::AccountUiRuntime, rendered_poll_revision), sizeof(app_detail::AccountUiRuntime::rendered_poll_revision)},
};

void report_account_ui_fields() noexcept {
    if (!g_app.account_ui)
        return;
    const u64 base = reinterpret_cast<u64>(g_app.account_ui);
    const u64 total = sizeof(app_detail::AccountUiRuntime);
    const u32 pages = static_cast<u32>((total + 4095) / 4096 + 1);
    static PSAPI_WORKING_SET_EX_INFORMATION info[1100];
    if (pages > 1100)
        return;
    const u64 first = base & ~4095ull;
    for (u32 i = 0; i < pages; ++i)
        info[i].VirtualAddress = reinterpret_cast<void*>(first + u64{i} * 4096);
    if (!K32QueryWorkingSetEx(GetCurrentProcess(), info, pages * sizeof(info[0])))
        return;
    for (const FieldSpan& field : kAccountUiFields) {
        if (field.bytes < 16 * 1024)
            continue;
        u64 resident = 0;
        const u64 begin = base + field.offset, end = begin + field.bytes;
        for (u64 page = begin & ~4095ull; page < end; page += 4096) {
            const u32 index = static_cast<u32>((page - first) / 4096);
            if (index < pages && info[index].VirtualAttributes.Valid) {
                const u64 lo = page > begin ? page : begin;
                const u64 hi = page + 4096 < end ? page + 4096 : end;
                resident += hi - lo;
            }
        }
        g_fields.s("ui_field name=");
        g_fields.s(field.name);
        g_fields.kv("kb", field.bytes / 1024);
        g_fields.kv("resident_kb", resident / 1024);
        g_fields.s("\n");
    }
}

void write_stdout(const char* text, u32 bytes) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, bytes, &written, nullptr);
}

bool profile_isolated() noexcept {
    wchar_t local[1024]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 1000);
    if (!count || count >= 1000)
        return false;
    constexpr wchar_t marker[] = L"\\.maicord-test-profile";
    for (u32 i = 0; marker[i]; ++i)
        local[count + i] = marker[i];
    const DWORD attributes = GetFileAttributesW(local);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

bool probe_initialize() noexcept {
    // Never touch a real profile: the build runner's temporary profile marks itself.
    if (!profile_isolated()) {
        constexpr char text[] = "perf probe: LOCALAPPDATA is not an isolated test profile\n";
        write_stdout(text, sizeof(text) - 1);
        return false;
    }
    g_virtual_alloc = VirtualAlloc;
    g_virtual_free = VirtualFree;
    g_heap_alloc = HeapAlloc;
    g_heap_realloc = HeapReAlloc;
    g_heap_free = HeapFree;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_probe.qpc_frequency = static_cast<u64>(frequency.QuadPart);
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    g_probe.process_create_100ns = (static_cast<u64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    g_main_thread_id = GetCurrentThreadId();
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_query_system = reinterpret_cast<NtQuerySystemInformationFunction>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
    g_query_thread = reinterpret_cast<NtQueryInformationThreadFunction>(GetProcAddress(ntdll, "NtQueryInformationThread"));
    // Probe-owned buffers (excluded from client attribution).
    struct Buffer {
        void** target;
        u64 bytes;
    };
    Buffer buffers[] = {
        {reinterpret_cast<void**>(&g_spi_buffer), kSpiBytes},
        {reinterpret_cast<void**>(&g_tracked), sizeof(TrackedRegion) * u64{kTrackedCapacity}},
        {reinterpret_cast<void**>(&g_frame_cycles), sizeof(u64) * u64{kFrameSampleCapacity}},
        {reinterpret_cast<void**>(&g_heap_ranges), sizeof(Range) * u64{kRangeCapacity} * 2},
        {reinterpret_cast<void**>(&g_snapshots[0]), sizeof(MemorySnapshot) * 3},
    };
    for (Buffer& buffer : buffers) {
        *buffer.target = probe_alloc(buffer.bytes);
        if (!*buffer.target)
            return false;
        const u64 begin = reinterpret_cast<u64>(*buffer.target);
        g_probe_ranges[g_probe_range_count++] = {begin, begin + ((buffer.bytes + 65535) & ~65535ull)};
    }
    g_stack_ranges = g_heap_ranges + kRangeCapacity;
    g_snapshots[1] = g_snapshots[0] + 1;
    g_snapshots[2] = g_snapshots[0] + 2;
    trace("buffers");
    g_probe.hooks = install_import_hooks();
    trace("hooks");
    wchar_t profile_flag[2]{};
    if (GetEnvironmentVariableW(L"MAICORD_PERF_PROFILE", profile_flag, 2) == 1 && profile_flag[0] == L'1') {
        const u64 bytes = sizeof(PhaseProfile) * static_cast<u64>(Phase::count);
        g_profiles = static_cast<PhaseProfile*>(probe_alloc(bytes));
        if (g_profiles && DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                                          &g_main_thread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                          THREAD_QUERY_INFORMATION, FALSE, 0)) {
            const u64 begin = reinterpret_cast<u64>(g_profiles);
            g_probe_ranges[g_probe_range_count++] = {begin, begin + ((bytes + 65535) & ~65535ull)};
            profile_refresh_modules();
            g_sampler = CreateThread(nullptr, 64 * 1024, sampler_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                     nullptr);
            if (g_sampler)
                SetThreadPriority(g_sampler, THREAD_PRIORITY_TIME_CRITICAL);
            trace("profiler");
        }
    }
    // The synthetic account starts only when a saved profile file exists.
    AccountPaths paths{};
    if (!account_paths_resolve(&paths).ok())
        return false;
    CreateDirectoryW(paths.application_directory, nullptr);
    CreateDirectoryW(paths.account_directory, nullptr);
    HANDLE profile = CreateFileW(paths.profile, GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (profile == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(profile);
    g_probe.lab_guild = perf_guild_id(0);
    g_probe.media_channel = perf_lab_text_channel(0);
    g_probe.text_channel = perf_lab_text_channel(1);
    g_tick_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (g_tick_timer)
        g_tick_thread = CreateThread(nullptr, 64 * 1024, probe_ticker, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION,
                                     nullptr);
    begin_phase(Phase::startup);
    g_probe.initialized = true;
    return true;
}

// Every account deadline the scheduler folds into its next wake.
void dump_account_deadlines() noexcept {
    using namespace app_detail;
    const u64 now_ms = clock_monotonic_milliseconds();
    auto relative = [now_ms](u64 at) noexcept -> u64 { return at ? (at > now_ms ? at - now_ms : 0) + 1 : 0; };
    if (!g_app.account_ui)
        return;
    static Out line;
    line.used = 0;
    line.s("trace deadlines");
    line.kv("profile_next_wake", relative(application_profile_next_wake()));
    line.kv("commands_next_wake", relative(application_commands_next_wake()));
    line.kv("friends_next_wake", relative(application_friends_next_wake()));
    line.kv("forum_preview_next_wake", relative(application_forum_preview_next_wake()));
    line.kv("composer_preview_next_wake", relative(application_composer_preview_next_wake()));
    line.kv("composer_spellcheck_next_wake_composer_s", relative(composer_spellcheck_next_wake(g_app.composer_spellcheck)));
    line.kv("search_next_wake", relative(application_search_next_wake()));
    line.kv("pinned_messages_next_wake", relative(application_pinned_messages_next_wake()));
    line.kv("thread_browser_next_wake", relative(application_thread_browser_next_wake()));
    line.kv("thread_creation_next_wake", relative(application_thread_creation_next_wake()));
    line.kv("forum_post_next_wake", relative(application_forum_post_next_wake()));
    line.kv("poll_next_wake", relative(application_poll_next_wake()));
    line.kv("member_moderation_next_wake", relative(application_member_moderation_next_wake()));
    line.kv("account_settings_next_wake", relative(application_account_settings_next_wake()));
    line.kv("media_picker_next_wake", relative(application_media_picker_next_wake()));
    line.kv("gif_favorites_next_wake", relative(application_gif_favorites_next_wake()));
    line.kv("emoji_preferences_next_wake", relative(application_emoji_preferences_next_wake()));
    line.kv("action_captcha_next_wake", relative(application_action_captcha_next_wake()));
    line.kv("self_status_next_wake", relative(application_self_status_next_wake()));
    line.kv("user_settings_next_wake", relative(application_user_settings_next_wake()));
    line.kv("guild_folders_request_wake", relative(application_guild_folders_request_wake()));
    line.kv("custom_status_next_wake", relative(application_custom_status_next_wake()));
    line.kv("activity_next_wake", relative(application_activity_next_wake()));
    line.kv("notification_controls_next_wake", relative(application_notification_controls_next_wake()));
    line.kv("inbox_next_wake", relative(application_inbox_next_wake()));
    line.kv("stream_preview_next_wake", relative(application_stream_preview_next_wake()));
    line.kv("stage_next_wake_details_request_owner_Me", relative(application_stage_next_wake(g_app.details_request_owner == MessageHistoryRequestOwner::none)));
    line.kv("soundboard_next_wake_details_request_own", relative(application_soundboard_next_wake(g_app.details_request_owner == MessageHistoryRequestOwner::none)));
    line.kv("message_components_next_wake", relative(application_message_components_next_wake()));
    line.kv("invite_cards_next_wake", relative(application_invite_cards_next_wake()));
    line.kv("channel_overview_next_wake", relative(application_channel_overview_next_wake()));
    line.kv("read_state_next_wake", relative(application_read_state_next_wake()));
    line.kv("scoped_read_next_wake", relative(application_scoped_read_next_wake()));
    line.kv("notifications_next_wake_g_app", relative(application_notifications_next_wake(&g_app)));
    line.kv("voice_next_wake", relative(application_voice_next_wake()));
    line.kv("presence_next_wake", relative(application_presence_next_wake()));
    line.kv("media_url_next_wake_media_urls", relative(media_url_next_wake(g_app.media_urls)));
    line.kv("account_text_retry_wake", relative(application_account_text_retry_wake()));
    line.kv("private_conversation_retry", relative(g_app.account_ui->private_conversation.retry_at_ms));
    line.kv("chat_state_restore_deadline_ms", relative(g_app.account_ui->chat_state.restore_deadline_ms));
    line.kv("message_draft_session_persist_at_ms", relative(g_app.account_ui->message_draft_session.persist_at_ms));
    line.kv("message_composer_controller_next_wake_at", relative(message_composer_controller_next_wake_at( g_app.account_ui->message_composer_controller)));
    line.kv("typing_store_next_wake_at_typing_store", relative(typing_store_next_wake_at(g_app.account_ui->typing_store)));
    line.kv("message_catchup_retry_at_ms", relative(g_app.message_catchup.retry_at_ms));
    line.kv("message_older_retry_at_ms", relative(g_app.message_older.retry_at_ms));
    line.kv("attachment_refresh_retry_at_ms", relative(g_app.attachment_refresh.retry_at_ms));
    line.kv("reply_refresh_retry_at_ms", relative(g_app.reply_refresh.retry_at_ms));
    line.kv("message_delete_retry_at_ms", relative(g_app.message_delete.retry_at_ms));
    line.kv("message_edit_retry_at_ms", relative(g_app.message_edit.retry_at_ms));
    line.kv("message_reaction_next_wake", relative(application_message_reaction_next_wake()));
    line.kv("thread_actions_next_wake", relative(application_thread_actions_next_wake()));
    line.kv("channel_prefetch_next_wake", relative(application_channel_prefetch_next_wake()));
    line.kv("member_list_subscription_retry_at_ms", relative(g_app.member_list_subscription.retry_at_ms));
    line.kv("author_hydration_next_wake", relative(application_author_hydration_next_wake()));
    line.kv("thread_member_list_subscription_retry_at", relative(g_app.thread_member_list_subscription.retry_at_ms));
    line.kv("reaction_viewer_controller_next_wake_at", relative(reaction_viewer_controller_next_wake_at(g_app.reaction_viewer_controller)));
    line.kv("message_media_retry_at_ms", relative(g_app.message_media.retry_at_ms));
    line.s("\n");
    write_stdout(line.text, line.used);
}

// Names the deadline that keeps the loop polling when a phase spins.
void dump_spin_state() noexcept {
    const u64 now_ms = clock_monotonic_milliseconds();
    const u64 now_us = clock_microseconds(g_app.clock, g_app.process_start, clock_now());
    auto relative = [now_ms](u64 at) noexcept -> u64 { return at ? (at > now_ms ? at - now_ms : 0) + 1 : 0; };
    static Out line;
    line.used = 0;
    line.s("trace spin");
    line.kv("frame_pending", g_app.frame_pending);
    line.kv("scan_pending", g_app.message_media.scan_pending);
    line.kv("media_retry", relative(g_app.message_media.retry_at_ms));
    line.kv("preview_retry", relative(g_app.message_media.preview_retry_at_ms));
    line.kv("account_wake", relative(g_app.account.status.next_wake_at_ms));
    line.kv("media_trim", relative(renderer_media_next_trim_ms(g_app.renderer)));
    line.kv("timeline_ms", animation_timeline_wait_timeout_ms(&g_app.animations.timeline, now_us));
    line.kv("text_effect", g_app.renderer.text_effect_animated);
    line.kv("text_effect_next", relative(g_app.renderer.text_effect_next_ms));
    line.kv("spinner", relative(app_detail::application_loading_spinner_wake()));
    line.kv("video", relative(app_detail::application_video_next_wake()));
    line.kv("tooltip", relative(app_detail::application_tooltip_next_wake()));
    line.kv("surface", relative(app_detail::application_surface_motion_next_wake(now_ms)));
    line.kv("background", relative(app_detail::application_background_next_wake(now_ms)));
    line.kv("presence", relative(app_detail::application_presence_next_wake()));
    line.kv("smooth", app_detail::application_smooth_scroll_active());
    line.kv("nonvisual_due", app_detail::application_nonvisual_wake_due(now_ms));
    line.kv("visual_requested", app_detail::application_visual_frame_requested(now_ms, now_us));
    line.kv("permit", g_app.renderer.begin_frame_permit);
    line.kv("rate_next", g_app.frame_rate.next_update_us ? 1 : 0);
    line.s("\n");
    write_stdout(line.text, line.used);
    dump_account_deadlines();
}

void probe_observe_wait(u32 timeout_ms, bool timeout, bool frame_signal, bool message, bool handle) noexcept {
    if (!g_probe.initialized)
        return;
    PhaseStats& stats = g_phases[static_cast<u32>(g_probe.phase)];
    ++stats.wakes;
    if (timeout_ms == 0 && timeout) {
        ++stats.zero_timeouts;
        if (++g_probe.zero_streak == 2'000 && !stats.spin_dumped) {
            stats.spin_dumped = true;
            dump_spin_state();
        }
    } else {
        g_probe.zero_streak = 0;
    }
    if (timeout)
        ++stats.wake_timeouts;
    else if (frame_signal)
        ++stats.wake_frames;
    else if (message)
        ++stats.wake_messages;
    else if (handle)
        ++stats.wake_handles;
}

// The scenario advances only when the client's loop wakes. An idle client
// (no animation, no timers) can sleep indefinitely, so the probe arms one
// timer for its own next deadline (a phase end or the next scripted input)
// and posts a single WM_NULL then; wakes the client makes itself re-arm it
// without extra messages.
u64 phase_deadline_qpc() noexcept {
    const u64 start = g_probe.phase_start_qpc;
    const auto at = [&](u64 ms) noexcept { return start + ms * g_probe.qpc_frequency / 1000; };
    const auto action = [&](u32 limit, u64 end_ms) noexcept {
        return g_probe.actions < limit ? g_probe.next_action_qpc : at(end_ms);
    };
    switch (g_probe.phase) {
    case Phase::startup: return at(30'000);
    case Phase::history_wait: return g_probe.history_100ns ? at(1'500) : at(15'000);
    case Phase::idle_static: return at(4'000);
    case Phase::scroll_timeline: return action(90, 5'500);
    case Phase::channel_switch: return action(24, 6'500);
    case Phase::member_list: return action(60, 4'000);
    case Phase::churn: return at(5'000);
    case Phase::settings: {
        const u64 next = g_probe.actions < 6 ? g_probe.next_action_qpc : at(3'800);
        return next > at(600) ? next : at(600);
    }
    case Phase::idle_animated: return at(3'000);
    case Phase::idle_final: return at(4'000);
    case Phase::idle_hidden: return at(4'000);
    default: return 0;
    }
}

void arm_phase_tick() noexcept {
    const u64 deadline = phase_deadline_qpc();
    if (!g_tick_timer || !deadline)
        return;
    const u64 now = qpc_now();
    // Relative due time in 100 ns units, at least 1 ms past the deadline.
    const u64 delay_us = deadline > now ? (deadline - now) * 1'000'000 / g_probe.qpc_frequency : 0;
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>((delay_us + 1'000) * 10);
    SetWaitableTimer(g_tick_timer, &due, 0, nullptr, nullptr, FALSE);
}

bool probe_step_phases(int* exit_code) noexcept;
bool probe_step(int* exit_code) noexcept {
    const bool finished = probe_step_phases(exit_code);
    if (finished || g_probe.phase == Phase::done) {
        if (g_tick_thread) {
            InterlockedExchange(&g_tick_stop, 1);
            LARGE_INTEGER now{};
            SetWaitableTimer(g_tick_timer, &now, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(g_tick_thread, INFINITE);
            CloseHandle(g_tick_thread);
            g_tick_thread = nullptr;
        }
    } else {
        arm_phase_tick();
    }
    return finished;
}

bool probe_step_phases(int* exit_code) noexcept {
    if (!g_probe.initialized)
        return false;
    ++g_probe.step_counter;
    // Presents since the last turn: record the main-thread cycles spent per frame.
    const u64 presents = static_cast<u64>(g_app.renderer.full_present_draws) + g_app.renderer.partial_present_draws;
    PhaseStats& stats = g_phases[static_cast<u32>(g_probe.phase)];
    if (presents != g_probe.present_counter) {
        ULONG64 cycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        cycles -= static_cast<u64>(g_replay.cycles);
        if (g_probe.present_counter == 0 && !g_probe.first_present_100ns)
            g_probe.first_present_100ns = now_100ns();
        if (g_frame_cycle_count < kFrameSampleCapacity && g_probe.last_present_cycles) {
            g_frame_cycles[g_frame_cycle_count++] = cycles - g_probe.last_present_cycles;
            ++stats.frame_count;
        }
        stats.presents += presents - g_probe.present_counter;
        const u64 now_ms = clock_monotonic_milliseconds();
        stats.next_pending += g_app.frame_pending;
        stats.next_timeline += animation_timeline_next_wake_at_us(&g_app.animations.timeline) != 0;
        stats.next_effect += g_app.renderer.text_effect_animated ||
                             profile_scene_animating(g_app.renderer.profile_scene, now_ms);
        stats.next_spinner += app_detail::application_loading_spinner_wake() != 0;
        stats.next_video += app_detail::application_video_next_wake() != 0;
        stats.next_tooltip += app_detail::application_tooltip_next_wake() != 0;
        stats.next_surface += app_detail::application_surface_motion_next_wake(now_ms) != 0;
        stats.next_background += app_detail::application_background_next_wake(now_ms) != 0;
        stats.next_smooth += app_detail::application_smooth_scroll_active();
        stats.next_caret += g_app.message_composer_caret_blink_at_ms != 0;
        stats.next_interaction += app_detail::application_message_interaction_next_wake(now_ms) != 0;
        stats.next_timestamp +=
            g_app.account_ui && g_app.account_ui->message_text.next_timestamp_update_unix_seconds != 0;
        stats.draw_calls += g_app.renderer.draw_calls_last_frame;
        g_probe.last_present_cycles = cycles;
        g_probe.present_counter = presents;
        if (g_probe.context_hooks && g_app.renderer.has_presented && !g_app.renderer.login_visible) {
            const bool full = g_app.renderer.full_present_draws != g_replay.full_presents;
            g_replay.full_presents = g_app.renderer.full_present_draws;
            gpu_replay(full);
        }
    }
    if (!g_probe.context_hooks && g_app.renderer.graphics.context) {
        // Count context calls; frames are timed by gpu_replay.
        g_probe.context_hooks = install_context_hooks(g_app.renderer.graphics.context, false);
        if (static_cast<void*>(g_app.renderer.graphics.context1) == static_cast<void*>(g_app.renderer.graphics.context))
            g_probe.context_hooks += install_context_hooks(g_app.renderer.graphics.context1, true);
        if (!g_probe.context_hooks)
            g_probe.context_hooks = ~0u;
    }
    if (!g_probe.control && g_app.account.module) {
        trace("module");
        g_probe.control = reinterpret_cast<PerfAccountControlFunction>(
            GetProcAddress(g_app.account.module, kPerfAccountControlExport));
        g_probe.counters = reinterpret_cast<PerfAccountCountersFunction>(
            GetProcAddress(g_app.account.module, kPerfAccountCountersExport));
    }
    const u64 phase_ms = elapsed_ms(g_probe.phase_start_qpc);
    constexpr u64 kStartupTimeoutMs = 30'000;
    switch (g_probe.phase) {
    case Phase::startup:
        if (shell_ready() && g_probe.first_present_100ns) {
            g_probe.ready_100ns = now_100ns();
            if (!select_channel(g_probe.lab_guild, g_probe.media_channel))
                fail("select_media_channel");
            next_phase(Phase::history_wait);
        } else if (phase_ms > kStartupTimeoutMs) {
            fail(g_probe.first_present_100ns ? "ready_timeout" : "first_present_timeout");
            next_phase(Phase::report);
        }
        break;
    case Phase::history_wait:
        // History applied and visible media decoded; then settle 1.5 s.
        if (!g_probe.history_100ns && g_app.message_view.store.count >= 20)
            g_probe.history_100ns = now_100ns();
        if (g_probe.history_100ns && phase_ms > 1'500) {
            take_snapshot(0);
            if (!select_channel(g_probe.lab_guild, g_probe.text_channel))
                fail("select_text_channel");
            next_phase(Phase::idle_static);
        } else if (phase_ms > 15'000) {
            fail("history_timeout");
            next_phase(Phase::report);
        }
        break;
    case Phase::idle_static:
        // Text-only channel: nothing animates, nothing arrives.
        if (phase_ms > 4'000) {
            if (!select_channel(g_probe.lab_guild, g_probe.media_channel))
                fail("select_media_channel_scroll");
            next_phase(Phase::scroll_timeline);
        }
        break;
    case Phase::scroll_timeline:
        // 60 notches up (older history loads on the way), then 30 down.
        if (g_probe.actions < 90 && due(45))
            post_wheel(message_x(), client_height_dip() * 0.5f, g_probe.actions <= 60 ? WHEEL_DELTA : -WHEEL_DELTA);
        if (g_probe.actions >= 90 && phase_ms > 5'500) {
            take_snapshot(1);
            next_phase(Phase::channel_switch);
        }
        break;
    case Phase::channel_switch:
        if (g_probe.actions < 24 && due(250)) {
            const u32 index = 2 + g_probe.actions % (kPerfLabCategories * kPerfLabTextPerCategory - 2);
            if (!select_channel(g_probe.lab_guild, perf_lab_text_channel(index)))
                fail("channel_switch");
        }
        if (g_probe.actions >= 24 && phase_ms > 6'500) {
            if (!select_channel(g_probe.lab_guild, g_probe.media_channel))
                fail("select_media_channel_members");
            next_phase(Phase::member_list);
        }
        break;
    case Phase::member_list:
        if (g_probe.actions < 60 && due(50))
            post_wheel(member_x(), client_height_dip() * 0.5f, g_probe.actions < 45 ? -WHEEL_DELTA : WHEEL_DELTA);
        if (g_probe.actions >= 60 && phase_ms > 4'000) {
            next_phase(Phase::churn);
            if (g_probe.control) {
                PerfAccountControl control{};
                control.command = PerfAccountCommand::start_churn;
                control.typing_per_second = 8;
                control.presence_per_second = 40;
                control.messages_per_second = 2;
                control.channel_id = g_probe.media_channel;
                control.guild_id = g_probe.lab_guild;
                if (!g_probe.control(&control))
                    fail("start_churn");
            }
        }
        break;
    case Phase::churn:
        if (phase_ms > 5'000) {
            if (g_probe.control) {
                PerfAccountControl control{};
                control.command = PerfAccountCommand::stop_churn;
                g_probe.control(&control);
            }
            next_phase(Phase::settings);
            if (!app_detail::application_settings_open())
                fail("settings_open");
        }
        break;
    case Phase::settings: {
        static const char* const pages[] = {"appearance", "notifications", "accessibility", "voice", "keybinds",
                                            "content.dms"};
        if (g_probe.actions < 6 && phase_ms > 600 && due(450)) {
            const char* key = pages[g_probe.actions - 1];
            u32 bytes = 0;
            while (key[bytes])
                ++bytes;
            app_detail::application_settings_open_key({key, bytes});
        }
        if (g_probe.actions >= 6 && phase_ms > 3'800) {
            app_detail::application_settings_close(false);
            next_phase(Phase::idle_animated);
        }
        break;
    }
    case Phase::idle_animated:
        // The media channel with visible animations: the steady animation cost.
        if (phase_ms > 3'000) {
            if (!select_channel(g_probe.lab_guild, g_probe.text_channel))
                fail("select_text_channel_final");
            next_phase(Phase::idle_final);
            collect_threads(true);
        }
        break;
    case Phase::idle_final:
        if (phase_ms > 4'000) {
            // End-of-session memory before minimizing (which may trim the working set).
            end_phase();
            RECT client{};
            GetClientRect(g_app.window, &client);
            g_probe.client_width = static_cast<u32>(client.right);
            g_probe.client_height = static_cast<u32>(client.bottom);
            take_snapshot(2);
            collect_threads(false);
            report_account_ui_fields();
            begin_phase(Phase::idle_hidden);
            ShowWindow(g_app.window, SW_SHOWMINNOACTIVE);
            // A minimized client may not wake for many seconds; the phase
            // tick posts one message at its end (counted as one message wake).
        }
        break;
    case Phase::idle_hidden:
        if (phase_ms > 4'000)
            next_phase(Phase::report);
        break;
    case Phase::report: {
        end_phase();
        if (g_sampler) {
            InterlockedExchange(&g_sampler_stop, 1);
            WaitForSingleObject(g_sampler, INFINITE);
            CloseHandle(g_sampler);
            g_sampler = nullptr;
        }
        g_probe.final_messages = g_app.message_view.store.count;
        if (g_app.account_ui) {
            g_probe.final_guilds = g_app.account_ui->chat_state.guild_count;
            g_probe.final_channels = g_app.account_ui->chat_state.channel_count;
            g_probe.final_members = g_app.account_ui->member_list.count;
        }
        if (g_probe.counters)
            g_probe.counters(&g_probe.module_counters);
        g_probe.phase = Phase::done;
        *exit_code = g_probe.failed ? 90 : 0;
        return true;
    }
    default:
        return true;
    }
    return false;
}

void probe_finish(int exit_code) noexcept {
    if (!g_probe.initialized)
        return;
    calibrate_cycles();
    Out& o = g_out;
    o.s("MAICORD_PERF v1");
    o.kv("exit", static_cast<u64>(exit_code));
    o.kv("hooks", g_probe.hooks);
    o.kv("context_hooks", g_probe.context_hooks);
    o.kv("dpi", g_app.renderer.dpi);
    o.kv("width", g_probe.client_width);
    o.kv("height", g_probe.client_height);
    o.kv("messages", g_probe.final_messages);
    o.kv("guilds", g_probe.final_guilds);
    o.kv("channels", g_probe.final_channels);
    o.kv("members", g_probe.final_members);
    o.kvf("tsc_mhz", cycles_per_us);
    if (g_probe.failed) {
        o.s(" failure=");
        o.s(g_probe.failure);
    }
    o.s("\n");
    const auto ms_since_create = [](u64 at) noexcept -> u64 {
        return at > g_probe.process_create_100ns ? (at - g_probe.process_create_100ns) / 10'000 : 0;
    };
    o.s("startup");
    o.kv("first_present_ms", ms_since_create(g_probe.first_present_100ns));
    o.kv("ready_ms", ms_since_create(g_probe.ready_100ns));
    o.kv("history_ms", ms_since_create(g_probe.history_100ns));
    o.s("\n");
    for (u32 p = 0; p < static_cast<u32>(Phase::report); ++p)
        report_phase(static_cast<Phase>(p));
    for (u32 s = 0; s < 3; ++s)
        report_snapshot(s);
    if (g_snapshots[2] && g_snapshots[2]->valid) {
        report_modules(*g_snapshots[2]);
        report_sites(*g_snapshots[2]);
    }
    report_threads();
    report_account_ui_fields_text();
    for (u32 p = 0; p < static_cast<u32>(Phase::report); ++p)
        report_profile(static_cast<Phase>(p));
    const PerfAccountCounters& m = g_probe.module_counters;
    o.s("synthetic");
    o.kv("ready_json_kb", m.ready_json_bytes / 1024);
    o.kv("ready_events", m.ready_events);
    o.kv("gateway_events", m.gateway_events);
    o.kv("gateway_payload_kb", m.gateway_payload_bytes / 1024);
    o.kv("rest_responses", m.rest_responses);
    o.kv("history_messages", m.history_messages);
    o.kv("media_frames", m.media_frames);
    o.kv("media_pixel_kb", m.media_pixel_bytes / 1024);
    o.kv("member_syncs", m.member_list_syncs);
    o.kv("stub_calls", m.stub_calls);
    o.kv("last_stub", m.last_stub_slot);
    o.kv("module_commit_kb", m.module_committed_bytes / 1024);
    o.s("\n");
    o.s("alloc_total");
    o.kv("va_calls", static_cast<u64>(g_alloc.virtual_alloc_calls));
    o.kv("va_commit_kb", static_cast<u64>(g_alloc.virtual_commit_bytes) / 1024);
    o.kv("va_commit_waste_kb", static_cast<u64>(g_alloc.virtual_commit_waste) / 1024);
    o.kv("va_reserve_kb", static_cast<u64>(g_alloc.virtual_reserve_bytes) / 1024);
    o.kv("vf_calls", static_cast<u64>(g_alloc.virtual_free_calls));
    o.kv("heap_calls", static_cast<u64>(g_alloc.heap_alloc_calls + g_alloc.heap_realloc_calls));
    o.kv("heap_kb", static_cast<u64>(g_alloc.heap_alloc_bytes) / 1024);
    o.kv("tracked_overflow", g_tracked_overflow);
    o.s("\nEND\n");
    write_stdout(o.text, o.used);
}

} // namespace maicord::perf
