#pragma once

#include "maicord/core/base.hpp"
#include "maicord/sys/wasapi.hpp"

namespace maicord {

// Bits AudioEndpointNotify::dirty accumulates until the audio thread takes them.
inline constexpr LONG kAudioEndpointDefaultChanged = 1, kAudioEndpointTopologyChanged = 2;

// COM-owned endpoint observer for the voice capture and playback threads. It
// owns a duplicate of the thread's wake event, so a callback racing
// unregistration never touches the thread's runtime.
struct AudioEndpointNotify final : IMMNotificationClient {
    static void* operator new(usize, void* p) noexcept {
        return p;
    }
    volatile LONG references{1}, dirty{};
    HANDLE event{};
    EDataFlow flow{};
    ERole role{};
    bool follow_default{};
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
            this->~AudioEndpointNotify();
            CoTaskMemFree(this);
        }
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow changed_flow, ERole changed_role,
                                                     LPCWSTR) override {
        if (follow_default && changed_flow == flow && changed_role == role)
            signal(kAudioEndpointDefaultChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        signal(kAudioEndpointTopologyChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        signal(kAudioEndpointTopologyChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        return S_OK; // Loss of the open endpoint surfaces as AUDCLNT_E_DEVICE_INVALIDATED.
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }
};

// Text of an endpoint ID, compared as an opaque case-sensitive value: owned_id is NUL-terminated with
// owned_chars characters, and event_id (OS-provided) is read at most that far plus its terminator.
inline bool audio_endpoint_id_matches(const wchar_t* event_id, const wchar_t* owned_id, usize owned_chars) noexcept {
    if (!event_id || !owned_id || !owned_chars)
        return false;
    for (usize i = 0; i < owned_chars; ++i)
        if (!event_id[i] || event_id[i] != owned_id[i])
            return false;
    return !event_id[owned_chars];
}

// COM-owned observer pinned to one render endpoint (the media and notification playback threads). It
// owns a duplicate of the thread's wake event and the endpoint ID, which are written before
// registration and immutable through the final Release; `active` gates signals after unregistration.
// Signals the default-endpoint change for the console render role and that endpoint's state change,
// removal or device-format property change.
struct PinnedEndpointNotify final : IMMNotificationClient {
    static void* operator new(usize, void* p) noexcept {
        return p;
    }
    volatile LONG references{1}, active{1}, dirty{};
    HANDLE event{};
    LPWSTR id{};
    usize id_chars{};
    void signal(LONG flags) noexcept {
        if (InterlockedCompareExchange(&active, 0, 0)) {
            InterlockedOr(&dirty, flags);
            SetEvent(event);
        }
    }
    bool matches(LPCWSTR value) const noexcept {
        return audio_endpoint_id_matches(value, id, id_chars);
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
            CoTaskMemFree(id);
            this->~PinnedEndpointNotify();
            CoTaskMemFree(this);
        }
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole)
            signal(kAudioEndpointDefaultChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR value, DWORD) override {
        if (matches(value))
            signal(kAudioEndpointTopologyChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR value) override {
        if (matches(value))
            signal(kAudioEndpointTopologyChanged);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR value, const PROPERTYKEY key) override {
        // SDK PKEY_AudioEngine_DeviceFormat, as a local value to avoid a propsys/GUID import. Volume
        // and name changes do not restart active playback.
        constexpr GUID format{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}};
        if (!key.pid && key.fmtid == format && matches(value))
            signal(kAudioEndpointTopologyChanged);
        return S_OK;
    }
};

// Creates the device enumerator and registers an observer that signals wake
// on device arrival and state changes and, with follow_default, when the
// default flow/role endpoint changes. On failure the caller still releases
// whatever was acquired through audio_endpoint_unwatch.
inline HRESULT audio_endpoint_watch(EDataFlow flow, ERole role, bool follow_default, HANDLE wake,
                                    IMMDeviceEnumerator** enumerator,
                                    AudioEndpointNotify** notify) noexcept {
    const HRESULT h = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                       __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator));
    if (FAILED(h))
        return h;
    void* memory = CoTaskMemAlloc(sizeof(AudioEndpointNotify));
    if (!memory)
        return E_OUTOFMEMORY;
    auto* observer = new (memory) AudioEndpointNotify;
    observer->flow = flow;
    observer->role = role;
    observer->follow_default = follow_default;
    *notify = observer;
    if (!DuplicateHandle(GetCurrentProcess(), wake, GetCurrentProcess(), &observer->event, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return HRESULT_FROM_WIN32(GetLastError());
    return (*enumerator)->RegisterEndpointNotificationCallback(observer);
}

inline void audio_endpoint_unwatch(IMMDeviceEnumerator*& enumerator,
                                   AudioEndpointNotify*& notify) noexcept {
    if (enumerator && notify)
        enumerator->UnregisterEndpointNotificationCallback(notify);
    com_release(notify);
    com_release(enumerator);
}

} // namespace maicord
