#include "maicord/platform/voice_audio_capture.hpp"
#include "audio_endpoint_watch.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/sys/wasapi.hpp"

namespace maicord {
namespace {
constexpr u32 kRate = 48000;
constexpr REFERENCE_TIME kBufferDuration = 400'000; // 40 ms in 100 ns units.
// PKEY_Device_FriendlyName without INITGUID.
constexpr PROPERTYKEY kFriendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                                    14};

u64 now_us() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}
struct Runtime {
    VoiceAudioCaptureSink sink;
    VoiceAudioCaptureTick tick;
    void* context;
    HANDLE thread, stop, wake, notify, poke, timer;
    volatile LONG state, opens, native_error;
    wchar_t device[kVoiceDeviceIdCharacters];
    bool processing;
};
struct Engine {
    IMMDeviceEnumerator* enumerator;
    AudioEndpointNotify* notify;
    IAudioClient* client;
    IAudioCaptureClient* capture;
};
void publish(Runtime& r, VoiceCaptureState state, HRESULT native = S_OK) noexcept {
    InterlockedExchange(&r.state, static_cast<LONG>(state));
    if (FAILED(native))
        InterlockedExchange(&r.native_error, static_cast<LONG>(native));
}
void close_stream(Engine& e) noexcept {
    if (e.client)
        e.client->Stop();
    com_release(e.capture);
    com_release(e.client);
}
HRESULT drain(Runtime& r, Engine& e) noexcept {
    i16 silence[480]{};
    for (;;) {
        u32 frames = 0;
        HRESULT h = e.capture->GetNextPacketSize(&frames);
        if (FAILED(h) || !frames)
            return h;
        BYTE* data = nullptr;
        DWORD flags = 0;
        h = e.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(h))
            return h;
        const u64 now = now_us();
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            for (u32 at = 0; at < frames; at += 480)
                r.sink(r.context, {silence, minimum(480u, frames - at)}, now);
        } else if (frames)
            r.sink(r.context, {reinterpret_cast<const i16*>(data), frames}, now);
        h = e.capture->ReleaseBuffer(frames);
        if (FAILED(h))
            return h;
    }
}
IMMDevice* endpoint(Runtime& r, Engine& e) noexcept {
    IMMDevice* device = nullptr;
    if (r.device[0] && SUCCEEDED(e.enumerator->GetDevice(r.device, &device))) {
        DWORD state = 0;
        if (SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE)
            return device;
        com_release(device);
    }
    // A missing chosen device falls back to the communications default.
    e.enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &device);
    return device;
}
HRESULT initialize(Engine& e, bool processing, bool raw) noexcept {
    IAudioClient2* client2 = nullptr;
    if (SUCCEEDED(e.client->QueryInterface(__uuidof(IAudioClient2), reinterpret_cast<void**>(&client2)))) {
        AudioClientProperties properties{};
        properties.cbSize = sizeof(properties);
        properties.eCategory = processing ? AudioCategory_Communications : AudioCategory_Other;
        properties.Options = raw ? AUDCLNT_STREAMOPTIONS_RAW : AUDCLNT_STREAMOPTIONS_NONE;
        const HRESULT h = client2->SetClientProperties(&properties);
        com_release(client2);
        if (FAILED(h) && raw)
            return h;
    }
    constexpr WAVEFORMATEX format{WAVE_FORMAT_PCM, 1, kRate, kRate * 2, 2, 16, 0};
    return e.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                kBufferDuration, 0, &format, nullptr);
}
HRESULT activate(Engine& e, IMMDevice* device) noexcept {
    com_release(e.client);
    return device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                            reinterpret_cast<void**>(&e.client));
}
HRESULT open_stream(Runtime& r, Engine& e) noexcept {
    IMMDevice* device = endpoint(r, e);
    HRESULT h = device ? activate(e, device) : E_NOTFOUND;
    // Raw bypasses the endpoint's processing; drivers without raw support
    // reject it, so retry with default processing on a fresh client.
    const bool raw = !r.processing;
    if (SUCCEEDED(h)) {
        h = initialize(e, r.processing, raw);
        if (FAILED(h) && raw && SUCCEEDED(activate(e, device)))
            h = initialize(e, false, false);
    }
    com_release(device);
    if (SUCCEEDED(h))
        h = e.client->SetEventHandle(r.wake);
    if (SUCCEEDED(h))
        h = e.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&e.capture));
    if (SUCCEEDED(h))
        h = e.client->Start();
    if (FAILED(h)) {
        close_stream(e);
        return h;
    }
    InterlockedIncrement(&r.opens);
    return h;
}
HRESULT watch(Runtime& r, Engine& e) noexcept {
    return audio_endpoint_watch(eCapture, eCommunications, !r.device[0], r.notify, &e.enumerator, &e.notify);
}
void arm(Runtime& r, u64 wake_us) noexcept {
    if (!wake_us || !r.timer)
        return;
    const u64 now = now_us();
    LARGE_INTEGER due{};
    due.QuadPart = wake_us > now ? -static_cast<i64>((wake_us - now) * 10) : -1;
    SetWaitableTimer(r.timer, &due, 0, nullptr, nullptr, FALSE);
}
DWORD WINAPI capture_thread(void* parameter) noexcept {
    auto& r = *static_cast<Runtime*>(parameter);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Engine e{};
    HRESULT h = FAILED(apartment) ? apartment : watch(r, e);
    if (FAILED(h))
        publish(r, VoiceCaptureState::waiting, h);
    u64 retry_us = 0;
    while (SUCCEEDED(h)) {
        if (!e.client && now_us() >= retry_us) {
            publish(r, VoiceCaptureState::opening);
            const HRESULT opened = open_stream(r, e);
            if (FAILED(opened)) {
                publish(r, VoiceCaptureState::waiting, opened);
                retry_us = now_us() + 1'000'000;
            } else
                publish(r, VoiceCaptureState::capturing);
        }
        HANDLE events[]{r.stop, r.notify, r.wake, r.poke, r.timer};
        const DWORD waited = WaitForMultipleObjects(r.timer ? 5 : 4, events, FALSE, e.client ? 200 : 250);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
            break;
        if (InterlockedExchange(&e.notify->dirty, 0) & kAudioEndpointDefaultChanged)
            close_stream(e);
        if (e.client) {
            const HRESULT drained = drain(r, e);
            if (FAILED(drained)) {
                // Device invalidated/removed or format change: reopen soon.
                close_stream(e);
                publish(r, VoiceCaptureState::waiting, drained);
                retry_us = now_us() + (drained == AUDCLNT_E_DEVICE_INVALIDATED ? 50'000 : 500'000);
            }
        }
        arm(r, r.tick(r.context, now_us()));
    }
    close_stream(e);
    audio_endpoint_unwatch(e.enumerator, e.notify);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    publish(r, VoiceCaptureState::stopped);
    return 0;
}
void free_runtime(Runtime* r) noexcept {
    const HANDLE handles[]{r->thread, r->stop, r->wake, r->notify, r->poke, r->timer};
    for (HANDLE handle : handles)
        if (handle)
            CloseHandle(handle);
    VirtualFree(r, 0, MEM_RELEASE);
}
} // namespace

