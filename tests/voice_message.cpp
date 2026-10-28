#include "maicord/app/account_voice_message.hpp"
#include "maicord/app/message_attachment_drafts.hpp"
#include "maicord/app/message_attachment_storage.hpp"
#include "maicord/app/voice_message.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/media/ogg_opus.hpp"
#include "maicord/media/ogg_opus_writer.hpp"
#include "maicord/media/opus_decoder.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/protocol/attachment_upload.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/https_upload.hpp"
#include "maicord/protocol/message_create.hpp"
#include "fixtures/opus_voice_signal.hpp"
#include "test_output.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
u32 failures;
void check(bool passed, const char* message) noexcept {
    test_check(&failures, passed, message);
}
bool text_equal(ConstSlice<u8> bytes, const char* expected) noexcept {
    const u32 n = c_string_length(expected);
    return bytes.count == n && bytes_equal(bytes.data, expected, n);
}
bool text_equal(Utf8View text, const char* expected) noexcept {
    return text_equal(ConstSlice<u8>{reinterpret_cast<const u8*>(text.data), text.bytes}, expected);
}
bool contains(ConstSlice<u8> bytes, const char* needle) noexcept {
    const u32 n = c_string_length(needle);
    for (u32 i = 0; n <= bytes.count && i <= bytes.count - n; ++i)
        if (bytes_equal(bytes.data + i, needle, n))
            return true;
    return false;
}
u32 le32(const u8* p) noexcept {
    return p[0] | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
}
u64 le64(const u8* p) noexcept {
    return le32(p) | (u64{le32(p + 4)} << 32);
}
// Independent RFC 3533 CRC (bitwise, polynomial 0x04c11db7, no reflection).
u32 reference_crc(const u8* page, u32 bytes) noexcept {
    u32 crc = 0;
    for (u32 i = 0; i < bytes; ++i) {
        crc ^= u32{i >= 22 && i < 26 ? u8{0} : page[i]} << 24;
        for (u32 bit = 0; bit < 8; ++bit)
            crc = crc & 0x80000000u ? (crc << 1) ^ 0x04c11db7u : crc << 1;
    }
    return crc;
}
struct PageWalk {
    u32 pages, packets, bos_pages, eos_pages, bad_crc, bad_sequence, bad_serial, decreasing_granule;
    u64 last_granule;
};
// Walks every page: header fields, checksum, sequence, serial and granules.
PageWalk walk(ConstSlice<u8> stream) noexcept {
    PageWalk w{};
    u32 offset = 0, serial = 0;
    while (offset + 27 <= stream.count) {
        const u8* p = stream.data + offset;
        if (!bytes_equal(p, "OggS", 4) || p[4] != 0)
            break;
        const u32 segments = p[26];
        u32 body = 0;
        for (u32 i = 0; i < segments; ++i) {
            body += p[27 + i];
            if (p[27 + i] < 255 && w.pages >= 2)
                ++w.packets;
        }
        const u32 bytes = 27 + segments + body;
        if (offset + bytes > stream.count)
            break;
        w.bad_crc += reference_crc(p, bytes) != le32(p + 22) || ogg_page_checksum({p, bytes}) != le32(p + 22);
        w.bad_sequence += le32(p + 18) != w.pages;
        if (!w.pages)
            serial = le32(p + 14);
        w.bad_serial += le32(p + 14) != serial;
        w.bos_pages += (p[5] & 2) != 0;
        w.eos_pages += (p[5] & 4) != 0;
        const u64 granule = le64(p + 6);
        w.decreasing_granule += granule < w.last_granule;
        w.last_granule = granule;
        ++w.pages;
        offset += bytes;
    }
    if (offset != stream.count)
        w.bad_crc += 1000; // Trailing or truncated bytes.
    return w;
}

