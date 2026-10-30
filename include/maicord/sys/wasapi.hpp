#pragma once

// WASAPI, the MMDevice API and wave formats, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/ole.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

// <mmeapi.h>
#define WAVE_FORMAT_PCM 1

// <mmdeviceapi.h>
#define E_NOTFOUND HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
#define DEVICE_STATE_ACTIVE 0x00000001
#define DEVICE_STATE_DISABLED 0x00000002

// <mmreg.h>
#define WAVE_FORMAT_IEEE_FLOAT 0x0003

// <AudioSessionTypes.h>
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK 0x00040000
#define AUDCLNT_STREAMFLAGS_NOPERSIST 0x00080000
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000

// <audioclient.h>
#define AUDCLNT_ERR(n) MAKE_HRESULT(SEVERITY_ERROR, FACILITY_AUDCLNT, n)
#define AUDCLNT_E_DEVICE_INVALIDATED AUDCLNT_ERR(0x004)

typedef LONGLONG REFERENCE_TIME;

MAICORD_SYS_TYPES_BEGIN
struct IAudioCaptureClient;
struct IAudioClient2;
struct IAudioClient;
struct IAudioClock;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceCollection;
struct IMMDeviceEnumerator;
struct IMMNotificationClient;
struct ISimpleAudioVolume;

