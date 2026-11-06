#!/usr/bin/env python3
"""Size-optimal deflate for small, build-time blobs (shader bytecode).

zlib's level 9 parses greedily with lazy matching and builds its Huffman codes
from that one parse. This encoder keeps the standard zlib container, so the
runtime inflater is unchanged, but spends build time on the smallest stream it
can find, in the manner of Zopfli:

* every position keeps, for each match length, the shortest distance that
  reaches it (``find_matches``);
* a shortest-path search over the positions prices literals and matches by the
  entropy of the previous parse's symbol frequencies, and repeats (restarting
  from perturbed frequencies once it stalls) while the encoded size improves;
* code lengths are length-limited by package-merge, from the frequencies and
  from their Zopfli run-length smoothing, whichever is smaller; and the
  code-length alphabet is run-length coded in whichever of the eight 16/17/18
  variants is smallest.

The result is one dynamic block: splitting the stream into several blocks did not
shrink shader bytecode. Pure Python 3 without third-party modules; the output is
deterministic.

The search takes seconds per 10 KB, so builds never run it. ``DeflateCache`` keeps
its streams in the tracked tools/deflate_cache.txt, one line per stream, keyed by
the SHA-256 of the data. A build reads it; a data change that misses it gets the
plain zlib stream and a warning (an error with MAICORD_REQUIRE_DEFLATE_CACHE=1,
which CI sets). The build target ``maicord_update_deflate_cache`` (that is,
MAICORD_UPDATE_DEFLATE_CACHE=1 for the generators) searches the misses and
rewrites each generator's own lines; commit the result.
"""
import base64
import hashlib
import math
import os
import pathlib
import random
import struct
import sys
import zlib

WINDOW = 32768
MIN_MATCH, MAX_MATCH = 3, 258
MAX_CHAIN = 2048

LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163,
               195, 227, 258]
LENGTH_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0]
DISTANCE_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                 4097, 6145, 8193, 12289, 16385, 24577]
DISTANCE_EXTRA = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13]
CODE_LENGTH_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]

LENGTH_SYMBOL = [0] * (MAX_MATCH + 1)
for _symbol, _base in enumerate(LENGTH_BASE):
    for _length in range(_base, (LENGTH_BASE[_symbol + 1] if _symbol + 1 < len(LENGTH_BASE) else 259)):
        LENGTH_SYMBOL[_length] = _symbol
LENGTH_SYMBOL[258] = 28
DISTANCE_SYMBOL = [0] * (WINDOW + 1)
for _symbol, _base in enumerate(DISTANCE_BASE):
    for _distance in range(_base, (DISTANCE_BASE[_symbol + 1] if _symbol + 1 < 30 else WINDOW + 1)):
        DISTANCE_SYMBOL[_distance] = _symbol


def find_matches(data):
    """For each position, ``{length: shortest distance}`` or None."""
    size = len(data)
    chains = {}
    matches = []
    for i in range(size):
        found = None
        if i + MIN_MATCH <= size:
            chain = chains.setdefault(data[i:i + 3], [])
            limit = min(MAX_MATCH, size - i)
            best = MIN_MATCH - 1
            tried = 0
            for j in reversed(chain):
                distance = i - j
                if distance > WINDOW or tried == MAX_CHAIN:
                    break
                tried += 1
                # Chains run from the nearest candidate out: the first to reach a
                # length has its shortest distance.
                if data[j + best] != data[i + best]:
                    continue
                length = MIN_MATCH
                while length < limit and data[j + length] == data[i + length]:
                    length += 1
                if length > best:
                    if found is None:
                        found = {}
                    for reach in range(best + 1, length + 1):
                        found[reach] = distance
                    best = length
                    if best == limit:
                        break
            chain.append(i)
        matches.append(found)
    return matches


def package_merge(frequencies, limit):
    """Optimal code lengths no longer than ``limit`` for the used symbols."""
    used = sorted((f, s) for s, f in enumerate(frequencies) if f)
    lengths = [0] * len(frequencies)
    if not used:
        return lengths
    if len(used) == 1:
        lengths[used[0][1]] = 1
        return lengths
    leaves = [(f, (s,)) for f, s in used]
    level = leaves
    for _ in range(limit - 1):
        packages = [(level[k][0] + level[k + 1][0], level[k][1] + level[k + 1][1])
                    for k in range(0, len(level) - 1, 2)]
        level = sorted(leaves + packages, key=lambda item: item[0])
    for _, symbols in level[:2 * len(used) - 2]:
        for symbol in symbols:
            lengths[symbol] += 1
    return lengths