void test_writer() noexcept {
    static u8 buffer[64 * 1024];
    OggOpusWriter w{};
    check(ogg_opus_writer_begin(&w, {buffer, sizeof(buffer)}, 0x1234, 312, 48000, utf8_literal("MaiCord")) ==
              OggOpusWriterError::none,
          "writer: begin");
    // OpusHead: magic, version 1, mono, pre-skip, rate, gain 0, family 0.
    check(bytes_equal(buffer, "OggS", 4) && buffer[5] == 2 && le64(buffer + 6) == 0 && buffer[26] == 1 &&
              buffer[27] == 19 && bytes_equal(buffer + 28, "OpusHead", 8) && buffer[36] == 1 && buffer[37] == 1 &&
              (buffer[38] | (buffer[39] << 8)) == 312 && le32(buffer + 40) == 48000 && buffer[44] == 0 &&
              buffer[45] == 0 && buffer[46] == 0,
          "writer: OpusHead page");
    const u8* tags = buffer + 47;
    check(bytes_equal(tags, "OggS", 4) && tags[5] == 0 && tags[26] == 1 && bytes_equal(tags + 28, "OpusTags", 8) &&
              le32(tags + 36) == 7 && bytes_equal(tags + 40, "MaiCord", 7) && le32(tags + 47) == 0,
          "writer: OpusTags page");
    // 120 packets, one of 300 bytes (two lacing values): three pages of audio.
    u8 packet[300];
    for (u32 i = 0; i < sizeof(packet); ++i)
        packet[i] = static_cast<u8>(i * 7);
    bool ok = true;
    for (u32 i = 0; i < 120; ++i)
        ok = ok && ogg_opus_writer_packet(&w, {packet, i == 60 ? 300u : 80u}, 960) == OggOpusWriterError::none;
    check(ok && !ogg_opus_writer_stream(w).count, "writer: packets stage until finish");
    // Final granule trims the last packet: pre-skip + audible samples.
    const u64 final_granule = 312 + 119 * 960 + 500;
    check(ogg_opus_writer_finish(&w, 120 * 960 + 1) == OggOpusWriterError::invalid_input &&
              ogg_opus_writer_finish(&w, final_granule) == OggOpusWriterError::none,
          "writer: finish bounds the final granule");
    const auto stream = ogg_opus_writer_stream(w);
    const auto walked = walk(stream);
    check(walked.pages == 5 && walked.packets == 120 && walked.bos_pages == 1 && walked.eos_pages == 1 &&
              !walked.bad_crc && !walked.bad_sequence && !walked.bad_serial && !walked.decreasing_granule &&
              walked.last_granule == final_granule,
          "writer: pages, checksums, sequence and granules");
    // Page granules land on packet boundaries: 50 and 100 packets.
    u32 offset = 0, page = 0;
    u64 granules[5]{};
    while (offset < stream.count && page < 5) {
        const u8* p = stream.data + offset;
        u32 body = 0;
        for (u32 i = 0; i < p[26]; ++i)
            body += p[27 + i];
        granules[page++] = le64(p + 6);
        offset += 27 + p[26] + body;
    }
    check(granules[2] == 50 * 960 && granules[3] == 100 * 960 && granules[4] == final_granule,
          "writer: page granules count completed packets");
    check(ogg_opus_writer_packet(&w, {packet, 80}, 960) == OggOpusWriterError::invalid_state,
          "writer: finished stream rejects packets");

    // Capacity: a refused packet leaves a stream that still closes cleanly.
    static u8 little[600];
    OggOpusWriter s{};
    check(ogg_opus_writer_begin(&s, {little, sizeof(little)}, 7, 312, 48000, utf8_literal("MaiCord")) ==
              OggOpusWriterError::none,
          "writer: little begin");
    u32 accepted = 0;
    while (ogg_opus_writer_packet(&s, {packet, 80}, 960) == OggOpusWriterError::none)
        ++accepted;
    check(accepted >= 2 && ogg_opus_writer_finish(&s, 312 + accepted * 960 - 960 + 1) == OggOpusWriterError::none &&
              walk(ogg_opus_writer_stream(s)).eos_pages == 1 && !walk(ogg_opus_writer_stream(s)).bad_crc,
          "writer: capacity failure keeps a valid stream");
    OggOpusWriter empty{};
    check(ogg_opus_writer_begin(&empty, {little, sizeof(little)}, 7, 312, 48000, {}) == OggOpusWriterError::none &&
              ogg_opus_writer_finish(&empty, 312) == OggOpusWriterError::invalid_state,
          "writer: a stream needs one packet");
}

// Synthetic microphone: two vowels at a time, then near silence.
struct Synthetic {
    OpusVoiceSignal signal;
    void fill(i16* pcm, u32 count, bool silent) noexcept {
        for (u32 i = 0; i < count; ++i)
            pcm[i] = silent ? 0 : opus_voice_speech_sample(&signal);
    }
};

