#!/usr/bin/env python3
"""Resolves the samples of `MaiCordChatListBenchmark --profile 1` against the linker map.

  profile_chat_list.py <samples.txt> <image.map> [--top N]

The benchmark's sampler (tools/benchmark_chat_list.ps1 -Profile) prints `P <module> <rva> <count>` lines: the UI
thread's instruction pointer at 1 kHz through the scroll stage, no unwinding, so every sample is a leaf (self time).
Addresses in the benchmark's own image are mapped to the symbol of the map that holds them; other modules (the GPU
driver, d3d11, ntdll) are listed by module, their top addresses by RVA.
"""
import re
import sys
from bisect import bisect_right
from collections import Counter, defaultdict


def load_map(path):
    """(start, size, name) of every public of the map (`0001:00000000  name  Rva+Base  Lib:Object`).

    The image is one LTO object, so only externally visible functions are listed: a static or inlined helper is charged
    to the public symbol before it. A symbol's size is the distance to the next one."""
    rows = []
    pattern = re.compile(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s")
    for line in open(path, encoding="utf-8", errors="replace"):
        match = pattern.match(line)
        if match:
            rows.append((int(match.group(2), 16) - 0x140000000, match.group(1)))
    rows.sort()
    out = []
    for i, (start, name) in enumerate(rows):
        end = rows[i + 1][0] if i + 1 < len(rows) else start + 1
        if end > start:
            out.append((start, end - start, readable(name)))
    return out


def readable(name):
    """`?f@?A0x12@detail@maicord@@YA...` -> `maicord::detail::f` (enough to read, not a demangler)."""
    if not name.startswith("?"):
        return name
    parts = name.lstrip("?").split("@")
    scopes = []
    for part in parts[1:]:
        if part == "":
            break
        if not part.startswith("?A0x"):
            scopes.append(part)
    return "::".join(list(reversed(scopes)) + [parts[0]]) if scopes else parts[0]


def main():
    args = sys.argv[1:]
    top = int(args[args.index("--top") + 1]) if "--top" in args else 40
    positional = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] != "--top")]
    samples_path, map_path = positional[0], positional[1]
    symbols = load_map(map_path)
    starts = [s[0] for s in symbols]

    by_module = Counter()
    own = Counter()
    foreign = defaultdict(Counter)
    total = 0
    header = ""
    for line in open(samples_path, encoding="ascii", errors="replace"):
        parts = line.split()
        if not parts or parts[0] != "P":
            continue
        if parts[1] == "total":
            header = line.strip()
            continue
        module, rva, count = parts[1], int(parts[2]), int(parts[3])
        total += count
        by_module[module] += count
        if module.lower().startswith("maicordchatlistbenchmark"):
            i = bisect_right(starts, rva) - 1
            name = "?"
            if i >= 0 and symbols[i][0] <= rva < symbols[i][0] + max(symbols[i][1], 1):
                name = symbols[i][2]
            own[name] += count
        else:
            foreign[module][rva] += count

    full = total
    print(f"{header}; resolved samples {total}")
    # The thread waits for the display between frames: the one ntdll address holding most samples is that wait.
    idle = 0
    for module, counter in foreign.items():
        if module.lower().startswith("ntdll") and counter:
            rva, count = counter.most_common(1)[0]
            if count / total > 0.3:
                idle = count
                del counter[rva]
                by_module[module] -= count
    print(f"idle (waiting for the display): {idle / full * 100:.1f}% of samples; the rest is the frame's work")
    total -= idle
    print("\nBy module (share of samples):")
    for module, count in by_module.most_common():
        print(f"  {count / total * 100:5.1f}%  {module}")
    image_total = sum(own.values())
    print(f"\nTop functions in the benchmark image ({image_total} samples = {image_total / total * 100:.1f}%; % of all samples):")
    for name, count in own.most_common(top):
        print(f"  {count / total * 100:5.1f}%  {name}")
    for module, counter in foreign.items():
        module_total = sum(counter.values())
        if module_total / total < 0.02:
            continue
        print(f"\nTop addresses in {module} ({module_total / total * 100:.1f}% of samples; RVA):")
        for rva, count in counter.most_common(8):
            print(f"  {count / total * 100:5.1f}%  +{rva:#x}")


if __name__ == "__main__":
    main()
