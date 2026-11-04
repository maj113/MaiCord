#!/usr/bin/env python3
"""Writes the AAC-LC conformance fixtures of tests/fixtures/aac: tests/platform/decode/aac_lc.cpp decodes each one
with src/media/audio/aac_lc.cpp and compares with the reference decoder's output stored beside the bitstream.

For every case the script encodes a short synthetic signal with libavcodec's native `aac` encoder (several bitrates,
mono and stereo, transients that force short blocks, and the coder options that switch on M/S, intensity stereo,
noise substitution and TNS), then decodes the packets with libavcodec's native `aac` decoder, the oracle, and
stores its output rounded to 16 bits. The decoder under test is compared in units of that rounding: its float output
times 32768 must stay within 0.55 of the oracle's integer sample (half a step of rounding plus the two decoders'
floating point noise). A stereo case also gets an MP4 file (libavformat's mp4 muxer) and an ADTS stream, for the
demuxer and framing paths the platform decoder is fed through.

Needs libavcodec-dev, libavformat-dev and libavutil-dev on a Linux host and clang; run it in WSL:

    tools/generate_aac_fixtures.py

File layout of aac_<name>.bin, all little endian: u32 'MAAC', u32 sample rate, u32 channels, u32 ASC bytes,
u32 frame count, the ASC padded to 4 bytes, then per frame u32 size and the raw AAC frame padded to 4 bytes,
then frame count * 1024 * channels int16 interleaved reference samples.
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(ROOT, "tests", "fixtures", "aac")

# name, rate, channels, bit rate, signal, frames, options
CASES = [
    ("stereo44_128k", 44100, 2, 128000, "tonal", 5, {}),
    ("mono44_64k", 44100, 1, 64000, "tonal", 5, {}),
    ("stereo48_64k", 48000, 2, 64000, "loud", 5, {}),
    ("mono48_24k_noise", 48000, 1, 24000, "noisy", 5, {"aac_pns": 1}),
    ("stereo44_128k_transient", 44100, 2, 128000, "transient", 8, {"aac_tns": 1}),
    ("stereo44_40k_joint", 44100, 2, 40000, "correlated", 5, {"aac_is": 1, "aac_pns": 1, "aac_ms": 1, "aac_tns": 1}),
    ("stereo44_256k_loud", 44100, 2, 256000, "loud", 5, {}),
    ("mono22_32k", 22050, 1, 32000, "tonal", 5, {}),
]
CONTAINER_CASE = "stereo44_128k"

SOURCE = r"""
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 12345;
static float noise(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (float)((int32_t)rng_state) / 2147483648.0f;
}

