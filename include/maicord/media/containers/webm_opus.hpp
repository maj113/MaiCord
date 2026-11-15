#pragma once
#include "maicord/media/containers/mp4_audio_demux.hpp"

namespace maicord {
// One Opus track of a WebM file, read in place as a grid of packets of one length. This is what YouTube's audio
// formats 249 to 251 are. The file can be sparse: the reader asks `availability` before it reads a byte, and a
// refused query returns data_pending and changes nothing. No allocation, no copy of the packets.
//
// The elements the reader knows:
//   - the EBML header and Segment
//   - Info: TimestampScale
//   - Tracks: the first A_OPUS entry, with TrackNumber, CodecPrivate and CodecDelay
//   - Cues
//   - Cluster: Timestamp, SimpleBlock, and BlockGroup with Block and DiscardPadding
// It skips every other element. It refuses:
//   - an element of unknown size inside the Segment (a live file)
//   - a laced block
//   - a packet whose length differs from the first packet's
//   - packets longer than 40 ms
//
// Packet `n` holds the decoded frames [n * frames_per_packet, (n + 1) * frames_per_packet). The timeline keeps the
// frames after the codec delay and before the padding that the last block discards.
inline constexpr u32 kWebmOpusMaximumPacketFrames = 1920;
struct WebmOpus {
    ConstSlice<u8> encoded;
    Mp4SourceAvailability availability;
    Mp4AudioTimeline timeline;
    // The first Cluster and the end of the Segment. A cue position counts from `segment`.
    u64 clusters, clusters_end, segment;
    // The content of Cues. Both are 0 for a file whose Cues do not come before its first Cluster.
    u64 cues, cues_end;
    // The cursor: the end of the current Cluster, the next element in it and the element after the peeked packet.
    u64 cluster_end, position, next;
    u32 packet_count, packet_index;
    // Nanoseconds in one timestamp unit.
    u32 scale;
    u16 frames_per_packet, pre_skip;
    i16 output_gain_q8;
    u8 channels, track;
};
// Reads the head, the first packet and the last Cluster. Only then are the packet count and the length known.
// Failure leaves `output` as it was. `output` must be empty.
Mp4DemuxError webm_opus_initialize(WebmOpus* output, ConstSlice<u8> encoded, Mp4SourceAvailability = {}) noexcept;
// True for bytes that start with the EBML magic number.
bool webm_present(ConstSlice<u8> head) noexcept;
// The packet at the cursor. The bytes stay borrowed from the file. end_of_stream after the last packet.
Mp4DemuxError webm_opus_peek(WebmOpus*, ConstSlice<u8>* packet) noexcept;
// Moves the cursor past the packet of the last successful peek.
void webm_opus_commit(WebmOpus*) noexcept;
// Moves the cursor to a packet. The packet count is the end. Failure leaves the cursor as it was.
Mp4DemuxError webm_opus_seek(WebmOpus*, u32 packet) noexcept;
// The byte range of the Cluster that holds a packet, and the range of packets in it. For buffered ranges.
struct WebmOpusCluster {
    u64 offset, bytes;
    u32 first_packet, end_packet;
};
// Walks the Cues: `cursor` starts at 0 and the call returns false after the last Cluster. Without Cues it returns
// false at once.
bool webm_opus_next_cluster(const WebmOpus&, u64* cursor, WebmOpusCluster*) noexcept;
} // namespace maicord
