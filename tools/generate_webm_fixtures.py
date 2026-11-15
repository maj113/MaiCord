#!/usr/bin/env python3
"""Writes the WebM Opus fixtures of tests/fixtures/webm: tests/media/containers/webm_opus.cpp reads the file with
src/media/containers/webm_opus.cpp and compares each packet with the list stored beside it.

The file is three seconds of a synthetic stereo signal (two tones, two sweeps and noise: no recorded content), coded
by libopus in 20 ms packets and muxed by libavformat's WebM muxer with a Cluster every 0.4 seconds and the Cues before
the first Cluster, as YouTube's audio formats 249 to 251 have them. The oracle is ffprobe's view of the same file.

Needs ffmpeg and ffprobe with libopus; run it in WSL:

    tools/generate_webm_fixtures.py

File layout of opus_stereo_packets.bin, all little endian: u32 'MWBM', u32 packet count, u32 codec delay in frames,
u32 frames that the last packet's padding discards, then for each packet u32 size and u32 FNV-1a hash of its bytes.
"""

import os
import re
import struct
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(ROOT, "tests", "fixtures", "webm")
SIGNAL = ("aevalsrc=0.4*sin(2*PI*440*t)+0.2*sin(2*PI*(1000+900*t)*t)+0.05*random(0)"
          "|0.4*sin(2*PI*660*t)+0.2*sin(2*PI*(3000-500*t)*t)+0.05*random(1):s=48000:d=3")


def fnv1a(data):
    value = 0x811C9DC5
    for byte in data:
        value = ((value ^ byte) * 0x01000193) & 0xFFFFFFFF
    return value


def main():
    os.makedirs(OUTPUT, exist_ok=True)
    webm = os.path.join(OUTPUT, "opus_stereo.webm")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", SIGNAL, "-c:a", "libopus", "-b:a", "96k",
                    "-frame_duration", "20", "-vbr", "on", "-application", "audio", "-map_metadata", "-1",
                    "-fflags", "+bitexact", "-flags:a", "+bitexact", "-f", "webm", "-cluster_time_limit", "400",
                    "-reserve_index_space", "256", webm], check=True)
    shown = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_packets", "-show_data", webm],
                           check=True, capture_output=True, text=True).stdout
    packets = []
    discard = 0
    for block in shown.split("[PACKET]")[1:]:
        data = bytearray()
        for line in block.splitlines():
            match = re.match(r"^[0-9a-f]{8}: ((?:[0-9a-f]{2,4} ?)+)", line)
            if match:
                data += bytes.fromhex(match.group(1).replace(" ", ""))
            match = re.match(r"^discard_padding=(\d+)", line)
            if match:
                discard = int(match.group(1))
        size = int(re.search(r"^size=(\d+)", block, re.M).group(1))
        assert len(data) == size, (len(data), size)
        packets.append(bytes(data))
    delay = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
                            "stream=initial_padding", "-of", "csv=p=0", webm],
                           check=True, capture_output=True, text=True).stdout.strip()
    with open(os.path.join(OUTPUT, "opus_stereo_packets.bin"), "wb") as out:
        out.write(struct.pack("<4sIII", b"MWBM", len(packets), int(delay or 312), discard))
        for packet in packets:
            out.write(struct.pack("<II", len(packet), fnv1a(packet)))
    print(len(packets), "packets,", os.path.getsize(webm), "bytes, codec delay", delay, "discard", discard)


if __name__ == "__main__":
    main()
