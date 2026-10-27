#pragma once

// Media Foundation, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/ole.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <mfapi.h>
#include <mftransform.h>
#include <mferror.h>
#endif

// <mfapi.h>
#define MF_SDK_VERSION 0x0002
#define MF_API_VERSION 0x0070
#define MF_VERSION (MF_SDK_VERSION << 16 | MF_API_VERSION)
#define MFSTARTUP_NOSOCKET 0x1
#define MFSTARTUP_LITE (MFSTARTUP_NOSOCKET)

// <mferror.h>
#define MF_E_BUFFERTOOSMALL _HRESULT_TYPEDEF_(0xC00D36B1L)
#define MF_E_INVALIDSTREAMNUMBER _HRESULT_TYPEDEF_(0xC00D36B3L)
#define MF_E_INVALIDMEDIATYPE _HRESULT_TYPEDEF_(0xC00D36B4L)
#define MF_E_NOTACCEPTING _HRESULT_TYPEDEF_(0xC00D36B5L)
#define MF_E_NO_MORE_TYPES _HRESULT_TYPEDEF_(0xC00D36B9L)
#define MF_E_TRANSFORM_STREAM_CHANGE _HRESULT_TYPEDEF_(0xC00D6D61L)
#define MF_E_TRANSFORM_NEED_MORE_INPUT _HRESULT_TYPEDEF_(0xC00D6D72L)

MAICORD_SYS_TYPES_BEGIN
struct IMFAttributes;
struct IMFCollection;
struct IMFMediaBuffer;
struct IMFMediaType;
struct IMFSample;
struct IMFTransform;