bool voice_audio_capture_start(VoiceAudioCapture* capture, const wchar_t* device_id, bool communications,
                               VoiceAudioCaptureSink sink, VoiceAudioCaptureTick tick, void* context) noexcept {
    if (!capture || capture->runtime || !sink || !tick)
        return false;
    auto* r = static_cast<Runtime*>(VirtualAlloc(nullptr, sizeof(Runtime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!r)
        return false;
    r->sink = sink;
    r->tick = tick;
    r->context = context;
    r->processing = communications;
    for (u32 i = 0; device_id && i + 1 < kVoiceDeviceIdCharacters && device_id[i]; ++i)
        r->device[i] = device_id[i];
    r->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    r->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r->notify = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r->poke = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // High resolution keeps 20 ms pacing wakes within about a millisecond.
    r->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!r->timer)
        r->timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    r->state = static_cast<LONG>(VoiceCaptureState::opening);
    if (r->stop && r->wake && r->notify && r->poke)
        r->thread =
            CreateThread(nullptr, 256 * 1024, capture_thread, r, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!r->thread) {
        free_runtime(r);
        return false;
    }
    capture->runtime = r;
    return true;
}

void voice_audio_capture_stop(VoiceAudioCapture* capture) noexcept {
    if (!capture || !capture->runtime)
        return;
    auto* r = static_cast<Runtime*>(capture->runtime);
    SetEvent(r->stop);
    // A wedged audio driver must not free memory the thread may still use.
    if (WaitForSingleObject(r->thread, 5000) != WAIT_OBJECT_0)
        return;
    free_runtime(r);
    capture->runtime = nullptr;
}

bool voice_audio_capture_running(const VoiceAudioCapture& capture) noexcept {
    return capture.runtime != nullptr;
}

VoiceAudioCaptureStatus voice_audio_capture_status(const VoiceAudioCapture& capture) noexcept {
    VoiceAudioCaptureStatus status{};
    if (const auto* r = static_cast<const Runtime*>(capture.runtime)) {
        status.state = static_cast<VoiceCaptureState>(ReadAcquire(&r->state));
        status.opens = static_cast<u32>(ReadAcquire(&r->opens));
        status.native_error = static_cast<u32>(ReadAcquire(&r->native_error));
    }
    return status;
}

u32 voice_audio_devices(bool capture, Slice<VoiceAudioDevice> output) noexcept {
    if (!output.data || !output.count)
        return 0;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* devices = nullptr;
    IMMDevice* fallback = nullptr;
    LPWSTR default_id = nullptr;
    u32 count = 0, total = 0;
    const EDataFlow flow = capture ? eCapture : eRender;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) &&
        SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices)) &&
        SUCCEEDED(devices->GetCount(&total))) {
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, capture ? eCommunications : eConsole, &fallback)))
            fallback->GetId(&default_id);
        for (u32 i = 0; i < total && count < output.count; ++i) {
            IMMDevice* device = nullptr;
            IPropertyStore* store = nullptr;
            LPWSTR id = nullptr;
            PROPVARIANT name{};
            if (SUCCEEDED(devices->Item(i, &device)) && SUCCEEDED(device->GetId(&id)) &&
                SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) &&
                SUCCEEDED(store->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR) {
                u32 length = 0;
                while (id[length])
                    ++length;
                if (length < kVoiceDeviceIdCharacters) {
                    auto& entry = output[count];
                    reset_value(&entry);
                    copy_bytes(entry.id, id, length * static_cast<u32>(sizeof(wchar_t)));
                    char utf8[512];
                    const i32 bytes = WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, utf8, sizeof(utf8),
                                                          nullptr, nullptr);
                    u32 used = bytes > 1 ? static_cast<u32>(bytes - 1) : 0;
                    if (used >= kVoiceAudioDeviceNameBytes) {
                        used = kVoiceAudioDeviceNameBytes - 1;
                        while (used && (static_cast<u8>(utf8[used]) & 0xc0) == 0x80)
                            --used; // Keep whole UTF-8 sequences.
                    }
                    copy_bytes(entry.name, utf8, used);
                    entry.name_bytes = used;
                    u32 at = 0;
                    entry.is_default = default_id != nullptr;
                    while (entry.is_default && (default_id[at] || id[at]))
                        entry.is_default = default_id[at] == id[at], ++at;
                    if (entry.is_default && count) {
                        // Default first.
                        const VoiceAudioDevice moved = entry;
                        for (u32 k = count; k > 0; --k)
                            copy_value(&output[k], output[k - 1]);
                        copy_value(&output[0], moved);
                    }
                    ++count;
                }
            }
            PropVariantClear(&name);
            if (id)
                CoTaskMemFree(id);
            com_release(store);
            com_release(device);
        }
    }
    if (default_id)
        CoTaskMemFree(default_id);
    com_release(fallback);
    com_release(devices);
    com_release(enumerator);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return count;
}
} // namespace maicord
