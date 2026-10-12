#include "maicord/platform/voice_audio_output.hpp"
#include "maicord/core/time.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

namespace maicord {
namespace {
constexpr u32 kRate = 48000, kChannels = 2, kChunkFrames = 2048;
constexpr REFERENCE_TIME kBufferDuration = 200'000; // 20 ms in 100 ns units.
constexpr LONG kDefaultChanged = 1, kTopologyChanged = 2;
// COM-owned endpoint observer. It owns a duplicate of the wake event so a
// callback racing unregistration never touches the thread's runtime.
struct EndpointNotify final : IMMNotificationClient {
    static void* operator new(usize, void* p) noexcept {
        return p;
    }
    volatile LONG references{1}, dirty{};
    HANDLE event{};
    void signal(LONG flags) noexcept {
        InterlockedOr(&dirty, flags);
        SetEvent(event);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** p) override {
        if (!p)
            return E_POINTER;
        *p = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMNotificationClient))
            return E_NOINTERFACE;
        *p = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references);
        if (!count) {
            if (event)
                CloseHandle(event);
            this->~EndpointNotify();
            CoTaskMemFree(this);
        }
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole)
            signal(kDefaultChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        signal(kTopologyChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        signal(kTopologyChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        return S_OK; // Loss of the open endpoint surfaces as AUDCLNT_E_DEVICE_INVALIDATED.
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }
};
struct Runtime {
    VoiceAudioRender render;
    void* context;
    HANDLE thread, stop, wake, notify;
    wchar_t device[128];
    volatile LONG state, buffer_frames, period_frames, opens, native_error;
};
struct Engine {
    IMMDeviceEnumerator* enumerator;
    EndpointNotify* notify;
    IAudioClient* client;
    IAudioRenderClient* render;
    u32 frames;
};
void publish(Runtime& r, VoiceAudioOutputState state, HRESULT native = S_OK) noexcept {
    InterlockedExchange(&r.state, static_cast<LONG>(state));
    if (FAILED(native))
        InterlockedExchange(&r.native_error, static_cast<LONG>(native));
}
void close_stream(Engine& e) noexcept {
    if (e.client)
        e.client->Stop();
    com_release(e.render);
    com_release(e.client);
    e.frames = 0;
}
HRESULT fill(Runtime& r, Engine& e) noexcept {
    u32 padding = 0;
    HRESULT h = e.client->GetCurrentPadding(&padding);
    if (FAILED(h) || padding >= e.frames)
        return h;
    const u32 frames = e.frames - padding;
    BYTE* data = nullptr;
    h = e.render->GetBuffer(frames, &data);
    if (FAILED(h))
        return h;
    const u64 now = clock_microseconds(clock_initialize(), 0, clock_now());
    auto* pcm = reinterpret_cast<i16*>(data);
    for (u32 at = 0; at < frames;) {
        const u32 chunk = minimum(kChunkFrames, frames - at);
        r.render(r.context, {pcm + at * kChannels, chunk * kChannels}, now);
        at += chunk;
    }
    return e.render->ReleaseBuffer(frames, 0);
}
HRESULT open_stream(Runtime& r, Engine& e) noexcept {
    IMMDevice* device = nullptr;
    HRESULT h = E_NOTFOUND;
    if (r.device[0] && SUCCEEDED(e.enumerator->GetDevice(r.device, &device))) {
        DWORD active = 0;
        if (SUCCEEDED(device->GetState(&active)) && active == DEVICE_STATE_ACTIVE)
            h = S_OK;
        else
            com_release(device);
    }
    // A missing chosen device falls back to the console default.
    if (FAILED(h))
        h = e.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(h))
        h = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                             reinterpret_cast<void**>(&e.client));
    com_release(device);
    constexpr u16 stride = kChannels * sizeof(i16);
    const WAVEFORMATEX format{WAVE_FORMAT_PCM, kChannels, kRate, kRate * stride, stride, 16, 0};
    REFERENCE_TIME period = 0;
    if (SUCCEEDED(h))
        h = e.client->GetDevicePeriod(&period, nullptr);
    if (SUCCEEDED(h))
        h = e.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                 kBufferDuration, 0, &format, nullptr);
    if (SUCCEEDED(h))
        h = e.client->SetEventHandle(r.wake);
    if (SUCCEEDED(h))
        h = e.client->GetBufferSize(&e.frames);
    if (SUCCEEDED(h) && !e.frames)
        h = E_UNEXPECTED;
    if (SUCCEEDED(h))
        h = e.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&e.render));
    if (SUCCEEDED(h))
        h = fill(r, e);
    if (SUCCEEDED(h))
        h = e.client->Start();
    if (FAILED(h)) {
        close_stream(e);
        return h;
    }
    InterlockedExchange(&r.buffer_frames, static_cast<LONG>(e.frames));
    InterlockedExchange(&r.period_frames, static_cast<LONG>(period * kRate / 10'000'000));
    InterlockedIncrement(&r.opens);
    return h;
}
HRESULT watch(Runtime& r, Engine& e) noexcept {
    HRESULT h = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                 __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&e.enumerator));
    if (FAILED(h))
        return h;
    void* memory = CoTaskMemAlloc(sizeof(EndpointNotify));
    if (!memory)
        return E_OUTOFMEMORY;
    e.notify = new (memory) EndpointNotify;
    if (!DuplicateHandle(GetCurrentProcess(), r.notify, GetCurrentProcess(), &e.notify->event, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return HRESULT_FROM_WIN32(GetLastError());
    return e.enumerator->RegisterEndpointNotificationCallback(e.notify);
}
// Waits on stop and endpoint notifications; true when stopping.
bool pause(Runtime& r, u32 milliseconds) noexcept {
    HANDLE events[]{r.stop, r.notify};
    return WaitForMultipleObjects(2, events, FALSE, milliseconds) == WAIT_OBJECT_0;
}
DWORD WINAPI audio_thread(void* parameter) noexcept {
    auto& r = *static_cast<Runtime*>(parameter);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Engine e{};
    HRESULT h = FAILED(apartment) ? apartment : watch(r, e);
    if (FAILED(h))
        publish(r, VoiceAudioOutputState::waiting, h);
    while (SUCCEEDED(h)) {
        if (!e.client) {
            publish(r, VoiceAudioOutputState::opening);
            const HRESULT opened = open_stream(r, e);
            if (FAILED(opened)) {
                publish(r, VoiceAudioOutputState::waiting, opened);
                if (pause(r, 1000))
                    break;
                continue;
            }
            publish(r, VoiceAudioOutputState::playing);
        }
        HANDLE events[]{r.stop, r.notify, r.wake};
        const DWORD waited = WaitForMultipleObjects(3, events, FALSE, 200);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
            break;
        if (InterlockedExchange(&e.notify->dirty, 0) & kDefaultChanged) {
            close_stream(e);
            continue;
        }
        const HRESULT filled = fill(r, e);
        if (FAILED(filled)) {
            // Device invalidated/removed or format change: reopen the default endpoint.
            close_stream(e);
            publish(r, VoiceAudioOutputState::waiting, filled);
            if (pause(r, filled == AUDCLNT_E_DEVICE_INVALIDATED ? 50 : 500))
                break;
        }
    }
    close_stream(e);
    if (e.enumerator && e.notify)
        e.enumerator->UnregisterEndpointNotificationCallback(e.notify);
    com_release(e.notify);
    com_release(e.enumerator);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    publish(r, VoiceAudioOutputState::stopped);
    return 0;
}
void free_runtime(Runtime* r) noexcept {
    const HANDLE handles[]{r->thread, r->stop, r->wake, r->notify};
    for (HANDLE handle : handles)
        if (handle)
            CloseHandle(handle);
    VirtualFree(r, 0, MEM_RELEASE);
}
} // namespace

