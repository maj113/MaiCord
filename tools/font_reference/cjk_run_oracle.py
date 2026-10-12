"""First-party Han/kana probes. FontTools writes QA fonts; Windows supplies answers.

No production text library is used. Preserve unsuccessful observations rather
than silently reducing the corpus. System-font extraction is QA-only and records
the original collection and face index as well as the exported font identity.
"""
from pathlib import Path
import argparse, concurrent.futures, hashlib, json, struct, subprocess, zipfile
from fontTools.fontBuilder import FontBuilder
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._g_l_y_f import Glyph
from feature_plan_oracle import TAGS, layout, script, language
from gsub_execution_oracle import lookup, single, obj
from gpos_execution_oracle import adjust


def identity(path):
    path = Path(path)
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(path=str(path.resolve()), bytes=path.stat().st_size, sha256=digest)


def make_font(path, scalars, omitted=(), kind=None, features=(), scripts=None, marks=False):
    order = ['.notdef'] + [f'u{cp:06x}' for cp in sorted(scalars)] + ['marker', 'marker2', 'marker3']
    cmap = {cp: f'u{cp:06x}' for cp in scalars if cp not in omitted}
    font = FontBuilder(1000, isTTF=True)
    font.setupGlyphOrder(order)
    font.setupCharacterMap(cmap)
    font.setupGlyf({g: Glyph() for g in order})
    font.setupHorizontalMetrics({g: (600, 0) for g in order})
    font.setupHorizontalHeader(ascent=800, descent=-200)
    name = 'MaiCord CJK ' + path.stem
    font.setupNameTable(dict(familyName=name, styleName='Regular', uniqueFontIdentifier=name,
                            fullName=name, psName=name.replace(' ', '')))
    font.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    font.setupPost()
    font.setupMaxp()
    if kind:
        inputs = [f'u{cp:06x}' for cp in (0x4e2d, 0x304b, 0x30ab)]
        tables = ([lookup(1, single({g: 'marker' for g in inputs}))] if kind == 'GSUB'
                  else [adjust(g, 13, 17, 19) for g in inputs])
        records = [(tag, list(range(len(tables)))) for tag in features]
        font.font[kind] = newTable(kind)
        font.font[kind].table = layout(kind, records, tables, scripts or {
            tag: script(language(range(len(records)))) for tag in ('DFLT', 'latn', 'hani', 'kana')})
    if marks:
        classes = {f'u{cp:06x}': 3 for cp in (0x3099, 0x309a, 0x301) if cp in scalars}
        font.font['GDEF'] = newTable('GDEF')
        font.font['GDEF'].table = obj('GDEF', Version=0x10000,
            GlyphClassDef=obj('ClassDef', classDefs=classes), AttachList=None,
            LigCaretList=None, MarkAttachClassDef=None)
    font.font['head'].created = font.font['head'].modified = 2082844800
    font.save(path)


def prepare(root, ucd):
    from sys import path as paths
    paths.insert(0, str(Path(__file__).resolve().parents[1] / 'unicode'))
    from latin_composition import PIN
    assert identity(ucd)['sha256'] == PIN
    with zipfile.ZipFile(ucd) as archive:
        data = archive.read('UnicodeData.txt').decode('utf-8')
    pairs = []
    for line in data.splitlines():
        fields = line.split(';'); cp = int(fields[0], 16); parts = fields[5].split()
        if 0x3040 <= cp <= 0x30ff and len(parts) == 2 and not parts[0].startswith('<'):
            pairs.append([int(v, 16) for v in parts] + [cp])
    assert len(pairs) == 58
    samples = [('hani', [0x4e2d, 0x6587]), ('hani', [0x20000, 0x4e2d]),
               ('hani', [0x4e2d, 0xe0100]), ('hani', [0x4e2d, 0xfe00]),
               ('kana', [0x304b]), ('kana', [0x30ab]),
               ('kana', [0xff76, 0xff9e]), ('kana', [0xff8a, 0xff9f]),
               ('kana', [0x30ab, 0x309b]), ('kana', [0x30cf, 0x309c]),
               ('kana', [0x30ab, 0x3099, 0x3099]), ('kana', [0x30ab, 0x3099, 0x309a]),
               ('kana', [0x3099, 0x30ab]), ('kana', [0x30ab, 0x34f, 0x3099]),
               ('kana', [0x30ab, 0x200d, 0x3099]), ('kana', [0x30ab, 0x200c, 0x3099]),
               ('kana', [0x31f7, 0x309a]), ('kana', [0x1b000, 0x3099])]
    for first, last, composed in pairs:
        samples += [('kana', [first, last]), ('kana', [composed])]
    for base in (0x304b, 0x30ab, 0xff76, 0x31f7, 0x1b000):
        for mark in (0xff9e, 0xff9f):
            samples += [('kana', [base, mark]), ('kana', [mark, base]),
                        ('kana', [base, mark, mark]), ('kana', [base, 0x3099, mark])]
    scalars = {cp for _, text in samples for cp in text} | {32, 0x25cc, 0x301}
    root.mkdir(parents=True, exist_ok=True); fonts = root / 'fonts'; fonts.mkdir(exist_ok=True)
    cases = []
    def add(path, group, samples, modes=(0, 1), sizes=(14., 17.5, 1000.)):
        for tag, text in samples:
            for mode in modes:
                for em in sizes:
                    cases.append(dict(font=identity(path), group=group, script=tag, text=text,
                                      independent=mode, em_bits=struct.unpack('<I', struct.pack('<f', em))[0]))
    for label, omitted, marks in [('all', (), False), ('marks', (), True),
            ('no-composites', [z for _, _, z in pairs], False),
            ('no-marks', [0x3099, 0x309a], False), ('no-circle', [0x25cc], False)]:
        path = fonts / (label + '.ttf'); make_font(path, scalars, omitted, marks=marks)
        add(path, 'preparation', samples)
    for kind in ('GSUB', 'GPOS'):
        for feature in dict.fromkeys(TAGS + 'hkna vkna chws vchw cpct apkn pwid qwid twid jp78 jp83 jp90 jp04 smpl trad expt'.split()):
            path = fonts / (kind.lower() + '-' + feature + '.ttf')
            make_font(path, scalars, kind=kind, features=[feature])
            add(path, 'feature-' + kind + '-' + feature,
                [('hani', [0x4e2d]), ('kana', [0x304b]), ('kana', [0x30ab])], sizes=(1000.,))
    for label, tags in [('requested', ['hani', 'kana']), ('fallback', ['DFLT', 'latn']),
                        ('default', ['DFLT']), ('latin', ['latn']), ('unrelated', ['grek'])]:
        path = fonts / ('script-' + label + '.ttf')
        make_font(path, scalars, kind='GSUB', features=['ccmp'],
                  scripts={tag: script(language([0])) for tag in tags})
        add(path, 'script-' + label, [('hani', [0x4e2d]), ('kana', [0x304b]), ('kana', [0x30ab])], sizes=(1000.,))
    (root / 'cases.json').write_text(json.dumps(dict(ucd=identity(ucd), pairs=pairs, cases=cases), indent=2), encoding='utf-8')
    print('Prepared', len(cases), 'cases', flush=True)


