#pragma once

#include "mp4_mft_decoder.hpp"

namespace maicord {
enum class Mp4DirectOperation : u32 {
    process_output, output_type, output_sample, input_prepare, process_input,
    end_of_stream, drain, reset, extract_frame
};
struct Mp4DirectTraceEvent {
    i64 begin_ticks, end_ticks;
    Mp4DirectOperation operation;
    u32 sample_index;
    i32 status;
};
struct Mp4DirectTrace {
    Slice<Mp4DirectTraceEvent> events;
    u32 count, dropped;
};
// Separate diagnostic specialization. Playback never records events, reads the
// clock or carries trace state. The linker discards this entry from the app/DLL.
Mp4DecodedFrame mp4_direct_decoder_read_profiled(Mp4DirectDecoder* decoder,
    bool loop_at_end, Mp4DirectTrace* trace) noexcept;
} // namespace maicord
