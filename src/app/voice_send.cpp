#include "maicord/app/voice_send.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/random.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_records.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord {
struct VoiceSendShared {
    SrwLock control_lock;
    VoiceSendControl control;
    bool (*probe)(void*, u8) noexcept;
    void* probe_context;
    volatile LONG write, read, end_lost;
    VoiceSendPacket packets[kVoiceSendQueue];
    OpusEncoder encoder;
    VoiceFramer framer;
    VoiceActivity activity;
    VoicePushToTalk push_to_talk;
    VoiceTransmitter transmitter;
    u64 wake_us;
    i16 frame[kVoiceFrameSamples];
};
namespace {
constexpr usize kEncoderOffset = (sizeof(VoiceSendShared) + 63) & ~usize{63};

bool key_down(VoiceSendShared& s, u8 key) noexcept {
    if (!key)
        return false;
    return s.probe ? s.probe(s.probe_context, key) : (GetAsyncKeyState(key) & 0x8000) != 0;
}
void wake_worker(const VoiceSend& send) noexcept {
    // The queue publication before this check is a full barrier (s.write is interlocked), so a
    // worker that has cleared `pending` is guaranteed to see the new packet.
    if (send.loop && send.signal && !read_acquire(&send.signal->pending))
        network_signal_post(send.loop, send.signal, 1);
}
// Capture thread: every frame pacing releases goes through meter, gate,
// state machine and (for voice) the encoder, then into the queue.
void process(VoiceSend& send, u64 now_us) noexcept {
    auto& s = *send.shared;
    AcquireSRWLockShared(s.control_lock.native());
    const VoiceSendControl c = s.control;
    ReleaseSRWLockShared(s.control_lock.native());
    const bool down = c.mode == VoiceInputMode::push_to_talk && key_down(s, c.key);
    write_release(&send.cells.key_down, down);
    bool queued = false;
    u64 index = 0;
    while (voice_framer_pop(&s.framer, now_us, {s.frame, kVoiceFrameSamples}, &index, &s.wake_us)) {
        voice_apply_gain({s.frame, kVoiceFrameSamples}, c.gain_percent);
        const u32 level = voice_level({s.frame, kVoiceFrameSamples});
        const bool active = voice_activity_update(&s.activity, level, c.sensitivity, c.automatic);
        const bool held = voice_push_to_talk_update(&s.push_to_talk, down, now_us, c.release_ms);
        const bool want = c.send && (c.mode == VoiceInputMode::push_to_talk ? held : active);
        const auto action = voice_transmit_next(&s.transmitter, want);
        write_release(&send.cells.level, static_cast<LONG>(level));
        write_release(&send.cells.threshold, s.activity.threshold);
        write_release(&send.cells.transmitting, s.transmitter.talking);
        write_release(&send.cells.announced, s.transmitter.announced);
        if (action == VoiceTransmitAction::idle)
            continue;
        const u32 write = static_cast<u32>(read_acquire(&s.write));
        if (write - static_cast<u32>(read_acquire(&s.read)) >= kVoiceSendQueue) {
            // Worker stalled: drop the frame, but never lose Speaking edges.
            if (action == VoiceTransmitAction::start)
                voice_transmit_reset(&s.transmitter);
            else if (action == VoiceTransmitAction::tail_end)
                InterlockedExchange(&s.end_lost, 1);
            continue;
        }
        auto& packet = s.packets[write % kVoiceSendQueue];
        packet.frame = index;
        packet.action = action;
        packet.bytes = 0;
        if (action == VoiceTransmitAction::start || action == VoiceTransmitAction::voice) {
            const auto encoded = opus_encoder_encode(&s.encoder, {s.frame, kVoiceFrameSamples},
                                                     {packet.opus, kOpusEncoderMaximumPacketBytes});
            if (encoded.ok() && encoded.bytes)
                packet.bytes = static_cast<u16>(encoded.bytes);
        }
        if (!packet.bytes) {
            copy_bytes(packet.opus, kVoiceSilenceFrame, sizeof(kVoiceSilenceFrame));
            packet.bytes = sizeof(kVoiceSilenceFrame);
        }
        InterlockedExchange(&s.write, static_cast<LONG>(write + 1));
        queued = true;
    }
    if (queued)
        wake_worker(send);
}
void discard(VoiceSendShared& s) noexcept {
    write_release(&s.read, read_acquire(&s.write));
}
bool same_device(const wchar_t* a, const wchar_t* b) noexcept {
    u32 i = 0;
    while (i < kVoiceDeviceIdCharacters && a[i] == b[i] && a[i])
        ++i;
    return i == kVoiceDeviceIdCharacters || a[i] == b[i];
}
void stop_capture(VoiceSend& send) noexcept {
    AcquireSRWLockExclusive(send.lock.native());
    send.capture_visible = false;
    ReleaseSRWLockExclusive(send.lock.native());
    voice_audio_capture_stop(&send.capture);
    // A wedged driver keeps the thread (and its view of the block) alive.
    if (voice_audio_capture_running(send.capture))
        return;
    voice_send_close(&send);
}
bool start_capture(VoiceSend& send) noexcept {
    if (!send.shared && !voice_send_open(&send))
        return false;
    copy_bytes(send.capture_device, send.applied.input_device, sizeof(send.capture_device));
    send.capture_processing = send.applied.voice_processing;
    if (!voice_audio_capture_start(&send.capture, send.capture_device, send.capture_processing, voice_send_push,
                                   voice_send_tick, &send)) {
        voice_send_close(&send);
        return false;
    }
    AcquireSRWLockExclusive(send.lock.native());
    send.capture_visible = true;
    ReleaseSRWLockExclusive(send.lock.native());
    return true;
}
bool connection_ready(void* context) noexcept {
    const auto& connection = *static_cast<VoiceConnection*>(context);
    return voice_connection_dave_ready(connection) && !connection.dave_recovering;
}
DaveSealResult connection_seal(void* context, ConstSlice<u8> opus, Slice<u8> output) noexcept {
    return voice_connection_dave_encrypt(static_cast<VoiceConnection*>(context), opus, output);
}
VoiceUdpMediaSend connection_send(void* context, ConstSlice<u8> packet) noexcept {
    auto* connection = static_cast<VoiceConnection*>(context);
    return voice_connection_media_send(connection, connection->ready_revision, packet);
}
VoiceSocketSend connection_speaking(void* context, u32 flags) noexcept {
    return voice_connection_speaking(static_cast<VoiceConnection*>(context), flags, GetTickCount64());
}
} // namespace

