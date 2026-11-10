// Screen and window capture on Windows (include/maicord/platform/capture/screen_capture.hpp).
//
// Methods by system build (KUSER_SHARED_DATA's NtBuildNumber):
//  * Windows.Graphics.Capture (WGC) exists on the whole floor (1809, build 17763: GraphicsCaptureItem from an HWND or
//    HMONITOR, the free-threaded frame pool). It takes windows everywhere. The cursor can be left out from 2004 (19041,
//    IsCursorCaptureEnabled); before that WGC always draws it. The yellow capture border can be turned off from
//    Windows 11 and Server 2022 (20348, IsBorderRequired, after GraphicsCaptureAccess allows borderless capture), and
//    the frame rate capped by the system from 24H2 (26100, MinUpdateInterval; earlier builds drop frames here).
//  * Screens on builds with the border use DXGI Desktop Duplication, which shows none: frames are copied on the GPU
//    into a GDI-compatible texture and the cursor drawn into it with DrawIconEx. A screen duplication refuses (another
//    adapter drives it, a rotated mode) goes to WGC with its border.
//  * Picker thumbnails never show a border: WGC where it is borderless, Desktop Duplication for screens before that,
//    and PrintWindow (PW_RENDERFULLCONTENT) into a GDI-compatible texture for windows before that.
// Every picture stays on the GPU; a thumbnail alone is read back, after GenerateMips reduced it.
#include "maicord/platform/capture/screen_capture.hpp"
#include "../capture_policy.hpp"
#include "capture_abi.hpp"
#include "thumbnail.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/desktop_capture.hpp"
#include "maicord/sys/dxgi.hpp"
#include "maicord/sys/gdi.hpp"

namespace maicord {

// The callback object, WGC's set-up and the Desktop Duplication set-up run once per capture: compiled for size. The
// per-picture calls (wgc_frame, draw_cursor, deliver and the two loops) stay at full speed.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
namespace capture_abi {
namespace {
HRESULT __stdcall callback_query(SignalCallback* self, const GUID* iid, void** out) noexcept {
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (*iid != __uuidof(IUnknown) && *iid != kIidAgileObject && *iid != self->iid)
        return E_NOINTERFACE;
    InterlockedIncrement(&self->references);
    *out = self;
    return S_OK;
}
ULONG __stdcall callback_add(SignalCallback* self) noexcept {
    return static_cast<ULONG>(InterlockedIncrement(&self->references));
}
ULONG __stdcall callback_release(SignalCallback* self) noexcept {
    const LONG count = InterlockedDecrement(&self->references);
    if (!count) {
        CloseHandle(self->event);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return static_cast<ULONG>(count);
}
// Invoke(sender, args) of an event handler and ActivateCompleted(operation) alike: the x64 convention lets the one
// body ignore the arguments it is passed.
HRESULT __stdcall callback_invoke(SignalCallback* self, void*, void*) noexcept {
    SetEvent(self->event);
    return S_OK;
}
void* const kCallbackTable[]{reinterpret_cast<void*>(&callback_query), reinterpret_cast<void*>(&callback_add),
                             reinterpret_cast<void*>(&callback_release), reinterpret_cast<void*>(&callback_invoke)};
} // namespace

SignalCallback* signal_callback_create(HANDLE event, const GUID& iid) noexcept {
    auto* callback = static_cast<SignalCallback*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SignalCallback)));
    if (!callback)
        return nullptr;
    callback->vtable = kCallbackTable;
    callback->references = 1;
    callback->iid = iid;
    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &callback->event, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        HeapFree(GetProcessHeap(), 0, callback);
        return nullptr;
    }
    return callback;
}
} // namespace capture_abi