// <mfobjects.h>
enum _MF_ATTRIBUTES_MATCH_TYPE {
    MF_ATTRIBUTES_MATCH_OUR_ITEMS = 0,
};
typedef _MF_ATTRIBUTES_MATCH_TYPE MF_ATTRIBUTES_MATCH_TYPE;
struct __declspec(uuid("2cd2d921-c447-44a7-a13c-4adabfc247e3")) __declspec(novtable) IMFAttributes : IUnknown {
    virtual HRESULT __stdcall GetItem(const GUID& guidKey, PROPVARIANT* pValue) = 0;
    MAICORD_COM_UNUSED(GetItemType);
    MAICORD_COM_UNUSED(CompareItem);
    virtual HRESULT __stdcall Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult) = 0;
    virtual HRESULT __stdcall GetUINT32(const GUID& guidKey, UINT32* punValue) = 0;
    MAICORD_COM_UNUSED(GetUINT64);
    MAICORD_COM_UNUSED(GetDouble);
    virtual HRESULT __stdcall GetGUID(const GUID& guidKey, GUID* pguidValue) = 0;
    virtual HRESULT __stdcall GetStringLength(const GUID& guidKey, UINT32* pcchLength) = 0;
    virtual HRESULT __stdcall GetString(const GUID& guidKey, LPWSTR pwszValue, UINT32 cchBufSize,
        UINT32* pcchLength) = 0;
    MAICORD_COM_UNUSED(GetAllocatedString);
    MAICORD_COM_UNUSED(GetBlobSize);
    MAICORD_COM_UNUSED(GetBlob);
    MAICORD_COM_UNUSED(GetAllocatedBlob);
    MAICORD_COM_UNUSED(GetUnknown);
    MAICORD_COM_UNUSED(SetItem);
    MAICORD_COM_UNUSED(DeleteItem);
    MAICORD_COM_UNUSED(DeleteAllItems);
    virtual HRESULT __stdcall SetUINT32(const GUID& guidKey, UINT32 unValue) = 0;
    MAICORD_COM_UNUSED(SetUINT64);
    MAICORD_COM_UNUSED(SetDouble);
    virtual HRESULT __stdcall SetGUID(const GUID& guidKey, const GUID& guidValue) = 0;
    MAICORD_COM_UNUSED(SetString);
    virtual HRESULT __stdcall SetBlob(const GUID& guidKey, const UINT8* pBuf, UINT32 cbBufSize) = 0;
    MAICORD_COM_UNUSED(SetUnknown);
    MAICORD_COM_UNUSED(LockStore);
    MAICORD_COM_UNUSED(UnlockStore);
    virtual HRESULT __stdcall GetCount(UINT32* pcItems) = 0;
    MAICORD_COM_UNUSED(GetItemByIndex);
    MAICORD_COM_UNUSED(CopyAllItems);
};
struct __declspec(uuid("045FA593-8799-42b8-BC8D-8968C6453507")) __declspec(novtable) IMFMediaBuffer : IUnknown {
    virtual HRESULT __stdcall Lock(BYTE** ppbBuffer, DWORD* pcbMaxLength, DWORD* pcbCurrentLength) = 0;
    virtual HRESULT __stdcall Unlock() = 0;
    MAICORD_COM_UNUSED(GetCurrentLength);
    virtual HRESULT __stdcall SetCurrentLength(DWORD cbCurrentLength) = 0;
};
struct __declspec(uuid("c40a00f2-b93a-4d80-ae8c-5a1c634f58e4")) __declspec(novtable) IMFSample : IMFAttributes {
    MAICORD_COM_UNUSED(GetSampleFlags);
    MAICORD_COM_UNUSED(SetSampleFlags);
    virtual HRESULT __stdcall GetSampleTime(LONGLONG* phnsSampleTime) = 0;
    virtual HRESULT __stdcall SetSampleTime(LONGLONG hnsSampleTime) = 0;
    MAICORD_COM_UNUSED(GetSampleDuration);
    virtual HRESULT __stdcall SetSampleDuration(LONGLONG hnsSampleDuration) = 0;
    MAICORD_COM_UNUSED(GetBufferCount);
    MAICORD_COM_UNUSED(GetBufferByIndex);
    MAICORD_COM_UNUSED(ConvertToContiguousBuffer);
    virtual HRESULT __stdcall AddBuffer(IMFMediaBuffer* pBuffer) = 0;
};
struct __declspec(uuid("44ae0fa8-ea31-4109-8d2e-4cae4997c555")) __declspec(novtable) IMFMediaType : IMFAttributes {
};
struct __declspec(uuid("5BC8A76B-869A-46a3-9B03-FA218A66AEBE")) __declspec(novtable) IMFCollection : IUnknown {
};
// <mftransform.h>
enum _MFT_OUTPUT_DATA_BUFFER_FLAGS {
    MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE = 0x300
};
enum _MFT_OUTPUT_STREAM_INFO_FLAGS {
    MFT_OUTPUT_STREAM_PROVIDES_SAMPLES = 0x100,
};
enum _MFT_MESSAGE_TYPE {
    MFT_MESSAGE_COMMAND_FLUSH = 0,
    MFT_MESSAGE_COMMAND_DRAIN = 0x1,
    MFT_MESSAGE_SET_D3D_MANAGER = 0x2,
    MFT_MESSAGE_NOTIFY_BEGIN_STREAMING = 0x10000000,
    MFT_MESSAGE_NOTIFY_END_STREAMING = 0x10000001,
    MFT_MESSAGE_NOTIFY_END_OF_STREAM = 0x10000002,
    MFT_MESSAGE_NOTIFY_START_OF_STREAM = 0x10000003,
};
typedef _MFT_MESSAGE_TYPE MFT_MESSAGE_TYPE;
struct _MFT_INPUT_STREAM_INFO {
    LONGLONG hnsMaxLatency;
    DWORD dwFlags;
    DWORD cbSize;
    DWORD cbMaxLookahead;
    DWORD cbAlignment;
};
typedef _MFT_INPUT_STREAM_INFO MFT_INPUT_STREAM_INFO;
struct _MFT_OUTPUT_STREAM_INFO {
    DWORD dwFlags;
    DWORD cbSize;
    DWORD cbAlignment;
};
typedef _MFT_OUTPUT_STREAM_INFO MFT_OUTPUT_STREAM_INFO;
struct _MFT_OUTPUT_DATA_BUFFER {
    DWORD dwStreamID;
    IMFSample* pSample;
    DWORD dwStatus;
    IMFCollection* pEvents;
};
typedef _MFT_OUTPUT_DATA_BUFFER MFT_OUTPUT_DATA_BUFFER;
struct __declspec(uuid("bf94c121-5b05-4e6f-8000-ba598961414d")) __declspec(novtable) IMFTransform : IUnknown {
    MAICORD_COM_UNUSED(GetStreamLimits);
    virtual HRESULT __stdcall GetStreamCount(DWORD* pcInputStreams, DWORD* pcOutputStreams) = 0;
    virtual HRESULT __stdcall GetStreamIDs(DWORD dwInputIDArraySize, DWORD* pdwInputIDs, DWORD dwOutputIDArraySize,
        DWORD* pdwOutputIDs) = 0;
    virtual HRESULT __stdcall GetInputStreamInfo(DWORD dwInputStreamID, MFT_INPUT_STREAM_INFO* pStreamInfo) = 0;
    virtual HRESULT __stdcall GetOutputStreamInfo(DWORD dwOutputStreamID, MFT_OUTPUT_STREAM_INFO* pStreamInfo) = 0;
    MAICORD_COM_UNUSED(GetAttributes);
    MAICORD_COM_UNUSED(GetInputStreamAttributes);
    MAICORD_COM_UNUSED(GetOutputStreamAttributes);
    MAICORD_COM_UNUSED(DeleteInputStream);
    MAICORD_COM_UNUSED(AddInputStreams);
    MAICORD_COM_UNUSED(GetInputAvailableType);
    virtual HRESULT __stdcall GetOutputAvailableType(DWORD dwOutputStreamID, DWORD dwTypeIndex,
        IMFMediaType** ppType) = 0;
    virtual HRESULT __stdcall SetInputType(DWORD dwInputStreamID, IMFMediaType* pType, DWORD dwFlags) = 0;
    virtual HRESULT __stdcall SetOutputType(DWORD dwOutputStreamID, IMFMediaType* pType, DWORD dwFlags) = 0;
    MAICORD_COM_UNUSED(GetInputCurrentType);
    MAICORD_COM_UNUSED(GetOutputCurrentType);
    MAICORD_COM_UNUSED(GetInputStatus);
    MAICORD_COM_UNUSED(GetOutputStatus);
    MAICORD_COM_UNUSED(SetOutputBounds);
    MAICORD_COM_UNUSED(ProcessEvent);
    virtual HRESULT __stdcall ProcessMessage(MFT_MESSAGE_TYPE eMessage, ULONG_PTR ulParam) = 0;
    virtual HRESULT __stdcall ProcessInput(DWORD dwInputStreamID, IMFSample* pSample, DWORD dwFlags) = 0;
    virtual HRESULT __stdcall ProcessOutput(DWORD dwFlags, DWORD cOutputBufferCount,
        MFT_OUTPUT_DATA_BUFFER* pOutputSamples, DWORD* pdwStatus) = 0;
};
MAICORD_SYS_TYPES_END