bool voice_send_open(VoiceSend* send) noexcept {
    if (!send || send->shared)
        return false;
    const u32 encoder_bytes = opus_encoder_storage_bytes(1);
    if (!encoder_bytes)
        return false;
    auto* block = static_cast<u8*>(
        VirtualAlloc(nullptr, kEncoderOffset + encoder_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!block)
        return false;
    auto* shared = reinterpret_cast<VoiceSendShared*>(block);
    if (!opus_encoder_initialize(&shared->encoder, {block + kEncoderOffset, encoder_bytes}, 1).ok()) {
        VirtualFree(block, 0, MEM_RELEASE);
        return false;
    }
    send->shared = shared;
    return true;
}

void voice_send_close(VoiceSend* send) noexcept {
    if (!send || !send->shared)
        return;
    opus_encoder_release(&send->shared->encoder);
    VirtualFree(send->shared, 0, MEM_RELEASE);
    send->shared = nullptr;
    InterlockedExchange(&send->cells.level, 0);
    InterlockedExchange(&send->cells.transmitting, 0);
    InterlockedExchange(&send->cells.announced, 0);
    InterlockedExchange(&send->cells.key_down, 0);
}

void voice_send_set_control(VoiceSend* send, const VoiceSendControl& control) noexcept {
    if (!send || !send->shared)
        return;
    AcquireSRWLockExclusive(send->shared->control_lock.native());
    send->shared->control = control;
    ReleaseSRWLockExclusive(send->shared->control_lock.native());
}

void voice_send_set_key_probe(VoiceSend* send, bool (*probe)(void*, u8) noexcept, void* context) noexcept {
    if (send && send->shared)
        send->shared->probe = probe, send->shared->probe_context = context;
}

void voice_send_push(void* context, ConstSlice<i16> mono, u64 now_us) noexcept {
    auto& send = *static_cast<VoiceSend*>(context);
    voice_framer_push(&send.shared->framer, mono);
    process(send, now_us);
}

u64 voice_send_tick(void* context, u64 now_us) noexcept {
    auto& send = *static_cast<VoiceSend*>(context);
    process(send, now_us);
    return send.shared->wake_us;
}

u32 voice_send_queued(const VoiceSend& send) noexcept {
    return send.shared ? static_cast<u32>(read_acquire(&send.shared->write)) -
                             static_cast<u32>(read_acquire(&send.shared->read))
                       : 0;
}

void voice_send_configure(VoiceSend* send, const VoiceAudioSettings& settings) noexcept {
    if (!send)
        return;
    AcquireSRWLockExclusive(send->lock.native());
    copy_value(&send->settings, settings);
    send->settings.input_device[kVoiceDeviceIdCharacters - 1] = 0;
    send->settings.output_device[kVoiceDeviceIdCharacters - 1] = 0;
    ++send->revision;
    ReleaseSRWLockExclusive(send->lock.native());
}

VoiceAudioStatus voice_send_status(VoiceSend* send) noexcept {
    VoiceAudioStatus status{};
    if (!send)
        return status;
    AcquireSRWLockShared(send->lock.native());
    if (send->capture_visible) {
        const auto capture = voice_audio_capture_status(send->capture);
        status.capture = capture.state;
        status.native_error = capture.native_error;
    }
    ReleaseSRWLockShared(send->lock.native());
    if (status.capture == VoiceCaptureState::capturing) {
        status.level = static_cast<u8>(read_acquire(&send->cells.level));
        status.transmitting = read_acquire(&send->cells.transmitting) != 0;
        status.push_to_talk_down = read_acquire(&send->cells.key_down) != 0;
    }
    status.threshold = static_cast<u8>(read_acquire(&send->cells.threshold));
    status.sent_packets = static_cast<u64>(read_acquire(&send->sent_packets));
    status.round_trip_ms = static_cast<u16>(minimum<LONG>(read_acquire(&send->cells.round_trip), 65535));
    return status;
}

void voice_send_flush(VoiceSend* send, const VoiceSendTransport& t) noexcept {
    if (!send || !t.ready || !t.seal || !t.send || !t.speaking)
        return;
    if (t.session != send->session) {
        u8 random[6]{};
        secure_random_fill({random, sizeof(random)});
        send->session = t.session;
        send->sequence = static_cast<u16>(random[0] | random[1] << 8);
        send->timestamp_base = static_cast<u32>(random[2] | random[3] << 8 | random[4] << 16) | u32{random[5]} << 24;
        send->speaking_on = send->speaking_off_pending = false;
    }
    auto* s = send->shared;
    if (s && InterlockedExchange(&s->end_lost, 0))
        send->speaking_off_pending = send->speaking_on;
    while (s) {
        const u32 read = static_cast<u32>(read_acquire(&s->read));
        if (read == static_cast<u32>(read_acquire(&s->write)))
            break;
        const auto& packet = s->packets[read % kVoiceSendQueue];
        if (!t.ready(t.context)) {
            ++send->dropped_packets;
        } else {
            bool announce_ok = true;
            if (packet.action == VoiceTransmitAction::start || packet.action == VoiceTransmitAction::voice) {
                send->speaking_off_pending = false;
                if (!send->speaking_on) {
                    const auto announced = t.speaking(t.context, kVoiceSpeakingMicrophone);
                    if (announced == VoiceSocketSend::busy)
                        return;
                    send->speaking_on = announce_ok = announced == VoiceSocketSend::accepted;
                }
            }
            if (announce_ok) {
                const u32 header = voice_rtp_header({send->datagram, sizeof(send->datagram)}, send->sequence,
                                                    send->timestamp_base + static_cast<u32>(packet.frame) *
                                                                               kVoiceFrameSamples,
                                                    t.ssrc);
                const auto sealed = t.seal(t.context, {packet.opus, packet.bytes},
                                           {send->datagram + header, sizeof(send->datagram) - header});
                const auto sent = sealed.ok() ? t.send(t.context, {send->datagram, header + sealed.bytes})
                                              : VoiceUdpMediaSend::invalid;
                if (sent == VoiceUdpMediaSend::busy)
                    return;
                if (sent == VoiceUdpMediaSend::accepted) {
                    ++send->sequence;
                    InterlockedIncrement64(&send->sent_packets);
                } else
                    ++send->dropped_packets;
            } else
                ++send->dropped_packets;
        }
        if (packet.action == VoiceTransmitAction::tail_end)
            send->speaking_off_pending = send->speaking_on;
        write_release(&s->read, static_cast<LONG>(read + 1));
    }
    if (send->speaking_off_pending) {
        const auto ended = t.speaking(t.context, 0);
        if (ended != VoiceSocketSend::busy)
            send->speaking_on = send->speaking_off_pending = false;
    }
}

void voice_send_drive(VoiceSend* send, VoiceConnection* connection, const GatewayVoiceControl& voice,
                      bool media_ready, NetworkLoop* loop, NetworkSignal* signal) noexcept {
    if (!send)
        return;
    send->loop = loop;
    send->signal = signal;
    AcquireSRWLockExclusive(send->lock.native());
    if (send->revision != send->applied_revision || !send->configured) {
        if (send->configured || send->revision)
            copy_value(&send->applied, send->settings);
        else
            send->applied = voice_audio_settings_defaults();
        send->applied_revision = send->revision;
        send->configured = true;
    }
    ReleaseSRWLockExclusive(send->lock.native());
    const auto& a = send->applied;
    const u32 flags = voice.setup.flags;
    // Stage: audience is suppressed; an invited speaker (unsuppressed with a
    // request_to_speak timestamp) stays silent until accepting the invite.
    const bool muted = voice.desired.self_mute || voice.desired.self_deaf ||
                       (flags & (gateway_voice_mute | gateway_voice_deaf | gateway_voice_suppress |
                                 gateway_voice_request_to_speak));
    media_ready = media_ready && connection && voice.setup.channel_id;
    const auto* heartbeat = connection ? &connection->socket.session.heartbeat : nullptr;
    const u64 round_trip = media_ready && heartbeat->has_round_trip ? minimum<u64>(heartbeat->round_trip_ms, 65535) : 0;
    write_release(&send->cells.round_trip, static_cast<LONG>(round_trip));
    const bool sending = media_ready && !muted;
    // A mute mid-sentence still finishes the silence tail before closing.
    const bool tail = media_ready && send->shared && read_acquire(&send->cells.announced);
    const bool want = sending || a.monitor || tail;
    const bool running = voice_audio_capture_running(send->capture);
    if (running && (!want || send->capture_processing != a.voice_processing ||
                    !same_device(send->capture_device, a.input_device)))
        stop_capture(*send);
    if (want && !voice_audio_capture_running(send->capture))
        start_capture(*send);
    voice_send_set_control(send, {a.input_gain_percent, a.push_to_talk_release_ms, a.mode, a.sensitivity,
                                  a.push_to_talk_key, a.auto_sensitivity, sending});
    if (media_ready) {
        voice_send_flush(send, {connection, connection->ready_revision, connection->media.ssrc, connection_ready,
                                connection_seal, connection_send, connection_speaking});
    } else {
        if (send->shared)
            discard(*send->shared);
        send->speaking_on = send->speaking_off_pending = false;
    }
}

void voice_send_stop(VoiceSend* send) noexcept {
    if (!send)
        return;
    if (voice_audio_capture_running(send->capture))
        stop_capture(*send);
    else
        voice_send_close(send);
    send->speaking_on = send->speaking_off_pending = false;
    send->session = 0;
}

bool voice_send_active(const VoiceSend& send) noexcept {
    return send.capture.runtime || send.shared || send.revision != send.applied_revision ||
           (send.configured && send.applied.monitor);
}

bool voice_send_speaking(const VoiceSend& send) noexcept {
    return send.shared && send.speaking_on && read_acquire(&send.cells.transmitting);
}
} // namespace maicord
