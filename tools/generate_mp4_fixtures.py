#!/usr/bin/env python3
"""Writes the MP4 attachment fixtures of tests/fixtures/mp4: short H.264 movies (and one with AAC audio) made by
libavformat's mp4 muxer from libx264 and the native AAC encoder over a synthetic moving picture and a two-tone
signal, and the pictures and PCM libavformat and libavcodec's own decoders give back from the finished file (with the
container's edit list applied) as the oracle. tests/platform/decode/mp4_decoder_linux.cpp decodes the movies through
mp4_decoder_* (libopenh264 or VA-API) and compares with them.

Needs libavformat-dev, libavcodec-dev, libavutil-dev (with libx264 in libavcodec) and clang on a Linux host:

    tools/generate_mp4_fixtures.py

Writes <name>.mp4 (the movie as muxed) and <name>.ref beside it. Layout of <name>.ref, all little endian:
u32 'MMP4', u32 width, u32 height (the visible extent), u32 frame count, u32 audio sample rate (0: no audio),
u32 audio channels, u32 audio sample count (per channel), then per frame i64 presentation time in microseconds (the
movie timeline: 0 is the movie start), u64 duration in microseconds and the picture (I420, width * height * 3 / 2
bytes, output order), then the PCM as int16 interleaved (sample count * channels), the priming the edit list drops
already removed.
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(ROOT, "tests", "fixtures", "mp4")

# name, width, height, frames, fps, audio (sample rate, channels; 0: none), movflags, profile, encoder parameters, encoder
CASES = [
    ("h264_aac_bframes", 64, 48, 25, 25, (22050, 2), "faststart",
     "main", "keyint=10:min-keyint=10:bframes=2:b-pyramid=none:weightp=0:ref=3:scenecut=0:threads=1:rc-lookahead=6",
     "libx264"),
    ("h264_baseline_gops", 48, 32, 20, 20, (0, 0), "",
     "baseline", "keyint=10:min-keyint=10:bframes=0:ref=2:scenecut=0:threads=1", "libx264"),
    ("h264_high_crop_pyramid", 50, 34, 18, 30, (0, 0), "",
     "high", "keyint=9:min-keyint=9:bframes=3:b-pyramid=normal:weightb=1:ref=4:scenecut=0:threads=1:rc-lookahead=8:8x8dct=1",
     "libx264"),
    ("h264_fragmented", 64, 48, 25, 25, (22050, 1), "frag_keyframe+empty_moov+default_base_moof+global_sidx",
     "main", "keyint=10:min-keyint=10:bframes=2:b-pyramid=none:ref=3:scenecut=0:threads=1:rc-lookahead=6", "libx264"),
    ("hevc_main_aac", 64, 48, 25, 25, (22050, 2), "faststart",
     "main", "keyint=10:min-keyint=10:bframes=2:log-level=none:frame-threads=1:pools=none", "libx265"),
]

SOURCE = r"""
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 4242;
static int noise(int range) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (int)((rng_state >> 16) % (uint32_t)range);
}

/* A moving box over a gradient with a slow fade and a little noise: motion for inter prediction, edges for the loop
   filters. */
static void synthesize(AVFrame *f, int n, int w, int h) {
    int fade = 16 + n * 4;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = 40 + (x * 3 + y * 2) % 150 + fade + noise(5);
            int bx = (n * 3) % (w - 24), by = (n * 2) % (h - 20);
            if (x >= bx && x < bx + 24 && y >= by && y < by + 20) v = 220 - ((x ^ y) & 31);
            f->data[0][y * f->linesize[0] + x] = v > 235 ? 235 : (uint8_t)v;
        }
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            f->data[1][y * f->linesize[1] + x] = (uint8_t)(110 + (x * 4 + n * 3) % 40);
            f->data[2][y * f->linesize[2] + x] = (uint8_t)(120 + (y * 4 + n * 2) % 30);
        }
}

