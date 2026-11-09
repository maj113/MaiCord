// Stream audio on Windows (include/maicord/platform/audio/stream_audio.hpp): WASAPI process loopback in 48 kHz stereo
// PCM16, from Windows 10 2004 (build 19041): the "VAD\Process_Loopback" virtual device activated with
// AUDIOCLIENT_ACTIVATION_PARAMS through ActivateAudioInterfaceAsync (mmdevapi.dll, resolved at run time). A screen
// records every process except MaiCord's tree; a window records its process tree. The virtual device follows the
// default playback device itself; the engine converts to the requested format (AUTOCONVERTPCM) and stamps each packet
// with its QPC time. Earlier builds have no loopback that leaves MaiCord out: sound sharing is unavailable there.
#include "maicord/platform/audio/stream_audio.hpp"
#include "capture_abi.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/sys/wasapi.hpp"

namespace maicord {
namespace {
using namespace capture_abi;

constexpr u32 kBuildProcessLoopback = 19041;
constexpr REFERENCE_TIME kBufferDuration = 400'000; // 40 ms in 100 ns units
constexpr u32 kSilenceFrames = 480;

u32 os_build() noexcept {
    return *reinterpret_cast<const volatile u32*>(0x7FFE0260ull) & 0xFFFFu;
}

struct Runtime {
    StreamAudioOptions options;
    StreamAudioSink sink;
    Thread* thread;
    HANDLE stop, wake, activated;
    i16 silence[kSilenceFrames * kStreamAudioChannels]; // zero (pages): fed for packets the engine flags silent
};
struct Engine {
    IAudioClient* client;
    IAudioCaptureClient* capture;
};

void close_stream(Engine& e) noexcept {
    if (e.client)
        e.client->Stop();
    com_release(e.capture);
    com_release(e.client);
}

// The process loopback client, activated asynchronously; waits for the completion (or stop).
HRESULT activate_process_loopback(Runtime& r, IAudioClient** client) noexcept {
    static const auto activate = [] {
        const HMODULE module = LoadLibraryExW(L"mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<ActivateAudioInterfaceFunction>(GetProcAddress(module, "ActivateAudioInterfaceAsync"))
                      : nullptr;
    }();
    if (!activate)
        return E_NOINTERFACE;
    const bool application = r.options.scope == StreamAudioScope::application && r.options.process_id;
    ActivationParams parameters{kActivationProcessLoopback,
                                application ? r.options.process_id : GetCurrentProcessId(),
                                application ? kLoopbackIncludeTree : kLoopbackExcludeTree};
    PROPVARIANT variant{};
    variant.vt = VT_BLOB;
    variant.blob.cbSize = sizeof(parameters);
    variant.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);
    SignalCallback* completion = signal_callback_create(r.activated, kIidActivateCompletionHandler);
    if (!completion)
        return E_OUTOFMEMORY;
    Object* operation = nullptr;
    HRESULT h = activate(kProcessLoopbackDevice, __uuidof(IAudioClient), &variant,
                         reinterpret_cast<IUnknown*>(completion), &operation);
    if (SUCCEEDED(h)) {
        HANDLE events[]{r.stop, r.activated};
        h = WaitForMultipleObjects(2, events, FALSE, 5000) == WAIT_OBJECT_0 + 1 ? S_OK : E_ABORT;
    }
    HRESULT activated = E_FAIL;
    IUnknown* unknown = nullptr;
    if (SUCCEEDED(h) && SUCCEEDED(h = call(operation, kGetActivateResult, &activated, &unknown)))
        h = FAILED(activated) ? activated : unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(client));
    com_release(unknown);
    release(operation);
    signal_callback_release(completion);
    return h;
}

HRESULT open_stream(Runtime& r, Engine& e) noexcept {
    HRESULT h = activate_process_loopback(r, &e.client);
    constexpr WAVEFORMATEX format{WAVE_FORMAT_PCM, kStreamAudioChannels, kStreamAudioRate,
                                  kStreamAudioRate * kStreamAudioChannels * 2, kStreamAudioChannels * 2, 16, 0};
    // The process loopback device takes the loopback, event and conversion flags alone.
    if (SUCCEEDED(h))
        h = e.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                     AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                                 kBufferDuration, 0, &format, nullptr);
    if (SUCCEEDED(h))
        h = e.client->SetEventHandle(r.wake);
    if (SUCCEEDED(h))
        h = e.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&e.capture));
    if (SUCCEEDED(h))
        h = e.client->Start();
    if (FAILED(h))
        close_stream(e);
    return h;
}

