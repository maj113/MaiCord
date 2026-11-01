#!/usr/bin/env python3
"""Generates src/render/effects/blue_noise_64.bin: a 64x64 tiling blue-noise table (one byte per texel).

Void-and-cluster (Ulichney, "Void-and-cluster method for dither array generation", 1993) on a torus with
a Gaussian of sigma 1.5: an initial pattern of 10% points is relaxed until stable, then every texel gets a
rank (tightest clusters of the pattern first, then largest voids), and the rank scales to 0..255 so the
histogram is flat. The fixed seed makes the output reproducible.

The table feeds mc_dither() in maicord/sdk/background.hlsli. Run it again only to change the table:

    python tools/generate_blue_noise.py
"""
import math
import pathlib
import random

SIZE = 64
CELLS = SIZE * SIZE
SIGMA = 1.5
RADIUS = 6
SEED = 0x6D63626E  # "mcbn"
OUTPUT = pathlib.Path(__file__).resolve().parent.parent / "src" / "render" / "effects" / "blue_noise_64.bin"


def kernel():
    taps = []
    for dy in range(-RADIUS, RADIUS + 1):
        for dx in range(-RADIUS, RADIUS + 1):
            taps.append((dx, dy, math.exp(-(dx * dx + dy * dy) / (2 * SIGMA * SIGMA))))
    return taps


TAPS = kernel()


def splat(energy, index, sign):
    x, y = index % SIZE, index // SIZE
    for dx, dy, weight in TAPS:
        energy[((y + dy) % SIZE) * SIZE + (x + dx) % SIZE] += sign * weight


def tightest(energy, pattern):
    best, best_value = -1, -1.0
    for i in range(CELLS):
        if pattern[i] and energy[i] > best_value:
            best, best_value = i, energy[i]
    return best


def largest_void(energy, pattern):
    best, best_value = -1, 1e30
    for i in range(CELLS):
        if not pattern[i] and energy[i] < best_value:
            best, best_value = i, energy[i]
    return best


def main():
    rng = random.Random(SEED)
    pattern = [0] * CELLS
    energy = [0.0] * CELLS
    for i in rng.sample(range(CELLS), CELLS // 10):
        pattern[i] = 1
        splat(energy, i, 1)
    # Relax: move the tightest cluster into the largest void until that is where it already sits.
    while True:
        cluster = tightest(energy, pattern)
        pattern[cluster] = 0
        splat(energy, cluster, -1)
        void = largest_void(energy, pattern)
        pattern[void] = 1
        splat(energy, void, 1)
        if void == cluster:
            break
    ones = sum(pattern)
    rank = [0] * CELLS
    # Phase 1: peel the stable pattern, tightest cluster first, ranks ones - 1 .. 0.
    work, work_energy = pattern[:], energy[:]
    for r in range(ones - 1, -1, -1):
        i = tightest(work_energy, work)
        work[i] = 0
        splat(work_energy, i, -1)
        rank[i] = r
    # Phases 2 and 3: grow the pattern into the largest void until every texel is ranked.
    work, work_energy = pattern[:], energy[:]
    for r in range(ones, CELLS):
        i = largest_void(work_energy, work)
        work[i] = 1
        splat(work_energy, i, 1)
        rank[i] = r
    data = bytes(r * 256 // CELLS for r in rank)
    OUTPUT.write_bytes(data)
    print(f"wrote {OUTPUT} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