void test_recorder() noexcept {
    static VoiceMessageRecorder r;
    check(voice_message_recorder_begin(&r, 99, 100) == VoiceMessageRecorderError::none && r.pre_skip > 0 &&
              r.pre_skip < kVoiceMessageFrameSamples,
          "recorder: begin");
    Synthetic source{};
    opus_voice_signal_initialize(&source.signal);
    // 2.5 s of speech in uneven capture chunks, then 0.5 s of silence.
    i16 chunk[441];
    u32 pushed = 0;
    while (pushed < 144000) {
        const u32 count = minimum(441u, 144000 - pushed);
        source.fill(chunk, count, pushed >= 120000);
        check(voice_message_recorder_push(&r, {chunk, count}) == VoiceMessageRecorderError::none,
              "recorder: push");
        pushed += count;
    }
    check(voice_message_recorder_duration_ms(r) == 3000 && r.live_count == 30, "recorder: duration and live levels");
    u8 levels[kVoiceMessageLiveLevels];
    const u32 level_count = voice_message_recorder_levels(r, {levels, kVoiceMessageLiveLevels});
    u8 loudest = 0;
    for (u32 i = 0; i < 25 && i < level_count; ++i)
        loudest = levels[i] > loudest ? levels[i] : loudest;
    check(level_count == 30 && loudest > 100 && levels[29] == 0, "recorder: live levels track speech and silence");
    check(voice_message_recorder_finish(&r) == VoiceMessageRecorderError::none, "recorder: finish");
    const auto stream = voice_message_recorder_stream(r);
    const auto walked = walk(stream);
    check(stream.count && walked.bos_pages == 1 && walked.eos_pages == 1 && !walked.bad_crc &&
              !walked.bad_sequence && !walked.decreasing_granule && walked.last_granule == 144000u + r.pre_skip,
          "recorder: Ogg pages and final granule");
    // Constrained VBR stays within the per-frame budget the buffer is sized for.
    check(walked.packets == r.frames && stream.count < r.frames * kVoiceMessagePacketBudget &&
              stream.count > r.frames * kVoiceMessagePacketBytes / 4 &&
              u64{r.frames} * kVoiceMessageFrameSamples >= 144000u + r.pre_skip,
          "recorder: packets cover the encoder delay within the budget");
    // The client's own demuxer accepts it and the audible span is exact.
    static u8 scratch[kOggOpusMaximumPacketBytes];
    OggOpusDemuxer demux{};
    check(ogg_opus_demux_initialize(&demux, stream, {scratch, sizeof(scratch)}) == OggOpusError::none &&
              demux.channels == 1 && demux.pre_skip == r.pre_skip && demux.total_frames == 144000 &&
              demux.input_sample_rate == 48000 && demux.packet_count == r.frames,
          "recorder: demuxer reads OpusHead/OpusTags and the span");
    // Decoding the stream reproduces speech then silence.
    alignas(16) static u8 decoder_storage[32768];
    OpusDecoder decoder{};
    static i16 pcm[kOpusDecoderMaximumFrames];
    u64 loud = 0, quiet = 0, decoded = 0;
    bool decodes = opus_decoder_initialize(&decoder, {decoder_storage, sizeof(decoder_storage)}, 1).ok();
    for (;;) {
        const auto packet = ogg_opus_demux_next(&demux, {scratch, sizeof(scratch)});
        if (!packet.ok())
            break;
        const auto result = opus_decoder_decode(&decoder, packet.bytes, {pcm, kOpusDecoderMaximumFrames});
        decodes = decodes && result.ok();
        for (u32 i = packet.trim_front_frames; result.ok() && i + packet.trim_back_frames < result.frames; ++i) {
            const u64 energy = static_cast<u64>(static_cast<i64>(pcm[i]) * pcm[i]);
            (decoded < 120000 ? loud : quiet) += energy;
            ++decoded;
        }
    }
    check(decodes && decoded == 144000 && loud / 120000 > 100 * (quiet / 24000 + 1),
          "recorder: decoded audio matches the input");
    u8 waveform[kVoiceMessageWaveformSamples];
    voice_message_recorder_waveform(r, waveform);
    u8 peak = 0;
    for (u32 i = 0; i < 200; ++i)
        peak = waveform[i] > peak ? waveform[i] : peak;
    check(peak > 100 && waveform[250] == 0, "recorder: waveform follows the recording");
    voice_message_recorder_release(&r);
    check(!r.block && voice_message_recorder_finish(&r) == VoiceMessageRecorderError::invalid_state,
          "recorder: release");
    // Gain 0 records silence; an empty recording cannot finish.
    check(voice_message_recorder_begin(&r, 1, 0) == VoiceMessageRecorderError::none &&
              voice_message_recorder_finish(&r) == VoiceMessageRecorderError::invalid_state,
          "recorder: empty recording");
    voice_message_recorder_release(&r);
}