HRESULT drain(Runtime& r, Engine& e) noexcept {
    for (;;) {
        u32 frames = 0;
        HRESULT h = e.capture->GetNextPacketSize(&frames);
        if (FAILED(h) || !frames)
            return h;
        BYTE* data = nullptr;
        DWORD flags = 0;
        UINT64 qpc_100ns = 0;
        h = e.capture->GetBuffer(&data, &frames, &flags, nullptr, &qpc_100ns);
        if (FAILED(h))
            return h;
        // The packet's QPC time (100 ns units, the client clock); a device that gives none is stamped on arrival.
        const u64 timestamp = qpc_100ns ? qpc_100ns / 10 : clock_microseconds(clock_initialize(), 0, clock_now());
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            for (u32 at = 0; at < frames; at += kSilenceFrames) {
                const u32 piece = minimum(kSilenceFrames, frames - at);
                r.sink.block(r.sink.context, {r.silence, piece * kStreamAudioChannels},
                             timestamp + u64{at} * 1'000'000 / kStreamAudioRate);
            }
        } else if (frames) {
            r.sink.block(r.sink.context, {reinterpret_cast<const i16*>(data), frames * kStreamAudioChannels}, timestamp);
        }
        h = e.capture->ReleaseBuffer(frames);
        if (FAILED(h))
            return h;
    }
}

u32 capture_thread(void* parameter) noexcept {
    auto& r = *static_cast<Runtime*>(parameter);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const bool com = thread_com_enter();
    Engine e{};
    u64 retry_ms = 0;
    for (;;) {
        if (!e.client && GetTickCount64() >= retry_ms) {
            const HRESULT opened = open_stream(r, e);
            if (FAILED(opened)) {
                log_u64(LogLevel::warning, "stream_audio", "open_failed", static_cast<u32>(opened));
                retry_ms = GetTickCount64() + 1000;
            }
        }
        HANDLE events[]{r.stop, r.wake};
        const DWORD waited = WaitForMultipleObjects(2, events, FALSE, e.client ? 200 : 250);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED)
            break;
        if (e.client && FAILED(drain(r, e))) {
            close_stream(e);
            retry_ms = GetTickCount64() + 50;
        }
    }
    close_stream(e);
    if (com)
        thread_com_leave();
    return 0;
}

void free_runtime(Runtime* r) noexcept {
    if (r->thread)
        thread_close(r->thread);
    const HANDLE handles[]{r->stop, r->wake, r->activated};
    for (HANDLE handle : handles)
        if (handle)
            CloseHandle(handle);
    pages_release(r);
}
} // namespace

StreamAudioMethod stream_audio_method() noexcept {
    return os_build() >= kBuildProcessLoopback ? StreamAudioMethod::process_loopback
                                               : StreamAudioMethod::unavailable_windows_version;
}

bool stream_audio_start(StreamAudioCapture* capture, const StreamAudioOptions& options,
                        const StreamAudioSink& sink) noexcept {
    if (!capture || capture->runtime || !sink.block || !stream_audio_available(stream_audio_method()))
        return false;
    auto* r = commit_pages<Runtime>();
    if (!r)
        return false;
    r->options = options;
    r->sink = sink;
    r->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    r->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r->activated = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (r->stop && r->wake && r->activated)
        r->thread = thread_start(capture_thread, r, 256 * 1024, nullptr);
    if (!r->thread) {
        free_runtime(r);
        return false;
    }
    capture->runtime = r;
    return true;
}

void stream_audio_stop(StreamAudioCapture* capture) noexcept {
    if (!capture || !capture->runtime)
        return;
    auto* r = static_cast<Runtime*>(capture->runtime);
    SetEvent(r->stop);
    // A wedged audio driver must not free memory the thread may still use.
    if (!thread_wait(r->thread, 5000))
        return;
    free_runtime(r);
    capture->runtime = nullptr;
}

} // namespace maicord
