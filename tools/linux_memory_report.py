#!/usr/bin/env python3
"""Breaks a running Linux process's resident memory down by mapping (/proc/<pid>/smaps).

    tools/linux_memory_report.py PID [--top N] [--anonymous]

Prints /proc/PID/smaps_rollup, then the resident size (Rss, Pss, private pages) of every file or region that holds more
than a few pages: the program image (.text, .rodata and .data arrive as separate mappings of the same file), the shared
libraries it loaded (libEGL and the GL driver, libLLVM under llvmpipe, libwayland), the heap, the thread stacks and the
anonymous mappings (the program's arenas), each with the number of mappings. --anonymous also lists the largest
anonymous mappings by address, which is how an arena's committed size is read off.

The numbers that matter for "how much memory does the program use" are the program's own (image + anonymous regions
without a driver library's name) against the driver's (everything under /usr/lib: Mesa, LLVM, the GL stack), reported
separately, because the driver is not ours to shrink.
"""
import argparse
import collections
import re
import sys

HEADER = re.compile(r'^([0-9a-f]+)-([0-9a-f]+) (\S+) (\S+) (\S+) (\S+)\s*(.*)$')
FIELD = re.compile(r'^(\w+):\s+(\d+) kB')


def mappings(pid):
    current = None
    with open('/proc/%d/smaps' % pid) as handle:
        for line in handle:
            header = HEADER.match(line)
            if header:
                if current:
                    yield current
                start, end, perms, _, _, _, name = header.groups()
                current = {'start': int(start, 16), 'end': int(end, 16), 'perms': perms, 'name': name.strip()}
                continue
            field = FIELD.match(line)
            if field and current is not None:
                current[field.group(1)] = int(field.group(2))
    if current:
        yield current


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('pid', type=int)
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument('--anonymous', action='store_true')
    arguments = parser.parse_args()
    pid = arguments.pid

    with open('/proc/%d/cmdline' % pid, 'rb') as handle:
        program = handle.read().split(b'\0')[0].decode(errors='replace')
    print('process %d: %s' % (pid, program))
    with open('/proc/%d/smaps_rollup' % pid) as handle:
        rollup = handle.read()
    print(''.join(line for line in rollup.splitlines(True)
                  if line.split(':')[0] in ('Rss', 'Pss', 'Pss_Anon', 'Pss_File', 'Pss_Shmem', 'Shared_Clean',
                                            'Shared_Dirty', 'Private_Clean', 'Private_Dirty', 'Anonymous', 'Swap')))

    groups = collections.OrderedDict()
    anonymous = []
    for entry in mappings(pid):
        name = entry['name'] or '[anonymous]'
        key = name if not name.startswith('[stack') else '[stack]'
        group = groups.setdefault(key, collections.Counter())
        group['maps'] += 1
        group['size'] += (entry['end'] - entry['start']) // 1024
        for field in ('Rss', 'Pss', 'Private_Clean', 'Private_Dirty', 'Shared_Clean', 'Shared_Dirty'):
            group[field] += entry.get(field, 0)
        if not entry['name'] and entry.get('Rss', 0) >= 256:
            anonymous.append(entry)

    rows = sorted(groups.items(), key=lambda item: -item[1]['Rss'])
    print('\n%-70s %5s %9s %8s %8s %9s' % ('mapping', 'maps', 'size kB', 'Rss kB', 'Pss kB', 'private kB'))
    for name, group in rows[:arguments.top]:
        print('%-70s %5d %9d %8d %8d %9d' % (name[-70:], group['maps'], group['size'], group['Rss'], group['Pss'],
                                               group['Private_Clean'] + group['Private_Dirty']))

    # Ours against the driver's: files under /usr, /lib or /opt are the system's; the program image and the
    # anonymous memory are the program's (the driver's own heap allocations are anonymous too, so the anonymous total
    # is an upper bound of ours).
    system = sum(g['Rss'] for n, g in groups.items() if n.startswith(('/usr/', '/lib', '/opt/', '/nix/')))
    own_files = sum(g['Rss'] for n, g in groups.items() if n.startswith('/') and not n.startswith(('/usr/', '/lib', '/opt/', '/nix/')))
    heap = groups.get('[heap]', collections.Counter())['Rss']
    anonymous_total = groups.get('[anonymous]', collections.Counter())['Rss']
    stacks = groups.get('[stack]', collections.Counter())['Rss']
    print('\nsystem libraries and data files %8d kB resident (the driver stack is in here)' % system)
    print('program image and its files     %8d kB resident' % own_files)
    print('[heap] (the C library)          %8d kB resident' % heap)
    print('anonymous mappings              %8d kB resident (arenas, driver heaps, thread stacks)' % anonymous_total)
    print('main-thread stack               %8d kB resident' % stacks)

    if arguments.anonymous:
        print('\nlargest anonymous mappings (address, size kB, Rss kB, perms):')
        for entry in sorted(anonymous, key=lambda e: -e.get('Rss', 0))[:arguments.top]:
            print('  %012x %9d %8d  %s' % (entry['start'], (entry['end'] - entry['start']) // 1024,
                                           entry.get('Rss', 0), entry['perms']))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        print('linux_memory_report: %s' % error, file=sys.stderr)
        sys.exit(1)
