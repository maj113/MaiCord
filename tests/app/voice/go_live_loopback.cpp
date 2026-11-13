// Go Live end to end through tools/fake_discord_server.py's stream server (tools/test_go_live_loopback.py starts it
// and points this process at it with MAICORD_TEST_SERVER and MAICORD_TEST_CA_FILE): MaiCord's sender session
// (gateway_go_live_*: the stream connection as a sender, DAVE, RTP/RTX/RTCP, Opus stream audio) streams synthetic
// access units to MaiCord's receive path (a viewer's stream connection, the RTP core, DAVE, the decoders' sink) over
// TLS and UDP, with DAVE on: the fake server runs the MLS group of the two. Every frame the viewer assembles must be
// the sender's access unit, byte for byte after DAVE; with MAICORD_LOOPBACK_CODEC=h264 (the default) the H.264
// frames are also decoded on the GPU and their pixels checked. MAICORD_LOOPBACK_SIZE=1440 streams Go Live at 1440p
// 60 fps from a 2560x1440 window, 2160 at Source from a 3840x2160 one. Exit 77: no hardware D3D11 video device.
#include "maicord/app/gateway/go_live_stream.hpp"
#include "maicord/app/voice/video_sink.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/media/audio/opus_decoder.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"
#include "maicord/protocol/voice/dave_codec.hpp"
#include "maicord/protocol/voice/dave_frame.hpp"
#include "maicord/sys/d3d11.hpp"
#include "../../support/hevc_stream_fixture.hpp"
#include "../../support/test_output.hpp"
#include "../../support/voice_log_sink.hpp"