void test_waveform() noexcept {
    u8 levels[512];
    for (u32 i = 0; i < 512; ++i)
        levels[i] = static_cast<u8>(i / 2);
    u8 waveform[kVoiceMessageWaveformSamples];
    voice_message_waveform({levels, 512}, waveform);
    check(waveform[0] == 0 && waveform[1] == 1 && waveform[255] == 255, "waveform: bucket maxima");
    voice_message_waveform({levels + 200, 10}, waveform);
    check(waveform[0] == 100 && waveform[51] == 100 && waveform[52] == 101 && waveform[255] == 104,
          "waveform: short recordings repeat frames");
    voice_message_waveform({}, waveform);
    bool silent = true;
    for (u8 v : waveform)
        silent = silent && !v;
    check(silent, "waveform: empty is silent");
    for (u32 i = 0; i < kVoiceMessageWaveformSamples; ++i)
        waveform[i] = static_cast<u8>(i);
    char encoded[kVoiceMessageWaveformBase64Bytes];
    const u32 bytes = voice_message_waveform_base64(waveform, {encoded, sizeof(encoded)});
    u8 decoded[kVoiceMessageWaveformSamples];
    const auto round = base64_decode({encoded, bytes}, {decoded, sizeof(decoded)});
    check(bytes == 344 && round.ok() && round.bytes == 256 && bytes_equal(decoded, waveform, 256) &&
              bytes_equal(encoded, "AAECAwQF", 8) && gateway_message_waveform_valid({encoded, bytes}),
          "waveform: base64 of u8 samples");
    check(voice_message_frame_level({}) == 0, "waveform: empty frame level");
    i16 full[960];
    for (u32 i = 0; i < 960; ++i)
        full[i] = i % 2 ? 32767 : -32767;
    i16 quiet[960];
    for (u32 i = 0; i < 960; ++i)
        quiet[i] = i % 2 ? 30 : -30; // About -61 dBFS.
    check(voice_message_frame_level({full, 960}) == 255 && voice_message_frame_level({quiet, 960}) == 0,
          "waveform: frame level spans -60..0 dBFS");
}

void test_duration() noexcept {
    char text[16];
    const auto duration = [&](u32 ms, const char* expected) noexcept {
        const u32 bytes = attachment_upload_write_duration(ms, {text, sizeof(text)});
        return text_equal(Utf8View{text, bytes}, expected);
    };
    check(duration(4480, "4.48") && duration(1000, "1") && duration(5, "0.005") && duration(12345, "12.345") &&
              duration(1200000, "1200") && duration(60010, "60.01"),
          "duration_secs formatting");
    check(voice_message_duration_ms(48000) == 1000 && voice_message_duration_ms(215040) == 4480 &&
              voice_message_duration_ms(24) == 1,
          "duration from samples");
}