// <mfapi.h>
DEFINE_GUID(MFSampleExtension_Discontinuity, 0x9cdf01d9, 0xa0f0, 0x43ba, 0xb0, 0x77, 0xea, 0xa0, 0x6c, 0xbd, 0x72, 0x8a);
DEFINE_GUID(MFAudioFormat_PCM, 1, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(MFAudioFormat_Float, 0x0003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(MFAudioFormat_AAC, 0x1610, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(MF_MT_MAJOR_TYPE, 0x48eba18e, 0xf8c9, 0x4687, 0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f);
DEFINE_GUID(MF_MT_SUBTYPE, 0xf7e34c9a, 0x42e8, 0x4714, 0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5);
DEFINE_GUID(MF_MT_AUDIO_NUM_CHANNELS, 0x37e48bf5, 0x645e, 0x4c5b, 0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a);
DEFINE_GUID(MF_MT_AUDIO_SAMPLES_PER_SECOND, 0x5faeeae7, 0x0290, 0x4c31, 0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba);
DEFINE_GUID(MF_MT_AUDIO_BITS_PER_SAMPLE, 0xf2deb57f, 0x40fa, 0x4764, 0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69);
DEFINE_GUID(MF_MT_AAC_PAYLOAD_TYPE, 0xbfbabe79, 0x7434, 0x4d1c, 0x94, 0xf0, 0x72, 0xa3, 0xb9, 0xe1, 0x71, 0x88);
DEFINE_GUID(MF_MT_USER_DATA, 0xb6bc765f, 0x4c3b, 0x40a4, 0xbd, 0x51, 0x25, 0x35, 0xb6, 0x6f, 0xe0, 0x9d);
DEFINE_GUID(MFMediaType_Audio, 0x73647561, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