namespace maicord {
namespace {
#include "../../data/av1_stream_vectors.hpp"
constexpr u64 kSender = 100000000000000011, kViewer = 100000000000000012;
constexpr u64 kGuild = 200000000000000001, kChannel = 300000000000000004;
constexpr u64 kStreamServer = 500000000000000001;
constexpr u32 kWantedFrames = 24, kExpected = 512, kUnitBytes = 128 * 1024;
constexpr u32 kPreparedBytes = kUnitBytes + kUnitBytes / 4 + 4, kStoreSlots = 32;
// 160x96 by default: the I_PCM keyframe (23 KB) stays within what a real encoder's rate control would make at the
// stream's bitrate, so the pacer sends it before the sender's unit queue overflows. 1440p and 4K keyframes are
// compact (each slice one I_PCM macroblock that the rest repeat: 14 and 32 KB), the size of a flat picture, in three
// slices of whole macroblock rows (90 and 135 rows), so no slice predicts from a row another slice began.
u16 g_width = 160, g_height = 96;
u16 g_resolution;
u8 g_fps = 30;
bool g_compact;
h264_fixture::Sps g_sps{0, 66, 160 / 16, 96 / 16, 2, 1, false, 0, 0, 0};

u32 g_failures;
void check(bool value, const char* label) noexcept {
    test_check(&g_failures, value, label);
}

// The synthetic encoder: what the session asked of it, and the units it emits.
struct Producer {
    GoLiveMedia* media;
    GpuStreamCodec codec;
    bool encoding, keyframe, capturing, sounding;
    u32 encoders, keyframe_requests, units, since_key, idr_pattern;
};
struct Expected {
    u32 rtp_timestamp, bytes, pattern;
    bool keyframe;
};
struct Received {
    u32 frames, keyframes, mismatches, audio_frames, audio_failures, video_failures, nonce_frames, newest_nonce,
        nonce_regressions;
};
struct Decoded {
    ID3D11Device* device;
    ID3D11DeviceContext* context;
    ID3D11Texture2D* staging;
    u32 frames, mismatches, pattern;
};
struct World {
    NetworkLoop loop;
    GatewayGoLive live;
    GatewayStreamControl viewer_control;
    VoiceConnection viewer;
    VoiceVideoSink sink;
    GatewayVoiceControl voice;
    Producer producer;
    Received received;
    Decoded decoded;
    GpuStreamDecoder* decoder;
    u64 viewer_media_revision;
    VoiceVideoCodec codec;
    Expected expected[kExpected];
    u32 expected_count;
    u8 unit[kUnitBytes], prepared[kPreparedBytes], bits[160 * 1024];
    u8 prepared_store[kStoreSlots][kPreparedBytes]; // The last kStoreSlots units' prepared forms.
    u32 prepared_bytes[kStoreSlots];
    u8 receive_buffer[kVoiceUdpMediaReceiveSlots * 2048], send_buffer[2048];
    u8 audio_scratch[2048], audio_plain[2048];
};
World* g;

} // namespace

u32 go_live_producer_encodable(void*, bool* known) noexcept {
    *known = true;
    return gpu_stream_codec_bit(GpuStreamCodec::h264) | gpu_stream_codec_bit(GpuStreamCodec::h265) |
           gpu_stream_codec_bit(GpuStreamCodec::av1);
}
bool go_live_producer_capture(void* context, GoLiveMedia* media, const CaptureSource&, bool, u32) noexcept {
    auto& p = *static_cast<Producer*>(context);
    p.media = media;
    p.capturing = true;
    return true;
}
void go_live_producer_capture_stop(void* context) noexcept {
    static_cast<Producer*>(context)->capturing = false;
}
bool go_live_producer_sound(void* context, GoLiveMedia*, u32) noexcept {
    static_cast<Producer*>(context)->sounding = true;
    return true;
}
void go_live_producer_sound_stop(void* context) noexcept {
    static_cast<Producer*>(context)->sounding = false;
}
bool go_live_producer_encoder(void* context, GoLiveMedia*, GpuStreamCodec codec,
                              const GpuStreamEncoderSettings& settings) noexcept {
    auto& p = *static_cast<Producer*>(context);
    p.codec = codec;
    p.encoding = settings.width == g_width && settings.height == g_height;
    p.keyframe = true;
    ++p.encoders;
    return p.encoding;
}
void go_live_producer_configure(void*, const GpuStreamEncoderSettings&) noexcept {}
void go_live_producer_bitrate(void*, u32) noexcept {}
void go_live_producer_keyframe(void* context) noexcept {
    auto& p = *static_cast<Producer*>(context);
    p.keyframe = true;
    ++p.keyframe_requests;
}
void go_live_producer_encoder_stop(void* context) noexcept {
    static_cast<Producer*>(context)->encoding = false;
}
bool go_live_producer_preview(void*, GpuVideoFrame*, u32*) noexcept {
    return false;
}

namespace {

// One synthetic access unit of the negotiated codec into g->unit.
u32 make_unit(bool key, u32 index, u32* pattern) noexcept {
    auto& p = g->producer;
    if (key) {
        p.since_key = 0;
        ++p.idr_pattern;
    }
    *pattern = p.idr_pattern;
    if (p.codec == GpuStreamCodec::av1)
        return test_unhex(av1_fixture_units[key ? 0 : 1 + (p.since_key++ % (kAv1FixtureFrames - 1))], g->unit);
    h264_fixture::Bits bits{g->bits, sizeof(g->bits), 0, true};
    h264_fixture::Output out{g->unit, kUnitBytes, 0, true};
    // The H.265 fixture has keyframes only, one per frame interval: its 64x64 picture (6 KB) is what the pacer sends
    // in real time at the stream's bitrate, where a larger one would make the run wait on the pacer.
    if (p.codec == GpuStreamCodec::h265)
        hevc_fixture::write_idr(&out, &bits, {0, 64, 64, 0, 4, 8, 2}, {0, 0}, p.idr_pattern);
    else if (key)
        h264_fixture::write_idr(&out, &bits, g_sps, {0, 0}, p.idr_pattern, static_cast<u16>(index & 0xffff),
                                g_compact ? 3 : 2, true, g_compact);
    else {
        ++p.since_key;
        h264_fixture::write_p(&out, &bits, g_sps, {0, 0}, static_cast<u16>(p.since_key & 15),
                              static_cast<u16>((p.since_key * 2) & 255));
    }
    return out.ok ? out.count : 0;
}
void produce(u32 index) noexcept {
    auto& p = g->producer;
    if (!p.encoding || !p.media || g->expected_count == kExpected)
        return;
    const bool key = p.keyframe || p.codec == GpuStreamCodec::h265;
    p.keyframe = false;
    u32 pattern = 0;
    const u32 bytes = make_unit(key, index, &pattern);
    const u32 rtp = index * 3000;
    // What the viewer must assemble: DAVE's prepared form of the unit.
    DaveClearRange ranges[kDaveMaximumClearRanges];
    const auto plan = dave_frame_prepare(static_cast<DaveVideoCodec>(p.codec), {g->unit, bytes},
                                         {g->prepared, sizeof(g->prepared)}, {ranges, kDaveMaximumClearRanges});
    const u32 slot = g->expected_count % kStoreSlots;
    copy_bytes(g->prepared_store[slot], g->prepared, plan.bytes);
    g->prepared_bytes[slot] = plan.bytes;
    g->expected[g->expected_count++] = {rtp, plan.bytes, pattern, key};
    go_live_media_unit(p.media, {{g->unit, bytes}, 0, rtp, g_width, g_height, p.codec, key});
}
void produce_audio(u32 index) noexcept {
    i16 stereo[960 * 2];
    for (u32 i = 0; i != 960; ++i) {
        const i32 phase = static_cast<i32>((index * 960 + i) % 120) - 60; // 400 Hz triangle
        stereo[2 * i] = stereo[2 * i + 1] = static_cast<i16>((phase < 0 ? -phase : phase) * 300 - 9000);
    }
    if (g->producer.sounding && g->producer.media)
        go_live_media_audio(g->producer.media, {stereo, 960 * 2});
}

// The viewer's decoder side: frames after RTP assembly and DAVE.
void verify_picture(const GpuStreamFrame& frame) noexcept {
    auto& d = g->decoded;
    const GpuVideoFrame& gpu = *frame.gpu;
    D3D11_TEXTURE2D_DESC desc{};
    gpu.texture->GetDesc(&desc);
    if (!d.staging) {
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.ArraySize = 1;
        staging.BindFlags = 0;
        staging.MiscFlags = 0;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(d.device->CreateTexture2D(&staging, nullptr, &d.staging))) {
            ++d.mismatches;
            return;
        }
    }
    d.context->CopySubresourceRegion(d.staging, 0, 0, 0, 0, gpu.texture, gpu.subresource, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d.context->Map(d.staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        ++d.mismatches;
        return;
    }
    const auto* luma = static_cast<const u8*>(mapped.pData);
    // A compact keyframe's slices repeat their first macroblocks: a flat picture, never black.
    const u32 total = g_sps.width_mbs * g_sps.height_mbs, per = (total + 2) / 3;
    bool ok = gpu.width == g_width && gpu.height == g_height;
    u32 m = 0;
    for (; ok && m != total; ++m) {
        const u32 x = m % g_sps.width_mbs, y = m / g_sps.width_mbs;
        const u8 expected = h264_fixture::pcm_luma(g_compact ? m / per * per : m, d.pattern);
        ok = luma[u64{y * 16u + 8u} * mapped.RowPitch + x * 16u + 3u] == expected && (!g_compact || expected > 16);
    }
    d.context->Unmap(d.staging, 0);
    if (!ok && !d.mismatches) {
        test_write_text("first differing picture: ");
        test_write_u64(gpu.width);
        test_write_text("x");
        test_write_u64(gpu.height);
        test_write_text(" macroblock ");
        test_write_u64(m ? m - 1 : 0);
        test_write_text(" after ");
        test_write_u64(d.frames);
        test_write_line(" pictures");
    }
    d.mismatches += ok ? 0 : 1;
    ++d.frames;
}
void decoded_frame(void*, u32, const GpuStreamFrame& frame) noexcept {
    verify_picture(frame);
}
void decoder_keyframe(void*, u32) noexcept {}

bool push_frame(void*, u32 ssrc, VoiceVideoCodec codec, ConstSlice<u8> frame, u32 rtp_timestamp, bool key,
                bool intact) noexcept {
    auto& r = g->received;
    const u32 index = (rtp_timestamp - g->live.video_timestamp_offset) / 3000;
    const Expected* expected = nullptr;
    for (u32 i = 0; i != g->expected_count && !expected; ++i)
        if (g->expected[i].rtp_timestamp == index * 3000)
            expected = &g->expected[i];
    const u32 slot = expected ? static_cast<u32>(expected - g->expected) % kStoreSlots : 0;
    const bool same = expected && intact && codec == g->codec && key == expected->keyframe &&
                      frame.count == expected->bytes && g->prepared_bytes[slot] == frame.count &&
                      bytes_equal(frame.data, g->prepared_store[slot], frame.count);
    if (!same && !r.mismatches) {
        test_write_text("first differing frame: index ");
        test_write_u64(index);
        test_write_text(expected ? " expected " : " unknown ");
        test_write_u64(expected ? expected->bytes : 0);
        test_write_text(" received ");
        test_write_u64(frame.count);
        test_write_text(intact ? " intact" : " not intact");
        test_write_line(key ? " key" : " delta");
    }
    r.mismatches += same ? 0 : 1;
    ++r.frames;
    r.keyframes += key ? 1 : 0;
    if (g->decoder && codec == VoiceVideoCodec::h264 && expected) {
        g->decoded.pattern = expected->pattern;
        gpu_stream_decoder_push(g->decoder, frame, rtp_timestamp, intact);
    }
    (void)ssrc;
    return true;
}
void end_stream(void*, u32) noexcept {}
u32 decodable(void*) noexcept {
    return voice_video_codec_bit(VoiceVideoCodec::h264) | voice_video_codec_bit(VoiceVideoCodec::h265) |
           voice_video_codec_bit(VoiceVideoCodec::av1);
}
// A frame completes at the viewer when its last packet arrives (audio: its one packet): the DAVE counters of those
// completions must never go backwards, or a receiver following libdave's replay window refuses the late frame.
void note_counter(ConstSlice<u8> frame) noexcept {
    DaveClearRange ranges[kDaveMaximumClearRanges];
    DaveFrameView view{};
    auto& r = g->received;
    if (!dave_frame_parse(frame, {ranges, kDaveMaximumClearRanges}, &view).ok())
        return; // Transport-only (before the group's first epoch).
    r.nonce_regressions += r.nonce_frames && view.nonce < r.newest_nonce ? 1 : 0;
    r.newest_nonce = r.nonce_frames && view.nonce < r.newest_nonce ? r.newest_nonce : view.nonce;
    ++r.nonce_frames;
}
DaveMediaResult decrypt_frame(void* context, u64 user_id, u32 ssrc, ConstSlice<u8> frame, Slice<u8> scratch,
                              Slice<u8> output) noexcept {
    note_counter(frame);
    const auto opened = voice_connection_dave_decrypt_frame(static_cast<VoiceConnection*>(context), user_id, ssrc, frame,
                                                            scratch, output, clock_monotonic_milliseconds());
    if (!opened.ok() && !g->received.video_failures++) {
        test_write_text("first video decrypt failure: error ");
        test_write_u64(static_cast<u64>(opened.error));
        test_write_text(" sender ");
        test_write_u64(static_cast<u64>(opened.sender.error));
        test_write_text(" after ");
        test_write_u64(g->received.frames);
        test_write_line(" frames");
    }
    return opened;
}
u64 microseconds() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}
bool viewer_receive(void*, ConstSlice<u8> packet, const VoiceReceivedPacket& received) noexcept {
    const auto& layout = received.transport.layout;
    if (layout.kind == MediaPacketKind::rtcp) {
        voice_video_sink_rtcp(&g->sink, packet, microseconds());
        return true;
    }
    if (voice_video_source(received.source)) {
        voice_video_sink_packet(&g->sink, received, packet, microseconds());
        return true;
    }
    if (layout.type == 120 && layout.payload_bytes && layout.payload_offset + layout.payload_bytes <= packet.count) {
        note_counter({packet.data + layout.payload_offset, layout.payload_bytes});
        const auto opened = voice_connection_dave_decrypt(
            &g->viewer, received, {packet.data + layout.payload_offset, layout.payload_bytes},
            {g->audio_scratch, sizeof(g->audio_scratch)}, {g->audio_plain, sizeof(g->audio_plain)},
            clock_monotonic_milliseconds());
        (opened.ok() && !opened.passthrough ? g->received.audio_frames : g->received.audio_failures) += 1;
    }
    return true;
}

GatewayStreamKey stream_key() noexcept {
    return gateway_stream_key_for(kGuild, kChannel, kSender);
}
// The main Gateway's answers (STREAM_CREATE, STREAM_SERVER_UPDATE), as the fake server sends them.
void stream_ready(GatewayStreamControl* control) noexcept {
    GatewayStreamCommand command{};
    while (gateway_stream_next(*control, &command))
        gateway_stream_submitted(control, command);
    GatewayStreamCreate created{};
    created.key = stream_key();
    created.rtc_server_id = kStreamServer;
    gateway_stream_create(control, created, clock_monotonic_milliseconds());
    GatewayStreamServer server{};
    server.key = stream_key();
    server.token = {utf8_literal("qa-stream-token"), 0, false};
    server.endpoint = {utf8_literal("us-qa-stream.discord.media"), 0, false};
    gateway_stream_server(control, server);
}
VoiceVideoCodec codec_from_environment() noexcept {
    wchar_t text[8]{};
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_LOOPBACK_CODEC", text, 8);
    if (length == 4 && text[0] == L'h' && text[3] == L'5')
        return VoiceVideoCodec::h265;
    if (length == 3 && text[0] == L'a')
        return VoiceVideoCodec::av1;
    return VoiceVideoCodec::h264;
}
// MAICORD_LOOPBACK_SIZE: 1440 (Go Live 1440p 60 fps) or 2160 (Source from a 4K window), H.264 level 5.1 in both.
void size_from_environment() noexcept {
    wchar_t text[8]{};
    const DWORD length = GetEnvironmentVariableW(L"MAICORD_LOOPBACK_SIZE", text, 8);
    if (length != 4 || (text[0] != L'1' && text[0] != L'2'))
        return;
    const bool uhd = text[0] == L'2';
    g_width = uhd ? 3840 : 2560;
    g_height = uhd ? 2160 : 1440;
    g_resolution = uhd ? 0 : 1440;
    g_fps = uhd ? 30 : 60;
    g_compact = true;
    g_sps = {0, 66, g_width / 16u, g_height / 16u, 2, 1, false, 0, 0, 51};
}

void route(const NetworkCompletion& completion, u64 now_ms) noexcept {
    if (voice_connection_owns(g->live.connection, completion))
        voice_connection_completion(&g->live.connection, g->live.control.setup, &g->loop, completion, now_ms);
    else if (voice_connection_owns(g->viewer, completion))
        voice_connection_completion(&g->viewer, g->viewer_control.setup, &g->loop, completion, now_ms);
}
void drive_viewer(u64 now_ms) noexcept {
    auto& viewer = g->viewer;
    viewer.video_decodable = decodable(nullptr);
    voice_connection_drive(&viewer, g->viewer_control.setup, &g->loop, now_ms);
    if (viewer.media.has_description && g->viewer_media_revision != viewer.ready_revision &&
        voice_connection_media_start(&viewer, {g->receive_buffer, sizeof(g->receive_buffer)},
                                     {g->send_buffer, sizeof(g->send_buffer)}, viewer_receive, nullptr,
                                     kVoiceUdpMediaReceiveSlots))
        g->viewer_media_revision = viewer.ready_revision;
    const bool current = g->viewer_media_revision && g->viewer_media_revision == viewer.ready_revision;
    voice_video_sink_drive(&g->sink, &viewer, current, now_ms, microseconds());
}

int run() noexcept {
    g->codec = codec_from_environment();
    size_from_environment();
    if (!network_loop_initialize(&g->loop).ok())
        return 1;
    auto& live = g->live;
    live.backend_context = &g->producer;
    live.decodable = decodable(nullptr);
    g->voice.setup.channel_id = kChannel;
    g->voice.setup.phase = VoiceSetupPhase::credentials_ready;
    g->sink.frames = {nullptr, push_frame, end_stream, decodable};
    g->sink.decrypt = decrypt_frame;
    g->sink.decrypt_context = &g->viewer;
    const char sender_session[] = "qa-sender-session", viewer_session[] = "qa-viewer-session";
    // The sender: Go Live from the UI's command; the viewer: watching it.
    GoLiveCommand start{};
    start.kind = GoLiveCommandKind::start;
    start.guild_id = kGuild;
    start.channel_id = kChannel;
    start.settings = {g_resolution, g_fps, StreamPreset::video, true, false, {}};
    start.source.kind = CaptureSourceKind::window;
    start.source.width = g_width;
    start.source.height = g_height;
    start.source.process_id = 1234;
    const u64 begin = clock_monotonic_milliseconds();
    check(gateway_go_live_command(&live, start, kSender, {sender_session, sizeof(sender_session) - 1}, begin) &&
              live.state == GoLiveState::starting && g->producer.capturing && g->producer.sounding,
          "Go Live loopback: start captures (with sound) and sends STREAM_CREATE");
    stream_ready(&live.control);
    gateway_stream_bind(&g->viewer_control, kViewer, {viewer_session, sizeof(viewer_session) - 1});
    check(gateway_stream_watch(&g->viewer_control, stream_key(), begin), "Go Live loopback: the viewer watches");
    stream_ready(&g->viewer_control);
    u32 unit_index = 0, audio_index = 0;
    u64 next_unit = begin, next_audio = begin;
    const u64 deadline = begin + 20'000;
    u64 now = begin;
    while (now < deadline) {
        const auto polled = network_loop_poll(&g->loop, 5);
        now = clock_monotonic_milliseconds();
        if (polled.state == NetworkPollState::error)
            return 2;
        if (polled.state == NetworkPollState::completion)
            route(polled.completion, now);
        gateway_go_live_drive(&live, g->voice, &g->loop, now);
        drive_viewer(now);
        for (; now >= next_unit; next_unit += 33)
            produce(unit_index++);
        for (; now >= next_audio; next_audio += 20)
            produce_audio(audio_index++);
        if (g->received.frames >= kWantedFrames && g->received.keyframes && g->received.audio_frames >= 10 &&
            live.sender.bandwidth.acked_since_us)
            break;
        if (live.state == GoLiveState::ended)
            break;
    }
    const auto& r = g->received;
    check(live.state == GoLiveState::live && live.dave_ready && g->viewer.dave_session.protocol_version == 1,
          "Go Live loopback: the stream is live with DAVE (protocol 1) on both connections");
    check(live.codec == static_cast<GpuStreamCodec>(g->codec == VoiceVideoCodec::av1    ? 2
                                                     : g->codec == VoiceVideoCodec::h265 ? 1
                                                                                         : 0),
          "Go Live loopback: the encoder runs the codec the stream server chose");
    check(r.frames >= kWantedFrames && r.keyframes >= 1 && !r.mismatches,
          "Go Live loopback: every frame the viewer assembles and DAVE-opens is the sender's access unit, starting "
          "with a keyframe");
    if (g->codec != VoiceVideoCodec::h265) // The H.265 fixture has keyframes only.
        check(r.keyframes * 2 < r.frames, "Go Live loopback: delta frames follow the keyframe (no keyframe storm)");
    check(r.audio_frames >= 10 && !r.audio_failures,
          "Go Live loopback: stream audio arrives DAVE-encrypted and opens for the viewer");
    check(!r.video_failures, "Go Live loopback: no video frame fails DAVE decryption, across the epoch change");
    check(r.nonce_frames >= 20 && !r.nonce_regressions,
          "Go Live loopback: the sender's DAVE counters (audio and video share one) arrive in increasing order");
    check(live.sender.bandwidth.acked_since_us != 0, "Go Live loopback: the viewer's transport feedback reaches the sender");
    if (g->decoder) {
        // Decoding is synchronous: every pushed H.264 frame produced a picture.
        const bool shown = g->decoded.frames >= kWantedFrames && !g->decoded.mismatches;
        if (!shown) {
            test_write_text("decoded pictures ");
            test_write_u64(g->decoded.frames);
            test_write_text(" of ");
            test_write_u64(r.frames);
            test_write_text(" frames, differing ");
            test_write_u64(g->decoded.mismatches);
            test_write_line("");
        }
        check(shown, "Go Live loopback: the GPU decoder shows the sender's pictures (I_PCM samples exact)");
    }
    // Stop: STREAM_DELETE is queued and both connections drain.
    GoLiveCommand stop{};
    stop.kind = GoLiveCommandKind::stop;
    gateway_go_live_command(&live, stop, kSender, {sender_session, sizeof(sender_session) - 1}, now);
    GatewayStreamCommand command{};
    check(live.state == GoLiveState::ended && live.end == GoLiveEnd::stopped && !g->producer.capturing &&
              !g->producer.encoding && gateway_stream_next(live.control, &command) &&
              command.kind == GatewayStreamCommandKind::remove,
          "Go Live loopback: stop ends the producers and sends STREAM_DELETE");
    voice_connection_stop(&g->viewer);
    voice_video_sink_stop(&g->sink);
    const u64 drain = clock_monotonic_milliseconds() + 3000;
    while ((!voice_connection_drained(live.connection) || !voice_connection_drained(g->viewer)) &&
           clock_monotonic_milliseconds() < drain) {
        const auto polled = network_loop_poll(&g->loop, 10);
        if (polled.state == NetworkPollState::completion)
            route(polled.completion, clock_monotonic_milliseconds());
        gateway_go_live_drive(&live, g->voice, &g->loop, clock_monotonic_milliseconds());
        voice_connection_drive(&g->viewer, g->viewer_control.setup, &g->loop, clock_monotonic_milliseconds());
    }
    const bool drained = voice_connection_drained(live.connection) && voice_connection_drained(g->viewer);
    check(drained, "Go Live loopback: both stream connections drain after stop");
    gateway_go_live_release(&live);
    gateway_stream_release(&g->viewer_control);
    if (drained)
        network_loop_release(&g->loop);
    return 0;
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_go_live_loopback_entry() noexcept {
    using namespace maicord;
    // Voice records of failures (no secrets: the voice log never carries keys or media) explain a failed run.
    g_voice_log_sink = [](VoiceLogLevel, const char* event, ConstSlice<VoiceLogField> fields) noexcept {
        for (const char* at = event; *at; ++at)
            if ((at[0] == 'f' && at[1] == 'a' && at[2] == 'i' && at[3] == 'l') ||
                (at[0] == 'r' && at[1] == 'e' && at[2] == 'j' && at[3] == 'e') ||
                (at[0] == 'i' && at[1] == 'n' && at[2] == 'v' && at[3] == 'a')) {
                test_write_text(event);
                for (u32 i = 0; i != fields.count; ++i) {
                    test_write_text(" ");
                    test_write_text(fields[i].key);
                    test_write_text("=");
                    test_write_u64(fields[i].value);
                }
                test_write_line("");
                return;
            }
    };
    g = static_cast<World*>(pages_allocate(sizeof(World)));
    if (!g)
        ExitProcess(1);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    constexpr D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) {
        test_write_text("go live loopback: no hardware D3D11 video device; skipped\n");
        ExitProcess(77);
    }
    ID3D11Multithread* multithread = nullptr;
    if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    g->decoded.device = device;
    g->decoded.context = context;
    if (codec_from_environment() == VoiceVideoCodec::h264)
        g->decoder = gpu_stream_decoder_create({device, 1, {nullptr, decoded_frame, decoder_keyframe},
                                                GpuStreamCodec::h264, true});
    const int result = run();
    if (result)
        check(false, "Go Live loopback: the network loop runs");
    gpu_stream_decoder_destroy(g->decoder);
    if (g->decoded.staging)
        g->decoded.staging->Release();
    context->Release();
    device->Release();
    test_exit(g_failures, "go live loopback");
}