/* One sample of the synthetic signal: a few tones, shaped by the kind. channel 0 / 1, absolute sample index. */
static float signal_at(const char *kind, int channel, long n, int rate) {
    double t = (double)n / rate;
    double left = 0.22 * sin(2 * M_PI * 440.0 * t) + 0.12 * sin(2 * M_PI * 1830.0 * t) + 0.05 * sin(2 * M_PI * 7210.0 * t);
    double right = 0.2 * sin(2 * M_PI * 554.0 * t) + 0.1 * sin(2 * M_PI * 2210.0 * t) + 0.04 * sin(2 * M_PI * 9100.0 * t);
    if (!strcmp(kind, "tonal"))
        return (float)((channel ? right : left) + 0.01 * noise());
    if (!strcmp(kind, "loud")) {
        double v = (channel ? right : left) * 2.2 + 0.2 * sin(2 * M_PI * 120.0 * t) * (channel ? 0.5 : 1.0) + 0.05 * noise();
        return (float)(v > 0.6 ? 0.6 : v < -0.6 ? -0.6 : v);
    }
    if (!strcmp(kind, "noisy"))
        return (float)(0.12 * noise() + 0.08 * (channel ? right : left));
    if (!strcmp(kind, "correlated")) {
        double common = 0.2 * noise() + 0.2 * sin(2 * M_PI * 660.0 * t) + 0.08 * sin(2 * M_PI * 5300.0 * t);
        return (float)(channel ? common * 0.7 + 0.01 * noise() : common);
    }
    /* transient: quiet tone with clicks and noise bursts after the encoder's first two frames */
    {
        double v = 0.02 * sin(2 * M_PI * 440.0 * t);
        long frame = n / 1024, within = n % 1024;
        if ((frame == 2 || frame == 3) && within > 300 && within < 700)
            v += (channel ? 0.5 : 0.6) * noise() * exp(-(double)(within - 300) / 60.0);
        if (frame == 3 && (within % 256) < 6)
            v += 0.7 * (within % 256 < 3 ? 1 : -1);
        return (float)v;
    }
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_padded(FILE *f, const uint8_t *data, int size) {
    static const uint8_t zero[4] = {0};
    fwrite(data, 1, size, f);
    if (size & 3) fwrite(zero, 1, 4 - (size & 3), f);
}

static int fail(const char *what, int code) {
    char text[128];
    av_strerror(code, text, sizeof text);
    fprintf(stderr, "%s: %s\n", what, text);
    return 1;
}

int main(int argc, char **argv) {
    /* name rate channels bitrate kind frames "option=value ..." output_directory write_containers */
    const char *name = argv[1];
    int rate = atoi(argv[2]), channels = atoi(argv[3]), bitrate = atoi(argv[4]), frames = atoi(argv[6]);
    const char *kind = argv[5], *options = argv[7], *directory = argv[8];
    int containers = atoi(argv[9]);

    const AVCodec *encoder = avcodec_find_encoder_by_name("aac");
    AVCodecContext *enc = avcodec_alloc_context3(encoder);
    enc->sample_rate = rate;
    enc->bit_rate = bitrate;
    enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc->profile = AV_PROFILE_AAC_LOW;
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_channel_layout_default(&enc->ch_layout, channels);
    av_opt_set(enc->priv_data, "aac_coder", "twoloop", 0);
    char copy[256];
    strncpy(copy, options, sizeof copy - 1);
    copy[sizeof copy - 1] = 0;
    for (char *token = strtok(copy, " "); token; token = strtok(NULL, " ")) {
        char *equals = strchr(token, '=');
        if (!equals) continue;
        *equals = 0;
        av_opt_set_int(enc->priv_data, token, atoi(equals + 1), 0);
    }
    int code = avcodec_open2(enc, encoder, NULL);
    if (code < 0) return fail("encoder open", code);

    AVFrame *frame = av_frame_alloc();
    frame->nb_samples = 1024;
    frame->format = AV_SAMPLE_FMT_FLTP;
    frame->sample_rate = rate;
    av_channel_layout_copy(&frame->ch_layout, &enc->ch_layout);
    av_frame_get_buffer(frame, 0);

    AVPacket **packets = calloc(frames + 8, sizeof(AVPacket *));
    int count = 0;
    AVPacket *packet = av_packet_alloc();
    long position = 0;
    for (int input = 0; input < frames + 3; ++input) {
        for (int c = 0; c < channels; ++c) {
            float *plane = (float *)frame->extended_data[c];
            for (int i = 0; i < 1024; ++i)
                plane[i] = signal_at(kind, c, position + i, rate);
        }
        position += 1024;
        frame->pts = input * 1024;
        code = avcodec_send_frame(enc, frame);
        if (code < 0) return fail("send_frame", code);
        while ((code = avcodec_receive_packet(enc, packet)) >= 0)
            if (count < frames + 8) { packets[count++] = av_packet_clone(packet); av_packet_unref(packet); }
    }
    avcodec_send_frame(enc, NULL);
    while ((code = avcodec_receive_packet(enc, packet)) >= 0)
        if (count < frames + 8) { packets[count++] = av_packet_clone(packet); av_packet_unref(packet); }
    if (count < frames) { fprintf(stderr, "only %d packets\n", count); return 1; }

    /* the oracle: libavcodec's native decoder over the same packets, without the priming side data */
    const AVCodec *decoder = avcodec_find_decoder_by_name("aac");
    AVCodecContext *dec = avcodec_alloc_context3(decoder);
    dec->extradata = av_mallocz(enc->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(dec->extradata, enc->extradata, enc->extradata_size);
    dec->extradata_size = enc->extradata_size;
    dec->sample_rate = rate;
    av_channel_layout_default(&dec->ch_layout, channels);
    code = avcodec_open2(dec, decoder, NULL);
    if (code < 0) return fail("decoder open", code);

    char path[512];
    snprintf(path, sizeof path, "%s/aac_%s.bin", directory, name);
    FILE *out = fopen(path, "wb");
    put32(out, 0x4341414d);
    put32(out, rate);
    put32(out, channels);
    put32(out, enc->extradata_size);
    put32(out, frames);
    put_padded(out, enc->extradata, enc->extradata_size);
    for (int i = 0; i < frames; ++i) {
        put32(out, packets[i]->size);
        put_padded(out, packets[i]->data, packets[i]->size);
    }
    AVFrame *decoded = av_frame_alloc();
    long long clipped = 0;
    for (int i = 0; i < frames; ++i) {
        AVPacket *plain = av_packet_alloc();
        av_new_packet(plain, packets[i]->size);
        memcpy(plain->data, packets[i]->data, packets[i]->size);
        code = avcodec_send_packet(dec, plain);
        av_packet_free(&plain);
        if (code < 0) return fail("send_packet", code);
        code = avcodec_receive_frame(dec, decoded);
        if (code < 0) return fail("receive_frame", code);
        if (decoded->nb_samples != 1024 || decoded->format != AV_SAMPLE_FMT_FLTP) {
            fprintf(stderr, "unexpected decoded frame %d samples format %d\n", decoded->nb_samples, decoded->format);
            return 1;
        }
        for (int n = 0; n < 1024; ++n)
            for (int c = 0; c < channels; ++c) {
                float value = ((float *)decoded->extended_data[c])[n] * 32768.0f;
                long rounded = lrintf(value);
                if (rounded > 32767 || rounded < -32768) { ++clipped; rounded = rounded > 0 ? 32767 : -32768; }
                int16_t sample = (int16_t)rounded;
                fwrite(&sample, 2, 1, out);
            }
    }
    fclose(out);
    if (clipped) { fprintf(stderr, "%s: %lld samples clipped; lower the level\n", name, clipped); return 1; }
    printf("%s: %d frames\n", name, frames);

    if (containers) {
        /* ADTS: seven byte headers in front of the same frames */
        static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};
        int index = 0;
        while (rates[index] != rate) ++index;
        snprintf(path, sizeof path, "%s/aac_%s.adts", directory, name);
        FILE *adts = fopen(path, "wb");
        for (int i = 0; i < frames; ++i) {
            int length = packets[i]->size + 7;
            uint8_t header[7] = {0xff, 0xf1, (uint8_t)((1 << 6) | (index << 2) | (channels >> 2)),
                                 (uint8_t)(((channels & 3) << 6) | (length >> 11)), (uint8_t)((length >> 3) & 0xff),
                                 (uint8_t)(((length & 7) << 5) | 0x1f), 0xfc};
            fwrite(header, 1, 7, adts);
            fwrite(packets[i]->data, 1, packets[i]->size, adts);
        }
        fclose(adts);
        /* MP4: libavformat's muxer, the packets as the encoder produced them */
        snprintf(path, sizeof path, "%s/aac_%s.mp4", directory, name);
        AVFormatContext *format = NULL;
        avformat_alloc_output_context2(&format, NULL, "mp4", path);
        AVStream *stream = avformat_new_stream(format, NULL);
        avcodec_parameters_from_context(stream->codecpar, enc);
        stream->time_base = (AVRational){1, rate};
        if (avio_open(&format->pb, path, AVIO_FLAG_WRITE) < 0) return 1;
        AVDictionary *muxer = NULL;
        av_dict_set(&muxer, "movflags", "+faststart", 0);
        code = avformat_write_header(format, &muxer);
        if (code < 0) return fail("write_header", code);
        for (int i = 0; i < frames; ++i) {
            AVPacket *copy_packet = av_packet_clone(packets[i]);
            copy_packet->stream_index = 0;
            av_packet_rescale_ts(copy_packet, (AVRational){1, rate}, stream->time_base);
            av_interleaved_write_frame(format, copy_packet);
            av_packet_free(&copy_packet);
        }
        av_write_trailer(format);
        avio_closep(&format->pb);
        avformat_free_context(format);
    }
    return 0;
}
"""


def main():
    os.makedirs(OUTPUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "generate.c")
        binary = os.path.join(directory, "generate")
        with open(source, "w") as handle:
            handle.write(SOURCE)
        subprocess.run(["clang", "-O1", "-o", binary, source, "-lavformat", "-lavcodec", "-lavutil", "-lm"], check=True)
        for name, rate, channels, bitrate, kind, frames, options in CASES:
            text = " ".join("%s=%d" % item for item in options.items())
            subprocess.run([binary, name, str(rate), str(channels), str(bitrate), kind, str(frames), text, OUTPUT,
                            "1" if name == CONTAINER_CASE else "0"], check=True)
    for name in sorted(os.listdir(OUTPUT)):
        print("%8d %s" % (os.path.getsize(os.path.join(OUTPUT, name)), name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