static void put32(FILE *out, uint32_t v) { fputc(v, out); fputc(v >> 8, out); fputc(v >> 16, out); fputc(v >> 24, out); }
static void put64(FILE *out, uint64_t v) { put32(out, (uint32_t)v); put32(out, (uint32_t)(v >> 32)); }

static int die(const char *what) { fprintf(stderr, "%s\n", what); exit(2); return 2; }

static void write_packets(AVFormatContext *mux, AVCodecContext *enc, AVStream *stream, AVFrame *frame, AVPacket *pkt) {
    if (avcodec_send_frame(enc, frame) < 0) die("send_frame");
    while (avcodec_receive_packet(enc, pkt) == 0) {
        /* The encoder leaves the last packet's duration unset, which the muxer then writes as a zero-length sample. */
        if (!pkt->duration && enc->codec_type == AVMEDIA_TYPE_VIDEO) pkt->duration = 1;
        av_packet_rescale_ts(pkt, enc->time_base, stream->time_base);
        pkt->stream_index = stream->index;
        if (av_interleaved_write_frame(mux, pkt) < 0) die("write_frame");
    }
}

/* generate <out.mp4> <out.ref> <w> <h> <frames> <fps> <rate> <channels> <movflags> <profile> <params> <encoder> */
int main(int argc, char **argv) {
    const char *movie = argv[1], *reference = argv[2];
    int w = atoi(argv[3]), h = atoi(argv[4]), frames = atoi(argv[5]), fps = atoi(argv[6]);
    int rate = atoi(argv[7]), channels = atoi(argv[8]);
    const char *movflags = argv[9], *profile = argv[10], *params = argv[11], *encoder = argv[12];
    const int hevc = strstr(encoder, "265") != NULL;

    AVFormatContext *mux = NULL;
    avformat_alloc_output_context2(&mux, NULL, "mp4", movie);
    if (!mux) die("no mp4 muxer");
    const AVCodec *vc = avcodec_find_encoder_by_name(encoder);
    if (!vc) die("no video encoder");
    AVCodecContext *ve = avcodec_alloc_context3(vc);
    ve->width = w; ve->height = h; ve->time_base = (AVRational){1, fps}; ve->framerate = (AVRational){fps, 1};
    ve->pix_fmt = AV_PIX_FMT_YUV420P; ve->gop_size = 10;
    ve->color_range = AVCOL_RANGE_MPEG; ve->colorspace = AVCOL_SPC_BT709;
    ve->color_primaries = AVCOL_PRI_BT709; ve->color_trc = AVCOL_TRC_BT709;
    if (mux->oformat->flags & AVFMT_GLOBALHEADER) ve->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(ve->priv_data, "profile", profile, 0);
    av_opt_set(ve->priv_data, "preset", "medium", 0);
    av_opt_set(ve->priv_data, "crf", "30", 0);
    av_opt_set(ve->priv_data, hevc ? "x265-params" : "x264-params", params, 0);
    if (avcodec_open2(ve, vc, NULL) < 0) die("cannot open the video encoder");
    AVStream *vs = avformat_new_stream(mux, NULL);
    vs->time_base = ve->time_base;
    avcodec_parameters_from_context(vs->codecpar, ve);
    if (hevc) vs->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1'); /* the sample entry players expect in MP4 */

    AVCodecContext *ae = NULL;
    AVStream *as = NULL;
    if (rate) {
        const AVCodec *ac = avcodec_find_encoder_by_name("aac");
        if (!ac) die("no aac encoder");
        ae = avcodec_alloc_context3(ac);
        ae->sample_rate = rate;
        av_channel_layout_default(&ae->ch_layout, channels);
        ae->sample_fmt = AV_SAMPLE_FMT_FLTP;
        ae->bit_rate = 48000 * channels;
        ae->time_base = (AVRational){1, rate};
        if (mux->oformat->flags & AVFMT_GLOBALHEADER) ae->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(ae, ac, NULL) < 0) die("cannot open aac");
        as = avformat_new_stream(mux, NULL);
        as->time_base = ae->time_base;
        avcodec_parameters_from_context(as->codecpar, ae);
    }
    AVDictionary *options = NULL;
    if (movflags[0]) av_dict_set(&options, "movflags", movflags, 0);
    if (!(mux->oformat->flags & AVFMT_NOFILE) && avio_open(&mux->pb, movie, AVIO_FLAG_WRITE) < 0) die("cannot open output");
    if (avformat_write_header(mux, &options) < 0) die("write_header");

    AVPacket *pkt = av_packet_alloc();
    AVFrame *vf = av_frame_alloc();
    vf->format = AV_PIX_FMT_YUV420P; vf->width = w; vf->height = h;
    av_frame_get_buffer(vf, 0);
    for (int i = 0; i < frames; ++i) {
        av_frame_make_writable(vf);
        synthesize(vf, i, w, h);
        vf->pts = i;
        write_packets(mux, ve, vs, vf, pkt);
    }
    write_packets(mux, ve, vs, NULL, pkt);
    if (ae) {
        int total = rate; /* one second */
        int size = ae->frame_size ? ae->frame_size : 1024;
        AVFrame *af = av_frame_alloc();
        af->format = AV_SAMPLE_FMT_FLTP; af->nb_samples = size; af->sample_rate = rate;
        av_channel_layout_copy(&af->ch_layout, &ae->ch_layout);
        av_frame_get_buffer(af, 0);
        long position = 0;
        for (int at = 0; at < total; at += size) {
            av_frame_make_writable(af);
            for (int c = 0; c < channels; ++c) {
                float *p = (float *)af->data[c];
                for (int s = 0; s < size; ++s) {
                    double t = (double)(position + s) / rate;
                    p[s] = at + s < total ? (float)(0.4 * sin(6.283185307 * (c ? 660.0 : 440.0) * t) +
                                                    0.2 * sin(6.283185307 * 1320.0 * t)) : 0.0f;
                }
            }
            af->pts = position;
            position += size;
            write_packets(mux, ae, as, af, pkt);
        }
        write_packets(mux, ae, as, NULL, pkt);
    }
    av_write_trailer(mux);
    avio_closep(&mux->pb);

    /* The oracle: libavformat and libavcodec reading the finished file. */
    AVFormatContext *in = NULL;
    if (avformat_open_input(&in, movie, NULL, NULL) < 0 || avformat_find_stream_info(in, NULL) < 0) die("cannot read the movie back");
    int vi = -1, ai = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) vi = (int)i;
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ai = (int)i;
    }
    if (vi < 0) die("no video");
    AVCodecContext *vd = avcodec_alloc_context3(avcodec_find_decoder(in->streams[vi]->codecpar->codec_id));
    avcodec_parameters_to_context(vd, in->streams[vi]->codecpar);
    vd->thread_count = 1;
    if (avcodec_open2(vd, vd->codec, NULL) < 0) die("video decoder");
    AVCodecContext *ad = NULL;
    if (ai >= 0) {
        ad = avcodec_alloc_context3(avcodec_find_decoder(in->streams[ai]->codecpar->codec_id));
        avcodec_parameters_to_context(ad, in->streams[ai]->codecpar);
        if (avcodec_open2(ad, ad->codec, NULL) < 0) die("audio decoder");
    }
    size_t picture_bytes = (size_t)w * h * 3 / 2;
    uint8_t *pictures = malloc(picture_bytes * (frames + 8));
    int64_t *times = malloc(sizeof(int64_t) * (frames + 8));
    int count = 0;
    int16_t *pcm = ai >= 0 ? malloc(sizeof(int16_t) * (size_t)(rate * 3) * channels) : NULL;
    size_t pcm_samples = 0;
    AVFrame *out = av_frame_alloc();
    AVPacket *rp = av_packet_alloc();
    AVRational vtb = in->streams[vi]->time_base;
    for (;;) {
        int more = av_read_frame(in, rp) >= 0;
        AVCodecContext *dc = NULL;
        if (!more) {
            avcodec_send_packet(vd, NULL);
            if (ad) avcodec_send_packet(ad, NULL);
        } else if (rp->stream_index == vi) dc = vd;
        else if (rp->stream_index == ai) dc = ad;
        else { av_packet_unref(rp); continue; }
        if (more) { avcodec_send_packet(dc, rp); av_packet_unref(rp); }
        for (int pass = 0; pass < 2; ++pass) {
            AVCodecContext *c = pass ? ad : vd;
            if (!c || (more && c != dc)) continue;
            while (avcodec_receive_frame(c, out) == 0) {
                if (pass == 0) {
                    if (out->width != w || out->height != h || out->format != AV_PIX_FMT_YUV420P) die("picture format");
                    uint8_t *at = pictures + (size_t)count * picture_bytes;
                    for (int y = 0; y < h; ++y) memcpy(at + y * w, out->data[0] + y * out->linesize[0], w);
                    at += (size_t)w * h;
                    for (int p = 1; p < 3; ++p) {
                        for (int y = 0; y < h / 2; ++y) memcpy(at + y * (w / 2), out->data[p] + y * out->linesize[p], w / 2);
                        at += (size_t)(w / 2) * (h / 2);
                    }
                    times[count++] = av_rescale_q(out->pts, vtb, (AVRational){1, 1000000});
                } else {
                    for (int s = 0; s < out->nb_samples; ++s)
                        for (int c2 = 0; c2 < channels; ++c2) {
                            float v = out->format == AV_SAMPLE_FMT_FLTP ? ((float *)out->data[c2])[s] : ((float *)out->data[0])[s * channels + c2];
                            long q = lrintf(v * 32768.0f);
                            pcm[(pcm_samples + s) * channels + c2] = (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
                        }
                    pcm_samples += out->nb_samples;
                }
                av_frame_unref(out);
            }
        }
        if (!more) break;
    }
    /* The oracle shows what the container presents. */
    if (count != frames) { fprintf(stderr, "%d pictures of %d\n", count, frames); return 5; }
    FILE *file = fopen(reference, "wb");
    put32(file, 0x3450324du); put32(file, w); put32(file, h); put32(file, count);
    put32(file, ai >= 0 ? rate : 0); put32(file, ai >= 0 ? channels : 0); put32(file, (uint32_t)pcm_samples);
    for (int i = 0; i < count; ++i) {
        int64_t duration = av_rescale_q(1, (AVRational){1, fps}, (AVRational){1, 1000000});
        put64(file, (uint64_t)times[i]); put64(file, (uint64_t)duration);
        fwrite(pictures + (size_t)i * picture_bytes, 1, picture_bytes, file);
    }
    for (size_t i = 0; i < pcm_samples * (size_t)channels; ++i) { fputc(pcm[i] & 255, file); fputc((pcm[i] >> 8) & 255, file); }
    fclose(file);
    printf("%s: %d pictures, %zu audio samples\n", movie, count, pcm_samples);
    return 0;
}
"""


def run(command, **kwargs):
    result = subprocess.run(command, capture_output=True, text=True, **kwargs)
    if result.returncode:
        sys.exit("%s failed:\n%s%s" % (" ".join(command[:2]), result.stdout, result.stderr))
    return result.stdout


def main():
    os.makedirs(OUTPUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        source, binary = os.path.join(directory, "generate.c"), os.path.join(directory, "generate")
        with open(source, "w") as handle:
            handle.write(SOURCE)
        flags = run(["pkg-config", "--cflags", "--libs", "libavformat", "libavcodec", "libavutil"]).split()
        run(["clang", "-O1", "-o", binary, source] + flags + ["-lm"])
        for name, w, h, frames, fps, (rate, channels), movflags, profile, params, encoder in CASES:
            movie = os.path.join(OUTPUT, name + ".mp4")
            reference = os.path.join(OUTPUT, name + ".ref")
            print(run([binary, movie, reference, str(w), str(h), str(frames), str(fps), str(rate), str(channels),
                       movflags, profile, params, encoder]), end="")


if __name__ == "__main__":
    main()