def canonical_codes(lengths):
    count = [0] * 16
    for length in lengths:
        count[length] += 1
    count[0] = 0
    code, first = 0, [0] * 16
    for bits in range(1, 16):
        code = (code + count[bits - 1]) << 1
        first[bits] = code
    codes = [0] * len(lengths)
    for symbol, length in enumerate(lengths):
        if length:
            codes[symbol] = first[length]
            first[length] += 1
    return codes


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.accumulator = 0
        self.count = 0

    def put(self, value, bits):
        self.accumulator |= value << self.count
        self.count += bits
        while self.count >= 8:
            self.out.append(self.accumulator & 255)
            self.accumulator >>= 8
            self.count -= 8

    def put_code(self, code, bits):
        reversed_code = 0
        for _ in range(bits):
            reversed_code = (reversed_code << 1) | (code & 1)
            code >>= 1
        self.put(reversed_code, bits)

    def finish(self):
        if self.count:
            self.out.append(self.accumulator & 255)
        return bytes(self.out)


def run_length(lengths, use16, use17, use18):
    """Code-length symbols as (symbol, extra_value, extra_bits)."""
    result = []
    i = 0
    while i < len(lengths):
        value = lengths[i]
        run = 1
        while i + run < len(lengths) and lengths[i + run] == value:
            run += 1
        i += run
        if value == 0:
            while run >= 3 and (use17 or use18):
                if use18 and run >= 11:
                    take = min(run, 138)
                    result.append((18, take - 11, 7))
                elif use17:
                    take = min(run, 10)
                    result.append((17, take - 3, 3))
                else:
                    break
                run -= take
        else:
            result.append((value, 0, 0))
            run -= 1
            while use16 and run >= 3:
                take = min(run, 6)
                result.append((16, take - 3, 2))
                run -= take
        result.extend([(value, 0, 0)] * run)
    return result


def header(lit_lengths, dist_lengths):
    """Cheapest dynamic-block header: (bit cost, writer)."""
    hlit = max(257, max((s + 1 for s, v in enumerate(lit_lengths) if v), default=0))
    hdist = max(1, max((s + 1 for s, v in enumerate(dist_lengths) if v), default=0))
    best = None
    for flags in range(8):
        symbols = run_length(lit_lengths[:hlit] + dist_lengths[:hdist], flags & 1, flags & 2, flags & 4)
        frequencies = [0] * 19
        for symbol, _, _ in symbols:
            frequencies[symbol] += 1
        lengths = package_merge(frequencies, 7)
        hclen = max(4, max((k + 1 for k, symbol in enumerate(CODE_LENGTH_ORDER) if lengths[symbol]), default=0))
        cost = 14 + 3 * hclen + sum(lengths[s] + b for s, _, b in symbols)
        if best is None or cost < best[0]:
            best = (cost, hlit, hdist, hclen, symbols, lengths)
    return best


def counts(path):
    """Literal/length and distance frequencies of the symbols in ``path``, a list
    of (length, distance, literal); a literal has distance 0."""
    lit = [0] * 286
    dist = [0] * 30
    for length, distance, literal in path:
        if distance:
            lit[257 + LENGTH_SYMBOL[length]] += 1
            dist[DISTANCE_SYMBOL[distance]] += 1
        else:
            lit[literal] += 1
    lit[256] += 1
    return lit, dist