namespace {
using namespace capture_abi;

constexpr u32 kBuildCursorControl = 19041; // Windows 10 2004
constexpr u32 kBuildBorderless = 20348;    // Windows Server 2022, Windows 11
constexpr u32 kFrameBuffers = 2;

u32 os_build() noexcept {
    return *reinterpret_cast<const volatile u32*>(0x7FFE0260ull) & 0xFFFFu;
}
u64 qpc_us(i64 ticks) noexcept {
    return clock_microseconds(clock_initialize(), 0, ticks);
}

__declspec(noinline) Object* activation_factory(const wchar_t* name, u32 length, const GUID& iid) noexcept {
    StringReference reference(name, length);
    Object* factory = nullptr;
    if (FAILED(RoGetActivationFactory(reference.get(), iid, reinterpret_cast<void**>(&factory))))
        return nullptr;
    return factory;
}
template <usize N> Object* activation_factory(const wchar_t (&name)[N], const GUID& iid) noexcept {
    return activation_factory(name, N - 1, iid);
}
// IClosable::Close, then the reference.
void close_object(Object*& object) noexcept {
    Object* closable = nullptr;
    if (object && SUCCEEDED(query(object, kIidClosable, &closable))) {
        call(closable, kClose);
        release(closable);
    }
    release(object);
}
template <class F> F system_function(const wchar_t* library, const char* name) noexcept {
    HMODULE module = LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<F>(GetProcAddress(module, name)) : nullptr;
}

// The renderer's device as the Windows Runtime's IDirect3DDevice.
Object* runtime_device(GpuDevice* device) noexcept {
    static const auto create = system_function<CreateDirect3DDeviceFunction>(L"d3d11.dll",
                                                                             "CreateDirect3D11DeviceFromDXGIDevice");
    IDXGIDevice* dxgi = nullptr;
    Object* wrapped = nullptr;
    if (create && SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))))
        create(dxgi, &wrapped);
    com_release(dxgi);
    return wrapped;
}

Object* capture_item(const CaptureSource& source) noexcept {
    Object* interop = activation_factory(kCaptureItemClass, kIidCaptureItemInterop);
    Object* item = nullptr;
    if (interop)
        call(interop, source.kind == CaptureSourceKind::window ? kCreateForWindow : kCreateForMonitor,
             reinterpret_cast<void*>(source.handle), &kIidCaptureItem, reinterpret_cast<void**>(&item));
    release(interop);
    return item;
}

// GraphicsCaptureAccess.RequestAccessAsync(Borderless), once per process: a desktop application is answered at once,
// without a prompt; the wait is bounded all the same.
void request_borderless_access() noexcept {
    static volatile LONG requested = 0;
    if (InterlockedExchange(&requested, 1))
        return;
    Object* statics = activation_factory(kCaptureAccessClass, kIidCaptureAccessStatics);
    Object* operation = nullptr;
    Object* info = nullptr;
    if (statics && SUCCEEDED(call(statics, kRequestAccess, kAccessBorderless, &operation)) &&
        SUCCEEDED(query(operation, kIidAsyncInfo, &info))) {
        i32 status = kAsyncStarted;
        for (u32 waited = 0; waited < 2000 && SUCCEEDED(call(info, kAsyncStatus, &status)) && status == kAsyncStarted;
             waited += 10)
            Sleep(10);
    }
    release(info);
    release(operation);
    release(statics);
}

struct WgcSession {
    Object* device;
    Object* item;
    Object* pool;
    Object* session;
    SignalCallback* arrived;
    i64 token;
    SizeInt32 size;
    bool cursor;
};

void wgc_close(WgcSession* w) noexcept {
    if (w->pool && w->arrived)
        call(w->pool, kPoolRemoveFrameArrived, w->token);
    close_object(w->session);
    close_object(w->pool);
    signal_callback_release(w->arrived);
    release(w->item);
    release(w->device);
    reset_value(w);
}

