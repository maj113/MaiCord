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