def system_fonts(root, paths):
    data = json.loads((root / 'cases.json').read_text(encoding='utf-8'))
    # Reuse every preparation sample and size/mode without the synthetic-font
    # cross-product. Missing cmap glyphs remain observations, not omissions.
    samples = [c for c in data['cases'] if c['group'] == 'preparation' and Path(c['font']['path']).stem == 'all']
    for source in paths:
        path = root / 'fonts' / ('system-' + source.stem + '.ttf')
        if source.suffix.lower() == '.ttc':
            with TTFont(source, fontNumber=0) as font: font.save(path)
        else:
            path.write_bytes(source.read_bytes())
        exported, original = identity(path), identity(source)
        for case in samples:
            data['cases'].append(dict(case, group='system', font=exported, original=original, face_index=0))
    (root / 'cases.json').write_text(json.dumps(data, indent=2), encoding='utf-8')
    print('Prepared', len(data['cases']), 'including system cases', flush=True)


def capture(root, exe):
    data = json.loads((root / 'cases.json').read_text(encoding='utf-8'))
    for item in {c['font']['path']: c['font'] for c in data['cases']}.values():
        assert identity(item['path']) == item
    def observe(pair):
        index, case = pair
        command = [str(exe), case['font']['path'], ','.join(f'{cp:x}' for cp in case['text']),
                   'liga=0,clig=0,kern=0' if case['independent'] else 'default',
                   str(case['em_bits']), '0', '0', 'en-US']
        result = subprocess.run(command, capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW)
        return dict(id=index, case=case, command=command, exit=result.returncode,
                    result=json.loads(result.stdout) if result.stdout else None, stderr=result.stderr)
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        with (root / 'observations.jsonl').open('w', encoding='utf-8') as output:
            for row in pool.map(observe, enumerate(data['cases'])):
                rows.append(row); output.write(json.dumps(row) + '\n')
                if len(rows) % 1000 == 0: print('Observed', len(rows), flush=True)
    modules = {r['result']['dwrite_module'] for r in rows if r['result'] and 'dwrite_module' in r['result']}
    (root / 'reference-identity.json').write_text(json.dumps(dict(executable=identity(exe),
        modules=[identity(p) for p in sorted(modules)], cases=identity(root / 'cases.json'),
        observations=identity(root / 'observations.jsonl')), indent=2), encoding='utf-8')
    records = []; rejected = []
    for row in rows:
        r = row['result']; c = row['case']
        if row['exit'] or not r or r['hresult'] or r['placement_hresult']:
            rejected.append(row['id']); continue
        path = c['font']['path'].encode('utf-16le'); text = ''.join(chr(cp) for cp in c['text']).encode('utf-8')
        record = struct.pack('<I', len(path)//2) + path + struct.pack('<I', len(text)) + text
        # Force full shaping: these are TextAnalyzer observations. Paragraph
        # nominal eligibility and fallback are separately measured app behavior.
        record += struct.pack('<6I', c['em_bits'], c['independent'] | 2, 0,
                              int.from_bytes(c['script'].encode(), 'big'), len(r['glyphs']), len(r['clusters']))
        record += struct.pack('<' + 'H'*len(r['glyphs']), *r['glyphs'])
        record += struct.pack('<' + 'I'*len(r['clusters']), *r['clusters'])
        record += b''.join(struct.pack('<3I', *v) for v in r['placement_bits'])
        records.append(record)
    (root / 'corpus.bin').write_bytes(b'MCSCRIP1' + struct.pack('<I', len(records)) + b''.join(records))
    (root / 'corpus-map.json').write_text(json.dumps(dict(record_ids=[r['id'] for r in rows if r['id'] not in rejected],
                                                       rejected_ids=rejected), indent=2), encoding='utf-8')
    print('Captured', len(rows), 'rows;', len(rejected), 'reference rejections', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'capture', 'system']); parser.add_argument('root', type=Path)
    parser.add_argument('--ucd', type=Path); parser.add_argument('--exe', type=Path)
    parser.add_argument('--font', type=Path, action='append', default=[])
    args = parser.parse_args()
    if args.action == 'prepare': prepare(args.root, args.ucd)
    elif args.action == 'system': system_fonts(args.root, args.font)
    else: capture(args.root, args.exe)
