/* Decodes one opus_demo bitstream (per packet: big-endian length, big-endian encoder final range, payload; length 0
 * marks a lost packet) with libopus to 48 kHz PCM16, driven as tests/support/opus_vector_decode.hpp drives
 * MaiCord's decoder: a lost packet is concealed with the previous duration, and the last loss before a packet that
 * carries LBRR is recovered from it. tools/generate_opus_decoder_references.py builds it against the libopus fork
 * that MaiCord used before its own decoder.
 *
 * Usage: opus_reference_decode CHANNELS IN.bit OUT.pcm */
#include <stdio.h>
#include <stdlib.h>
#include "opus.h"

static unsigned big_endian(const unsigned char* p) {
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

int main(int argc, char** argv) {
    if (argc != 4)
        return 2;
    const int channels = atoi(argv[1]);
    FILE* in = fopen(argv[2], "rb");
    FILE* out = fopen(argv[3], "wb");
    if (!in || !out || (channels != 1 && channels != 2))
        return 2;
    static unsigned char stream[1 << 20];
    const size_t bytes = fread(stream, 1, sizeof(stream), in);
    void* state = malloc((size_t)opus_decoder_get_size(channels));
    void* scratch = malloc(OPUS_SCRATCH_BYTES + 16);
    OpusDecoder* decoder = (OpusDecoder*)state;
    if (!state || !scratch || opus_decoder_init(decoder, 48000, channels) != OPUS_OK)
        return 3;
    scratch = (void*)(((size_t)scratch + 15) & ~(size_t)15);
    static opus_int16 pcm[5760 * 2];
    int lost_count = 0, last_duration = 0;
    size_t at = 0;
    while (bytes - at >= 8) {
        const int length = (int)big_endian(stream + at);
        at += 8;
        if (length > 1500 || (size_t)length > bytes - at)
            return 4;
        const unsigned char* packet = stream + at;
        at += (size_t)length;
        const int runs = length == 0 ? 0 : 1 + lost_count;
        if (length == 0)
            ++lost_count;
        for (int run = 0; run < runs; ++run) {
            int samples;
            if (run + 1 == lost_count && opus_packet_has_lbrr(packet, length) > 0)
                samples = opus_decode(decoder, packet, length, pcm, last_duration, 1, scratch);
            else if (run < lost_count)
                samples = opus_decode(decoder, NULL, 0, pcm, last_duration, 0, scratch);
            else
                samples = opus_decode(decoder, packet, length, pcm, 5760, 0, scratch);
            if (samples <= 0)
                return 5;
            last_duration = samples;
            fwrite(pcm, sizeof(opus_int16), (size_t)samples * (size_t)channels, out);
        }
        if (length != 0)
            lost_count = 0;
    }
    return fclose(out) == 0 && at == bytes ? 0 : 6;
}
