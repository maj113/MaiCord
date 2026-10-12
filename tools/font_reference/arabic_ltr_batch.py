"""Bounded, batched Windows/native forced-LTR Arabic comparisons (offline QA)."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import struct
import subprocess
import time


def identity(path):
    return dict(path=str(path.resolve()), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def samples():
    words = ['سلام', 'سَلَامٌ', 'لَا لِا', 'العربية', 'أردو', 'فارسی', 'مرحبا',
             'لأ لإ لآ', 'اَلْعَرَبِيَّةُ', 'َ', 'َب', 'َّب', 'بَّ', 'بَّ',
             'ب\u200cب', 'ب\u200dب', '\u200dب', 'ب\u200d', 'ب\u200c',
             'ب\u034fب', '\u034fبب', 'بَ\u034fب']
    for a, b in itertools.product('ابتلمس', repeat=2):
        words.extend([a+b, a+'َ'+b, a+'َّ'+b, a+'\u200d'+b])
    for marks in ['َ', 'ّ', 'َّ', 'َّ', 'ََ', 'ٔ', 'ِ', 'ٌ']:
        words.extend([marks+'ب', 'ب'+marks+'ب', 'لا'+marks, 'ب'+marks+'لا'])
    for control in ('\u200c', '\u200d'):
        words.extend(['ب'+control+'َ', 'َب'+control, 'َ'+control+'ب',
                      control+'َب', control+'بَ', 'ب'+control+control])
    return list(dict.fromkeys(words))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--font', type=Path, action='append', required=True)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--reference', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True)
    p.add_argument('--case', help='One comma-separated BMP scalar sequence')
    p.add_argument('--cases-json', type=Path, help='Explicit list of BMP scalar sequences for a targeted corpus')
    a = p.parse_args()
    if a.case and a.cases_json:
        p.error('--case and --cases-json are mutually exclusive')
    texts = ([''.join(map(chr, values)) for values in json.loads(a.cases_json.read_text(encoding='utf-8'))] if a.cases_json else
             [''.join(chr(int(x, 16)) for x in a.case.split(','))] if a.case else samples())
    if not texts:
        p.error('The corpus must not be empty')
    if any(not text or len(text)>128 or any(ord(c)>65535 or 0xd800<=ord(c)<=0xdfff for c in text) for text in texts):
        p.error('This fixture reader requires nonempty BMP text of at most 128 scalars')
    output = a.output_dir.resolve()/('run-'+str(time.time_ns()))
    output.mkdir(parents=True)
    start = time.perf_counter()
    deadline = start+10
    rows = []
    report = dict(status='running', native=identity(a.native), reference=identity(a.reference),
                  fonts=[identity(font) for font in a.font], comparisons=0, differences=[])

    def run(command, **kwargs):
        remaining = deadline-time.perf_counter()
        if remaining <= 0:
            raise TimeoutError('Ten-second whole-run deadline')
        result = subprocess.run(command, capture_output=True, timeout=remaining,
                                creationflags=subprocess.CREATE_NO_WINDOW, **kwargs)
        if result.returncode:
            raise RuntimeError(f'Process exited {result.returncode}: {result.stderr!r}')
        return result.stdout

    try:
        for font_index, font in enumerate(a.font):
            for em in (14., 1000.):
                bits = struct.unpack('<I', struct.pack('<f', em))[0]
                cases = [(text, mode) for text in texts for mode in (0, 1)]
                payload = b'MCSHPB1\0'+struct.pack('<I', len(cases))
                payload += b''.join(struct.pack('<II', len(text), mode)+text.encode('utf-16-le') for text, mode in cases)
                env = os.environ.copy()
                env['MAICORD_SHAPE_BATCH'] = '1'
                raw = run([str(a.reference.resolve()), str(font.resolve()), '627', 'default', str(bits), '0', '0', 'en-US'], input=payload, env=env)
                assert raw[:12] == payload[:12]
                at = 12
                for text, mode in cases:
                    count, = struct.unpack_from('<I', raw, at); at += 4
                    glyphs = list(struct.unpack_from('<'+'H'*count, raw, at)); at += count*2
                    clusters = list(struct.unpack_from('<'+'H'*len(text), raw, at)); at += len(text)*2
                    placements = [list(struct.unpack_from('<3I', raw, at+i*12)) for i in range(count)]; at += count*12
                    rows.append(dict(font=font_index, scalars=list(map(ord, text)), mode=mode, em_bits=bits,
                                     glyphs=glyphs, clusters=clusters, placement_bits=placements))
                assert at == len(raw)
        blob = b'MCARPL3\0'+struct.pack('<II', len(a.font), len(rows))
        for font in a.font:
            raw = font.read_bytes(); blob += struct.pack('<I', len(raw))+raw
        for row in rows:
            fields = [row['em_bits'], 0, row['font'], row['mode'], len(row['scalars']), len(row['glyphs'])]
            blob += struct.pack('<6I', *fields)
            for key, fmt in [('scalars', 'I'), ('glyphs', 'H'), ('clusters', 'I')]:
                blob += struct.pack('<'+fmt*len(row[key]), *row[key])
            blob += b''.join(struct.pack('<3I', *value) for value in row['placement_bits'])
        fixture = output/'corpus.bin'; fixture.write_bytes(blob)
        raw = run([str(a.native.resolve()), str(fixture), '--arabic-script'])
        (output/'native.jsonl').write_bytes(raw)
        (output/'reference.json').write_text(json.dumps(rows, ensure_ascii=True), encoding='utf-8')
        observed = [json.loads(line) for line in raw.decode('utf-8').splitlines() if line.startswith('{')]
        assert len(observed) == len(rows)
        for expected, actual in zip(rows, observed):
            assert actual['case'] == report['comparisons'] and actual['gate'] == 1
            keys = ('glyphs', 'clusters', 'placement_bits')
            if actual['status'] or any(expected[key] != actual[key] for key in keys):
                report['differences'].append(dict(case=actual['case'], expected=expected, actual=actual))
            report['comparisons'] += 1
        report['status'] = 'different' if report['differences'] else 'passed'
    except Exception as error:
        report['status'] = 'failed'; report['error'] = repr(error)
    report['seconds'] = time.perf_counter()-start
    (output/'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"{report['comparisons']} comparisons, {len(report['differences'])} differences, {report['seconds']:.2f}s: {output}")
    if 'error' in report:
        print(report['error'])
    return int(report['status'] != 'passed')


if __name__ == '__main__':
    raise SystemExit(main())