// Starts a WGC session whose FrameArrived sets `wake`. `borderless` false accepts the border.
bool wgc_open(WgcSession* w, GpuDevice* device, const CaptureSource& source, HANDLE wake, bool cursor, bool borderless,
              u32 max_fps) noexcept {
    const u32 build = os_build();
    if (borderless && build < kBuildBorderless)
        return false;
    w->device = runtime_device(device);
    w->item = w->device ? capture_item(source) : nullptr;
    Object* statics = w->item ? activation_factory(kFramePoolClass, kIidFramePoolStatics2) : nullptr;
    bool ok = statics && SUCCEEDED(call(w->item, kItemSize, &w->size)) && w->size.width > 0 && w->size.height > 0 &&
              SUCCEEDED(call(statics, kCreateFreeThreaded, w->device, kPixelFormatBgra8, static_cast<i32>(kFrameBuffers),
                             size_argument(w->size.width, w->size.height), &w->pool));
    release(statics);
    if (ok) {
        w->arrived = signal_callback_create(wake, kIidFrameArrivedHandler);
        ok = w->arrived && SUCCEEDED(call(w->pool, kPoolAddFrameArrived, w->arrived, &w->token));
        if (!ok)
            signal_callback_release(w->arrived);
    }
    ok = ok && SUCCEEDED(call(w->pool, kPoolCreateSession, w->item, &w->session));
    w->cursor = true;
    Object* extension = nullptr;
    if (ok && build >= kBuildCursorControl && SUCCEEDED(query(w->session, kIidCaptureSession2, &extension)) &&
        SUCCEEDED(call(extension, kSessionPutCursor, static_cast<u8>(cursor))))
        w->cursor = cursor;
    release(extension);
    if (ok && build >= kBuildBorderless) {
        request_borderless_access();
        const bool off = SUCCEEDED(query(w->session, kIidCaptureSession3, &extension)) &&
                         SUCCEEDED(call(extension, kSessionPutBorder, static_cast<u8>(0)));
        release(extension);
        ok = off || !borderless;
    }
    if (ok && max_fps && SUCCEEDED(query(w->session, kIidCaptureSession5, &extension)))
        call(extension, kSessionPutMinInterval, static_cast<i64>(10'000'000 / max_fps));
    release(extension);
    ok = ok && SUCCEEDED(call(w->session, kSessionStart));
    if (!ok)
        wgc_close(w);
    return ok;
}

#pragma clang attribute pop

// The texture, picture size and time of a frame; the texture is a new reference.
bool wgc_frame(Object* frame, ID3D11Texture2D** texture, SizeInt32* content, i64* time_100ns) noexcept {
    Object* surface = nullptr;
    Object* access = nullptr;
    const bool ok = SUCCEEDED(call(frame, kFrameContentSize, content)) && SUCCEEDED(call(frame, kFrameTime, time_100ns)) &&
                    SUCCEEDED(call(frame, kFrameSurface, &surface)) &&
                    SUCCEEDED(query(surface, kIidDxgiInterfaceAccess, &access)) &&
                    SUCCEEDED(call(access, kGetInterface, &__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture)));
    release(access);
    release(surface);
    return ok;
}

// ---- Desktop Duplication ---------------------------------------------------------------------------------------------

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// The duplication of `monitor`'s output on the device's own adapter. DXGI_ERROR_UNSUPPORTED: duplication cannot serve
// it (another adapter, a rotated mode); WGC takes over.
HRESULT duplication_open(GpuDevice* device, HMONITOR monitor, IDXGIOutputDuplication** duplication, RECT* desktop) noexcept {
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    HRESULT result = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi));
    if (SUCCEEDED(result))
        result = dxgi->GetAdapter(&adapter);
    com_release(dxgi);
    IDXGIOutput* output = nullptr;
    DXGI_OUTPUT_DESC description{};
    for (UINT index = 0; SUCCEEDED(result); ++index) {
        result = adapter->EnumOutputs(index, &output);
        if (SUCCEEDED(result) && SUCCEEDED(output->GetDesc(&description)) && description.Monitor == monitor)
            break;
        com_release(output);
    }
    com_release(adapter);
    if (!output)
        return DXGI_ERROR_UNSUPPORTED;
    IDXGIOutput1* output1 = nullptr;
    result = description.Rotation > DXGI_MODE_ROTATION_IDENTITY ? DXGI_ERROR_UNSUPPORTED
                                                                  : output->QueryInterface(__uuidof(IDXGIOutput1),
                                                                                           reinterpret_cast<void**>(&output1));
    if (SUCCEEDED(result))
        result = output1->DuplicateOutput(device, duplication);
    com_release(output1);
    com_release(output);
    *desktop = description.DesktopCoordinates;
    return result;
}