void test_requests() noexcept {
    // 1. POST /channels/{id}/attachments for an upload URL.
    u8 target[128], body[8192];
    const auto request = attachment_upload_write(123456789, 0, kVoiceMessageFilename, 48123, {target, sizeof(target)},
                                                 {body, sizeof(body)});
    check(request.ok && text_equal({target, request.target_bytes}, "/api/v9/channels/123456789/attachments") &&
              text_equal({body, request.body_bytes},
                         "{\"files\":[{\"filename\":\"voice-message.ogg\",\"file_size\":48123,\"id\":\"0\","
                         "\"is_clip\":false}]}"),
          "upload: attachment URL request");
    // 2. PUT the Ogg bytes as audio/ogg.
    u8 head[1024];
    const u32 head_bytes = https_upload_write_head(utf8_literal("https://discord-attachments-uploads-prd.storage."
                                                                "googleapis.com/abc?upload_id=x"),
                                                   48123, {head, sizeof(head)}, HttpsUploadContentType::audio_ogg);
    check(head_bytes && contains({head, head_bytes}, "PUT /abc?upload_id=x HTTP/1.1\r\n") &&
              contains({head, head_bytes}, "\r\nContent-Type: audio/ogg\r\nContent-Length: 48123\r\n") &&
              !contains({head, head_bytes}, "octet-stream"),
          "upload: PUT head carries audio/ogg");
    const u32 file_head = https_upload_write_head(utf8_literal("https://uploads.example.invalid/x"), 1, {head, 1024});
    check(file_head && contains({head, file_head}, "Content-Type: application/octet-stream"),
          "upload: files keep octet-stream");
    // 3. The message: IS_VOICE_MESSAGE, no content, one voice attachment.
    u8 waveform_samples[kVoiceMessageWaveformSamples]{};
    waveform_samples[0] = 255;
    char waveform[kVoiceMessageWaveformBase64Bytes];
    const u32 waveform_bytes = voice_message_waveform_base64(waveform_samples, {waveform, sizeof(waveform)});
    UploadedAttachment voice{kVoiceMessageFilename, utf8_literal("abc/voice-message.ogg"), {}, 0, false,
                             {waveform, waveform_bytes}, 4480};
    static u8 message[kMessageCreateMaximumBodyBytes];
    const auto written = write_message_create_body({}, 1300000000000000001ull, 0, 0, {&voice, 1}, {},
                                                   {message, sizeof(message)}, true,
                                                   kMessageFlagVoiceMessage);
    char expected[1024];
    u32 used = 0;
    const auto append = [&](const char* text, u32 bytes = ~0u) noexcept {
        if (bytes == ~0u)
            bytes = c_string_length(text);
        copy_bytes(expected + used, text, bytes);
        used += bytes;
    };
    append("{\"content\":\"\",\"nonce\":\"1300000000000000001\",\"tts\":false,\"flags\":8192,\"attachments\":[{\"id\":"
           "\"0\",\"filename\":\"voice-message.ogg\",\"uploaded_filename\":\"abc/voice-message.ogg\","
           "\"duration_secs\":4.48,\"waveform\":\"");
    append(waveform, waveform_bytes);
    append("\"}],\"mobile_network_type\":\"unknown\",\"enforce_nonce\":true}");
    check(written.ok() && written.bytes == used && bytes_equal(message, expected, used), "message: voice body");
    // Rejections: content, extra attachments, stickers, or a waveform without the flag.
    UploadedAttachment pair[2]{voice, voice};
    pair[1].id = 1;
    const u64 sticker = 5;
    check(!write_message_create_body(utf8_literal("hi"), 1, 0, 0, {&voice, 1}, {}, {message, 4096}, true,
                                     kMessageFlagVoiceMessage)
                   .ok() &&
              !write_message_create_body({}, 1, 0, 0, {pair, 2}, {}, {message, 4096}, true,
                                         kMessageFlagVoiceMessage)
                   .ok() &&
              !write_message_create_body({}, 1, 0, 0, {&voice, 1}, {&sticker, 1}, {message, 4096}, true,
                                         kMessageFlagVoiceMessage)
                   .ok() &&
              !write_message_create_body({}, 1, 0, 0, {&voice, 1}, {}, {message, 4096}, true, 0).ok(),
          "message: voice rules");
    UploadedAttachment plain{kVoiceMessageFilename, utf8_literal("abc/voice-message.ogg"), {}, 0, false};
    UploadedAttachment no_duration = voice;
    no_duration.duration_ms = 0;
    UploadedAttachment bad_waveform = voice;
    bad_waveform.waveform = utf8_literal("not base64!");
    check(!write_message_create_body({}, 1, 0, 0, {&plain, 1}, {}, {message, 4096}, true,
                                     kMessageFlagVoiceMessage)
                   .ok() &&
              !attachment_upload_valid_reference(no_duration) && !attachment_upload_valid_reference(bad_waveform) &&
              write_message_create_body({}, 1, 0, 0, {&plain, 1}, {}, {message, 4096}).ok(),
          "message: voice attachment validity");
    // The request bank keeps the voice fields for retries.
    u64 size = 48123, sizes[1]{};
    u8 bank[1024];
    UploadedAttachment restored[1]{};
    u32 count = 0;
    check(message_attachment_storage_write({&voice, 1}, {bank, sizeof(bank)}, {&size, 1}) &&
              message_attachment_storage_read({bank, message_attachment_storage_bytes({&voice, 1}, {&size, 1})},
                                              {restored, 1}, &count, {sizes, 1}) &&
              count == 1 && sizes[0] == 48123 && restored[0].duration_ms == 4480 &&
              utf8_equal(restored[0].waveform, voice.waveform) && attachment_upload_is_voice(restored[0]),
          "message: request bank keeps voice fields");
}

// Memory upload source standing in for the client's copied stream.
struct Memory {
    volatile LONG references;
    u8 bytes[256];
};
void memory_retain(void* m) noexcept {
    InterlockedIncrement(&static_cast<Memory*>(m)->references);
}
void memory_release(void* m) noexcept {
    InterlockedDecrement(&static_cast<Memory*>(m)->references);
}
ConstSlice<u8> memory_read(void* m, u64 offset, Slice<u8> scratch) noexcept {
    if (!scratch.data || !scratch.count || offset >= 256) return {};
    return {static_cast<Memory*>(m)->bytes + offset, static_cast<u32>(minimum<u64>(256 - offset, scratch.count))};
}
constexpr UploadSourceOps kMemoryOps{memory_retain, memory_release, memory_read};

void test_drafts() noexcept {
    static MessageAttachmentDraftStore store;
    static Memory memory;
    memory.references = 1;
    check(message_attachment_drafts_initialize(&store), "drafts: initialize");
    u64 id = 0;
    const char waveform[] = "AAECAw==";
    check(message_attachment_drafts_add_voice(&store, 77, {kVoiceMessageFilenameWide, 17},
                                              {&memory, &kMemoryOps, 256}, {waveform, 8}, 4480,
                                              &id) == MessageAttachmentDraftError::none &&
              memory.references == 2,
          "drafts: voice draft retains its source");
    const auto* record = message_attachment_drafts_get(&store, id);
    check(record && record->voice && record->phase == MessageAttachmentDraftPhase::waiting_url &&
              record->source.bytes == 256 && message_attachment_drafts_channel(&store, 77, {}) == 0 &&
              !message_attachment_drafts_description(&store, id).bytes &&
              text_equal(message_attachment_drafts_filename(&store, id), "voice-message.ogg"),
          "drafts: voice drafts stay out of the file strip");
    UploadedAttachment reference{};
    check(!message_attachment_drafts_reference(&store, id, 0, &reference) &&
              message_attachment_drafts_uploaded(&store, id, utf8_literal("up/voice-message.ogg")) &&
              message_attachment_drafts_reference(&store, id, 0, &reference) && reference.duration_ms == 4480 &&
              text_equal(reference.waveform, "AAECAw==") && !reference.description.bytes &&
              text_equal(reference.uploaded_filename, "up/voice-message.ogg"),
          "drafts: ready voice reference");
    check(message_attachment_drafts_remove(&store, id) && memory.references == 1, "drafts: removal releases");
    message_attachment_drafts_release(&store);
}

