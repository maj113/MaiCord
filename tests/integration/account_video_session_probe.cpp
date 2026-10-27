// Account video session probe (MaiCordAccountVideoSessionProbe). It is not in the default
// suite: the --audio suite needs a WASAPI endpoint and runs only with
// -AudioPlaybackProbe (maicord_account_audio_playback); --opus is one case of it; the plain
// mode needs MAICORD_ACCOUNT_VIDEO_SESSION_PROBE=1 and MAICORD_ACCOUNT_VIDEO_SESSION_INPUT=<mp4>
// (MAICORD_ACCOUNT_VIDEO_GENERIC_PROBE=1 accepts any clip), so it is a manual run.
#include "maicord/app/account_video_session.hpp"
#include "maicord/render/video_frame_gpu.hpp"
#include "maicord/platform/hevc_gpu_decoder.hpp"
#include "maicord/media/encoded_audio.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"

#if !defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
#error The native playback probe requires the actual endpoint diagnostic contract.
#endif

// EXCLUDE_FROM_ALL, never default CTest. The entry requires an explicit opt-in
// and a bounded original QA movie, --opus original memory stream, or --audio
// original Opus/MP3/WAV memory streams under one shared deadline.
// It opens a real endpoint and plays that
// original signal. No network, credential, registry, enrollment or output file.
// GPU acknowledgement follows an actual copy on the decoder's protected
// immediate context: queued command order protects the source lease without
// CPU pixel readback, a second producer thread or a GPU polling loop.
namespace maicord {
namespace {
constexpr u32 kInputLimit = 256 * 1024;
struct Probe {
    AccountVideoSession session{};
    OggOpusDemuxer opus_metadata{};
    EncodedAudioMetadata audio_metadata{};
    Mp4Decoder decoder{};
    AccountVideoIdentity identity{1, 2, 3, 4, 0, 1, 0};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    ID3D11DeviceContext1* render_context{};
    ID3D11Texture2D* copied{};
    ID3D11Texture2D* rgba{};
    ID3D11Texture2D* pixel{};
    ID3D11UnorderedAccessView* rgba_view{};
    VideoFrameGpu conversion{};
    HANDLE lane{}, file{};
    u8* input{};
    u32 input_bytes{}, stage{}, native{}, copied_frames{};
    u32 rgba_frames{}, rgba_readbacks{};
    u64 deadline{}, serial{}, max_written{}, max_submitted{}, max_consumed{};
    bool com{}, retained{}, joined{}, played{}, paused{}, sought{}, volume{}, muted{}, unmuted{}, ended{},
        replayed{}, stopped{}, generic{}, opus{}, audio{};
    u64 opus_total_frames{}, opus_end_written{}, opus_end_consumed{};
    u64 audio_total_frames{}, audio_end_written{}, audio_end_consumed{};
};
Probe g_probe{}; // Failed join retains every borrowed owner until process exit.
void metric(const char* key, u64 value) noexcept {
    char out[128]{};
    u32 count{};
    while (*key && count < 90)
        out[count++] = *key++;
    out[count++] = '=';
    char digits[20]{};
    u32 n{};
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value && n < 20);
    while (n)
        out[count++] = digits[--n];
    out[count++] = '\n';
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, count, &written, nullptr);
}
bool fail(Probe& p, u32 native) noexcept {
    p.native = native;
    return false;
}
void measure(Probe& p) noexcept {
    const auto a = account_video_session_audio_progress(&p.session);
    p.max_written = maximum(p.max_written, a.written_frames);
    p.max_submitted = maximum(p.max_submitted, a.submitted_frames);
    p.max_consumed = maximum(p.max_consumed, a.consumed_frames);
}
bool verify_rgba(Probe& p) noexcept {
    if (!p.rgba || !p.pixel)
        return fail(p, ERROR_INVALID_DATA);
    D3D11_TEXTURE2D_DESC source{};
    p.rgba->GetDesc(&source);
    const u32 x = source.Width / 2, y = source.Height / 2;
    const D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
    p.context->CopySubresourceRegion(p.pixel, 0, 0, 0, 0, p.rgba, 0, &box);
    p.context->Flush();
    const u64 deadline = GetTickCount64() + 2000;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT result{};
    do {
        result = p.context->Map(p.pixel, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result != DXGI_ERROR_WAS_STILL_DRAWING)
            break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    if (FAILED(result))
        return fail(p, static_cast<u32>(result));
    const auto* color = static_cast<const u8*>(mapped.pData);
    const bool opaque = color && color[3] == 255;
    if (opaque) {
        const u32 packed =
            u32{color[0]} | (u32{color[1]} << 8) | (u32{color[2]} << 16) | (u32{color[3]} << 24);
        metric(p.rgba_readbacks ? "end_rgba_pixel" : "first_rgba_pixel", packed);
    }
    p.context->Unmap(p.pixel, 0);
    if (!opaque)
        return fail(p, ERROR_INVALID_DATA);
    result = p.device->GetDeviceRemovedReason();
    if (FAILED(result))
        return fail(p, static_cast<u32>(result));
    ++p.rgba_readbacks;
    return true;
}
bool copy(Probe& p, const AccountVideoService& service) noexcept {
    const auto* frame = service.frame.gpu;
    if (!frame || !frame->texture || !frame->width || !frame->height)
        return fail(p, ERROR_INVALID_DATA);
    D3D11_TEXTURE2D_DESC source{};
    frame->texture->GetDesc(&source);
    if ((!p.generic && (source.Width != 64 || source.Height != 64)) || source.Format != DXGI_FORMAT_NV12 ||
        source.MipLevels != 1 || frame->subresource >= source.ArraySize)
        return fail(p, ERROR_NOT_SUPPORTED);
    if (!p.copied) {
        auto target = source;
        target.ArraySize = 1;
        target.Usage = D3D11_USAGE_DEFAULT;
        target.BindFlags = target.CPUAccessFlags = target.MiscFlags = 0;
        const HRESULT h = p.device->CreateTexture2D(&target, nullptr, &p.copied);
        if (FAILED(h))
            return fail(p, static_cast<u32>(h));
    }
    if (!p.rgba) {
        D3D11_TEXTURE2D_DESC target{};
        target.Width = frame->width;
        target.Height = frame->height;
        target.MipLevels = target.ArraySize = target.SampleDesc.Count = 1;
        target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target.Usage = D3D11_USAGE_DEFAULT;
        target.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        HRESULT h = p.device->CreateTexture2D(&target, nullptr, &p.rgba);
        if (SUCCEEDED(h))
            h = p.device->CreateUnorderedAccessView(p.rgba, nullptr, &p.rgba_view);
        target.Width = target.Height = 1;
        target.Usage = D3D11_USAGE_STAGING;
        target.BindFlags = 0;
        target.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(h))
            h = p.device->CreateTexture2D(&target, nullptr, &p.pixel);
        if (FAILED(h))
            return fail(p, static_cast<u32>(h));
    }
    constexpr float transparent[]{0, 0, 0, 0};
    p.context->ClearUnorderedAccessViewFloat(p.rgba_view, transparent);
    const HRESULT converted =
        video_frame_gpu_present(&p.conversion, p.device, p.render_context, *frame, p.rgba_view, 0, 0);
    if (FAILED(converted))
        return fail(p, static_cast<u32>(converted));
    ++p.rgba_frames;
    if (!p.rgba_readbacks && !verify_rgba(p))
        return false;
    p.context->CopySubresourceRegion(p.copied, 0, 0, 0, 0, frame->texture, frame->subresource, nullptr);
    ++p.copied_frames;
    if (!account_video_session_acknowledge(&p.session, p.identity, service.playback_epoch,
                                           service.frame_serial))
        return fail(p, ERROR_INVALID_DATA);
    return true;
}
enum class Until : u8 {
    playing,
    paused,
    seek,
    volume,
    mute,
    unmute,
    end,
    replay,
    stop
};
bool reached(Probe& p, Until until) noexcept {
    const auto s = account_video_session_status(&p.session);
    const auto a = account_video_session_audio_progress(&p.session);
    if (s.observed_command != p.serial)
        return false;
    switch (until) {
    case Until::playing:
    case Until::replay:
        return s.playing && s.clock_valid &&
               (s.has_audio ? (a.epoch == s.playback_epoch && a.consumed_frames > 0)
                            : p.generic && p.copied_frames > 0);
    case Until::paused:
        return s.paused && !s.playing && s.clock_valid;
    case Until::seek:
        return !s.seeking && s.paused && !s.playing && s.position_100ns == 5000000;
    case Until::volume:
    case Until::unmute:
        return s.volume_valid && s.applied_volume == .25f && !s.muted;
    case Until::mute:
        return s.volume_valid && s.applied_volume == 0 && s.muted;
    case Until::end:
        return s.ended && !s.playing &&
               (s.has_audio ? (a.written_frames && a.consumed_frames == a.written_frames)
                            : p.generic && p.copied_frames > 0);
    case Until::stop:
        return s.paused && !s.playing && !s.seeking && s.position_100ns == 0;
    }
    return false;
}
bool pump(Probe& p, Until until) noexcept {
    for (u32 turn = 0; turn < 65536; ++turn) {
        if (GetTickCount64() >= p.deadline)
            return fail(p, WAIT_TIMEOUT);
        const auto service = account_video_session_service(&p.session);
        if (service.frame_ready && !copy(p, service))
            return false;
        // A visible renderer submits its immediate-context work with Present.
        // This owned headless test has no swapchain, so flush once per service
        // turn instead of leaving decoder/copy work in a driver command batch.
        if (p.context)
            p.context->Flush();
        const auto s = account_video_session_status(&p.session);
        measure(p);
        if (s.error != AccountVideoError::none)
            return fail(p, s.native_error);
        if (reached(p, until))
            return true;
        // Copy acknowledgement changed the software frame lease/readiness.
        // The service result predates that transition; recompute its next frame
        // and deadline just as production does on the UI-ack lane wake.
        if (service.frame_ready)
            continue;
        if (service.needs_service)
            continue;
        const u64 now = GetTickCount64();
        if (now >= p.deadline)
            return fail(p, WAIT_TIMEOUT);
        const DWORD timeout =
            static_cast<DWORD>(minimum(p.deadline - now, u64{service.next_service_delay_ms}));
        const DWORD waited = WaitForSingleObject(p.lane, timeout);
        if (waited == WAIT_FAILED)
            return fail(p, GetLastError());
        if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT)
            return fail(p, ERROR_INVALID_DATA);
    }
    return fail(p, ERROR_INVALID_DATA);
}
bool command(Probe& p, AccountVideoCommand kind, i64 target = 0, float gain = 1,
             bool muted = false) noexcept {
    const auto value =
        account_video_session_control(&p.session, p.identity, ++p.serial, kind, target, gain, muted);
    if (!value.admitted || !value.ok())
        return fail(p, value.native_error);
    return true;
}
bool option(const wchar_t* wanted) noexcept {
    const wchar_t* cursor = GetCommandLineW();
    if (!cursor)
        return false;
    bool quoted = false;
    while (*cursor) {
        if (*cursor == L'"')
            quoted = !quoted;
        else if (!quoted && (*cursor == L' ' || *cursor == L'\t'))
            break;
        ++cursor;
    }
    while (*cursor == L' ' || *cursor == L'\t')
        ++cursor;
    while (*wanted) {
        if (*cursor != *wanted)
            return false;
        ++cursor;
        ++wanted;
    }
    while (*cursor == L' ' || *cursor == L'\t')
        ++cursor;
    return !*cursor;
}
u32 opus_page(u8* output, u32 sequence, u8 flags, u64 granule, const u8* packet, u8 count) noexcept {
    const u32 bytes = 28u + count;
    for (u32 i = 0; i < bytes; ++i)
        output[i] = 0;
    constexpr char capture[] = "OggS";
    for (u32 i = 0; i < 4; ++i)
        output[i] = static_cast<u8>(capture[i]);
    output[5] = flags;
    for (u32 i = 0; i < 8; ++i)
        output[6 + i] = static_cast<u8>(granule >> (8 * i));
    output[14] = 123;
    for (u32 i = 0; i < 4; ++i)
        output[18 + i] = static_cast<u8>(sequence >> (8 * i));
    output[26] = 1;
    output[27] = count;
    for (u32 i = 0; i < count; ++i)
        output[28 + i] = packet[i];
    u32 crc{};
    for (u32 i = 0; i < bytes; ++i) {
        crc ^= static_cast<u32>(output[i]) << 24;
        for (u32 bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0u);
    }
    for (u32 i = 0; i < 4; ++i)
        output[22 + i] = static_cast<u8>(crc >> (8 * i));
    return bytes;
}
bool build_opus(Probe& p) noexcept {
    constexpr u32 packets = 40, encoded_bytes = 47 + 44 + 31 * packets;
    p.input =
        static_cast<u8*>(VirtualAlloc(nullptr, encoded_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p.input)
        return fail(p, GetLastError());
    u8 head[19]{}, tags[16]{};
    constexpr char head_magic[] = "OpusHead", tags_magic[] = "OpusTags";
    for (u32 i = 0; i < 8; ++i) {
        head[i] = static_cast<u8>(head_magic[i]);
        tags[i] = static_cast<u8>(tags_magic[i]);
    }
    head[8] = head[9] = 1;
    head[10] = 120;
    head[12] = 0x80;
    head[13] = 0xbb;
    head[17] = 0xff;
    constexpr u8 silence[]{0xf8, 0xff, 0xfe};
    p.input_bytes = opus_page(p.input, 0, 2, 0, head, 19);
    p.input_bytes += opus_page(p.input + p.input_bytes, 1, 0, 0, tags, 16);
    for (u32 i = 0; i < packets; ++i) {
        const bool last = i + 1 == packets;
        p.input_bytes += opus_page(p.input + p.input_bytes, i + 2, last ? 4 : 0,
                                   u64{i + 1} * 960 - (last ? 80 : 0), silence, 3);
    }
    const auto parsed = ogg_opus_demux_initialize(&p.opus_metadata, {p.input, p.input_bytes}, {});
    if (parsed != OggOpusError::none)
        return fail(p, static_cast<u32>(parsed));
    p.opus_total_frames = packets * 960 - 120 - 80;
    if (p.input_bytes != encoded_bytes || p.opus_metadata.total_frames != p.opus_total_frames ||
        p.opus_metadata.pre_skip != 120 || p.opus_metadata.output_gain_q8 != -256)
        return fail(p, ERROR_INVALID_DATA);
    p.audio_metadata = EncodedAudioMetadata{p.opus_metadata};
    return true;
}
void little16(u8* bytes, u16 value) noexcept {
    bytes[0] = static_cast<u8>(value);
    bytes[1] = static_cast<u8>(value >> 8);
}
void little32(u8* bytes, u32 value) noexcept {
    for (u32 i = 0; i < 4; ++i)
        bytes[i] = static_cast<u8>(value >> (8 * i));
}
bool build_mp3(Probe& p) noexcept {
    constexpr u32 packets = 31, packet_bytes = 417;
    p.input_bytes = packets * packet_bytes;
    p.input =
        static_cast<u8*>(VirtualAlloc(nullptr, p.input_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p.input)
        return fail(p, GetLastError());
    // Authored MPEG1 LayerIII mono44.1kHz128kbps: zero side information and
    // main data encode zero granules, with no reservoir or invented delay tag.
    for (u32 i = 0; i < packets; ++i) {
        auto* frame = p.input + i * packet_bytes;
        frame[0] = 0xff;
        frame[1] = 0xfb;
        frame[2] = 0x90;
        frame[3] = 0xc0;
    }
    Mp3Stream source{};
    const auto parsed = mp3_stream_initialize(&source, {p.input, p.input_bytes});
    if (parsed != Mp3Error::none)
        return fail(p, static_cast<u32>(parsed));
    if (source.sample_rate != 44100 || source.channels != 1 || source.frame_count != packets ||
        source.total_frames != u64{packets} * 1152 || source.delay_frames || source.padding_frames ||
        source.gapless)
        return fail(p, ERROR_INVALID_DATA);
    p.audio_metadata = EncodedAudioMetadata{source};
    return true;
}
bool build_wave(Probe& p) noexcept {
    constexpr u32 rate = 96000, frames = 76800, data_bytes = frames * 2;
    p.input_bytes = 44 + data_bytes;
    p.input =
        static_cast<u8*>(VirtualAlloc(nullptr, p.input_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p.input)
        return fail(p, GetLastError());
    copy_bytes(p.input, "RIFF", 4);
    little32(p.input + 4, p.input_bytes - 8);
    copy_bytes(p.input + 8, "WAVEfmt ", 8);
    little32(p.input + 16, 16);
    little16(p.input + 20, 1);
    little16(p.input + 22, 1);
    little32(p.input + 24, rate);
    little32(p.input + 28, rate * 2);
    little16(p.input + 32, 2);
    little16(p.input + 34, 16);
    copy_bytes(p.input + 36, "data", 4);
    little32(p.input + 40, data_bytes);
    WavePcmSource source{};
    const auto parsed = wave_pcm_parse(&source, {p.input, p.input_bytes});
    if (parsed != WavePcmError::none)
        return fail(p, static_cast<u32>(parsed));
    if (source.sample_rate != rate || source.channels != 1 || source.total_frames != frames ||
        source.container_bits != 16 || source.block_align != 2 || source.duration_ms != 800)
        return fail(p, ERROR_INVALID_DATA);
    p.audio_metadata = EncodedAudioMetadata{source};
    return true;
}
bool run_audio(Probe& p, MediaAttachmentFormat format) noexcept {
    p.audio = true;
    p.opus = format == MediaAttachmentFormat::ogg_opus;
    if (!p.deadline)
        p.deadline = GetTickCount64() + 2500;
    p.stage = 30;
    const bool built = p.opus                                 ? build_opus(p)
                       : format == MediaAttachmentFormat::mp3 ? build_mp3(p)
                       : format == MediaAttachmentFormat::wav ? build_wave(p)
                                                              : false;
    if (!built || !encoded_audio_ready(p.audio_metadata))
        return false;
    p.audio_total_frames = encoded_audio_total_frames(p.audio_metadata);
    const u32 rate = encoded_audio_rate(p.audio_metadata);
    const auto expected_kind = p.opus                                 ? AccountPlaybackKind::ogg_opus
                               : format == MediaAttachmentFormat::mp3 ? AccountPlaybackKind::mp3
                                                                      : AccountPlaybackKind::wave_pcm;
    p.lane = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!p.lane)
        return fail(p, GetLastError());
    const auto initialized =
        account_video_session_initialize_audio(&p.session, &p.audio_metadata, p.identity, p.lane);
    if (!initialized.completed || !initialized.ok())
        return fail(p, initialized.native_error);
    if (p.session.video || account_video_session_owned_bytes(&p.session) ||
        p.session.status.kind != expected_kind || !p.session.status.has_audio ||
        p.session.status.duration_100ns !=
            static_cast<i64>(encoded_audio_duration_units(p.audio_metadata, 10000000)))
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 31;
    if (!command(p, AccountVideoCommand::volume, 0, .1f) || !command(p, AccountVideoCommand::play) ||
        !pump(p, Until::playing))
        return false;
    p.played = true;
    const auto native = account_video_session_probe_snapshot(&p.session).audio;
    if (!native.worker_started || !native.stream_open || !native.clock_valid || !native.clock_frequency ||
        !native.consumed_frames)
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 32;
    if (!command(p, AccountVideoCommand::pause) || !pump(p, Until::paused))
        return false;
    p.paused = true;
    const auto paused = account_video_session_status(&p.session);
    const auto paused_audio = account_video_session_audio_progress(&p.session);
    Sleep(75);
    account_video_session_service(&p.session);
    const auto still = account_video_session_status(&p.session);
    if (!still.paused || still.playing || still.position_100ns != paused.position_100ns ||
        account_video_session_audio_progress(&p.session).consumed_frames != paused_audio.consumed_frames)
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 33;
    const u64 before_epoch = p.session.playback_epoch;
    if (!command(p, AccountVideoCommand::seek, 5000000) || !pump(p, Until::seek))
        return false;
    p.sought = true;
    const auto sought = account_video_session_audio_progress(&p.session);
    if (sought.epoch <= before_epoch || sought.consumed_frames)
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 34;
    if (!command(p, AccountVideoCommand::play) || !pump(p, Until::playing))
        return false;
    p.stage = 35;
    if (!command(p, AccountVideoCommand::volume, 0, .25f) || !pump(p, Until::volume))
        return false;
    p.volume = true;
    if (!command(p, AccountVideoCommand::mute, 0, 1, true) || !pump(p, Until::mute))
        return false;
    p.muted = true;
    if (!command(p, AccountVideoCommand::mute, 0, 1, false) || !pump(p, Until::unmute))
        return false;
    p.unmuted = true;
    p.stage = 36;
    if (!pump(p, Until::end))
        return false;
    p.ended = true;
    const auto ended = account_video_session_audio_progress(&p.session);
    p.audio_end_written = ended.written_frames;
    p.audio_end_consumed = ended.consumed_frames;
    if (p.opus) {
        p.opus_end_written = ended.written_frames;
        p.opus_end_consumed = ended.consumed_frames;
    }
    if (ended.written_frames != p.audio_total_frames - rate / 2 ||
        ended.consumed_frames != ended.written_frames || ended.submitted_frames != ended.written_frames)
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 37;
    if (!command(p, AccountVideoCommand::play) || !pump(p, Until::replay))
        return false;
    p.replayed = true;
    p.stage = 38;
    if (!command(p, AccountVideoCommand::stop) || !pump(p, Until::stop))
        return false;
    p.stopped = true;
    return p.max_consumed > 0 && !p.copied_frames && !p.rgba_frames && !p.device && !p.context &&
           !p.session.status.frame_pending && !p.session.video &&
           p.session.status.error == AccountVideoError::none;
}
bool run(Probe& p) noexcept {
    p.stage = 1;
    if (option(L"--opus"))
        return run_audio(p, MediaAttachmentFormat::ogg_opus);
    wchar_t flag[4]{};
    if (GetEnvironmentVariableW(L"MAICORD_ACCOUNT_VIDEO_SESSION_PROBE", flag, 4) != 1 || flag[0] != L'1')
        return fail(p, ERROR_ACCESS_DENIED);
    wchar_t generic[4]{};
    p.generic = GetEnvironmentVariableW(L"MAICORD_ACCOUNT_VIDEO_GENERIC_PROBE", generic, 4) == 1 &&
                generic[0] == L'1';
    wchar_t path[1024]{};
    const DWORD count = GetEnvironmentVariableW(L"MAICORD_ACCOUNT_VIDEO_SESSION_INPUT", path, 1024);
    if (!count || count >= 1024)
        return fail(p, ERROR_INVALID_PARAMETER);
    p.file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    if (p.file == INVALID_HANDLE_VALUE) {
        p.file = nullptr;
        return fail(p, GetLastError());
    }
    LARGE_INTEGER bytes{};
    if (!GetFileSizeEx(p.file, &bytes) || bytes.QuadPart <= 0 ||
        bytes.QuadPart > (p.generic ? 64u * 1024u * 1024u : kInputLimit))
        return fail(p, ERROR_FILE_TOO_LARGE);
    p.input_bytes = static_cast<u32>(bytes.QuadPart);
    p.input =
        static_cast<u8*>(VirtualAlloc(nullptr, p.input_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p.input)
        return fail(p, GetLastError());
    DWORD read{};
    if (!ReadFile(p.file, p.input, p.input_bytes, &read, nullptr) || read != p.input_bytes)
        return fail(p, ERROR_READ_FAULT);
    if (!CloseHandle(p.file))
        return fail(p, GetLastError());
    p.file = nullptr;
    p.stage = 2;
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment))
        return fail(p, static_cast<u32>(apartment));
    p.com = true;
    constexpr D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actual{};
    const HRESULT created =
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                          D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, requested,
                          1, D3D11_SDK_VERSION, &p.device, &actual, &p.context);
    if (FAILED(created))
        return fail(p, static_cast<u32>(created));
    const HRESULT render_context = p.context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                                             reinterpret_cast<void**>(&p.render_context));
    if (FAILED(render_context))
        return fail(p, static_cast<u32>(render_context));
    ID3D11Multithread* guard{};
    const HRESULT protected_context =
        p.context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&guard));
    if (FAILED(protected_context))
        return fail(p, static_cast<u32>(protected_context));
    guard->SetMultithreadProtected(TRUE);
    guard->Release();
    p.lane = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!p.lane)
        return fail(p, GetLastError());
    p.stage = 3;
    if (!mp4_decoder_initialize(&p.decoder, {p.input, p.input_bytes}, p.generic ? 0 : 64,
                                p.generic ? 0 : 64, p.device))
        return fail(p, static_cast<u32>(p.decoder.native.status));
    const auto movie = mp4_decoder_movie(p.decoder);
    Mp4AudioDemuxer audio{};
    if (!p.generic &&
        (movie.samples != 50 || movie.duration_us < 1900000 || movie.duration_us > 2100000 ||
         mp4_audio_demux_initialize(&audio, mp4_decoder_source(p.decoder)) != Mp4DemuxError::none ||
         audio.sample_rate != 48000 || audio.channels != 2 || audio.samples.sample_count != 94))
        return fail(p, ERROR_INVALID_DATA);
    if (p.generic &&
        (!movie.samples || movie.samples > 36000 || !movie.duration_us || movie.duration_us > 180000000))
        return fail(p, ERROR_NOT_SUPPORTED);
    metric("movie_samples", movie.samples);
    metric("movie_duration_us", movie.duration_us);
    p.stage = 4;
    const auto init = account_video_session_initialize(&p.session, &p.decoder, p.identity, p.lane);
    if (!init.completed || !init.ok())
        return fail(p, init.native_error);
    if (account_video_session_owned_bytes(&p.session))
        return fail(p, ERROR_INVALID_DATA);
    metric("metadata_owned_bytes", account_video_session_owned_bytes(&p.session));
    metric("input_bytes", p.input_bytes);
    p.deadline = GetTickCount64() + (p.generic ? movie.duration_us / 1000 + 10000 : 20000);
    if (p.generic) {
        p.stage = 20;
        if (!command(p, AccountVideoCommand::volume, 0, 0) || !command(p, AccountVideoCommand::play) ||
            !pump(p, Until::playing))
            return false;
        p.played = true;
        p.stage = 21;
        if (!pump(p, Until::end))
            return false;
        if (!verify_rgba(p))
            return false;
        p.ended = true;
        p.stage = 22;
        if (!command(p, AccountVideoCommand::play) || !pump(p, Until::replay))
            return false;
        p.replayed = true;
        p.stage = 23;
        if (!command(p, AccountVideoCommand::pause) || !pump(p, Until::paused))
            return false;
        p.paused = true;
        p.stage = 24;
        if (!command(p, AccountVideoCommand::seek, 5000000) || !pump(p, Until::seek))
            return false;
        p.sought = true;
        p.stage = 25;
        if (!command(p, AccountVideoCommand::stop) || !pump(p, Until::stop))
            return false;
        p.stopped = true;
        return p.copied_frames > 0;
    }
    p.stage = 5;
    if (!command(p, AccountVideoCommand::volume, 0, .1f) || !command(p, AccountVideoCommand::play) ||
        !pump(p, Until::playing))
        return false;
    p.played = true;
    p.stage = 6;
    if (!command(p, AccountVideoCommand::pause) || !pump(p, Until::paused))
        return false;
    p.paused = true;
    const i64 paused = account_video_session_status(&p.session).position_100ns;
    if (account_video_session_status(&p.session).position_100ns != paused)
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 7;
    if (!command(p, AccountVideoCommand::seek, 5000000) || !pump(p, Until::seek))
        return false;
    p.sought = true;
    p.stage = 8;
    if (!command(p, AccountVideoCommand::play) || !pump(p, Until::playing))
        return false;
    p.stage = 9;
    if (!command(p, AccountVideoCommand::volume, 0, .25f) || !pump(p, Until::volume))
        return false;
    p.volume = true;
    p.stage = 10;
    if (!command(p, AccountVideoCommand::mute, 0, 1, true) || !pump(p, Until::mute))
        return false;
    p.muted = true;
    p.stage = 11;
    if (!command(p, AccountVideoCommand::mute, 0, 1, false) || !pump(p, Until::unmute))
        return false;
    p.unmuted = true;
    p.stage = 12;
    if (!pump(p, Until::end))
        return false;
    p.ended = true;
    p.stage = 13;
    if (!command(p, AccountVideoCommand::play) || !pump(p, Until::replay))
        return false;
    p.replayed = true;
    p.stage = 14;
    if (!command(p, AccountVideoCommand::stop) || !pump(p, Until::stop))
        return false;
    p.stopped = true;
    return p.max_consumed > 0 && p.max_submitted >= p.max_consumed && p.max_written >= p.max_submitted &&
           p.copied_frames > 1;
}
bool release(Probe& p) noexcept {
    // The only producer is this quiescent entry thread. No context/source/event
    // is freed if a native renderer join or decoder release retains the owner.
    const u64 now = GetTickCount64();
    const u32 join_ms =
        p.audio ? static_cast<u32>(minimum(u64{1000}, p.deadline > now ? p.deadline - now : 0)) : 2000;
    const auto value = account_video_session_release(&p.session, join_ms);
    if (!value.completed || !value.ok()) {
        p.retained = true;
        return fail(p, value.native_error);
    }
    if (p.audio && (p.session.initialized || p.session.workspace || p.session.lane_event ||
                    p.session.encoded_metadata || account_video_session_owned_bytes(&p.session))) {
        p.retained = true;
        return fail(p, ERROR_INVALID_DATA);
    }
    p.joined = true;
    mp4_decoder_release(&p.decoder);
    video_frame_gpu_release(&p.conversion);
    if (p.rgba_view) {
        p.rgba_view->Release();
        p.rgba_view = nullptr;
    }
    if (p.pixel) {
        p.pixel->Release();
        p.pixel = nullptr;
    }
    if (p.rgba) {
        p.rgba->Release();
        p.rgba = nullptr;
    }
    if (p.render_context) {
        p.render_context->Release();
        p.render_context = nullptr;
    }
    if (p.copied) {
        p.copied->Release();
        p.copied = nullptr;
    }
    if (p.context) {
        p.context->Release();
        p.context = nullptr;
    }
    if (p.device) {
        p.device->Release();
        p.device = nullptr;
    }
    if (p.lane) {
        if (!CloseHandle(p.lane)) {
            p.retained = true;
            return fail(p, GetLastError());
        }
        p.lane = nullptr;
    }
    if (p.file) {
        if (!CloseHandle(p.file)) {
            p.retained = true;
            return fail(p, GetLastError());
        }
        p.file = nullptr;
    }
    if (p.input) {
        if (!VirtualFree(p.input, 0, MEM_RELEASE)) {
            p.retained = true;
            return fail(p, GetLastError());
        }
        p.input = nullptr;
    }
    if (p.com) {
        CoUninitialize();
        p.com = false;
    }
    return true;
}
void report(const Probe& p, bool okay) noexcept {
    metric("pass", okay);
    metric("stage", p.stage);
    metric("native_error", p.native);
    metric("joined", p.joined);
    metric("retained", p.retained);
    metric("opus_mode", p.opus);
    metric("opus_total_frames", p.opus_total_frames);
    metric("opus_end_written_frames", p.opus_end_written);
    metric("opus_end_consumed_frames", p.opus_end_consumed);
    metric("audio_total_frames", p.audio_total_frames);
    metric("audio_end_written_frames", p.audio_end_written);
    metric("audio_end_consumed_frames", p.audio_end_consumed);
    metric("played", p.played);
    metric("paused", p.paused);
    metric("sought", p.sought);
    metric("volume", p.volume);
    metric("muted", p.muted);
    metric("unmuted", p.unmuted);
    metric("ended", p.ended);
    metric("replayed", p.replayed);
    metric("stopped", p.stopped);
    metric("copied_frames", p.copied_frames);
    metric("rgba_frames", p.rgba_frames);
    metric("rgba_readbacks", p.rgba_readbacks);
}
bool run_audio_suite(Probe& p) noexcept {
    const u64 deadline = GetTickCount64() + 4500;
    constexpr MediaAttachmentFormat formats[]{MediaAttachmentFormat::ogg_opus, MediaAttachmentFormat::mp3,
                                              MediaAttachmentFormat::wav};
    u32 completed = 0;
    for (const auto format : formats) {
        p = Probe{}; // Previous source/event/workspace has joined before reuse.
        p.deadline = deadline;
        p.audio_metadata.format = format;
        const bool ran = GetTickCount64() < deadline && run_audio(p, format);
        const u32 stage = p.stage, native = p.native;
        const bool released = release(p);
        if (released) {
            p.stage = stage;
            p.native = native;
        }
        const bool okay = ran && released && p.played && p.paused && p.sought && p.volume && p.muted &&
                          p.unmuted && p.ended && p.replayed && p.stopped && p.joined && !p.retained &&
                          p.audio_end_written && p.audio_end_consumed == p.audio_end_written;
        report(p, okay);
        if (!okay) {
            metric("audio_cases_completed", completed);
            return false;
        }
        ++completed;
    }
    metric("audio_cases_completed", completed);
    metric("audio_suite_pass", completed == 3);
    return completed == 3;
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_video_session_probe_entry() noexcept {
    auto& p = maicord::g_probe;
    if (maicord::option(L"--audio"))
        ExitProcess(maicord::run_audio_suite(p) ? 0 : 1);
    const bool ran = maicord::run(p);
    const maicord::u32 stage = p.stage, native = p.native;
    const bool released = maicord::release(p);
    if (released) {
        p.stage = stage;
        p.native = native;
    }
    maicord::report(p, ran && released);
    ExitProcess(ran && released ? 0 : 1);
}
