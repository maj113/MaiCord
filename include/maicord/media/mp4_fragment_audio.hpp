#pragma once
#include "maicord/media/mp4_fragment_source.hpp"
#include "maicord/media/mp4_audio_demux.hpp"

namespace maicord {
struct Mp4FragmentAudioPacket {
    ConstSlice<u8> bytes{};
    u64 first_source_frame{}, duration_100ns{};
    i64 presentation_time_100ns{};
};
// Borrowed sequential AAC adapter. The source remains exclusively owned by
// this lane, stays at the same address, and outlives the adapter. No allocation,
// copied packet table, codec, worker or audio endpoint. Do not externally select
// or consume its source while attached. Release the source after decoder join.
struct Mp4FragmentAudio {
    Mp4FragmentSource* source{};
    Mp4AudioTimeline timeline{};
    u32 packet_ticks{}, packet_count{}, packet_index{};
    // Some flat indexes describe edited presentation intervals. Their first
    // interval omits this encoder priming; decoded packets still start at zero.
    u32 index_edit_ticks{};
    bool pending{};
};
static_assert(sizeof(Mp4FragmentAudio) <= 88);
// Empty output, initial audio source only. Validates the indexed constant AAC
// frame grid, allowing a short final packet. Track/movie clocks stay exact;
// packet decode times and durations are checked again against each real moof.
// An edited flat index may need its first fragment to prove priming alignment;
// missing bytes return data_pending and preserve output. Success retains that
// first packet without consuming it, so normal playback reuses its metadata.
Mp4DemuxError mp4_fragment_audio_initialize(Mp4FragmentSource*, Mp4FragmentAudio*) noexcept;
// Extend a live packet timeline from received files without moving its cursor.
Mp4DemuxError mp4_fragment_audio_refresh(Mp4FragmentAudio*) noexcept;
// Failure leaves caller output and accepted packet index unchanged. Missing
// source bytes register demand; codec backpressure repeats the same packet.
// Native input duration includes decoder padding; sample_window trims it using
// the original content endpoint. Commit only after successful AAC submission.
Mp4DemuxError mp4_fragment_audio_peek(Mp4FragmentAudio*, Mp4FragmentAudioPacket*) noexcept;
bool mp4_fragment_audio_commit(Mp4FragmentAudio*) noexcept;
// Transactional packet seek. The caller chooses AAC preroll and flushes its
// decoder only after success, under the playback-generation fence. Missing
// bytes register demand without discarding the old packet. Count denotes EOF.
Mp4DemuxError mp4_fragment_audio_seek(Mp4FragmentAudio*, u32 packet) noexcept;
} // namespace maicord
