#include "maicord/platform/aac_decoder.hpp"
#include "maicord/media/aac_configuration.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/sys/media_foundation.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord {
namespace {
using Error = AacDecodeError;
struct Api {
    HRESULT(WINAPI* startup)(ULONG, DWORD) {};
    HRESULT(WINAPI* shutdown)() {};
    HRESULT(WINAPI* type)(IMFMediaType**) {};
    HRESULT(WINAPI* sample)(IMFSample**) {};
    HRESULT(WINAPI* buffer)(DWORD, IMFMediaBuffer**) {};
    HRESULT(WINAPI* aligned)(DWORD, DWORD, IMFMediaBuffer**) {};
};
struct State {
    HMODULE library{};
    Api api{};
    IMFTransform* transform{};
    IMFSample* output{};
    IMFMediaBuffer* output_buffer{};
    DWORD input_id{}, output_id{};
    u32 input_bytes{}, input_alignment{};
    bool mf{}, ready{}, need_output{}, discontinuity{true};
};
static_assert(sizeof(State) <= 256);
constexpr GUID kAacDecoder{0x32d186a7, 0x218f, 0x4c75, {0x88, 0x76, 0xdd, 0x77, 0x27, 0x3a, 0x89, 0x99}};
AacDecodeResult fail(AacDecoder& owner, Error error, HRESULT native = S_OK) noexcept {
    owner.native_status = native;
    return {error, native};
}
bool range(const void* p, u32 n) noexcept {
    return byte_range_valid({static_cast<const u8*>(p), n});
}
bool disjoint(const void* a, u32 an, const void* b, u32 bn) noexcept {
    return range(a, an) && range(b, bn) &&
           !byte_ranges_overlap({static_cast<const u8*>(a), an}, {static_cast<const u8*>(b), bn});
}
bool thread(const AacDecoder& owner) noexcept {
    return !owner.runtime || owner.thread_id == GetCurrentThreadId();
}
HRESULT output_type(AacDecoder& owner, State& s) noexcept {
    // Use the advertised complete native type (including channel assignment
    // and implementation-specific attributes), not a synthesized approximation.
    // Enumeration stays bounded and never substitutes another sample format,
    // rate or channel count for the caller's validated PCM configuration.
    HRESULT h = MF_E_INVALIDMEDIATYPE;
    const u32 wanted = owner.format == AacPcmFormat::float32 ? 2u : 1u;
    for (u32 index = 0; index < 32; ++index) {
        IMFMediaType* type = nullptr;
        const HRESULT found = s.transform->GetOutputAvailableType(s.output_id, index, &type);
        if (FAILED(found)) {
            com_release(type);
            if (found != MF_E_NO_MORE_TYPES)
                return found;
            break;
        }
        GUID major{}, subtype{};
        UINT32 rate{}, channels{}, bits{};
        const bool audio = SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
                           bytes_equal(&major, &MFMediaType_Audio, sizeof(GUID));
        const bool typed = audio && SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                           SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits));
        const u32 format =
            typed && bytes_equal(&subtype, &MFAudioFormat_PCM, sizeof(GUID)) && bits == 16     ? 1u
            : typed && bytes_equal(&subtype, &MFAudioFormat_Float, sizeof(GUID)) && bits == 32 ? 2u
                                                                                               : 0u;
        const bool exact = format && SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) &&
                           rate == owner.sample_rate &&
                           SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) &&
                           channels == owner.channels;
        if (exact && format == wanted) {
            h = s.transform->SetOutputType(s.output_id, type, 0);
            com_release(type);
            break;
        }
        com_release(type);
    }
    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(h))
        h = s.transform->GetOutputStreamInfo(s.output_id, &info);
    u32 alignment_mask{}, capacity{};
    if (SUCCEEDED(h) && (!aac_decoder_output_capacity(info.cbSize, &capacity) ||
                         !aac_decoder_alignment_mask(info.cbAlignment, &alignment_mask)))
        h = MF_E_BUFFERTOOSMALL;
    // The system AAC MFT's caller-supplied mode avoids native output lifetime
    // ambiguity. Unsupported provided-only allocation mode fails.
    if (SUCCEEDED(h) && (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES))
        h = MF_E_INVALIDMEDIATYPE;
    if (SUCCEEDED(h)) {
        com_release(s.output);
        com_release(s.output_buffer);
        h = s.api.sample(&s.output);
        if (SUCCEEDED(h))
            h = s.api.aligned(capacity, alignment_mask, &s.output_buffer);
        if (SUCCEEDED(h))
            h = s.output->AddBuffer(s.output_buffer);
    }
    return h;
}
HRESULT open(AacDecoder& owner) noexcept {
    AacConfiguration configuration{};
    if (!aac_configuration_decode({owner.asc, owner.asc_bytes}, &configuration) ||
        configuration.sample_rate != owner.sample_rate || configuration.channels != owner.channels)
        return MF_E_INVALIDMEDIATYPE;
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    HRESULT h = CoGetApartmentType(&apartment, &qualifier);
    if (FAILED(h))
        return h;
    if (apartment != APTTYPE_MTA)
        return RPC_E_CHANGED_MODE;
    auto* s =
        static_cast<State*>(VirtualAlloc(nullptr, sizeof(State), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!s)
        return E_OUTOFMEMORY;
    owner.runtime = s;
    owner.thread_id = GetCurrentThreadId();
    s->discontinuity = true;
    s->library = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s->library)
        return HRESULT_FROM_WIN32(GetLastError());
    s->api.startup = reinterpret_cast<decltype(s->api.startup)>(GetProcAddress(s->library, "MFStartup"));
    s->api.shutdown = reinterpret_cast<decltype(s->api.shutdown)>(GetProcAddress(s->library, "MFShutdown"));
    s->api.type = reinterpret_cast<decltype(s->api.type)>(GetProcAddress(s->library, "MFCreateMediaType"));
    s->api.sample = reinterpret_cast<decltype(s->api.sample)>(GetProcAddress(s->library, "MFCreateSample"));
    s->api.buffer =
        reinterpret_cast<decltype(s->api.buffer)>(GetProcAddress(s->library, "MFCreateMemoryBuffer"));
    s->api.aligned = reinterpret_cast<decltype(s->api.aligned)>(
        GetProcAddress(s->library, "MFCreateAlignedMemoryBuffer"));
    if (!s->api.startup || !s->api.shutdown || !s->api.type || !s->api.sample || !s->api.buffer ||
        !s->api.aligned)
        return E_NOTIMPL;
    h = s->api.startup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(h))
        return h;
    s->mf = true;
    h = CoCreateInstance(kAacDecoder, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform),
                         reinterpret_cast<void**>(&s->transform));
    DWORD inputs{}, outputs{};
    if (SUCCEEDED(h))
        h = s->transform->GetStreamCount(&inputs, &outputs);
    if (SUCCEEDED(h) && (inputs != 1 || outputs != 1))
        h = MF_E_INVALIDSTREAMNUMBER;
    if (SUCCEEDED(h)) {
        h = s->transform->GetStreamIDs(1, &s->input_id, 1, &s->output_id);
        if (h == E_NOTIMPL) {
            s->input_id = s->output_id = 0;
            h = S_OK;
        }
    }
    IMFMediaType* input = nullptr;
    if (SUCCEEDED(h))
        h = s->api.type(&input);
    if (SUCCEEDED(h))
        h = input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(h))
        h = input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    // MFAudioFormat_AAC attributes describe the core, before SBR/PS. Exact
    // output enumeration above still requires the declared final PCM domain.
    // https://learn.microsoft.com/en-us/windows/win32/medfound/aac-decoder
    if (SUCCEEDED(h))
        h = input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, configuration.core_sample_rate);
    if (SUCCEEDED(h))
        h = input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, configuration.core_channels);
    if (SUCCEEDED(h))
        h = input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    u8 userdata[28]{};
    for (u32 i = 0; i < owner.asc_bytes; ++i)
        userdata[12 + i] = owner.asc[i];
    if (SUCCEEDED(h))
        h = input->SetBlob(MF_MT_USER_DATA, userdata, 12 + owner.asc_bytes);
    if (SUCCEEDED(h))
        h = s->transform->SetInputType(s->input_id, input, 0);
    com_release(input);
    if (SUCCEEDED(h))
        h = output_type(owner, *s);
    MFT_INPUT_STREAM_INFO input_info{};
    if (SUCCEEDED(h))
        h = s->transform->GetInputStreamInfo(s->input_id, &input_info);
    if (SUCCEEDED(h) && (input_info.cbSize > kAacDecoderMaximumPacketBytes ||
                         !aac_decoder_alignment_mask(input_info.cbAlignment, &s->input_alignment)))
        h = MF_E_BUFFERTOOSMALL;
    if (SUCCEEDED(h))
        s->input_bytes = input_info.cbSize;
    if (SUCCEEDED(h))
        h = s->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(h))
        h = s->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    s->ready = SUCCEEDED(h);
    return h;
}
} // namespace
bool aac_decoder_alignment_mask(u32 alignment, u32* mask) noexcept {
    if (!mask || alignment > 4096 || (alignment > 1 && (alignment & (alignment - 1))))
        return false;
    *mask = alignment > 1 ? alignment - 1 : 0;
    return true;
}
bool aac_decoder_output_capacity(u32 minimum, u32* capacity) noexcept {
    if (!capacity || minimum > kAacDecoderMaximumNativeOutputBytes)
        return false;
    *capacity = maximum(minimum, kAacDecoderMaximumOutputBytes);
    return true;
}
bool aac_decoder_configuration_valid(ConstSlice<u8> asc, u32 rate, u32 channels) noexcept {
    AacConfiguration configuration{};
    return aac_configuration_decode(asc, &configuration) && configuration.sample_rate == rate &&
           configuration.channels == channels;
}
AacDecodeResult aac_decoder_initialize(AacDecoder* owner, ConstSlice<u8> asc, u32 rate, u32 channels,
                                       AacPcmFormat format) noexcept {
    if (!owner || !aac_decoder_configuration_valid(asc, rate, channels) ||
        (format != AacPcmFormat::signed16 && format != AacPcmFormat::float32) ||
        !disjoint(owner, sizeof(*owner), asc.data, asc.count))
        return {Error::invalid_input};
    if (owner->initialized || owner->runtime)
        return {Error::busy};
    *owner = {};
    copy_bytes(owner->asc, asc.data, asc.count);
    owner->asc_bytes = asc.count;
    owner->sample_rate = rate;
    owner->channels = channels;
    owner->format = format;
    owner->initialized = true;
    return {};
}
AacDecodeResult aac_decoder_submit(AacDecoder* owner, ConstSlice<u8> frame, i64 pts,
                                   u64 duration) noexcept {
    if (!owner || !owner->initialized || !frame.count || frame.count > kAacDecoderMaximumPacketBytes ||
        pts == (-0x7fffffffffffffffll - 1) || !duration ||
        duration > static_cast<u64>(0x7fffffffffffffffll) ||
        !disjoint(owner, sizeof(*owner), frame.data, frame.count))
        return {Error::invalid_input};
    if (!thread(*owner))
        return {Error::thread};
    if (owner->draining || owner->ended)
        return {Error::end_of_stream};
    if (!owner->runtime) {
        const HRESULT h = open(*owner);
        if (FAILED(h))
            return fail(*owner, Error::runtime, h);
    }
    auto& s = *static_cast<State*>(owner->runtime);
    if (!s.ready)
        return fail(*owner, Error::runtime, E_UNEXPECTED);
    if (s.need_output)
        return {Error::need_output};
    if (owner->accepted_frames == ~u64{0})
        return {Error::capacity};
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    HRESULT h = s.api.sample(&sample);
    // Each fresh input sample can be retained by ProcessInput. Honor its
    // advertised byte alignment/minimum capacity without reusing a sample;
    // current length stays the compressed packet size, never padded AAC data.
    const u32 allocation = maximum(frame.count, s.input_bytes);
    if (SUCCEEDED(h))
        h = s.input_alignment ? s.api.aligned(allocation, s.input_alignment, &buffer)
                              : s.api.buffer(allocation, &buffer);
    BYTE* address = nullptr;
    bool locked = false;
    if (SUCCEEDED(h)) {
        h = buffer->Lock(&address, nullptr, nullptr);
        locked = SUCCEEDED(h);
    }
    if (SUCCEEDED(h) && !address)
        h = E_UNEXPECTED;
    if (SUCCEEDED(h))
        copy_bytes(address, frame.data, frame.count);
    if (locked) {
        const HRESULT unlocked = buffer->Unlock();
        if (SUCCEEDED(h))
            h = unlocked;
    }
    if (SUCCEEDED(h))
        h = buffer->SetCurrentLength(frame.count);
    if (SUCCEEDED(h))
        h = sample->AddBuffer(buffer);
    com_release(buffer);
    if (SUCCEEDED(h))
        h = sample->SetSampleTime(pts);
    if (SUCCEEDED(h))
        h = sample->SetSampleDuration(static_cast<LONGLONG>(duration));
    if (SUCCEEDED(h) && s.discontinuity)
        h = sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    if (SUCCEEDED(h))
        h = s.transform->ProcessInput(s.input_id, sample, 0);
    com_release(sample);
    if (h == MF_E_NOTACCEPTING) {
        s.need_output = true;
        return {Error::need_output, h};
    }
    if (FAILED(h))
        return fail(*owner, Error::input, h);
    s.need_output = true;
    s.discontinuity = false;
    ++owner->accepted_frames;
    return {};
}
AacDecodedBlock aac_decoder_read(AacDecoder* owner, Slice<u8> pcm) noexcept {
    if (!owner || !owner->initialized || !disjoint(owner, sizeof(*owner), pcm.data, pcm.count))
        return {Error::invalid_input};
    if (!thread(*owner))
        return {Error::thread};
    if (owner->ended)
        return {Error::end_of_stream};
    if (pcm.count < kAacDecoderMaximumOutputBytes)
        return {Error::capacity};
    if (!owner->runtime)
        return {Error::need_input};
    auto& s = *static_cast<State*>(owner->runtime);
    if (!s.ready)
        return {Error::runtime, owner->native_status};
    if (!disjoint(&s, sizeof(s), pcm.data, pcm.count))
        return {Error::invalid_input};
    for (u32 attempt = 0; attempt < 2; ++attempt) {
        HRESULT h = s.output_buffer->SetCurrentLength(0);
        if (FAILED(h))
            return {Error::output, h};
        constexpr LONGLONG unstamped = -0x7fffffffffffffffll - 1;
        h = s.output->SetSampleTime(unstamped);
        if (FAILED(h))
            return {Error::output, h};
        MFT_OUTPUT_DATA_BUFFER output{s.output_id, s.output, 0, nullptr};
        DWORD status{};
        h = s.transform->ProcessOutput(0, 1, &output, &status);
        com_release(output.pEvents);
        if (output.pSample && output.pSample != s.output) {
            com_release(output.pSample);
            return {Error::unsupported, E_UNEXPECTED};
        }
        if (h == MF_E_TRANSFORM_STREAM_CHANGE) {
            h = output_type(*owner, s);
            if (FAILED(h)) {
                s.ready = false;
                owner->native_status = h;
                return {Error::media_type, h};
            }
            continue;
        }
        if (h == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            s.need_output = false;
            if (owner->draining) {
                owner->ended = true;
                return {Error::end_of_stream};
            }
            return {Error::need_input};
        }
        if (FAILED(h))
            return {Error::output, h};
        if (!output.pSample || (output.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE)) {
            s.need_output = false;
            return {Error::need_input};
        }
        BYTE* data = nullptr;
        DWORD bytes{};
        h = s.output_buffer->Lock(&data, nullptr, &bytes);
        if (FAILED(h))
            return {Error::output, h};
        const u32 stride = owner->channels * (owner->format == AacPcmFormat::float32 ? 4u : 2u);
        const bool valid = data && bytes && bytes <= kAacDecoderMaximumOutputBytes && !(bytes % stride) &&
                           owner->output_frames <= ~u64{0} - bytes / stride;
        if (valid)
            copy_bytes(pcm.data, data, bytes);
        const HRESULT unlocked = s.output_buffer->Unlock();
        if (!valid)
            return {Error::output, E_UNEXPECTED};
        if (FAILED(unlocked))
            return {Error::output, unlocked};
        LONGLONG timestamp{};
        const bool stamped = SUCCEEDED(s.output->GetSampleTime(&timestamp)) && timestamp != unstamped;
        owner->output_frames += bytes / stride;
        return {Error::none, 0, bytes, bytes / stride, stamped ? timestamp : 0, stamped};
    }
    return {Error::media_type, MF_E_TRANSFORM_STREAM_CHANGE};
}
AacDecodeResult aac_decoder_drain(AacDecoder* owner) noexcept {
    if (!owner || !owner->initialized)
        return {Error::invalid_input};
    if (!thread(*owner))
        return {Error::thread};
    if (owner->draining)
        return {};
    if (!owner->runtime) {
        owner->draining = owner->ended = true;
        return {};
    }
    auto& s = *static_cast<State*>(owner->runtime);
    if (!s.ready)
        return {Error::runtime};
    HRESULT h = s.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, s.input_id);
    if (SUCCEEDED(h))
        h = s.transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(h))
        return fail(*owner, Error::output, h);
    owner->draining = true;
    return {};
}
AacDecodeResult aac_decoder_flush(AacDecoder* owner) noexcept {
    if (!owner || !owner->initialized)
        return {Error::invalid_input};
    if (!thread(*owner))
        return {Error::thread};
    if (owner->runtime) {
        auto& s = *static_cast<State*>(owner->runtime);
        if (!s.ready)
            return {Error::runtime};
        HRESULT h = s.transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (SUCCEEDED(h))
            h = s.transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(h))
            return fail(*owner, Error::input, h);
        s.need_output = false;
        s.discontinuity = true;
    }
    owner->draining = owner->ended = false;
    owner->accepted_frames = owner->output_frames = 0;
    owner->native_status = 0;
    return {};
}
AacDecodeResult aac_decoder_release(AacDecoder* owner) noexcept {
    if (!owner)
        return {Error::invalid_input};
    if (!thread(*owner))
        return {Error::thread};
    if (auto* s = static_cast<State*>(owner->runtime)) {
        com_release(s->output);
        com_release(s->output_buffer);
        com_release(s->transform);
        if (s->mf) {
            const HRESULT h = s->api.shutdown();
            if (FAILED(h))
                return fail(*owner, Error::release, h);
            s->mf = false;
        }
        if (s->library) {
            if (!FreeLibrary(s->library))
                return fail(*owner, Error::release, HRESULT_FROM_WIN32(GetLastError()));
            s->library = nullptr;
        }
        if (!VirtualFree(s, 0, MEM_RELEASE))
            return fail(*owner, Error::release, HRESULT_FROM_WIN32(GetLastError()));
    }
    *owner = {};
    return {};
}
} // namespace maicord