// One BGRA texture duplication frames are copied into: GDI-compatible, so the cursor can be drawn on it.
ID3D11Texture2D* gdi_texture(GpuDevice* device, u32 width, u32 height) noexcept {
    const D3D11_TEXTURE2D_DESC description{width, height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                           D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
                                           D3D11_RESOURCE_MISC_GDI_COMPATIBLE};
    ID3D11Texture2D* texture = nullptr;
    device->CreateTexture2D(&description, nullptr, &texture);
    return texture;
}

#pragma clang attribute pop

struct CursorCache {
    HCURSOR cursor;
    i32 hot_x, hot_y;
};
// Draws the cursor (when it shows over the desktop rectangle) into `target`; true when it did.
bool draw_cursor(ID3D11Texture2D* target, const RECT& desktop, CursorCache* cache) noexcept {
    CURSORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING) || !info.hCursor)
        return false;
    if (info.hCursor != cache->cursor) {
        ICONINFO icon{};
        if (!GetIconInfo(info.hCursor, &icon))
            return false;
        cache->cursor = info.hCursor;
        cache->hot_x = static_cast<i32>(icon.xHotspot);
        cache->hot_y = static_cast<i32>(icon.yHotspot);
        if (icon.hbmMask)
            DeleteObject(icon.hbmMask);
        if (icon.hbmColor)
            DeleteObject(icon.hbmColor);
    }
    IDXGISurface1* surface = nullptr;
    HDC dc = nullptr;
    if (FAILED(target->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(&surface))) ||
        FAILED(surface->GetDC(FALSE, &dc))) {
        com_release(surface);
        return false;
    }
    DrawIconEx(dc, info.ptScreenPos.x - desktop.left - cache->hot_x, info.ptScreenPos.y - desktop.top - cache->hot_y,
               info.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    surface->ReleaseDC(nullptr);
    surface->Release();
    return true;
}

} // namespace

