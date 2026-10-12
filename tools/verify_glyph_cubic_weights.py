"""Check embedded cubic weights against the original binary32 operation sequence."""
from pathlib import Path
import json
import re
import struct


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def main():
    source = Path(__file__).resolve().parents[1] / 'src/render/shaders/glyph_atlas.hlsl'
    text = source.read_text(encoding='utf-8')
    block = text.split('static const float4 cubic_weights[12] = {', 1)[1].split('};', 1)[0]
    actual = [[int(v, 16) for v in re.findall(r'0x([0-9a-f]{8})u', row)]
              for row in re.findall(r'asfloat\(uint4\(([^)]+)\)\)', block)]
    expected = []
    for segment in range(1, 13):
        amount = f32(segment / 12)
        inverse = f32(1 - amount)
        expected.append([bits(f32(f32(inverse * inverse) * inverse)),
                         bits(f32(f32(f32(3 * inverse) * inverse) * amount)),
                         bits(f32(f32(f32(3 * inverse) * amount) * amount)),
                         bits(f32(f32(amount * amount) * amount))])
    if actual != expected:
        raise RuntimeError('Embedded weights differ from original binary32 evaluations')
    print(json.dumps(dict(status='passed',vectors=len(expected),exact_coefficients=48,
                          source=str(source))))


if __name__ == '__main__':
    main()
