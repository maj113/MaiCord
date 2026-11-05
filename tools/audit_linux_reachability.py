#!/usr/bin/env python3
"""Reports the code the Linux client links but never reaches, from two links of the same objects.

Usage: audit_linux_reachability.py --map NOGC.map --gc-log GC.log [--compile-commands compile_commands.json]
                                   [--top N] [--functions PREFIX]

tools/audit_linux_reachability.sh makes the inputs: the client built without link-time optimization (with LTO the
compiler deletes the unreachable functions before the linker is asked, and nothing is left to see), linked once
with --no-gc-sections and a map (every section of every archive member the link pulled in: NOGC.map), and once with
--gc-sections --print-gc-sections (the sections nothing reaches from the entry point: GC.log). The difference is
what the client carries as code that no path from `_start` calls, grouped by the source file it came from. A unit
dropped whole, or a feature whose entry points are all dropped, is where a stub or an #if cuts a path that Windows
keeps (or where the feature is intentionally Windows-only).

--functions PREFIX lists the dropped functions (demangled) of the units whose source path starts with PREFIX.
"""
import argparse
import collections
import json
import re
import subprocess
import sys

REMOVED = re.compile(r"removing unused section '(?P<object>[^']*?):\((?P<section>[^']*)\)'")
# lld's map: "<vma> <lma> <size> <align> <outsec>" for an output section, "<vma> <lma> <size> <align> <obj>:(<section>)"
# for an input section (the numbers are hexadecimal).
INPUT = re.compile(r"^\s*[0-9a-f]+\s+[0-9a-f]+\s+(?P<size>[0-9a-f]+)\s+\d+\s+(?P<object>\S.*?):\((?P<section>[^)]+)\)\s*$")
REMOVED = re.compile(r"removing unused section '?(?P<object>.+?):\((?P<section>[^)]*)\)'?\s*$")

def read_sizes(path):
    sizes = {}
    with open(path, errors="replace") as map_file:
        for line in map_file:
            match = INPUT.match(line)
            if match:
                sizes[(match.group("object"), match.group("section"))] = int(match.group("size"), 16)
    return sizes


def read_removed(path):
    removed = []
    with open(path, errors="replace") as log:
        for line in log:
            match = REMOVED.search(line)
            if match:
                removed.append((match.group("object"), match.group("section")))
    return removed


def source_index(path):
    """(archive basename, member) -> source files, from compile_commands.json."""
    index = collections.defaultdict(list)
    if not path:
        return index
    with open(path) as database:
        for entry in json.load(database):
            output = entry.get("output", "")
            match = re.match(r"CMakeFiles/(?P<target>[^/]+)\.dir/(?P<rest>.*)$", output)
            if match:
                index[("lib" + match.group("target") + ".a", output.rsplit("/", 1)[-1])].append(entry["file"])
    return index


def source_of(object_name, index):
    match = re.match(r"(?P<archive>.*/)?(?P<lib>[^/]+\.a)\((?P<member>[^)]+)\)$", object_name)
    if match:
        files = index.get((match.group("lib"), match.group("member")), [])
        return files[0] if len(files) == 1 else (match.group("lib") + "(" + match.group("member") + ")")
    match = re.match(r".*CMakeFiles/[^/]+\.dir/(?P<rest>.*)\.o$", object_name)
    return match.group("rest") if match else object_name


def demangle(names):
    result = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True)
    return result.stdout.splitlines() if result.returncode == 0 else names


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--map", required=True)
    parser.add_argument("--gc-log", required=True)
    parser.add_argument("--compile-commands")
    parser.add_argument("--top", type=int, default=60)
    parser.add_argument("--functions")
    arguments = parser.parse_args()

    sizes = read_sizes(arguments.map)
    index = source_index(arguments.compile_commands)
    total = collections.Counter()
    dropped = collections.Counter()
    dropped_functions = collections.defaultdict(list)
    for (object_name, section), size in sizes.items():
        if section.startswith(".text"):
            total[source_of(object_name, index)] += size
    for object_name, section in read_removed(arguments.gc_log):
        if section.startswith(".text"):
            source = source_of(object_name, index)
            dropped[source] += sizes.get((object_name, section), 0)
            dropped_functions[source].append(section[len(".text."):] if section.startswith(".text.") else section)

    all_code = sum(total.values())
    all_dropped = sum(dropped.values())
    print("code in the linked units: %d bytes; reached from the entry point: %d; unreachable: %d (%.1f%%)" %
          (all_code, all_code - all_dropped, all_dropped, 100.0 * all_dropped / max(all_code, 1)))
    print()
    print("%-9s %-9s %-6s  %s" % ("dropped", "of", "share", "source (units with every function dropped are marked *)"))
    rows = sorted(dropped, key=lambda source: -dropped[source])
    for source in rows[:arguments.top]:
        whole = "*" if dropped[source] >= total[source] and total[source] else " "
        print("%-9d %-9d %5.1f%% %s %s" % (dropped[source], total[source], 100.0 * dropped[source] / max(total[source], 1),
                                           whole, source))
    if arguments.functions:
        print()
        for source in rows:
            if source.startswith(arguments.functions) or arguments.functions in source:
                print("== " + source)
                for name in demangle(sorted(dropped_functions[source])):
                    print("   " + name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
