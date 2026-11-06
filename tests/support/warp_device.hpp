#pragma once
// Test images only: every D3D11CreateDevice call of this image (its own and the renderer's, whose
// device code it links) makes a WARP device instead of a hardware one, with the Direct3D 11 debug layer when it is
// asked for and installed. The image's import slots of D3D11CreateDevice are patched; nothing that ships changes.
// Hang reproductions and validation runs use it, so they never submit work to the hardware adapter.
#include "maicord/core/decimal.hpp"
#include "maicord/core/heap.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/d3d11.hpp"
#include "../../tools/perf/perf_imports.hpp"

namespace maicord::warp_device {

// DXGI_ERROR_SDK_COMPONENT_MISSING: the debug layer (the Graphics Tools feature) is not installed.
inline constexpr HRESULT kDebugLayerMissing = static_cast<HRESULT>(0x887A002Du);
inline constexpr u32 kQueues = 8;
inline constexpr u32 kKinds = 32;
inline constexpr u32 kKindText = 240;

// One message ID and how often it came.
struct Kind {
    D3D11_MESSAGE_ID id;
    D3D11_MESSAGE_SEVERITY severity;
    u64 count;
    char text[kKindText];
    u32 text_bytes;
};

struct State {
    decltype(&D3D11CreateDevice) create;
    bool debug;        // the debug layer was asked for
    u32 devices;       // WARP devices made
    u32 undebugged;    // of them, without the debug layer (not installed)
    ID3D11InfoQueue* queues[kQueues];
    u32 queue_count;
    u64 severity[5];   // by D3D11_MESSAGE_SEVERITY
    u64 allowed;       // NULL_TILE_MAPPING_ACCESS_WARNING (src/render/README.md)
    u32 kind_count;
    Kind kinds[kKinds];
};
inline State g_state{};

inline HRESULT WINAPI create_device(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT flags,
                                    const D3D_FEATURE_LEVEL* levels, UINT level_count, UINT sdk, ID3D11Device** device,
                                    D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) noexcept {
    State& s = g_state;
    HRESULT result = kDebugLayerMissing;
    if (s.debug)
        result = s.create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags | D3D11_CREATE_DEVICE_DEBUG, levels, level_count,
                          sdk, device, level, context);
    const bool layered = s.debug && SUCCEEDED(result);
    if (result == kDebugLayerMissing)
        result = s.create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, level_count, sdk, device, level, context);
    if (SUCCEEDED(result)) {
        ++s.devices;
        s.undebugged += layered ? 0u : 1u;
        ID3D11InfoQueue* queue = nullptr;
        if (layered && device && *device && s.queue_count < kQueues &&
            SUCCEEDED((*device)->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&queue)))) {
            // No limit (the default keeps 1024 and drops the rest): a caller that never drains still sees every message.
            queue->SetMessageCountLimit(~0ull);
            s.queues[s.queue_count++] = queue;
        }
    }
    return result;
}

// Patches this image's D3D11CreateDevice slots (static and delay-load); false when it imports none.
inline bool install(bool debug) noexcept {
    State& s = g_state;
    const HMODULE d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    s.create = d3d11 ? reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(d3d11, "D3D11CreateDevice"))
                     : nullptr;
    s.debug = debug;
    if (!s.create)
        return false;
    u32 patched = 0;
    perf::for_each_import_slot(
        reinterpret_cast<u8*>(GetModuleHandleW(nullptr)), 1, [](u32) noexcept { return "D3D11CreateDevice"; }, true,
        [&](u32, IMAGE_THUNK_DATA64* slot) noexcept {
            DWORD old = 0;
            if (!VirtualProtect(&slot->u1.Function, sizeof(u64), PAGE_READWRITE, &old))
                return false;
            slot->u1.Function = reinterpret_cast<u64>(&create_device);
            VirtualProtect(&slot->u1.Function, sizeof(u64), old, &old);
            ++patched;
            return true;
        });
    return patched != 0;
}

