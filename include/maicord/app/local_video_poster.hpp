#pragma once

#include "maicord/app/attachment_media_probe.hpp"
#include "maicord/media/progressive_source.hpp"

namespace maicord {

// Composer MP4/MOV first-frame posters. A local file is never read whole: the
// top-level box headers, the moov and the samples the decoder actually asks
// for are copied into a sparse ProgressiveSource, one 256 KiB block at a time,
// through a caller-owned scratch block. Everything else stays on disk.
inline constexpr u64 kLocalVideoPosterReadBudget = 24u * 1024u * 1024u;
inline constexpr u32 kLocalVideoPosterScratchBytes = kProgressiveBlockBytes;
// Box-header probes of a local file read at most this much per step (the
// HTTP probe asks for 8 MiB at a time to save round trips; a disk need not).
inline constexpr u32 kLocalVideoPosterMetadataStep = 1024u * 1024u;

struct LocalVideoPosterReader {
    AttachmentProbeRead read;
    void* context;
    u8* scratch; // kLocalVideoPosterScratchBytes
    u64 budget;  // bytes this poster may still read
    u64 bytes_read;
    u32 reads;
};

enum class LocalVideoPosterStep : u8 {
    ready,
    unsupported, // fragmented movie, oversized moov, or no supported video track
    malformed,
    read_failed, // short read, cancellation or a replaced file
    budget,      // the poster needed more than kLocalVideoPosterReadBudget
};

// Copies every block the range touches (block aligned, clipped to the file).
LocalVideoPosterStep local_video_poster_read(ProgressiveSource* source, ProgressiveRange range,
                                             LocalVideoPosterReader* reader) noexcept;
// Walks the top-level boxes until the moov is resident (progressive_mp4_probe).
LocalVideoPosterStep local_video_poster_metadata(ProgressiveSource* source, LocalVideoPosterReader* reader) noexcept;
// Fetches the range the video reader was last refused, if any. False when
// nothing was pending (the caller's failure is real) or the read failed.
bool local_video_poster_service(ProgressiveSource* source, LocalVideoPosterReader* reader,
                                LocalVideoPosterStep* step) noexcept;
// Metadata, then the first video access unit (H.264 or HEVC sample entry),
// so a GPU decoder can be configured over the sparse source.
LocalVideoPosterStep local_video_poster_prepare(ProgressiveSource* source, LocalVideoPosterReader* reader) noexcept;

} // namespace maicord
