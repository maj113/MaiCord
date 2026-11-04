#!/usr/bin/env python3
"""Writes the live-video conformance fixtures of tests/fixtures/video: short elementary streams from libavcodec's
encoders (libx264 for H.264, libx265 for HEVC, libaom for AV1) over a synthetic moving picture, and, for the
streams the software tier decodes (H.264, AV1), the pictures libavcodec's own decoder (h264, libaom) produces from
them. tests/platform/decode/video_software.cpp decodes the streams with libopenh264 and libdav1d and compares
with those pictures (the one H.264 stream with B pictures is the stream libopenh264 cannot show in order); tests/platform/decode/vaapi_parameters.cpp feeds all of them through the stream parsers and
the VA-API parameter builders.

It also writes tests/fixtures/video/slice_oracle.inc: the slice header fields of every H.264 and HEVC slice as
libavcodec's own parser reads them (its trace_headers bitstream filter), including where slice data starts, which
tests/platform/decode/vaapi_parameters.cpp compares with the in-house slice header parsers the VA-API decoder uses.

Needs libavcodec-dev, libavutil-dev (with libx264, libx265 and libaom in libavcodec) and clang on a Linux host:

    tools/generate_video_fixtures.py

File layout of <name>.bin, all little endian: u32 'MVID', u32 codec (0 H.264, 1 HEVC, 2 AV1), u32 width, u32
height (the visible extent), u32 packet count, u32 picture count (0: no reference), then per packet u32 size, u32
flags (bit 0: keyframe) and the packet padded to 4 bytes (Annex B access units; AV1 temporal units of OBUs, in
decoding order), then picture count pictures of width * height * 3 / 2 bytes (I420, output order).
"""

import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(ROOT, "tests", "fixtures", "video")

# name, codec id, encoder, decoder, width, height, frames, reference pictures (True: decoded by libavcodec and stored;
# "verify": decoded and counted, not stored; False: not decoded), private options
CASES = [
    ("h264_baseline", 0, "libx264", "h264", 48, 32, 8, True,
     {"profile": "baseline", "preset": "veryfast", "crf": "34", "x264-params": "keyint=8:bframes=0:scenecut=0:threads=1:ref=2"}),
    ("h264_main_bframes", 0, "libx264", "h264", 64, 48, 8, True,
     {"profile": "main", "preset": "medium", "crf": "34",
      "x264-params": "keyint=8:bframes=2:b-pyramid=none:weightp=2:weightb=1:ref=3:scenecut=0:threads=1:rc-lookahead=4"}),
    ("h264_main_weighted", 0, "libx264", "h264", 64, 48, 8, True,
     {"profile": "main", "preset": "medium", "crf": "34",
      "x264-params": "keyint=8:bframes=0:weightp=2:ref=3:scenecut=0:threads=1:rc-lookahead=0"}),
    ("h264_high_crop_slices", 0, "libx264", "h264", 50, 34, 8, True,
     {"profile": "high", "preset": "medium", "crf": "34",
      "x264-params": "keyint=8:bframes=0:ref=2:slices=3:scenecut=0:threads=1:8x8dct=1:rc-lookahead=0"}),
    ("hevc_main", 1, "libx265", "hevc", 128, 64, 6, "verify",
     {"preset": "ultrafast", "crf": "34", "x265-params": "keyint=6:bframes=2:log-level=none:frame-threads=1:pools=none"}),
    ("av1_basic", 2, "libaom-av1", "libaom-av1", 64, 48, 6, True,
     {"crf": "50", "cpu-used": "8", "lag-in-frames": "0", "g": "6", "usage": "realtime"}),
    ("av1_tiles", 2, "libaom-av1", "libaom-av1", 256, 32, 3, True,
     {"crf": "50", "cpu-used": "8", "lag-in-frames": "0", "g": "3", "usage": "realtime", "tile-columns": "1", "row-mt": "0"}),
]

