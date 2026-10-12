"""Independent rational fvar/avar-v1 normalization check (development only)."""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import struct
from fontTools.ttLib import TTFont


def rounded(value):
    return (value + Fraction(1, 2)).numerator // (value + Fraction(1, 2)).denominator


def fixed16(value):
    return Fraction(rounded(value * 65536), 65536)


def normalize(axis, value, segment):
    low, normal, high = map(Fraction, (axis.minValue, axis.defaultValue, axis.maxValue))
    value = fixed16(min(high, max(low, value)))
    value = fixed16((value-normal)/(normal-low) if value < normal else
                    (value-normal)/(high-normal) if value > normal else Fraction(0))
    if segment and all(segment.get(p) == p for p in (-1, 0, 1)):
        points = sorted((Fraction(a), Fraction(b)) for a, b in segment.items())
        for index, (end_x, end_y) in enumerate(points):
            if value == end_x:
                value = end_y
                break
            if value < end_x:
                start_x, start_y = points[index-1]
                ratio = fixed16((value-start_x)/(end_x-start_x))
                value = start_y + fixed16(ratio*(end_y-start_y))
                break
    value = min(Fraction(1), max(Fraction(-1), fixed16(value)))
    return int(value*65536), rounded(value*16384)


def compare(path, log):
    font = TTFont(path)
    if 'avar' in font and font['avar'].majorVersion != 1:
        raise ValueError('This independent oracle covers avar v1 only')
    axes = font['fvar'].axes
    segments = font['avar'].segments if 'avar' in font else {}
    current = {}; rows = []
    for line in log.read_text(encoding='utf-8-sig').splitlines():
        key, separator, value = line.partition('=')
        if separator and key.startswith('axes_'):
            current[key] = int(value)
            if key == 'axes_condition_biased':
                rows.append(current); current = {}
    if not rows:
        raise ValueError('No native normalization records in the supplied log')
    differences = []; seen = set()
    for row in rows:
        index, sample = row['axes_index'], row['axes_sample']
        if (sample, index) in seen:
            raise ValueError('Duplicate normalization record')
        seen.add((sample, index))
        user = struct.unpack('<f', struct.pack('<I', row['axes_user_float_bits']))[0]
        expected = normalize(axes[index], Fraction(user), segments.get(axes[index].axisTag))
        actual = (row['axes_fixed16_biased']-65536, row['axes_condition_biased']-16384)
        if actual != expected:
            differences.append(dict(sample=sample, axis=index, user=user, expected=expected, actual=actual))
    expected_rows = len(axes) * (10 ** len(axes))
    if len(rows) != expected_rows:
        raise ValueError(f'Expected {expected_rows} axis records, got {len(rows)}')
    return dict(font=str(path), axis_records=len(rows), differences=differences, matches=not differences)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('font', type=Path); parser.add_argument('--native-log', type=Path, required=True)
    args = parser.parse_args(); result = compare(args.font, args.native_log)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['matches'] else 1)