inline void note(const D3D11_MESSAGE& message) noexcept {
    State& s = g_state;
    if (static_cast<u32>(message.Severity) < 5)
        ++s.severity[message.Severity];
    if (message.ID == D3D11_MESSAGE_ID_NULL_TILE_MAPPING_ACCESS_WARNING &&
        message.Severity == D3D11_MESSAGE_SEVERITY_WARNING)
        ++s.allowed;
    for (u32 i = 0; i < s.kind_count; ++i) {
        if (s.kinds[i].id == message.ID) {
            ++s.kinds[i].count;
            return;
        }
    }
    if (s.kind_count == kKinds)
        return;
    Kind& kind = s.kinds[s.kind_count++];
    kind.id = message.ID;
    kind.severity = message.Severity;
    kind.count = 1;
    kind.text_bytes = 0;
    for (SIZE_T i = 0; message.pDescription && i < message.DescriptionByteLength && kind.text_bytes < kKindText; ++i) {
        const char c = message.pDescription[i];
        if (!c)
            break;
        kind.text[kind.text_bytes++] = c == '\n' || c == '\r' ? ' ' : c;
    }
}

// Tallies and clears the messages the debug layer stored since the last drain (call it once a frame: the queue
// keeps a bounded number).
inline void drain() noexcept {
    State& s = g_state;
    for (u32 q = 0; q < s.queue_count; ++q) {
        ID3D11InfoQueue* queue = s.queues[q];
        const UINT64 stored = queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < stored; ++i) {
            SIZE_T bytes = 0;
            if (FAILED(queue->GetMessage(i, nullptr, &bytes)) || bytes < sizeof(D3D11_MESSAGE))
                continue;
            alignas(8) u8 local[1024];
            void* memory = bytes <= sizeof(local) ? static_cast<void*>(local) : heap_allocate(bytes, false);
            if (!memory)
                continue;
            auto* message = static_cast<D3D11_MESSAGE*>(memory);
            if (SUCCEEDED(queue->GetMessage(i, message, &bytes)))
                note(*message);
            if (memory != local)
                heap_free(memory);
        }
        queue->ClearStoredMessages();
    }
}

// Messages that fail a validation run: corruption, errors and every warning but the documented one.
inline u64 failures() noexcept {
    const State& s = g_state;
    return s.severity[D3D11_MESSAGE_SEVERITY_CORRUPTION] + s.severity[D3D11_MESSAGE_SEVERITY_ERROR] +
           s.severity[D3D11_MESSAGE_SEVERITY_WARNING] - s.allowed;
}

// Writes the tallies, then one line per message ID: "D3D11 <severity> #<id> x<count>: <text>".
inline void report(void (*line)(const char* text, u32 bytes) noexcept) noexcept {
    drain();
    const State& s = g_state;
    char text[kKindText + 96];
    u32 used = 0;
    const auto put = [&](const char* part) noexcept {
        while (*part && used < sizeof(text))
            text[used++] = *part++;
    };
    const auto number = [&](u64 value) noexcept {
        if (used + 20 <= sizeof(text))
            used += decimal_write(text + used, value);
    };
    put("D3D11 WARP devices ");
    number(s.devices);
    put(", without debug layer ");
    number(s.undebugged);
    put(": corruption ");
    number(s.severity[D3D11_MESSAGE_SEVERITY_CORRUPTION]);
    put(", errors ");
    number(s.severity[D3D11_MESSAGE_SEVERITY_ERROR]);
    put(", warnings ");
    number(s.severity[D3D11_MESSAGE_SEVERITY_WARNING]);
    put(" (NULL_TILE_MAPPING_ACCESS_WARNING ");
    number(s.allowed);
    put("), info ");
    number(s.severity[D3D11_MESSAGE_SEVERITY_INFO] + s.severity[D3D11_MESSAGE_SEVERITY_MESSAGE]);
    put("\n");
    line(text, used);
    constexpr const char* kSeverity[5] = {"corruption", "error", "warning", "info", "message"};
    for (u32 i = 0; i < s.kind_count; ++i) {
        const Kind& kind = s.kinds[i];
        used = 0;
        put("D3D11 ");
        put(static_cast<u32>(kind.severity) < 5 ? kSeverity[kind.severity] : "?");
        put(" #");
        number(static_cast<u64>(kind.id));
        put(" x");
        number(kind.count);
        put(": ");
        for (u32 c = 0; c < kind.text_bytes && used < sizeof(text) - 1; ++c)
            text[used++] = kind.text[c];
        text[used++] = '\n';
        line(text, used);
    }
}

} // namespace maicord::warp_device