SOURCE = r"""
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 99991;
static int noise(int range) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return (int)((rng_state >> 16) % (uint32_t)range);
}

/* A moving box on a diagonal gradient with a slow fade and a little noise: motion for inter prediction, a fade
   for weighted prediction, edges for the loop filters. */
static void synthesize(AVFrame *f, int n, int w, int h) {
    int fade = 16 + n * 6;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int v = 40 + (x * 3 + y * 2) % 150 + fade + noise(5);
            int bx = (n * 5) % (w - 24), by = (n * 3) % (h - 20);
            if (x >= bx && x < bx + 24 && y >= by && y < by + 20) v = 220 - ((x ^ y) & 31);
            f->data[0][y * f->linesize[0] + x] = v > 235 ? 235 : (uint8_t)v;
        }
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            f->data[1][y * f->linesize[1] + x] = (uint8_t)(110 + (x * 4 + n * 3) % 40);
            f->data[2][y * f->linesize[2] + x] = (uint8_t)(120 + (y * 4 + n * 2) % 30);
        }
}

typedef struct { uint8_t *data; int size; int flags; } Packet;

static void put32(FILE *out, uint32_t v) { fputc(v, out); fputc(v >> 8, out); fputc(v >> 16, out); fputc(v >> 24, out); }

int main(int argc, char **argv) {
    /* name codec encoder decoder width height frames reference output key=value... */
    const char *name = argv[1];
    int codec_id = atoi(argv[2]);
    const char *encoder = argv[3], *decoder = argv[4];
    int w = atoi(argv[5]), h = atoi(argv[6]), frames = atoi(argv[7]), reference = atoi(argv[8]);
    const char *output = argv[9];
    const AVCodec *enc = avcodec_find_encoder_by_name(encoder);
    if (!enc) { fprintf(stderr, "%s: no encoder %s\n", name, encoder); return 2; }
    AVCodecContext *ec = avcodec_alloc_context3(enc);
    ec->width = w; ec->height = h; ec->time_base = (AVRational){1, 30}; ec->framerate = (AVRational){30, 1};
    ec->pix_fmt = AV_PIX_FMT_YUV420P; ec->gop_size = frames;
    ec->color_range = AVCOL_RANGE_MPEG; ec->colorspace = AVCOL_SPC_BT709;
    for (int i = 10; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "g")) ec->gop_size = atoi(argv[i + 1]);
        else if (av_opt_set(ec->priv_data, argv[i], argv[i + 1], 0) < 0) { fprintf(stderr, "%s: option %s\n", name, argv[i]); return 2; }
    }
    if (avcodec_open2(ec, enc, NULL) < 0) { fprintf(stderr, "%s: cannot open encoder\n", name); return 2; }
    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P; frame->width = w; frame->height = h;
    av_frame_get_buffer(frame, 0);
    AVPacket *pkt = av_packet_alloc();
    Packet *packets = calloc(frames * 4 + 16, sizeof(Packet));
    int npackets = 0;
    for (int i = 0; i <= frames; ++i) {
        if (i < frames) {
            av_frame_make_writable(frame);
            synthesize(frame, i, w, h);
            frame->pts = i;
            if (avcodec_send_frame(ec, frame) < 0) return 3;
        } else avcodec_send_frame(ec, NULL);
        while (avcodec_receive_packet(ec, pkt) == 0) {
            packets[npackets].data = malloc(pkt->size);
            memcpy(packets[npackets].data, pkt->data, pkt->size);
            packets[npackets].size = pkt->size;
            packets[npackets].flags = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
            ++npackets;
            av_packet_unref(pkt);
        }
    }
    int npictures = 0;
    uint8_t *pictures = NULL;
    size_t picture_bytes = (size_t)w * h * 3 / 2;
    if (reference) {
        const AVCodec *dec = avcodec_find_decoder_by_name(decoder);
        if (!dec) { fprintf(stderr, "%s: no decoder %s\n", name, decoder); return 2; }
        AVCodecContext *dc = avcodec_alloc_context3(dec);
        dc->thread_count = 1;
        if (avcodec_open2(dc, dec, NULL) < 0) return 3;
        AVFrame *out = av_frame_alloc();
        pictures = malloc(picture_bytes * (frames + 4));
        for (int i = 0; i <= npackets; ++i) {
            if (i < npackets) {
                AVPacket *in = av_packet_alloc();
                av_new_packet(in, packets[i].size);
                memcpy(in->data, packets[i].data, packets[i].size);
                avcodec_send_packet(dc, in);
                av_packet_free(&in);
            } else avcodec_send_packet(dc, NULL);
            while (avcodec_receive_frame(dc, out) == 0) {
                if (out->width != w || out->height != h || out->format != AV_PIX_FMT_YUV420P) { fprintf(stderr, "%s: picture %dx%d format %d\n", name, out->width, out->height, out->format); return 4; }
                uint8_t *at = pictures + (size_t)npictures * picture_bytes;
                for (int y = 0; y < h; ++y) memcpy(at + y * w, out->data[0] + y * out->linesize[0], w);
                at += (size_t)w * h;
                for (int p = 1; p < 3; ++p) {
                    for (int y = 0; y < h / 2; ++y) memcpy(at + y * (w / 2), out->data[p] + y * out->linesize[p], w / 2);
                    at += (size_t)(w / 2) * (h / 2);
                }
                ++npictures;
                av_frame_unref(out);
            }
        }
        if (npictures != frames) { fprintf(stderr, "%s: %d pictures of %d\n", name, npictures, frames); return 5; }
    }
    FILE *file = fopen(output, "wb");
    put32(file, 0x4449564du); put32(file, codec_id); put32(file, w); put32(file, h);
    put32(file, npackets); put32(file, reference == 1 ? npictures : 0);
    for (int i = 0; i < npackets; ++i) {
        put32(file, packets[i].size); put32(file, packets[i].flags);
        fwrite(packets[i].data, 1, packets[i].size, file);
        for (int pad = packets[i].size; pad & 3; ++pad) fputc(0, file);
    }
    if (npictures && reference == 1) fwrite(pictures, 1, picture_bytes * npictures, file);
    fclose(file);
    printf("%s: %d packets, %d pictures%s\n", name, npackets, npictures, reference == 1 ? "" : " (not stored)");
    return 0;
}
"""