// <mmeapi.h>
#pragma pack(push, 1)
struct tWAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
};
#pragma pack(pop)
typedef tWAVEFORMATEX WAVEFORMATEX;
// <mmdeviceapi.h>
enum __MIDL___MIDL_itf_mmdeviceapi_0000_0000_0001 {
    eRender = 0,
    eCapture = (eRender + 1),
};
typedef __MIDL___MIDL_itf_mmdeviceapi_0000_0000_0001 EDataFlow;
enum __MIDL___MIDL_itf_mmdeviceapi_0000_0000_0002 {
    eConsole = 0,
    eCommunications = 2,
};
typedef __MIDL___MIDL_itf_mmdeviceapi_0000_0000_0002 ERole;
struct __declspec(uuid("7991EEC9-7E89-4D85-8390-6C703CEC60C0")) __declspec(novtable) IMMNotificationClient : IUnknown {
    virtual HRESULT __stdcall OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) = 0;
    virtual HRESULT __stdcall OnDeviceAdded(LPCWSTR pwstrDeviceId) = 0;
    virtual HRESULT __stdcall OnDeviceRemoved(LPCWSTR pwstrDeviceId) = 0;
    virtual HRESULT __stdcall OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) = 0;
    virtual HRESULT __stdcall OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) = 0;
};
struct __declspec(uuid("D666063F-1587-4E43-81F1-B948E807363F")) __declspec(novtable) IMMDevice : IUnknown {
    virtual HRESULT __stdcall Activate(const IID& iid, DWORD dwClsCtx, PROPVARIANT* pActivationParams,
        void** ppInterface) = 0;
    virtual HRESULT __stdcall OpenPropertyStore(DWORD stgmAccess, IPropertyStore** ppProperties) = 0;
    virtual HRESULT __stdcall GetId(LPWSTR* ppstrId) = 0;
    virtual HRESULT __stdcall GetState(DWORD* pdwState) = 0;
};
struct __declspec(uuid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E")) __declspec(novtable) IMMDeviceCollection : IUnknown {
    virtual HRESULT __stdcall GetCount(UINT* pcDevices) = 0;
    virtual HRESULT __stdcall Item(UINT nDevice, IMMDevice** ppDevice) = 0;
};
struct __declspec(uuid("A95664D2-9614-4F35-A746-DE8DB63617E6")) __declspec(novtable) IMMDeviceEnumerator : IUnknown {
    virtual HRESULT __stdcall EnumAudioEndpoints(EDataFlow dataFlow, DWORD dwStateMask,
        IMMDeviceCollection** ppDevices) = 0;
    virtual HRESULT __stdcall GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, IMMDevice** ppEndpoint) = 0;
    virtual HRESULT __stdcall GetDevice(LPCWSTR pwstrId, IMMDevice** ppDevice) = 0;
    virtual HRESULT __stdcall RegisterEndpointNotificationCallback(IMMNotificationClient* pClient) = 0;
    virtual HRESULT __stdcall UnregisterEndpointNotificationCallback(IMMNotificationClient* pClient) = 0;
};
class __declspec(uuid("BCDE0395-E52F-467C-8E3D-C4579291692E")) MMDeviceEnumerator;
// <AudioSessionTypes.h>
enum _AUDCLNT_SHAREMODE {
    AUDCLNT_SHAREMODE_SHARED,
};
typedef _AUDCLNT_SHAREMODE AUDCLNT_SHAREMODE;
enum _AUDIO_STREAM_CATEGORY {
    AudioCategory_Other = 0,
    AudioCategory_Communications = 3,
};
typedef _AUDIO_STREAM_CATEGORY AUDIO_STREAM_CATEGORY;
// <audioclient.h>
enum _AUDCLNT_BUFFERFLAGS {
    AUDCLNT_BUFFERFLAGS_SILENT = 0x2,
};
enum AUDCLNT_STREAMOPTIONS {
    AUDCLNT_STREAMOPTIONS_NONE = 0,
    AUDCLNT_STREAMOPTIONS_RAW = 0x1,
};
struct AudioClientProperties {
    UINT32 cbSize;
    BOOL bIsOffload;
    AUDIO_STREAM_CATEGORY eCategory;
    AUDCLNT_STREAMOPTIONS Options;
};
struct __declspec(uuid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2")) __declspec(novtable) IAudioClient : IUnknown {
    virtual HRESULT __stdcall Initialize(AUDCLNT_SHAREMODE ShareMode, DWORD StreamFlags,
        REFERENCE_TIME hnsBufferDuration, REFERENCE_TIME hnsPeriodicity, const WAVEFORMATEX* pFormat,
        LPCGUID AudioSessionGuid) = 0;
    virtual HRESULT __stdcall GetBufferSize(UINT32* pNumBufferFrames) = 0;
    virtual HRESULT __stdcall GetStreamLatency(REFERENCE_TIME* phnsLatency) = 0;
    virtual HRESULT __stdcall GetCurrentPadding(UINT32* pNumPaddingFrames) = 0;
    virtual HRESULT __stdcall IsFormatSupported(AUDCLNT_SHAREMODE ShareMode, const WAVEFORMATEX* pFormat,
        WAVEFORMATEX** ppClosestMatch) = 0;
    virtual HRESULT __stdcall GetMixFormat(WAVEFORMATEX** ppDeviceFormat) = 0;
    virtual HRESULT __stdcall GetDevicePeriod(REFERENCE_TIME* phnsDefaultDevicePeriod,
        REFERENCE_TIME* phnsMinimumDevicePeriod) = 0;
    virtual HRESULT __stdcall Start() = 0;
    virtual HRESULT __stdcall Stop() = 0;
    virtual HRESULT __stdcall Reset() = 0;
    virtual HRESULT __stdcall SetEventHandle(HANDLE eventHandle) = 0;
    virtual HRESULT __stdcall GetService(const IID& riid, void** ppv) = 0;
};
struct __declspec(uuid("726778CD-F60A-4eda-82DE-E47610CD78AA")) __declspec(novtable) IAudioClient2 : IAudioClient {
    MAICORD_COM_UNUSED(IsOffloadCapable);
    virtual HRESULT __stdcall SetClientProperties(const AudioClientProperties* pProperties) = 0;
};
struct __declspec(uuid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2")) __declspec(novtable) IAudioRenderClient : IUnknown {
    virtual HRESULT __stdcall GetBuffer(UINT32 NumFramesRequested, BYTE** ppData) = 0;
    virtual HRESULT __stdcall ReleaseBuffer(UINT32 NumFramesWritten, DWORD dwFlags) = 0;
};
struct __declspec(uuid("C8ADBD64-E71E-48a0-A4DE-185C395CD317")) __declspec(novtable) IAudioCaptureClient : IUnknown {
    virtual HRESULT __stdcall GetBuffer(BYTE** ppData, UINT32* pNumFramesToRead, DWORD* pdwFlags,
        UINT64* pu64DevicePosition, UINT64* pu64QPCPosition) = 0;
    virtual HRESULT __stdcall ReleaseBuffer(UINT32 NumFramesRead) = 0;
    virtual HRESULT __stdcall GetNextPacketSize(UINT32* pNumFramesInNextPacket) = 0;
};
struct __declspec(uuid("CD63314F-3FBA-4a1b-812C-EF96358728E7")) __declspec(novtable) IAudioClock : IUnknown {
    virtual HRESULT __stdcall GetFrequency(UINT64* pu64Frequency) = 0;
    virtual HRESULT __stdcall GetPosition(UINT64* pu64Position, UINT64* pu64QPCPosition) = 0;
};
struct __declspec(uuid("87CE5498-68D6-44E5-9215-6DA47EF883D8")) __declspec(novtable) ISimpleAudioVolume : IUnknown {
    virtual HRESULT __stdcall SetMasterVolume(float fLevel, LPCGUID EventContext) = 0;
};
MAICORD_SYS_TYPES_END