// ---- Thumbnails -----------------------------------------------------------------------------------------------------

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool capture_reduce_to_rgba(GpuDevice* device, GpuTexture* source, u32 width, u32 height, u32 max_width,
                            u32 max_height, u8* rgba, u32* out_width, u32* out_height) noexcept {
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    const u32 level = capture_thumbnail_level(width, height, max_width, max_height);
    const u32 level_width = capture_level_extent(width, level), level_height = capture_level_extent(height, level);
    ID3D11Texture2D* reduced = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11Texture2D* staging = nullptr;
    const D3D11_BOX box{0, 0, 0, width, height, 1};
    bool ok = true;
    if (level) {
        const D3D11_TEXTURE2D_DESC description{width, height, level + 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                               D3D11_USAGE_DEFAULT,
                                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
                                               D3D11_RESOURCE_MISC_GENERATE_MIPS};
        ok = SUCCEEDED(device->CreateTexture2D(&description, nullptr, &reduced)) &&
             SUCCEEDED(device->CreateShaderResourceView(reduced, nullptr, &view));
        if (ok) {
            context->CopySubresourceRegion(reduced, 0, 0, 0, 0, source, 0, &box);
            context->GenerateMips(view);
        }
    }
    const D3D11_TEXTURE2D_DESC read{level_width, level_height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                    D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ok = ok && SUCCEEDED(device->CreateTexture2D(&read, nullptr, &staging));
    if (ok) {
        const D3D11_BOX level_box{0, 0, 0, level_width, level_height, 1};
        context->CopySubresourceRegion(staging, 0, 0, 0, 0, level ? reduced : source, level, &level_box);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ok = SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        if (ok) {
            for (u32 y = 0; y < level_height; ++y) {
                const u8* row = static_cast<const u8*>(mapped.pData) + usize{y} * mapped.RowPitch;
                u8* out = rgba + usize{y} * level_width * 4;
                for (u32 x = 0; x < level_width; ++x) {
                    out[x * 4 + 0] = row[x * 4 + 2];
                    out[x * 4 + 1] = row[x * 4 + 1];
                    out[x * 4 + 2] = row[x * 4 + 0];
                    out[x * 4 + 3] = 255;
                }
            }
            context->Unmap(staging, 0);
        }
    }
    com_release(staging);
    com_release(view);
    com_release(reduced);
    context->Release();
    *out_width = level_width;
    *out_height = level_height;
    return ok;
}

#pragma clang attribute pop

namespace {

// ---- The capture thread ---------------------------------------------------------------------------------------------

struct Runtime {
    CaptureOptions options;
    CaptureSink sink;
    Thread* thread;
    HANDLE stop, wake;
    CapturePacer pacer;
};

bool stopping(const Runtime& r) noexcept {
    return WaitForSingleObject(r.stop, 0) == WAIT_OBJECT_0;
}

// The source is still there (and, for a window, reports minimize edges).
bool source_alive(Runtime& r, bool* minimized) noexcept {
    const CaptureSource& source = r.options.source;
    if (source.kind == CaptureSourceKind::window) {
        const HWND window = reinterpret_cast<HWND>(source.handle);
        if (!IsWindow(window))
            return false;
        const bool iconic = IsIconic(window) != 0;
        if (iconic != *minimized) {
            *minimized = iconic;
            if (r.sink.minimized)
                r.sink.minimized(r.sink.context, iconic);
        }
        return true;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    return GetMonitorInfoW(reinterpret_cast<HMONITOR>(source.handle), &info) != 0;
}

void deliver(Runtime& r, ID3D11Texture2D* texture, u32 width, u32 height, u64 timestamp_us, bool cursor) noexcept {
    if (!capture_pacer_admit(&r.pacer, timestamp_us, r.options.max_fps))
        return;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    CaptureFrame frame{};
    frame.texture = texture;
    frame.width = minimum(width, description.Width);
    frame.height = minimum(height, description.Height);
    frame.timestamp_us = timestamp_us;
    frame.cursor = cursor;
    if (frame.width && frame.height)
        r.sink.frame(r.sink.context, frame);
}

CaptureEnd run_wgc(Runtime& r) noexcept {
    WgcSession w{};
    if (!wgc_open(&w, r.options.device, r.options.source, r.wake, r.options.cursor, false, r.options.max_fps)) {
        log_write(LogLevel::warning, "capture", "wgc_start_failed");
        return CaptureEnd::unsupported;
    }
    CaptureEnd end = CaptureEnd::stopped;
    bool minimized = false;
    for (;;) {
        HANDLE events[]{r.stop, r.wake};
        const DWORD waited = WaitForMultipleObjects(2, events, FALSE, 250);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
            break;
        if (!source_alive(r, &minimized)) {
            end = CaptureEnd::closed;
            break;
        }
        if (FAILED(r.options.device->GetDeviceRemovedReason())) {
            end = CaptureEnd::failed;
            break;
        }
        for (Object* frame = nullptr; SUCCEEDED(call(w.pool, kPoolTryGetNextFrame, &frame)) && frame;) {
            ID3D11Texture2D* texture = nullptr;
            SizeInt32 content{};
            i64 time = 0;
            if (wgc_frame(frame, &texture, &content, &time) && content.width > 0 && content.height > 0) {
                deliver(r, texture, static_cast<u32>(content.width), static_cast<u32>(content.height),
                        static_cast<u64>(time) / 10, w.cursor);
                // A window that changed size: later frames come at the new size.
                if (content.width != w.size.width || content.height != w.size.height) {
                    w.size = content;
                    call(w.pool, kPoolRecreate, w.device, kPixelFormatBgra8, static_cast<i32>(kFrameBuffers),
                         size_argument(content.width, content.height));
                }
            }
            com_release(texture);
            close_object(frame);
        }
    }
    wgc_close(&w);
    return end;
}

// Desktop Duplication of a screen. False when duplication cannot serve it (WGC takes over); otherwise *end is set.
bool run_duplication(Runtime& r, CaptureEnd* end) noexcept {
    GpuDevice* device = r.options.device;
    const HMONITOR monitor = reinterpret_cast<HMONITOR>(r.options.source.handle);
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    IDXGIOutputDuplication* duplication = nullptr;
    ID3D11Texture2D* target = nullptr;
    u32 target_width = 0, target_height = 0;
    RECT desktop{};
    CursorCache cursor_cache{};
    bool minimized = false, served = false, fresh = false;
    *end = CaptureEnd::stopped;
    while (!stopping(r)) {
        if (!duplication) {
            const HRESULT opened = duplication_open(device, monitor, &duplication, &desktop);
            // Before the first frame, anything but the secure desktop (access denied) hands the screen to WGC.
            if (FAILED(opened) && opened != E_ACCESSDENIED && !served) {
                log_u64(LogLevel::info, "capture", "duplication_unsupported", static_cast<u32>(opened));
                break;
            }
            if (FAILED(opened)) {
                // The secure desktop, a mode change in progress, too many duplications: try again shortly.
                if (!source_alive(r, &minimized)) {
                    *end = CaptureEnd::closed;
                    served = true;
                    break;
                }
                if (WaitForSingleObject(r.stop, 250) == WAIT_OBJECT_0)
                    break;
                continue;
            }
            served = fresh = true;
            const u32 width = static_cast<u32>(desktop.right - desktop.left);
            const u32 height = static_cast<u32>(desktop.bottom - desktop.top);
            if (width != target_width || height != target_height) {
                com_release(target);
                target = gdi_texture(device, width, height);
                target_width = width;
                target_height = height;
            }
            if (!target) {
                *end = CaptureEnd::failed;
                break;
            }
        }
        DXGI_OUTDUPL_FRAME_INFO info{};
        IDXGIResource* resource = nullptr;
        const HRESULT acquired = duplication->AcquireNextFrame(100, &info, &resource);
        if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
            if (!source_alive(r, &minimized)) {
                *end = CaptureEnd::closed;
                break;
            }
            continue;
        }
        if (FAILED(acquired)) {
            // DXGI_ERROR_ACCESS_LOST: a mode change, the secure desktop, a full-screen application; duplicate again.
            com_release(duplication);
            if (FAILED(device->GetDeviceRemovedReason())) {
                *end = CaptureEnd::failed;
                break;
            }
            continue;
        }
        const bool picture = info.LastPresentTime.QuadPart != 0;
        const bool pointer = r.options.cursor && info.LastMouseUpdateTime.QuadPart != 0;
        ID3D11Texture2D* desktop_texture = nullptr;
        if ((picture || pointer || fresh) &&
            SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&desktop_texture)))) {
            const D3D11_BOX box{0, 0, 0, target_width, target_height, 1};
            context->CopySubresourceRegion(target, 0, 0, 0, 0, desktop_texture, 0, &box);
        }
        com_release(resource);
        duplication->ReleaseFrame();
        if (desktop_texture) {
            desktop_texture->Release();
            fresh = false;
            const bool cursor = r.options.cursor && draw_cursor(target, desktop, &cursor_cache);
            const u64 timestamp = picture ? qpc_us(info.LastPresentTime.QuadPart) : qpc_us(clock_now());
            deliver(r, target, target_width, target_height, timestamp, cursor);
        }
    }
    com_release(duplication);
    com_release(target);
    context->Release();
    return served || stopping(r);
}