TRACE_SOURCE = r"""
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t rd(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
/* trace <fixture> <packet>: libavcodec's trace_headers over the packets up to <packet>, logging only the last one. */
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) return 1;
    int codec = rd(d + 4), packets = rd(d + 16), want = atoi(argv[2]);
    const AVBitStreamFilter *bsf = av_bsf_get_by_name("trace_headers");
    AVBSFContext *ctx;
    av_bsf_alloc(bsf, &ctx);
    ctx->par_in->codec_id = codec == 0 ? AV_CODEC_ID_H264 : codec == 1 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_AV1;
    av_bsf_init(ctx);
    long at = 24;
    for (int i = 0; i < packets && i <= want; ++i) {
        uint32_t size = rd(d + at);
        at += 8;
        av_log_set_level(i == want ? AV_LOG_TRACE : AV_LOG_QUIET);
        AVPacket *p = av_packet_alloc();
        av_new_packet(p, size);
        memcpy(p->data, d + at, size);
        av_bsf_send_packet(ctx, p);
        AVPacket *o = av_packet_alloc();
        while (av_bsf_receive_packet(ctx, o) == 0) av_packet_unref(o);
        at += (size + 3) & ~3u;
    }
    return 0;
}
"""

ELEMENT = re.compile(r"^(\d+)\s+(\S+)\s+([01]+) = (-?\d+)$")


def trace_slices(binary, path, packet, header):
    """The elements of every slice header of one packet: lists of (position, name, bit count, value)."""
    result = subprocess.run([binary, path, str(packet)], capture_output=True, text=True, check=True)
    slices, current = [], None
    for line in (result.stderr + result.stdout).splitlines():
        line = re.sub(r"^\[trace_headers @ 0x[0-9a-f]+\] ", "", line)
        if line.startswith(header):
            current = []
            slices.append(current)
            continue
        match = ELEMENT.match(line)
        if match and current is not None:
            current.append((int(match[1]), match[2], len(match[3]), int(match[4])))
        else:
            current = None
    return slices


