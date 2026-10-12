"""Compare the actual native getters with Unicode's independent flat XML data.

Does not import the generator or reconstruct its trie/packed-record algorithm.
Enum labels describe the generated public ABI; expected values come from XML.
All code points, consumed predicates and mapping values are compared exactly.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET
import zipfile

LIMIT = 0x110000


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--xml', required=True, type=Path)
    parser.add_argument('--ucd', required=True, type=Path)
    parser.add_argument('--schema', required=True, type=Path)
    parser.add_argument('--native', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    schema = json.loads(args.schema.read_text())
    expected = {'ucd': '2066d1909b2ea93916ce092da1c0ee4808ea3ef8407c94b4f14f5b7eb263d28e',
                'xml': '0466e880488c08f7f5373ada4da5746f2223b654c622d98fae7fb9edb7c5fd01'}
    for name, digest in expected.items():
        if hashlib.sha256(getattr(args, name).read_bytes()).hexdigest() != digest:
            raise ValueError(f'Unpinned Unicode {name} archive')
    aliases = {}
    with zipfile.ZipFile(args.ucd) as archive:
        for raw in archive.read('PropertyValueAliases.txt').decode('utf-8').splitlines():
            data = raw.partition('#')[0].strip()
            if not data:
                continue
            parts = [v.strip() for v in data.split(';')]
            for value in parts[1:]:
                aliases.setdefault(parts[0], {})[value] = parts[1]
    covered = script_members = 0
    description = None
    with zipfile.ZipFile(args.xml) as archive, args.native.open('rb') as native:
        magic = native.read(8)
        if magic not in (b'MCUCD17\0', b'MCUCDS1\0', b'MCUCDS2\0'):
            raise ValueError('Invalid native dump header')
        full_extensions = magic != b'MCUCDS2\0'
        header = struct.Struct(f'<{len(schema["fields"])}B3I' + ('BB' if full_extensions else 'B'))
        with archive.open('ucd.nounihan.flat.xml') as xml:
            for _, element in ET.iterparse(xml, events=('end',)):
                tag = element.tag.rsplit('}', 1)[-1]
                if tag == 'description':
                    description = element.text
                if tag not in ('char', 'reserved', 'noncharacter', 'surrogate'):
                    element.clear()
                    continue
                values = element.attrib
                if 'cp' in values:
                    first = last = int(values['cp'], 16)
                else:
                    first, last = int(values['first-cp'], 16), int(values['last-cp'], 16)
                if first != covered or not first <= last < LIMIT:
                    raise ValueError(f'XML coverage is not contiguous at {covered:x}')
                expected = []
                for field in schema['fields']:
                    key = field['ucd']
                    if key == 'ccc':
                        expected.append(int(values[key]))
                    else:
                        value = aliases[key].get(values[key], values[key])
                        expected.append(field['values'].index(value))
                flag_bits = 0
                for flag in schema['flags']:
                    key = flag['xml']
                    if key == 'scx':
                        enabled = any(aliases['sc'].get(v, v) == 'Arab' for v in values[key].split())
                    elif key == 'ea':
                        enabled = aliases['ea'].get(values[key], values[key]) in ('F', 'W', 'H')
                    else:
                        value = values[key]
                        if value not in ('Y', 'N'):
                            raise ValueError(f'Invalid binary property {value}')
                        enabled = value == 'Y'
                    if enabled:
                        flag_bits |= 1 << flag['bit']
                scripts = tuple(sorted(int.from_bytes(aliases['sc'].get(v, v).encode('ascii'), 'big')
                                       for v in values['scx'].split()))
                bracket_type = {'n': 0, 'o': 1, 'c': 2}[values['bpt']]
                for cp in range(first, last + 1):
                    raw = native.read(header.size)
                    if len(raw) != header.size:
                        raise ValueError(f'Truncated native properties at {cp:x}')
                    actual = header.unpack(raw)
                    mirror = cp if values['bmg'] in ('', '#') else int(values['bmg'], 16)
                    bracket = cp if values['bpb'] in ('', '#') else int(values['bpb'], 16)
                    expected_row = tuple(expected) + (flag_bits, mirror, bracket, bracket_type)
                    if full_extensions:
                        expected_row += (len(scripts),)
                    if actual != expected_row:
                        names = [f['ucd'] for f in schema['fields']] + ['flags', 'bmg', 'bpb', 'bpt'] + (['scx_count'] if full_extensions else [])
                        differences = {name: {'native': a, 'xml': b} for name, a, b in zip(names, actual, expected_row) if a != b}
                        raise ValueError(f'U+{cp:04X}: {differences}')
                    if full_extensions:
                        raw = native.read(len(scripts) * 4)
                        if len(raw) != len(scripts) * 4 or struct.unpack('<' + 'I' * len(scripts), raw) != scripts:
                            raise ValueError(f'Script_Extensions mismatch at U+{cp:04X}')
                        script_members += len(scripts)
                covered = last + 1
                element.clear()
        if description != 'Unicode 17.0.0' or covered != LIMIT or native.read(1):
            raise ValueError('Version, coverage or trailing native data mismatch')
    report = {'matches': True, 'unicode_version': '17.0.0', 'code_points': covered,
              'scalar_values': covered - 2048, 'enumerated_fields_per_code_point': len(schema['fields']),
              'binary_flags_per_code_point': len(schema['flags']), 'script_extension_members': script_members,
              'derived_predicate_checks': 0 if full_extensions else covered * 2,
              'mirror_and_bracket_mapping_checks': covered * 3,
              'identities': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                             for path in [args.xml, args.ucd, args.schema, args.native]}}
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k:v for k,v in report.items() if k != 'identities'}))


if __name__ == '__main__':
    main()
