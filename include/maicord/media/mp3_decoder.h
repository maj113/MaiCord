#pragma once

// MPEG-1, MPEG-2 and MPEG-2.5 Layer III frames (ISO/IEC 11172-3, 13818-3), in C:
// src/media/mp3_decoder.c. mp3_frame_parse validates a header and its side
// information; mp3_frame_decode turns a parsed frame into interleaved float
// PCM (full scale 1.0, not clipped), keeping the bit reservoir, IMDCT overlap
// and synthesis history in caller state. Reads stay inside the frame and the
// retained reservoir for any byte sequence; nothing allocates, and a decode
// needs under 4 KiB of stack.
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct Mp3Granule {
    uint16_t part2_3_bits, big_values, scalefac_compress;
    uint8_t global_gain, block_type, mixed, table[3], subblock_gain[3], region0, region1;
    uint8_t preflag, scalefac_scale, count1_table, scfsi;
} Mp3Granule;

typedef struct Mp3Frame {
    uint32_t bytes, sample_rate, main_data_begin, main_bytes;
    uint16_t samples;
    // version: the header's ID bits, 3 MPEG-1, 2 MPEG-2, 0 MPEG-2.5. rate_index:
    // 0-2 MPEG-1 44.1/48/32 kHz, 3-5 MPEG-2 22.05/24/16, 6-8 MPEG-2.5 11.025/12/8.
    uint8_t channels, version, rate_index, mid_side, intensity;
    Mp3Granule granules[4]; // [granule * channels + channel]
} Mp3Frame;

// Zero-initialized is the initial state. 6,660 bytes.
typedef struct Mp3DecoderState {
    float overlap[2][9][32];   // per channel: IMDCT tail terms by subband
    float history[2][32][15];  // per channel: synthesis DCT rows of the last 15 slots
    uint8_t reservoir[511];    // main data not consumed by earlier frames
    uint8_t previous_block[2]; // per channel: block type | mixed << 2 of the last granule
    uint16_t reservoir_bytes;
} Mp3DecoderState;

// 0 when the header at frame and its side information describe a Layer III
// frame of at most available bytes; else Mp3Error's malformed (2),
// unsupported (3, free format) or truncated (4).
int mp3_frame_parse(const uint8_t* frame, uint32_t available, Mp3Frame* out);
// Decodes frame->bytes bytes at data, which mp3_frame_parse accepted as frame,
// into frame->samples * frame->channels samples, which are also its working
// space. Returns frame->samples, or 0 with only the reservoir updated (pcm
// untouched) when it holds fewer than main_data_begin bytes.
uint32_t mp3_frame_decode(Mp3DecoderState* state, const uint8_t* data, const Mp3Frame* frame, float* pcm);

#if defined(__cplusplus)
}
#endif