H264_WEIGHT_ENTRIES = 4
H264_MODIFICATIONS = 8


def h264_row(items):
    first_mb = slice_type = override = l0 = l1 = cabac = qp = disabled = alpha = beta = 0
    mods = [[], []]
    listing, idc = 0, 0
    denominators = [0, 0]
    weighted = False
    entries = [[[0] * 8 for _ in range(H264_WEIGHT_ENTRIES)] for _ in range(2)]
    end = 0
    for position, name, bits, value in items:
        end = position + bits
        key = re.sub(r"\[\d+\]", "", name)
        index = [int(i) for i in re.findall(r"\[(\d+)\]", name)]
        if name == "first_mb_in_slice": first_mb = value
        elif name == "slice_type": slice_type = value
        elif name == "num_ref_idx_active_override_flag": override = value
        elif name == "num_ref_idx_l0_active_minus1": l0 = value
        elif name == "num_ref_idx_l1_active_minus1": l1 = value
        elif name == "ref_pic_list_modification_flag_l0": listing = 0
        elif name == "ref_pic_list_modification_flag_l1": listing = 1
        elif name == "modification_of_pic_nums_idc": idc = value
        elif name in ("abs_diff_pic_num_minus1", "long_term_pic_num"): mods[listing].append((idc, value))
        elif name == "luma_log2_weight_denom": weighted, denominators[0] = True, value
        elif name == "chroma_log2_weight_denom": denominators[1] = value
        elif key.startswith(("luma_weight_l", "luma_offset_l", "chroma_weight_l", "chroma_offset_l")):
            side = int(key[-1]) if key[-1] in "01" else int(re.search(r"_l(\d)", key)[1])
            side = int(re.search(r"_l(\d)", key)[1])
            at = index[0]
            if at >= H264_WEIGHT_ENTRIES: continue
            row = entries[side][at]
            if key.startswith("luma_weight_l") and key.endswith("_flag"): row[0] = value
            elif key.startswith("luma_weight_l"): row[1] = value
            elif key.startswith("luma_offset_l"): row[2] = value
            elif key.startswith("chroma_weight_l") and key.endswith("_flag"): row[3] = value
            elif key.startswith("chroma_weight_l"): row[4 + 2 * index[1]] = value
            elif key.startswith("chroma_offset_l"): row[5 + 2 * index[1]] = value
        elif name == "cabac_init_idc": cabac = value
        elif name == "slice_qp_delta": qp = value
        elif name == "disable_deblocking_filter_idc": disabled = value
        elif name == "slice_alpha_c0_offset_div2": alpha = value
        elif name == "slice_beta_offset_div2": beta = value
    flat = [first_mb, slice_type, override, l0, l1, cabac, qp & 0xFFFFFFFF, disabled, alpha & 0xFFFFFFFF, beta & 0xFFFFFFFF, end,
            len(mods[0]), len(mods[1])]
    for listing in (0, 1):
        for k in range(H264_MODIFICATIONS):
            flat += list(mods[listing][k]) if k < len(mods[listing]) else [0, 0]
    flat += [1 if weighted else 0, denominators[0], denominators[1]]
    for side in (0, 1):
        for row in entries[side]:
            flat += [v & 0xFFFFFFFF for v in row]
    return flat


