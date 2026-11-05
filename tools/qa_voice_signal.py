#!/usr/bin/env python3
"""Feeds a speech-like signal (a 130 Hz buzz with vowel-ish harmonics and a three-per-second syllable envelope) in real time
into the FIFO of a PulseAudio module-pipe-source (s16le, 48 kHz, mono), the microphone of tools/test_discord_wayland.sh --audio.

    tools/qa_voice_signal.py FIFO

A steady tone is something a voice activity gate may reject; this is not. Stops when the FIFO closes or on a signal.
"""
import array
import math
import sys
import time

RATE, CHUNK = 48000, 960  # 20 ms


def main() -> int:
    fifo = open(sys.argv[1], "wb", buffering=0)
    sample = 0
    deadline = time.monotonic()
    while True:
        chunk = array.array("h")
        for i in range(CHUNK):
            t = (sample + i) / RATE
            envelope = max(0.0, math.sin(2 * math.pi * 3 * t)) ** 0.5
            value = sum(math.sin(2 * math.pi * 130 * k * t) / (1 + 0.35 * k) for k in range(1, 24)) * envelope
            chunk.append(int(max(-1.0, min(1.0, value * 0.35)) * 32767))
        sample += CHUNK
        try:
            fifo.write(chunk.tobytes())
        except (BrokenPipeError, OSError):
            return 0
        deadline += CHUNK / RATE
        time.sleep(max(0.0, deadline - time.monotonic()))


if __name__ == "__main__":
    sys.exit(main())