void test_gating() noexcept {
    const u32 dm = channel_actions(1, 0, true);
    const u64 text_permissions = kPermissionViewChannel | kPermissionSendMessages | kPermissionAttachFiles;
    const u32 guild_without = channel_actions(0, text_permissions, true);
    const u32 guild_with = channel_actions(0, text_permissions | kPermissionSendVoiceMessages, true);
    const u32 no_attach = channel_actions(0, kPermissionViewChannel | kPermissionSendMessages |
                                                 kPermissionSendVoiceMessages, true);
    const auto gate = [](u32 actions) noexcept { return VoiceMessageGate{actions, true, false, false, false, false}; };
    check(voice_message_available(gate(dm)) && voice_message_available(gate(guild_with)) &&
              !voice_message_available(gate(guild_without)) && !voice_message_available(gate(no_attach)) &&
              !voice_message_available(gate(channel_actions(0, kPermissionViewChannel |
                                                                    kPermissionSendVoiceMessages | kPermissionAttachFiles,
                                                                true))),
          "gating: Send Messages, Attach Files and Send Voice Messages");
    auto g = gate(dm);
    g.draft_empty = false;
    check(!voice_message_available(g), "gating: only with an empty draft");
    g = gate(dm);
    g.attachments_pending = true;
    check(!voice_message_available(g), "gating: not with attachments");
    g = gate(dm);
    g.editing = true;
    check(!voice_message_available(g), "gating: not while editing");
    g = gate(dm);
    g.slowmode_cooling = true;
    check(!voice_message_available(g), "gating: not during a slowmode cooldown");
    g = gate(dm);
    g.sending_restricted = true;
    check(!voice_message_available(g), "gating: not while sending is restricted");
}

