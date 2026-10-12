"""First-party CGJ fonts and Windows observations; QA dependencies only."""
import argparse, itertools, json, subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import arabic_joining_oracle as joining
from arabic_plan_oracle import OPTIONS
from feature_plan_oracle import layout, language, script
from gsub_execution_oracle import obj, lookup, single, context, identity
from fontTools.ttLib import TTFont, newTable


def run(root, exe):
    root.mkdir(parents=True, exist_ok=True)
    assert not (root/'observations.json').exists(), 'Choose a fresh capture directory'
    joining.SCALARS = [0x628, 0x34f, 0x64e, 0x651, 0x200c, 0x200d, 0x627, 0x640, 0x621, 0x25cc]
    beh, cgj = 'u0628', 'u034f'
    alphabet = [0x628, 0x34f, 0x64e, 0x200c, 0x200d, 32, 0x627]
    texts = {v for n in (2, 3) for v in itertools.product(alphabet, repeat=n) if 0x628 in v and 0x34f in v}
    for text in ('628,64e,34f,628', '64e,34f,651,628', '628,34f,34f,628', '628,628,34f,628',
                 '34f,628,20,628', '34f,628,34f,628', '34f,628,200d,628', '34f,628,200c,628',
                 '34f,628,64e,628', '34f,64e,628,628', '34f,34f,628,628', '34f,628,628,20,628,628',
                 '34f,628,628,34f,628,628', '34f,628,628,200c,628,628', '34f,628,628,64e,628,628',
                 '34f,200d,628,628', '34f,200c,628,628', '34f,34f,34f,628,628',
                 '34f,628,64e,651,628,628', '34f,628,64e,34f,628,628', '34f,627,628',
                 '34f,640,628', '34f,621,628', '628,34f,627,628'):
        texts.add(tuple(int(v, 16) for v in text.split(',')))
    requests = []
    names = ['baseline', 'cmap-absent', 'cgj-mark', 'ccmp-cgj', 'ccmp-context',
             'ccmp-expand', 'rlig-expand', 'ccmp-expand3', 'rlig-expand3']
    for name in names:
        path = root/(name+'.ttf')
        order = joining.make(path)
        font = TTFont(path, recalcTimestamp=False)
        old = font['GSUB'].table
        lookups = old.LookupList.Lookup[:]
        features = [(r.FeatureTag, r.Feature.LookupListIndex) for r in old.FeatureList.FeatureRecord]
        if name == 'cmap-absent':
            for table in font['cmap'].tables:
                table.cmap.pop(0x34f, None)
        if name == 'ccmp-cgj':
            features.append(('ccmp', [4]))
            lookups.append(lookup(1, single({cgj: beh+'_isol'})))
        if name == 'ccmp-context':
            features.append(('ccmp', [4]))
            lookups.extend([context([beh, cgj], [(0, 5)]), lookup(1, single({beh: beh+'_isol'}))])
        if 'expand' in name:
            features.append((name[:4], [4]))
            outputs = [beh] * (3 if name.endswith('3') else 2)
            lookups.append(lookup(2, obj('MultipleSubst', mapping={g: outputs for g in [beh]+[beh+'_'+f for f in joining.FORMS]})))
        features.sort()
        font['GSUB'].table = layout('GSUB', features, lookups, {'arab': script(language(range(len(features))))})
        classes = {n: 3 if n in ['u064e', 'u0651'] or name == 'cgj-mark' and n == cgj else 1 for n in order}
        font['GDEF'] = newTable('GDEF')
        font['GDEF'].table = obj('GDEF', Version=0x10000, GlyphClassDef=obj('ClassDef', classDefs=classes),
                                 AttachList=None, LigCaretList=None, MarkAttachClassDef=None)
        font.save(path)
        for sample, scalars in enumerate(sorted(texts)):
            for options, rtl, em in itertools.product(OPTIONS, (0, 1), (1096810496, 1148846080)):
                requests.append(dict(name=name, sample=sample, font=identity(path), scalars=scalars,
                                     options=options, rtl=rtl, em=em))

    def capture(request):
        command = [str(exe), request['font']['path'], ','.join(f'{v:x}' for v in request['scalars']),
                   request['options'], str(request['em']), str(request['rtl']), '0', 'en-US']
        result = subprocess.run(command, capture_output=True, text=True, timeout=30, creationflags=0x08000000)
        assert result.returncode == 0, (command, result.stdout, result.stderr)
        value = json.loads(result.stdout)
        assert value['hresult'] == value['placement_hresult'] == 0
        return {**request, 'result': value}

    with ThreadPoolExecutor(max_workers=8) as pool:
        cases = list(pool.map(capture, requests))
    (root/'observations.json').write_text(json.dumps(dict(reference=identity(exe), glyph_order=order, cases=cases), indent=2))
    print(len(texts), 'texts', len(names), 'fonts', len(cases), 'CGJ observations')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--exe', type=Path, required=True)
    args = parser.parse_args()
    run(args.root.resolve(), args.exe.resolve())