u32 capture_thread(void* parameter) noexcept {
    auto& r = *static_cast<Runtime*>(parameter);
    const bool com = thread_com_enter();
    CaptureEnd end = CaptureEnd::stopped;
    const bool screen = r.options.source.kind == CaptureSourceKind::screen;
    if (!screen || os_build() >= kBuildBorderless || !run_duplication(r, &end))
        end = run_wgc(r);
    if (end != CaptureEnd::stopped && !stopping(r))
        r.sink.ended(r.sink.context, end);
    if (com)
        thread_com_leave();
    return 0;
}

// Teardown, the picker's source list and thumbnails, and the start run once per capture or per picker refresh:
// compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
void free_runtime(Runtime* r) noexcept {
    if (r->thread)
        thread_close(r->thread);
    if (r->stop)
        CloseHandle(r->stop);
    if (r->wake)
        CloseHandle(r->wake);
    pages_release(r);
}

// ---- Sources --------------------------------------------------------------------------------------------------------

using DwmAttribute = decltype(&DwmGetWindowAttribute);

struct SourceList {
    Slice<CaptureSource> output;
    u32 count;
    DWORD own_process;
    HWND shell;
    DwmAttribute dwm;
};

void name_from_wide(CaptureSource* source, const wchar_t* text) noexcept {
    char utf8[kCaptureSourceNameBytes * 2];
    const i32 bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
    const u32 used = capture_name_cut(utf8, bytes > 1 ? static_cast<u32>(bytes - 1) : 0, kCaptureSourceNameBytes);
    copy_bytes(source->name, utf8, used);
    source->name_bytes = static_cast<u16>(used);
}

