"""Generate original MaiCord stereo tones. Run with Python -B; no recordings."""
import array
import hashlib
import json
import math
import pathlib
import struct
import sys
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
RATE = 48000
# Peak level of each tone: -1 dBFS. Notifications play at 0.4 gain, so a peak
# well below full scale makes them too quiet to notice.
PEAK = round(32767 * 10 ** (-1 / 20))


def tone(frames, start_hz, end_hz):
    duration = frames / RATE
    shape = []
    for i in range(frames):
        t = i / RATE
        attack = min(1.0, t / 0.012)
        release = min(1.0, (duration - t) / 0.075)
        envelope = attack * release * math.exp(-6.0 * t / duration)
        phase = 2 * math.pi * (start_hz * t + (end_hz - start_hz) * t * t / (2 * duration))
        shape.append(envelope * (math.sin(phase) + 0.18 * math.sin(2.013 * phase)) / 1.18)
    scale = PEAK / max(abs(value) for value in shape)
    samples = array.array("h")
    for value in shape:
        sample = round(scale * value)
        samples.extend((sample, sample))
    if sys.byteorder != "little":
        samples.byteswap()
    return samples.tobytes()


def planes(pcm, orders):
    samples = array.array("H", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    history = [[0, 0], [0, 0]]
    out = bytearray()
    for first in range(0, len(samples) // 2, 256):
        count = min(256, len(samples) // 2 - first)
        lanes = [[], []]
        for i in range(first, first + count):
            left = samples[2 * i]
            side = (samples[2 * i + 1] - left) & 65535
            for lane, value in enumerate((left, side)):
                previous, before = history[lane]
                predictor = 0 if orders[lane] == 0 else previous if orders[lane] == 1 else 2 * previous - before
                residual = (value - predictor) & 65535
                zigzag = (residual << 1) & 65535 if residual < 32768 else ((65535 - residual) << 1) | 1
                lanes[lane].append(zigzag)
                history[lane] = [value, previous]
        for lane in lanes:
            for shift in (0, 8):
                out.extend((value >> shift) & 255 for value in lane)
    return bytes(out)


def main():
    directory = ROOT / "assets" / "sounds"
    rows = []
    for name, frames, start, end in (("message-alert", 11040, 740, 870), ("focused-message-alert", 7200, 610, 660)):
        pcm = tone(frames, start, end)
        candidates = [(zlib.compress(planes(pcm, (left, side)), 9), (left, side))
                      for left in range(3) for side in range(3)]
        packed, orders = min(candidates, key=lambda row: (len(row[0]), row[1]))
        blob = b"MCSP" + bytes((1, *orders, 2)) + struct.pack("<II", frames, RATE) + packed
        path = directory / (name + ".mcp")
        path.write_bytes(blob)
        rows.append({"name": name, "frames": frames, "rate": RATE, "channels": 2,
                     "pcm_bytes": len(pcm), "pcm_sha256": hashlib.sha256(pcm).hexdigest(),
                     "resource_bytes": len(blob), "resource_sha256": hashlib.sha256(blob).hexdigest(),
                     "predictor_orders": list(orders), "start_hz": start, "end_hz": end})
    manifest = {"provenance": "Original MaiCord mathematical synthesis; no recordings or third-party sound inputs.",
                "license": "CC0-1.0", "generator": "tools/generate_message_alerts.py", "assets": rows}
    (directory / "sources.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
