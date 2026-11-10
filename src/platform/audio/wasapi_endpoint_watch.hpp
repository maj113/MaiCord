#pragma once

#include "maicord/core/base.hpp"
#include "maicord/platform/audio/endpoint_watch.hpp"
#include "maicord/sys/wasapi.hpp"

namespace maicord {

// The WASAPI half of endpoint watching: the change bits (kAudioEndpoint*) and the ID comparison are shared with
// Linux in maicord/platform/audio/endpoint_watch.hpp.

// IUnknown of the COM-owned endpoint observers, which live in CoTaskMem storage. Each owns a
// duplicate of its audio thread's wake event, so a callback racing unregistration never touches the
// thread's runtime. Self releases its own members in release_members before the final destruction.
template <class Self> struct EndpointNotifyBase : IMMNotificationClient {
    static void* operator new(usize, void* p) noexcept {
        return p;
    }
    volatile LONG references{1}, dirty{};
    HANDLE event{};
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
            auto* self = static_cast<Self*>(this);
            if (event)
                CloseHandle(event);
            self->release_members();
            self->~Self();
            CoTaskMemFree(self);
        }
        return static_cast<ULONG>(count);
    }
    void signal(LONG flags) noexcept {
        InterlockedOr(&dirty, flags);
        SetEvent(event);
    }
};

// Endpoint observer for the voice capture and playback threads.
struct AudioEndpointNotify final : EndpointNotifyBase<AudioEndpointNotify> {
    EDataFlow flow{};
    ERole role{};
    bool follow_default{};
    void release_members() noexcept {}
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

// COM-owned observer pinned to one render endpoint (the media and notification playback threads). The
// endpoint ID is written before registration and immutable through the final Release; `active` gates
// signals after unregistration. Signals the default-endpoint change for the console render role and
// that endpoint's state change, removal or device-format property change.
struct PinnedEndpointNotify final : EndpointNotifyBase<PinnedEndpointNotify> {
    volatile LONG active{1};
    LPWSTR id{};
    usize id_chars{};
    void release_members() noexcept {
        CoTaskMemFree(id);
    }
    void signal(LONG flags) noexcept {
        if (InterlockedCompareExchange(&active, 0, 0))
            EndpointNotifyBase::signal(flags);
    }
    bool matches(LPCWSTR value) const noexcept {
        return audio_endpoint_id_matches(value, id, id_chars);
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

// The media and notification playback threads share the life of their pinned observer. Each owns the observer
// (*callback), its device enumerator and, under `lock`, a count of the wake-event duplicates the observers hold
// (*handles). One out-of-line body of each serves both threads.

// Unregisters and releases the observer.
__declspec(noinline) inline void pinned_endpoint_close(IMMDeviceEnumerator* enumerator, PinnedEndpointNotify** callback,
                                                       SRWLOCK* lock, u32* handles) noexcept {
    PinnedEndpointNotify* const pinned = *callback;
    const bool attached = pinned && pinned->event;
    if (pinned)
        InterlockedExchange(&pinned->active, 0);
    if (enumerator && pinned)
        enumerator->UnregisterEndpointNotificationCallback(pinned);
    com_release(*callback);
    if (attached) {
        AcquireSRWLockExclusive(lock);
        --*handles;
        ReleaseSRWLockExclusive(lock);
    }
}

// Pins the observer to the endpoint `*id` (taking over the ID's memory), unless it already is; its wake event is a
// duplicate of `wake`.
__declspec(noinline) inline HRESULT pinned_endpoint_refresh(IMMDeviceEnumerator* enumerator,
                                                            PinnedEndpointNotify** callback, HANDLE wake, SRWLOCK* lock,
                                                            u32* handles, LPWSTR* id) noexcept {
    if (!*id || !**id)
        return E_UNEXPECTED;
    if (*callback && (*callback)->matches(*id))
        return S_OK;
    pinned_endpoint_close(enumerator, callback, lock, handles);
    void* memory = CoTaskMemAlloc(sizeof(PinnedEndpointNotify));
    if (!memory)
        return E_OUTOFMEMORY;
    auto* pinned = new (memory) PinnedEndpointNotify;
    *callback = pinned;
    pinned->id = *id;
    *id = nullptr;
    // GetId supplies a terminated full opaque ID; no guessed truncation/hash.
    while (pinned->id[pinned->id_chars])
        ++pinned->id_chars;
    HRESULT h = S_OK;
    if (!DuplicateHandle(GetCurrentProcess(), wake, GetCurrentProcess(), &pinned->event, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        h = HRESULT_FROM_WIN32(GetLastError());
    } else {
        AcquireSRWLockExclusive(lock);
        ++*handles;
        ReleaseSRWLockExclusive(lock);
    }
    if (SUCCEEDED(h))
        h = enumerator->RegisterEndpointNotificationCallback(pinned);
    if (FAILED(h))
        pinned_endpoint_close(enumerator, callback, lock, handles);
    return h;
}

// Whether the console render default is no longer the endpoint the observer is pinned to (the notification of the
// change is advisory: revalidated on the worker before an unchanged stream is disturbed).
__declspec(noinline) inline bool pinned_endpoint_default_changed(IMMDeviceEnumerator* enumerator,
                                                                 const PinnedEndpointNotify* callback) noexcept {
    IMMDevice* device = nullptr;
    LPWSTR id = nullptr;
    HRESULT h = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(h))
        h = device->GetId(&id);
    const bool changed = FAILED(h) || !callback || !callback->matches(id);
    CoTaskMemFree(id);
    com_release(device);
    return changed;
}

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