def hevc_row(items):
    want = {"slice_segment_address": 0, "slice_type": 0, "slice_sao_luma_flag": 0, "slice_sao_chroma_flag": 0,
            "num_ref_idx_active_override_flag": 0, "num_ref_idx_l0_active_minus1": 0, "num_ref_idx_l1_active_minus1": 0,
            "mvd_l1_zero_flag": 0, "cabac_init_flag": 0, "collocated_from_l0_flag": 0, "collocated_ref_idx": 0,
            "five_minus_max_num_merge_cand": 0, "slice_qp_delta": 0, "num_entry_point_offsets": 0,
            "slice_temporal_mvp_enabled_flag": 0, "slice_deblocking_filter_disabled_flag": 0,
            "slice_loop_filter_across_slices_enabled_flag": 0}
    end = 0
    for position, name, bits, value in items:
        end = position + bits
        if name in want: want[name] = value & 0xFFFFFFFF
    return list(want.values()) + [end]


def write_oracle(binary, outputs):
    lines = ["// Generated by tools/generate_video_fixtures.py from libavcodec's trace_headers: do not edit.",
             "// H.264 rows: fixture, packet, slice, first_mb, slice_type, override, l0_minus1, l1_minus1, cabac_init_idc,",
             "//   slice_qp_delta, disable_deblocking, alpha, beta, end bit of the header (from the NAL header byte),",
             "//   change counts l0 and l1, 8 changes per list (idc, value), pred_weight_table present, luma and chroma",
             "//   denominators, then per list and entry (4 each) luma flag, weight, offset, chroma flag, weight cb,",
             "//   offset cb, weight cr, offset cr.",
             "// HEVC rows: fixture, packet, slice, address, slice_type, sao luma, sao chroma, override, l0_minus1, l1_minus1,",
             "//   mvd_l1_zero, cabac_init, collocated_from_l0, collocated_ref_idx, five_minus_max_num_merge_cand,",
             "//   slice_qp_delta, entry points, temporal mvp, deblocking disabled, loop filter across slices, end bit of",
             "//   the header (from the NAL header's first byte)."]
    for kind, files, header, row in (("H264", outputs["h264"], "Slice Header", h264_row),
                                     ("Hevc", outputs["hevc"], "Slice Segment Header", hevc_row)):
        lines.append("inline constexpr u32 k%sSliceOracle[][%d]{" % (kind, 3 + len(row([(0, "x", 1, 0)])) + (0 if kind == "Hevc" else 0)))
        for fixture, path in enumerate(files):
            count = struct_packets(path)
            for packet in range(count):
                for slice_index, items in enumerate(trace_slices(binary, path, packet, header)):
                    values = [fixture, packet, slice_index] + row(items)
                    lines.append("    {" + ", ".join(str(v) for v in values) + "},")
        lines.append("};")
    with open(os.path.join(OUTPUT, "slice_oracle.inc"), "w") as handle:
        handle.write("\n".join(lines) + "\n")


def struct_packets(path):
    import struct
    with open(path, "rb") as handle:
        return struct.unpack("<I", handle.read(24)[16:20])[0]



def main():
    os.makedirs(OUTPUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as directory:
        source = os.path.join(directory, "gen.c")
        binary = os.path.join(directory, "gen")
        with open(source, "w") as handle:
            handle.write(SOURCE)
        subprocess.run(["clang", "-O1", "-o", binary, source, "-lavcodec", "-lavutil", "-lm"], check=True)
        for name, codec, encoder, decoder, width, height, frames, reference, options in CASES:
            arguments = [binary, name, str(codec), encoder, decoder, str(width), str(height), str(frames),
                         "1" if reference is True else "2" if reference == "verify" else "0", os.path.join(OUTPUT, name + ".bin")]
            for key, value in options.items():
                arguments += [key, value]
            subprocess.run(arguments, check=True)
        trace_source = os.path.join(directory, "trace.c")
        trace_binary = os.path.join(directory, "trace")
        with open(trace_source, "w") as handle:
            handle.write(TRACE_SOURCE)
        subprocess.run(["clang", "-O1", "-o", trace_binary, trace_source, "-lavcodec", "-lavutil"], check=True)
        paths = lambda codec: [os.path.join(OUTPUT, case[0] + ".bin") for case in CASES if case[1] == codec]
        write_oracle(trace_binary, {"h264": paths(0), "hevc": paths(1)})


if __name__ == "__main__":
    sys.exit(main())