BOOL CALLBACK visit_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto& list = *reinterpret_cast<SourceList*>(parameter);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (list.count >= list.output.count || !GetMonitorInfoW(monitor, &info))
        return TRUE;
    CaptureSource& source = list.output[list.count++];
    reset_value(&source);
    source.handle = reinterpret_cast<u64>(monitor);
    source.kind = CaptureSourceKind::screen;
    source.width = static_cast<u32>(info.rcMonitor.right - info.rcMonitor.left);
    source.height = static_cast<u32>(info.rcMonitor.bottom - info.rcMonitor.top);
    source.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    return TRUE;
}

BOOL CALLBACK visit_window(HWND window, LPARAM parameter) {
    auto& list = *reinterpret_cast<SourceList*>(parameter);
    if (list.count >= list.output.count)
        return FALSE;
    CaptureWindowFacts facts{};
    facts.visible = IsWindowVisible(window) != 0;
    if (!facts.visible)
        return TRUE;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    facts.own_process = process == list.own_process;
    facts.tool_window = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    facts.owned = GetWindow(window, GW_OWNER) != nullptr;
    facts.shell = window == list.shell;
    facts.minimized = IsIconic(window) != 0;
    DWORD cloaked = 0;
    if (list.dwm)
        list.dwm(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    facts.cloaked = cloaked != 0;
    facts.title_chars = static_cast<u32>(maximum(GetWindowTextLengthW(window), 0));
    RECT client{};
    GetClientRect(window, &client);
    facts.width = static_cast<u32>(maximum(client.right - client.left, 0L));
    facts.height = static_cast<u32>(maximum(client.bottom - client.top, 0L));
    if (!capture_window_listed(facts))
        return TRUE;
    CaptureSource& source = list.output[list.count];
    reset_value(&source);
    wchar_t title[256];
    if (GetWindowTextW(window, title, 256) <= 0)
        return TRUE;
    name_from_wide(&source, title);
    source.handle = reinterpret_cast<u64>(window);
    source.process_id = process;
    source.kind = CaptureSourceKind::window;
    source.width = facts.width;
    source.height = facts.height;
    ++list.count;
    return TRUE;
}

} // namespace

CaptureSupport capture_support() noexcept {
    return {true, true, false, os_build() >= kBuildCursorControl};
}

u32 capture_sources(Slice<CaptureSource> output) noexcept {
    if (!output.data || !output.count)
        return 0;
    SourceList list{output, 0, GetCurrentProcessId(), GetShellWindow(),
                    system_function<DwmAttribute>(L"dwmapi.dll", "DwmGetWindowAttribute")};
    EnumDisplayMonitors(nullptr, nullptr, visit_monitor, reinterpret_cast<LPARAM>(&list));
    // The primary screen first, then "Screen 1", "Screen 2", ... in that order.
    const u32 screens = list.count;
    for (u32 i = 1; i < screens; ++i)
        if (output[i].primary) {
            const CaptureSource primary = output[i];
            for (u32 k = i; k > 0; --k)
                copy_value(&output[k], output[k - 1]);
            copy_value(&output[0], primary);
        }
    for (u32 i = 0; i < screens; ++i) {
        static constexpr char kScreen[] = "Screen ";
        copy_bytes(output[i].name, kScreen, sizeof(kScreen) - 1);
        output[i].name_bytes = static_cast<u16>(sizeof(kScreen) - 1 + decimal_write(output[i].name + sizeof(kScreen) - 1, i + 1));
    }
    EnumWindows(visit_window, reinterpret_cast<LPARAM>(&list));
    return list.count;
}

