#include "maicord/platform/voice_audio_output.hpp"
#include "audio_endpoint_watch.hpp"
#include "maicord/core/time.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

namespace maicord {
namespace {
constexpr u32 kRate = 48000, kChannels = 2, kChunkFrames = 2048;
constexpr REFERENCE_TIME kBufferDuration = 200'000; // 20 ms in 100 ns units.
struct Runtime {
    VoiceAudioRender render;
    void* context;
    HANDLE thread, stop, wake, notify;
    wchar_t device[128];
};
struct Engine {
    IMMDeviceEnumerator* enumerator;
    AudioEndpointNotify* notify;
    IAudioClient* client;
    IAudioRenderClient* render;
    u32 frames;
};
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
    if (FAILED(h))
        close_stream(e);
    return h;
}
HRESULT watch(Runtime& r, Engine& e) noexcept {
    return audio_endpoint_watch(eRender, eConsole, true, r.notify, &e.enumerator, &e.notify);
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
    while (SUCCEEDED(h)) {
        if (!e.client && FAILED(open_stream(r, e))) {
            if (pause(r, 1000))
                break;
            continue;
        }
        HANDLE events[]{r.stop, r.notify, r.wake};
        const DWORD waited = WaitForMultipleObjects(3, events, FALSE, 200);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
            break;
        if (InterlockedExchange(&e.notify->dirty, 0) & kAudioEndpointDefaultChanged) {
            close_stream(e);
            continue;
        }
        const HRESULT filled = fill(r, e);
        if (FAILED(filled)) {
            // Device invalidated/removed or format change: reopen the default endpoint.
            close_stream(e);
            if (pause(r, filled == AUDCLNT_E_DEVICE_INVALIDATED ? 50 : 500))
                break;
        }
    }
    close_stream(e);
    audio_endpoint_unwatch(e.enumerator, e.notify);
    if (SUCCEEDED(apartment))
        CoUninitialize();
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
} // namespace maicord