void test_flow() noexcept {
    VoiceMessageFlow f{};
    check(voice_message_flow_apply(&f, VoiceMessageEvent::send, 0, 9) == VoiceMessageAction::none &&
              voice_message_flow_apply(&f, VoiceMessageEvent::press, 0, 0) == VoiceMessageAction::none &&
              f.phase == VoiceMessagePhase::idle,
          "flow: idle ignores everything but a press on a channel");
    // Hold to record, release over the button to send.
    check(voice_message_flow_apply(&f, VoiceMessageEvent::press, 1000, 9) == VoiceMessageAction::start &&
              f.phase == VoiceMessagePhase::recording && f.held && f.hold_inside && f.channel_id == 9,
          "flow: press starts a held recording");
    check(voice_message_flow_apply(&f, VoiceMessageEvent::release_inside, 3000) ==
                  VoiceMessageAction::finish_and_send &&
              f.phase == VoiceMessagePhase::sending && !f.held,
          "flow: releasing a hold sends");
    check(voice_message_flow_apply(&f, VoiceMessageEvent::upload_ready, 3100) == VoiceMessageAction::queue &&
              voice_message_flow_apply(&f, VoiceMessageEvent::queued, 3100) == VoiceMessageAction::release &&
              f.phase == VoiceMessagePhase::idle,
          "flow: upload then queue");
    // A tap locks; stop reviews; play toggles the preview; send sends.
    voice_message_flow_apply(&f, VoiceMessageEvent::press, 5000, 9);
    check(voice_message_flow_apply(&f, VoiceMessageEvent::release_inside, 5100) == VoiceMessageAction::none &&
              f.phase == VoiceMessagePhase::recording && !f.held,
          "flow: a tap records hands-free");
    check(voice_message_flow_apply(&f, VoiceMessageEvent::release_outside, 5200) == VoiceMessageAction::none &&
              voice_message_flow_apply(&f, VoiceMessageEvent::stop, 9000) == VoiceMessageAction::finish &&
              f.phase == VoiceMessagePhase::review &&
              voice_message_flow_apply(&f, VoiceMessageEvent::play, 9100) == VoiceMessageAction::toggle_preview &&
              voice_message_flow_apply(&f, VoiceMessageEvent::send, 9200) == VoiceMessageAction::send &&
              f.phase == VoiceMessagePhase::sending,
          "flow: stop, preview, send");
    check(voice_message_flow_apply(&f, VoiceMessageEvent::upload_failed, 9300) == VoiceMessageAction::none &&
              f.phase == VoiceMessagePhase::failed && f.notice == VoiceMessageNotice::upload &&
              voice_message_flow_apply(&f, VoiceMessageEvent::retry, 9400) == VoiceMessageAction::retry &&
              f.phase == VoiceMessagePhase::sending &&
              voice_message_flow_apply(&f, VoiceMessageEvent::cancel, 9500) == VoiceMessageAction::discard &&
              f.phase == VoiceMessagePhase::idle,
          "flow: failed upload retries or deletes");
    // Releasing a hold elsewhere cancels; Escape cancels; changes stop it.
    voice_message_flow_apply(&f, VoiceMessageEvent::press, 10000, 9);
    check(voice_message_flow_apply(&f, VoiceMessageEvent::release_outside, 12000) == VoiceMessageAction::discard &&
              f.phase == VoiceMessagePhase::idle,
          "flow: release elsewhere cancels");
    voice_message_flow_apply(&f, VoiceMessageEvent::toggle, 13000, 9);
    check(!f.held && voice_message_flow_apply(&f, VoiceMessageEvent::deactivated, 14000) == VoiceMessageAction::finish &&
              f.phase == VoiceMessagePhase::review && f.notice == VoiceMessageNotice::inactive,
          "flow: deactivation stops into review");
    check(voice_message_flow_apply(&f, VoiceMessageEvent::channel_changed, 15000) == VoiceMessageAction::discard &&
              f.phase == VoiceMessagePhase::idle,
          "flow: channel change discards");
    voice_message_flow_apply(&f, VoiceMessageEvent::toggle, 16000, 9);
    check(voice_message_flow_apply(&f, VoiceMessageEvent::limit_reached, 17000) == VoiceMessageAction::finish &&
              f.notice == VoiceMessageNotice::limit,
          "flow: the limit stops into review");
    voice_message_flow_apply(&f, VoiceMessageEvent::cancel, 17100);
    voice_message_flow_apply(&f, VoiceMessageEvent::toggle, 18000, 9);
    check(voice_message_flow_apply(&f, VoiceMessageEvent::capture_failed, 18100) == VoiceMessageAction::discard &&
              f.phase == VoiceMessagePhase::idle && f.notice == VoiceMessageNotice::microphone,
          "flow: microphone failure");
    voice_message_flow_apply(&f, VoiceMessageEvent::toggle, 19000, 9);
    voice_message_flow_apply(&f, VoiceMessageEvent::send, 19100);
    check(voice_message_flow_apply(&f, VoiceMessageEvent::channel_changed, 19200) == VoiceMessageAction::discard &&
              f.phase == VoiceMessagePhase::idle,
          "flow: channel change abandons an upload");
}

// Synthetic capture: the test thread plays the capture thread.
struct FakeCapture {
    VoiceAudioCaptureSink sink;
    void* context;
    bool running, refuse;
    VoiceAudioCaptureStatus status;
    wchar_t device[kVoiceDeviceIdCharacters];
    bool processing;
    u32 starts, stops;
};
bool fake_start(void* self, const wchar_t* device, bool processing, VoiceAudioCaptureSink sink,
                VoiceAudioCaptureTick, void* context) noexcept {
    auto* c = static_cast<FakeCapture*>(self);
    ++c->starts;
    if (c->refuse)
        return false;
    c->sink = sink;
    c->context = context;
    c->running = true;
    c->processing = processing;
    copy_bytes(c->device, device, sizeof(c->device));
    c->status = {VoiceCaptureState::capturing, 1, 0};
    return true;
}
void fake_stop(void* self) noexcept {
    auto* c = static_cast<FakeCapture*>(self);
    ++c->stops;
    c->running = false;
}
VoiceAudioCaptureStatus fake_status(void* self) noexcept {
    return static_cast<FakeCapture*>(self)->status;
}
constexpr VoiceMessageCaptureOps kFakeOps{fake_start, fake_stop, fake_status};