bool capture_thumbnail(GpuDevice* device, const CaptureSource& source, u32 max_width, u32 max_height, u8* rgba,
                       u32* width, u32* height) noexcept {
    if (!device || !rgba || !width || !height || !max_width || !max_height)
        return false;
    *width = *height = 0;
    const bool window = source.kind == CaptureSourceKind::window;
    if (window && (!IsWindow(reinterpret_cast<HWND>(source.handle)) || IsIconic(reinterpret_cast<HWND>(source.handle))))
        return false;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    ID3D11Texture2D* picture = nullptr;
    u32 picture_width = 0, picture_height = 0;
    if (os_build() >= kBuildBorderless) {
        const HANDLE arrived = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WgcSession w{};
        if (arrived && wgc_open(&w, device, source, arrived, false, true, 0)) {
            Object* frame = nullptr;
            SizeInt32 content{};
            i64 time = 0;
            for (u32 tries = 0; tries < 2 && !frame; ++tries)
                if (WaitForSingleObject(arrived, 1000) != WAIT_OBJECT_0 ||
                    FAILED(call(w.pool, kPoolTryGetNextFrame, &frame)))
                    break;
            ID3D11Texture2D* texture = nullptr;
            if (frame && wgc_frame(frame, &texture, &content, &time)) {
                picture = texture;
                picture_width = static_cast<u32>(content.width);
                picture_height = static_cast<u32>(content.height);
                ok = capture_reduce_to_rgba(device, picture, picture_width, picture_height, max_width, max_height,
                                            rgba, width, height);
            }
            close_object(frame);
            wgc_close(&w);
        }
        if (arrived)
            CloseHandle(arrived);
    } else if (!window) {
        IDXGIOutputDuplication* duplication = nullptr;
        RECT desktop{};
        if (SUCCEEDED(duplication_open(device, reinterpret_cast<HMONITOR>(source.handle), &duplication, &desktop))) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            IDXGIResource* resource = nullptr;
            // The first frame of a duplication holds the whole desktop image.
            if (SUCCEEDED(duplication->AcquireNextFrame(500, &info, &resource))) {
                if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&picture))))
                    ok = capture_reduce_to_rgba(device, picture, static_cast<u32>(desktop.right - desktop.left),
                                                static_cast<u32>(desktop.bottom - desktop.top), max_width, max_height,
                                                rgba, width, height);
                resource->Release();
                duplication->ReleaseFrame();
            }
            duplication->Release();
        }
    } else {
        // PrintWindow draws the window, its DirectX content included, into a GDI-compatible GPU texture.
        RECT bounds{};
        GetWindowRect(reinterpret_cast<HWND>(source.handle), &bounds);
        picture_width = static_cast<u32>(maximum(bounds.right - bounds.left, 0L));
        picture_height = static_cast<u32>(maximum(bounds.bottom - bounds.top, 0L));
        picture = picture_width && picture_height ? gdi_texture(device, picture_width, picture_height) : nullptr;
        IDXGISurface1* surface = nullptr;
        HDC dc = nullptr;
        if (picture && SUCCEEDED(picture->QueryInterface(__uuidof(IDXGISurface1), reinterpret_cast<void**>(&surface))) &&
            SUCCEEDED(surface->GetDC(TRUE, &dc))) {
            const bool printed = PrintWindow(reinterpret_cast<HWND>(source.handle), dc, PW_RENDERFULLCONTENT) != 0;
            surface->ReleaseDC(nullptr);
            ok = printed && capture_reduce_to_rgba(device, picture, picture_width, picture_height, max_width,
                                                   max_height, rgba, width, height);
        }
        com_release(surface);
    }
    com_release(picture);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return ok;
}

ScreenCapture* capture_start(const CaptureOptions& options, const CaptureSink& sink) noexcept {
    if (!options.device || !sink.frame || !sink.ended || !options.source.handle)
        return nullptr;
    auto* r = commit_pages<Runtime>();
    if (!r)
        return nullptr;
    copy_value(&r->options, options);
    copy_value(&r->sink, sink);
    r->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    r->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (r->stop && r->wake)
        r->thread = thread_start(capture_thread, r, 256 * 1024, nullptr);
    if (!r->thread) {
        free_runtime(r);
        return nullptr;
    }
    return reinterpret_cast<ScreenCapture*>(r);
}

void capture_stop(ScreenCapture* capture) noexcept {
    auto* r = reinterpret_cast<Runtime*>(capture);
    if (!r)
        return;
    SetEvent(r->stop);
    thread_wait(r->thread, INFINITE);
    free_runtime(r);
}

} // namespace maicord
#pragma clang attribute pop