bool voice_audio_output_start(VoiceAudioOutput* output, VoiceAudioRender render, void* context) noexcept {
    return voice_audio_output_start_device(output, nullptr, render, context);
}

bool voice_audio_output_start_device(VoiceAudioOutput* output, const wchar_t* device_id, VoiceAudioRender render,
                                     void* context) noexcept {
    if (!output || output->runtime || !render)
        return false;
    auto* r = static_cast<Runtime*>(VirtualAlloc(nullptr, sizeof(Runtime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!r)
        return false;
    r->render = render;
    r->context = context;
    for (u32 i = 0; device_id && i + 1 < sizeof(r->device) / sizeof(r->device[0]) && device_id[i]; ++i)
        r->device[i] = device_id[i];
    r->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    r->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r->notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r->state = static_cast<LONG>(VoiceAudioOutputState::opening);
    if (r->stop && r->wake && r->notify)
        r->thread = CreateThread(nullptr, 256 * 1024, audio_thread, r, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!r->thread) {
        free_runtime(r);
        return false;
    }
    output->runtime = r;
    return true;
}

void voice_audio_output_stop(VoiceAudioOutput* output) noexcept {
    if (!output || !output->runtime)
        return;
    auto* r = static_cast<Runtime*>(output->runtime);
    SetEvent(r->stop);
    // A wedged audio driver must not free memory the thread may still use.
    if (WaitForSingleObject(r->thread, 5000) != WAIT_OBJECT_0)
        return;
    free_runtime(r);
    output->runtime = nullptr;
}

bool voice_audio_output_running(const VoiceAudioOutput& output) noexcept {
    return output.runtime != nullptr;
}

VoiceAudioOutputStatus voice_audio_output_status(const VoiceAudioOutput& output) noexcept {
    VoiceAudioOutputStatus status{};
    if (const auto* r = static_cast<const Runtime*>(output.runtime)) {
        status.state = static_cast<VoiceAudioOutputState>(ReadAcquire(&r->state));
        status.buffer_frames = static_cast<u32>(ReadAcquire(&r->buffer_frames));
        status.period_frames = static_cast<u32>(ReadAcquire(&r->period_frames));
        status.opens = static_cast<u32>(ReadAcquire(&r->opens));
        status.native_error = static_cast<u32>(ReadAcquire(&r->native_error));
    }
    return status;
}
} // namespace maicord
