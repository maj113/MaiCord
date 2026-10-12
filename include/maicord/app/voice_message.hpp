#pragma once
#include "maicord/app/voice_audio_settings.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/ogg_opus_writer.hpp"
#include "maicord/media/opus_encoder.hpp"

namespace maicord {
// Recorded voice messages, sent the way the official mobile clients send them:
// 48 kHz mono Opus in Ogg, uploaded as "voice-message.ogg" (audio/ogg), then
// a message with IS_VOICE_MESSAGE, no content and one attachment carrying
// duration_secs and a base64 waveform of u8 amplitudes.
inline constexpr Utf8View kVoiceMessageFilename = utf8_literal("voice-message.ogg");
inline constexpr wchar_t kVoiceMessageFilenameWide[] = L"voice-message.ogg";
inline constexpr u32 kVoiceMessageSampleRate = 48000;
inline constexpr u32 kVoiceMessageFrameSamples = 960; // 20 ms.
// The longest recording the clients allow (20 minutes); recording stops there.
inline constexpr u32 kVoiceMessageMaximumSeconds = 20 * 60;
inline constexpr u32 kVoiceMessageMaximumFrames = kVoiceMessageMaximumSeconds * 50;
// Constrained VBR at 32 kb/s: packets average 80 bytes, so 20 minutes stay
// near 5 MB, inside every upload limit. The fixed stream buffer budgets 1.5x
// that per frame; running out ends the recording like the time limit.
inline constexpr u32 kVoiceMessageBitrate = 32000;
inline constexpr u32 kVoiceMessagePacketBytes = kVoiceMessageBitrate / 50 / 8;
inline constexpr u32 kVoiceMessagePacketBudget = kVoiceMessagePacketBytes * 3 / 2;
// Delay padding: the lookahead (312 samples) needs at most one extra frame.
inline constexpr u32 kVoiceMessagePaddingFrames = 2;
inline constexpr u32 kVoiceMessageWaveformSamples = 256;
inline constexpr u32 kVoiceMessageWaveformBase64Bytes = 344;
// Live meter: one level per 100 ms (five frames); the newest are reported.
inline constexpr u32 kVoiceMessageLevelFrames = 5;
inline constexpr u32 kVoiceMessageLiveLevels = 64;
// A press held at least this long sends on release; a shorter one is a tap
// that keeps recording hands-free ("locked").
inline constexpr u64 kVoiceMessageHoldMs = 300;
inline constexpr Utf8View kVoiceMessageVendor = utf8_literal("MaiCord");

// 0..255 display level of one PCM16 frame: -60..0 dBFS on a linear scale.
u8 voice_message_frame_level(ConstSlice<i16> pcm) noexcept;
// The uploaded waveform: kVoiceMessageWaveformSamples buckets over the recording,
// each the loudest frame level in its span (short recordings repeat frames).
// Zero frames yields silence.
void voice_message_waveform(ConstSlice<u8> frame_levels, u8 (&waveform)[kVoiceMessageWaveformSamples]) noexcept;
// Base64 into kVoiceMessageWaveformBase64Bytes; returns the encoded bytes.
u32 voice_message_waveform_base64(const u8 (&waveform)[kVoiceMessageWaveformSamples], Slice<char> output) noexcept;
// Audible duration of `samples` 48 kHz samples, rounded to milliseconds.
constexpr u32 voice_message_duration_ms(u64 samples) noexcept {
    return static_cast<u32>((samples * 1000 + kVoiceMessageSampleRate / 2) / kVoiceMessageSampleRate);
}

// Encoder, framing, live levels and the Ogg stream of one recording, in one
// fixed block sized for the maximum duration. Single producer: push and
// finish never run concurrently (the module holds its lock).
enum class VoiceMessageRecorderError : u8 { none, invalid_input, invalid_state, memory, encoder, capacity };
struct VoiceMessageRecorder {
    OpusEncoder encoder;
    OggOpusWriter writer;
    u8* block;
    u32 block_bytes;
    u8* frame_levels;   // One display level per encoded audible frame.
    i16 frame[kVoiceMessageFrameSamples];
    u32 frame_fill;
    u32 frames;         // Frames encoded, including the delay padding.
    u64 samples;        // Audible input samples.
    u32 gain_q16;       // Input gain applied before encoding.
    u16 pre_skip;
    u8 live[kVoiceMessageLiveLevels];
    u32 live_count;     // Total live levels produced.
    u32 level_peak;     // Loudest frame of the live level in progress.
    bool limit_reached;
    bool finished;
    VoiceMessageRecorderError error;
};
// Reserves and commits the block, configures the encoder (VOIP, CBR, no FEC)
// and writes the Ogg headers. gain_percent follows the input volume (0..200).
VoiceMessageRecorderError voice_message_recorder_begin(VoiceMessageRecorder*, u32 serial, u32 gain_percent) noexcept;
// Captured 48 kHz mono PCM. Stops accepting at the maximum duration and sets
// limit_reached; later input is ignored without error.
VoiceMessageRecorderError voice_message_recorder_push(VoiceMessageRecorder*, ConstSlice<i16> mono) noexcept;
// Pads the encoder delay, then closes the stream. Needs at least one frame.
VoiceMessageRecorderError voice_message_recorder_finish(VoiceMessageRecorder*) noexcept;
inline u32 voice_message_recorder_duration_ms(const VoiceMessageRecorder& r) noexcept {
    return voice_message_duration_ms(r.samples);
}
inline ConstSlice<u8> voice_message_recorder_stream(const VoiceMessageRecorder& r) noexcept {
    return r.finished ? ogg_opus_writer_stream(r.writer) : ConstSlice<u8>{};
}
// The newest live levels, oldest first; returns the count.
u32 voice_message_recorder_levels(const VoiceMessageRecorder&, Slice<u8> output) noexcept;
void voice_message_recorder_waveform(const VoiceMessageRecorder&, u8 (&waveform)[kVoiceMessageWaveformSamples]) noexcept;
void voice_message_recorder_release(VoiceMessageRecorder*) noexcept;

// Module ABI: recording and preview inside the account module (the encoder
// lives there), driven by the client. Status is copied out on every call.
enum class VoiceMessageOperation : u8 { status, start, finish, discard, play, pause, copy };
struct VoiceMessageCommand {
    VoiceMessageOperation operation;
    u8 reserved[7];
    Slice<u8> output; // copy: receives the finished Ogg stream (status.bytes).
};
static_assert(sizeof(VoiceMessageCommand) == 24);
enum class VoiceMessageState : u8 { idle, recording, finished, failed };
enum class VoiceMessageFailure : u8 { none, memory, encoder, microphone, microphone_access };
struct VoiceMessageStatus {
    VoiceMessageState state;
    VoiceMessageFailure failure;
    bool limit_reached;
    bool playing;
    bool capturing;          // The microphone stream is open and delivering.
    u8 reserved[3];
    u32 native_error;
    u32 duration_ms;         // Recorded so far, or the final audible duration.
    u32 position_ms;         // Preview playback position.
    u32 bytes;               // Finished Ogg stream size.
    u32 level_count;         // Total live levels; levels holds the newest.
    u8 levels[kVoiceMessageLiveLevels];
    u8 waveform[kVoiceMessageWaveformSamples]; // Valid once finished.
};
static_assert(sizeof(VoiceMessageStatus) == 348);

// Client-side flow of the composer's voice message control. Pure: callers
// perform the returned action against the module and the upload pipeline.
enum class VoiceMessagePhase : u8 { idle, recording, review, sending, failed };
enum class VoiceMessageEvent : u8 {
    press,           // Pointer down on the microphone: hold to record.
    release_inside,  // Pointer up over it: send a hold, lock a tap.
    release_outside, // Pointer up elsewhere: cancel a hold.
    toggle,          // Keyboard activation: start hands-free.
    stop,            // Stop a recording and keep it for review.
    cancel,          // Escape or the trash control.
    send,
    play,
    deactivated,     // The window lost activation.
    channel_changed,
    capture_failed,
    limit_reached,
    upload_ready,
    upload_failed,
    retry,
    queued,
};
enum class VoiceMessageAction : u8 {
    none,
    start,           // Begin capture (module start).
    finish,          // Stop capture, keep the recording (module finish).
    discard,         // Drop the recording and any upload.
    finish_and_send, // Finish, then upload and send.
    send,            // Upload and send the finished recording.
    toggle_preview,  // Play or pause the preview.
    retry,           // Upload again.
    queue,           // Upload done: queue the message.
    release,         // Message queued: free the recording.
};
enum class VoiceMessageNotice : u8 { none, inactive, limit, microphone, microphone_access, upload, failed_start };
struct VoiceMessageFlow {
    VoiceMessagePhase phase;
    VoiceMessageNotice notice;
    bool held;        // Recording while the pointer holds the control.
    bool hold_inside; // The held pointer is over the control.
    u64 channel_id;
    u64 pressed_ms;
};
// channel_id is the channel a press/toggle records for; ignored otherwise.
VoiceMessageAction voice_message_flow_apply(VoiceMessageFlow*, VoiceMessageEvent, u64 now_ms,
                                            u64 channel_id = 0) noexcept;
constexpr bool voice_message_flow_active(const VoiceMessageFlow& flow) noexcept {
    return flow.phase != VoiceMessagePhase::idle;
}

// The microphone shows in place of Send when the draft is empty, nothing is
// attached or being edited, and the channel allows voice messages: Send
// Messages, Attach Files and Send Voice Messages (DMs grant all three), and
// no slowmode cooldown is running.
struct VoiceMessageGate {
    u32 channel_actions;
    bool draft_empty;
    bool attachments_pending;
    bool editing;
    bool sending_restricted;
    bool slowmode_cooling;
};
bool voice_message_available(const VoiceMessageGate&) noexcept;
} // namespace maicord