void test_module_flow() noexcept {
    static AccountVoiceMessage v;
    static FakeCapture capture;
    account_voice_message_initialize(&v, &kFakeOps, &capture);
    auto settings = voice_audio_settings_defaults();
    copy_bytes(settings.input_device, L"{mic}", 6 * sizeof(wchar_t));
    settings.voice_processing = false;
    settings.input_gain_percent = 150;
    account_voice_message_configure(&v, settings);
    VoiceMessageStatus status{};
    VoiceMessageCommand command{};
    const auto run = [&](VoiceMessageOperation operation, Slice<u8> output = {}) noexcept {
        command.operation = operation;
        command.output = output;
        return account_voice_message_command(&v, &command, &status);
    };
    // The composer flow drives the module like the client does.
    VoiceMessageFlow flow{};
    check(voice_message_flow_apply(&flow, VoiceMessageEvent::press, 100, 42) == VoiceMessageAction::start &&
              run(VoiceMessageOperation::start) && status.state == VoiceMessageState::recording && capture.running &&
              !capture.processing && bytes_equal(capture.device, L"{mic}", 6 * sizeof(wchar_t)) &&
              v.recorder.gain_q16 == (150u << 16) / 100,
          "module: start opens the selected microphone with the voice settings");
    Synthetic source{};
    opus_voice_signal_initialize(&source.signal);
    i16 chunk[480];
    for (u32 i = 0; i < 200; ++i) { // 2 s in 10 ms capture packets.
        source.fill(chunk, 480, false);
        capture.sink(capture.context, {chunk, 480}, 0);
    }
    u8 live_peak = 0;
    const bool polled = run(VoiceMessageOperation::status);
    for (u32 i = 0; i < 20; ++i)
        live_peak = status.levels[i] > live_peak ? status.levels[i] : live_peak;
    check(polled && status.duration_ms == 2000 && status.capturing && status.level_count == 20 && live_peak > 100 &&
              !status.bytes,
          "module: live duration and levels");
    check(voice_message_flow_apply(&flow, VoiceMessageEvent::release_inside, 2100) ==
                  VoiceMessageAction::finish_and_send &&
              run(VoiceMessageOperation::finish) && status.state == VoiceMessageState::finished && !capture.running &&
              status.bytes > 2000 && status.duration_ms == 2000 && status.waveform[100] > 0,
          "module: finish closes the stream");
    static u8 copied[64 * 1024];
    check(!run(VoiceMessageOperation::copy, {copied, 16}) && run(VoiceMessageOperation::copy, {copied, sizeof(copied)}) &&
              bytes_equal(copied, "OggS", 4) && walk({copied, status.bytes}).eos_pages == 1,
          "module: copy hands the client the Ogg stream");
    // Preview rendering (the output thread's callback), without a device.
    AcquireSRWLockExclusive(v.lock.native());
    v.playing = true;
    ReleaseSRWLockExclusive(v.lock.native());
    static i16 rendered[960 * 2];
    u64 energy = 0;
    for (u32 i = 0; i < 20; ++i) {
        account_voice_message_render(&v, {rendered, 960 * 2}, 0);
        for (u32 s = 0; s < 960; ++s)
            energy += static_cast<u64>(static_cast<i64>(rendered[2 * s]) * rendered[2 * s]);
    }
    check(energy > 0 && rendered[0] == rendered[1] && run(VoiceMessageOperation::status) && status.playing &&
              status.position_ms == 400,
          "module: preview renders the recording");
    for (u32 i = 0; i < 200; ++i)
        account_voice_message_render(&v, {rendered, 960 * 2}, 0);
    check(run(VoiceMessageOperation::status) && !status.playing && status.position_ms == 2000 && rendered[0] == 0,
          "module: preview stops at the end");
    check(run(VoiceMessageOperation::discard) && status.state == VoiceMessageState::idle && !status.bytes &&
              !v.recorder.block,
          "module: discard frees the recording");
    // Microphone failures: refused start, then Windows privacy denial.
    capture.refuse = true;
    check(run(VoiceMessageOperation::start) && status.state == VoiceMessageState::failed &&
              status.failure == VoiceMessageFailure::microphone,
          "module: a microphone that cannot start fails");
    capture.refuse = false;
    check(run(VoiceMessageOperation::start) && status.state == VoiceMessageState::recording, "module: restart");
    capture.status = {VoiceCaptureState::waiting, 0, 0x80070005u};
    check(run(VoiceMessageOperation::status) && status.state == VoiceMessageState::failed &&
              status.failure == VoiceMessageFailure::microphone_access && status.native_error == 0x80070005u &&
              !capture.running,
          "module: blocked microphone access is reported");
    // Finishing before any audio arrives is a microphone failure, not a stream.
    capture.status = {VoiceCaptureState::capturing, 1, 0};
    check(run(VoiceMessageOperation::start) && run(VoiceMessageOperation::finish) &&
              status.state == VoiceMessageState::failed && status.failure == VoiceMessageFailure::microphone,
          "module: silence from the device fails");
    check(!run(VoiceMessageOperation::copy, {copied, sizeof(copied)}) && run(VoiceMessageOperation::discard),
          "module: nothing to copy after a failure");
    account_voice_message_release(&v);
}
} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_voice_message_tests_entry() noexcept {
    maicord::test_writer();
    maicord::test_recorder();
    maicord::test_waveform();
    maicord::test_duration();
    maicord::test_requests();
    maicord::test_drafts();
    maicord::test_gating();
    maicord::test_flow();
    maicord::test_module_flow();
    ExitProcess(maicord::failures ? 1 : 0);
}
