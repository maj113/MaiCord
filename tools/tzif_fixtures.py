#!/usr/bin/env python3
"""Generates tests/generated/tzif_fixture.bin and tzif_oracle.bin for tests/platform/time/tzif_test.cpp.

The fixture is a handful of real zones compiled by zic from the IANA source (tzdata.zi), slim and fat, packed in one
file. The oracle is what Python's zoneinfo (an independent TZif reader) says about the very same bytes: the offset and
the daylight flag at every transition (the second before and the first second after) and at sample instants, and the
UTC instant of local readings around each change (fold=0: the first occurrence of a repeated reading, the offset before
the change for a missing one), which is what tzif_local_to_utc promises.

The same run writes tests/fixtures/zoneinfo (a few slim zone files) for the local time tests.

Needs zic on PATH and Python 3.9+ (zoneinfo). Run on Linux (WSL):
  python3 tools/tzif_fixtures.py --source /usr/share/zoneinfo/tzdata.zi
Fixture format ('TZFX', u32 count, then per zone: u8 name length, name, u8 fat, u32 size, bytes) and oracle format
('TZSM', u32 count, then per zone: u8 name length, name, u8 fat, u32 n, n x (i64 utc, i32 offset, i32 flags), u32 m,
m x (i64 local, i64 utc)); everything little endian. Flags: bit 0 daylight, bit 1 the daylight flag is meaningful.
"""
import argparse
import datetime
import io
import os
import struct
import subprocess
import sys
import tempfile
import zoneinfo

# name, also fat. Chosen for what each exercises: no DST (UTC, Kolkata), 30-minute zones (Kolkata, St_Johns,
# Kathmandu 45), +14:00, a 30-minute DST step (Lord_Howe), southern DST that was abolished (Sao_Paulo), a date line
# jump (Apia), negative DST (Dublin, Casablanca), a 2 hour step (Troll), and the common northern rules.
ZONES = [
    ("Etc/UTC", False),
    ("America/New_York", True),
    ("Europe/London", False),
    ("Europe/Berlin", True),
    ("Asia/Kolkata", False),
    ("Asia/Kathmandu", False),
    ("Australia/Lord_Howe", True),
    ("America/Sao_Paulo", True),
    ("Pacific/Apia", False),
    ("Pacific/Kiritimati", False),
    ("Africa/Casablanca", False),
    ("Europe/Dublin", False),
    ("America/St_Johns", False),
    ("Antarctica/Troll", False),
]
# Zones whose daylight flag is not the offset comparison Python makes (negative DST).
FLAG_UNCHECKED = {"Europe/Dublin", "Africa/Casablanca"}
EPOCH = datetime.datetime(1970, 1, 1)
START = int((datetime.datetime(1900, 1, 1) - EPOCH).total_seconds())
END = int((datetime.datetime(2100, 1, 1) - EPOCH).total_seconds())


def offset_of(zone, t):
    moment = (EPOCH + datetime.timedelta(seconds=t)).replace(tzinfo=datetime.timezone.utc).astimezone(zone)
    return int(moment.utcoffset().total_seconds()), moment.dst() != datetime.timedelta(0)


def changes(zone):
    """Instants where the offset or the daylight flag changes: the first second of the new state."""
    found = []
    previous = offset_of(zone, START)
    t = START
    while t < END:
        nxt = min(t + 86400, END)
        state = offset_of(zone, nxt)
        if state != previous:
            low, high = t, nxt  # change in (low, high]
            while high - low > 1:
                mid = (low + high) // 2
                if offset_of(zone, mid) == previous:
                    low = mid
                else:
                    high = mid
            found.append(high)
            previous = offset_of(zone, high)
            if previous != state:  # two changes inside the day
                found.append(nxt)
                previous = state
        t = nxt
    return found


def local_expected(zone, local):
    naive = EPOCH + datetime.timedelta(seconds=local)
    moment = naive.replace(tzinfo=zone, fold=0)
    return local - int(moment.utcoffset().total_seconds())


def samples(name, zone):
    utc = {}
    local = {}
    found = changes(zone)
    # Every change from 1970 to 2030, every fifth one before and every fourth one after (the footer's rule years).
    keep = []
    for i, t in enumerate(found):
        if (t < 0 and i % 5 == 0) or 0 <= t < 1893456000 or (t >= 1893456000 and i % 4 == 0):
            keep.append(t)
    for t in keep:
        for instant in (t - 1, t):
            utc[instant] = offset_of(zone, instant)
        before = offset_of(zone, t - 1)[0]
        after = offset_of(zone, t)[0]
        low, high = min(before, after), max(before, after)
        for reading in (t + low - 1, t + low, t + high):
            local[reading] = local_expected(zone, reading)
    # Spread instants across the range, to cover rule years and the ends of the table.
    t = START
    n = 0
    while t < END:
        utc[t] = offset_of(zone, t)
        n += 1
        t += 86400 * 613 + 3600 * (n % 24)
    for reading in sorted(utc)[::15]:
        local[reading] = local_expected(zone, reading)
    return sorted(utc.items()), sorted(local.items())


def build_zones(source):
    root = tempfile.mkdtemp()
    out = {}
    for kind, flag in (("slim", False), ("fat", True)):
        directory = os.path.join(root, kind)
        subprocess.run(["zic", "-b", kind, "-d", directory, source], check=True)
        out[flag] = directory
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default="/usr/share/zoneinfo/tzdata.zi")
    parser.add_argument("--output", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "generated"))
    arguments = parser.parse_args()
    directories = build_zones(arguments.source)
    fixture = bytearray(b"TZFX" + struct.pack("<I", 0))
    oracle = bytearray(b"TZSM" + struct.pack("<I", 0))
    count = 0
    for name, with_fat in ZONES:
        for fat in ([False, True] if with_fat else [False]):
            with open(os.path.join(directories[fat], name), "rb") as file:
                data = file.read()
            zone = zoneinfo.ZoneInfo.from_file(io.BytesIO(data), key=name)
            tag = name.encode()
            fixture += struct.pack("<B", len(tag)) + tag + struct.pack("<BI", fat, len(data)) + data
            utc, local = samples(name, zone)
            oracle += struct.pack("<B", len(tag)) + tag + struct.pack("<BI", fat, len(utc))
            for t, (offset, dst) in utc:
                flags = 0 if name in FLAG_UNCHECKED else (2 | (1 if dst else 0))
                oracle += struct.pack("<qii", t, offset, flags)
            oracle += struct.pack("<I", len(local))
            for reading, expected in local:
                oracle += struct.pack("<qq", reading, expected)
            count += 1
            print(name, "fat" if fat else "slim", len(data), "bytes", len(utc), "instants", len(local), "readings")
    fixture[4:8] = struct.pack("<I", count)
    oracle[4:8] = struct.pack("<I", count)
    with open(os.path.join(arguments.output, "tzif_fixture.bin"), "wb") as file:
        file.write(fixture)
    with open(os.path.join(arguments.output, "tzif_oracle.bin"), "wb") as file:
        file.write(oracle)
    # A zoneinfo directory (slim files) for the tests that load a zone by name or by path: tests/fixtures/zoneinfo.
    tree = os.path.join(arguments.output, "..", "fixtures", "zoneinfo")
    for name in ("Etc/UTC", "Europe/Berlin", "America/New_York", "Asia/Kolkata", "Australia/Lord_Howe"):
        target = os.path.join(tree, name)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(os.path.join(directories[False], name), "rb") as source, open(target, "wb") as file:
            file.write(source.read())


if __name__ == "__main__":
    sys.exit(main())