def smooth_for_run_lengths(frequencies):
    """Zopfli's OptimizeHuffmanForRle: nudge frequencies so neighbouring symbols
    share a code length, which the run-length coded header describes cheaply."""
    counts = list(frequencies)
    length = len(counts)
    while length and counts[length - 1] == 0:
        length -= 1
    if length == 0:
        return counts
    good = [False] * length
    symbol, stride = counts[0], 0
    for i in range(length + 1):
        if i == length or counts[i] != symbol:
            if (symbol == 0 and stride >= 5) or (symbol != 0 and stride >= 7):
                for k in range(stride):
                    good[i - k - 1] = True
            stride = 1
            if i != length:
                symbol = counts[i]
        else:
            stride += 1
    stride, limit, total = 0, counts[0], 0
    for i in range(length + 1):
        if i == length or good[i] or abs(counts[i] - limit) >= 4:
            if stride >= 4 or (stride >= 3 and total == 0):
                value = max((total + stride // 2) // stride, 1)
                if total == 0:
                    value = 0
                for k in range(stride):
                    counts[i - k - 1] = value
            stride, total = 0, 0
            if i < length - 3:
                limit = (counts[i] + counts[i + 1] + counts[i + 2] + counts[i + 3] + 2) // 4
            elif i < length:
                limit = counts[i]
            else:
                limit = 0
        stride += 1
        if i != length:
            total += counts[i]
    return counts


def plan_block(lit, dist):
    """(bit size, tables) of the smaller dynamic block for these frequencies,
    with and without run-length smoothing of the code lengths."""
    best = None
    for smooth in (False, True):
        lit_lengths = package_merge(smooth_for_run_lengths(lit) if smooth else lit, 15)
        dist_lengths = package_merge(smooth_for_run_lengths(dist) if smooth else dist, 15)
        if any(f and not lit_lengths[s] for s, f in enumerate(lit)) or any(f and not dist_lengths[s] for s, f in enumerate(dist)):
            continue
        if not any(dist_lengths):
            dist_lengths[0] = 1
        cost, hlit, hdist, hclen, symbols, header_lengths = header(lit_lengths, dist_lengths)
        bits = cost + 3
        bits += sum(f * (lit_lengths[s] + (LENGTH_EXTRA[s - 257] if s > 256 else 0)) for s, f in enumerate(lit) if f)
        bits += sum(f * (dist_lengths[s] + DISTANCE_EXTRA[s]) for s, f in enumerate(dist) if f)
        if best is None or bits < best[0]:
            best = (bits, (lit_lengths, dist_lengths, hlit, hdist, hclen, symbols, header_lengths))
    return best


def write_block(writer, path, tables, final):
    lit_lengths, dist_lengths, hlit, hdist, hclen, symbols, header_lengths = tables
    writer.put(1 if final else 0, 1)
    writer.put(2, 2)
    writer.put(hlit - 257, 5)
    writer.put(hdist - 1, 5)
    writer.put(hclen - 4, 4)
    for k in range(hclen):
        writer.put(header_lengths[CODE_LENGTH_ORDER[k]], 3)
    header_codes = canonical_codes(header_lengths)
    for symbol, extra, extra_bits in symbols:
        writer.put_code(header_codes[symbol], header_lengths[symbol])
        if extra_bits:
            writer.put(extra, extra_bits)
    lit_codes = canonical_codes(lit_lengths)
    dist_codes = canonical_codes(dist_lengths)
    for length, distance, literal in path:
        if distance:
            symbol = LENGTH_SYMBOL[length]
            writer.put_code(lit_codes[257 + symbol], lit_lengths[257 + symbol])
            if LENGTH_EXTRA[symbol]:
                writer.put(length - LENGTH_BASE[symbol], LENGTH_EXTRA[symbol])
            symbol = DISTANCE_SYMBOL[distance]
            writer.put_code(dist_codes[symbol], dist_lengths[symbol])
            if DISTANCE_EXTRA[symbol]:
                writer.put(distance - DISTANCE_BASE[symbol], DISTANCE_EXTRA[symbol])
        else:
            writer.put_code(lit_codes[literal], lit_lengths[literal])
    writer.put_code(lit_codes[256], lit_lengths[256])


def shortest_path(data, matches, start, end, lit_cost, len_cost, dist_cost):
    """Cheapest parse of data[start:end] under the given symbol prices."""
    size = end - start
    infinity = float("inf")
    cost = [infinity] * (size + 1)
    step = [None] * (size + 1)
    cost[0] = 0.0
    for k in range(size):
        here = cost[k]
        i = start + k
        literal = here + lit_cost[data[i]]
        if literal < cost[k + 1]:
            cost[k + 1] = literal
            step[k + 1] = (1, 0, data[i])
        found = matches[i]
        if found:
            previous_distance = 0
            distance_price = 0.0
            room = size - k
            for length, distance in found.items():
                if length > room:
                    break
                if distance != previous_distance:
                    distance_price = dist_cost[DISTANCE_SYMBOL[distance]]
                    previous_distance = distance
                total = here + len_cost[length] + distance_price
                if total < cost[k + length]:
                    cost[k + length] = total
                    step[k + length] = (length, distance, 0)
    path = []
    k = size
    while k:
        item = step[k]
        path.append(item)
        k -= item[0]
    path.reverse()
    return path


def price_tables(lit, dist):
    """Per-symbol prices from the symbol frequencies of the previous parse: the
    entropy, with an unused symbol priced like one use."""
    def entropy(frequencies):
        total = sum(frequencies) or 1
        log_total = math.log2(total)
        return [log_total - math.log2(max(count, 1)) for count in frequencies]
    lit_cost = entropy(lit)
    dist_cost = [price + DISTANCE_EXTRA[s] for s, price in enumerate(entropy(dist))]
    len_cost = [0.0] * (MAX_MATCH + 1)
    for length in range(MIN_MATCH, MAX_MATCH + 1):
        symbol = LENGTH_SYMBOL[length]
        len_cost[length] = lit_cost[257 + symbol] + LENGTH_EXTRA[symbol]
    return lit_cost, len_cost, dist_cost


def randomize(frequencies, generator):
    """Each frequency is replaced by another one with probability 1/3 (Zopfli's
    way out of a local optimum)."""
    return [frequencies[generator.randrange(len(frequencies))] if generator.randrange(3) == 0 else count
            for count in frequencies]


def optimize_range(data, matches, start, end, iterations, seed_costs, generator):
    """Best parse of data[start:end] as one block: (bits, path, tables)."""
    lit_cost, len_cost, dist_cost = seed_costs
    best = None
    last_bits = None
    for iteration in range(iterations):
        path = shortest_path(data, matches, start, end, lit_cost, len_cost, dist_cost)
        lit, dist = counts(path)
        bits, tables = plan_block(lit, dist)
        if best is None or bits < best[0]:
            best = (bits, path, tables, lit, dist)
        if iteration > 5 and bits == last_bits:
            lit, dist = randomize(best[3], generator), randomize(best[4], generator)
        last_bits = bits
        lit_cost, len_cost, dist_cost = price_tables(lit, dist)
    return best[:3]


def flat_costs():
    lit_cost = [8.0] * 286
    len_cost = [0.0] * (MAX_MATCH + 1)
    for length in range(MIN_MATCH, MAX_MATCH + 1):
        len_cost[length] = 7.0 + LENGTH_EXTRA[LENGTH_SYMBOL[length]]
    return lit_cost, len_cost, [5.0 + DISTANCE_EXTRA[s] for s in range(30)]


def deflate_raw(data, iterations=48):
    """Smallest raw deflate stream this search finds, or None for empty input."""
    if not data:
        return None
    matches = find_matches(data)
    _, path, tables = optimize_range(data, matches, 0, len(data), iterations, flat_costs(), random.Random(1))
    writer = Bits()
    write_block(writer, path, tables, True)
    return writer.finish()


def zlib_stream(data, iterations=48):
    """A zlib container (header, optimal deflate, Adler-32) for ``data``."""
    raw = deflate_raw(data, iterations)
    stream = b"\x78\xda" + raw + struct.pack(">I", zlib.adler32(data))
    if zlib.decompress(stream) != data:
        raise ValueError("optimal deflate does not round-trip")
    return stream


def zlib_best(data):
    """The smallest zlib stream of ``data`` among zlib level 9's memory levels and strategies (the fallback encoder)."""
    best = None
    for mem in (8, 9):
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED):
            compressor = zlib.compressobj(9, zlib.DEFLATED, 15, mem, strategy)
            out = compressor.compress(data) + compressor.flush()
            if best is None or len(out) < len(best):
                best = out
    assert zlib.decompress(best) == data
    return best


CACHE_PATH = pathlib.Path(__file__).with_name("deflate_cache.txt")


class DeflateCache:
    """One generator's streams in tools/deflate_cache.txt (lines of
    ``namespace sha256 base64``, sorted)."""

    def __init__(self, namespace):
        self.namespace = namespace
        self.update = os.environ.get("MAICORD_UPDATE_DEFLATE_CACHE") == "1"
        self.strict = os.environ.get("MAICORD_REQUIRE_DEFLATE_CACHE") == "1"
        self.entries = {}
        self.used = {}
        self.missing = 0
        if CACHE_PATH.exists():
            for line in CACHE_PATH.read_text(encoding="ascii").splitlines():
                name, key, packed = line.split(" ")
                self.entries[(name, key)] = base64.b64decode(packed)

    def stream(self, data, fallback):
        """The cached stream for ``data``; ``fallback(data)`` (zlib) when missing."""
        key = hashlib.sha256(data).hexdigest()
        stream = self.entries.get((self.namespace, key))
        if stream is None or zlib.decompress(stream) != data:
            stream = fallback(data)
            if self.update:
                searched = zlib_stream(data)
                stream = searched if len(searched) < len(stream) else stream
            else:
                self.missing += 1
                message = (f"{self.namespace}: no cached optimal deflate stream for {key[:12]} ({len(data)} bytes); "
                           "using zlib. Run the maicord_update_deflate_cache target and commit tools/deflate_cache.txt.")
                if self.strict:
                    raise SystemExit("error: " + message)
                print("warning: " + message, file=sys.stderr)
        self.used[key] = stream
        return stream

    def save(self):
        """Rewrites this namespace to exactly the streams used (update mode)."""
        if not self.update:
            return
        entries = {k: v for k, v in self.entries.items() if k[0] != self.namespace}
        entries.update({(self.namespace, key): stream for key, stream in self.used.items()})
        lines = [f"{name} {key} {base64.b64encode(stream).decode('ascii')}" for (name, key), stream in sorted(entries.items())]
        CACHE_PATH.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
