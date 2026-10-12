"""Bounded Khmer provider disassembly and call traces in a disposable QA child.

Public symbols are cached under the evidence directory. Debugger durations are
not performance measurements. No attachment to the app or persistent target edits.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_native_text import run_check, CheckFailure


def identity(path):
    path = Path(path).resolve()
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--font', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--symbol-cache', type=Path, required=True)
    parser.add_argument('--fetch-symbols', action='store_true')
    parser.add_argument('--text', default='1780,17d2,179a,17be,1780')
    args = parser.parse_args()
    scalars = [int(s, 16) for s in args.text.split(',')]
    if not scalars or len(scalars) > 64 or any(c < 0 or c > 0x10ffff or 0xd800 <= c <= 0xdfff for c in scalars):
        parser.error('Expected 1–64 Unicode scalars in hexadecimal')
    root = args.output_dir.resolve(); root.mkdir(parents=True, exist_ok=True)
    symbols = args.symbol_cache.resolve(); symbols.mkdir(parents=True, exist_ok=True)
    cdb = Path('C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe')
    objdump = Path('C:/Program Files/LLVM/bin/llvm-objdump.exe')
    functions = [('GLYPHS', 'KhmerEngineGetGlyphs'),
                 ('REORDER', 'ShapingLibraryInternal::ShapeKhmerText'),
                 ('POSITIONS', 'KhmerEngineGetGlyphPositions')]
    command = 'sxe ld:TextShaping.dll; g; .reload /f TextShaping.dll; lmvm TextShaping; lmvm DWrite; '
    for label, name in functions:
        command += f'.echo DISASM_{label}_BEGIN; uf TextShaping!{name}; .echo DISASM_{label}_END; '
    points = functions + [('MAP_SUBSTITUTE', 'SubstituteOtlChars'),
                         ('GLYPH_SUBSTITUTE', 'ShapingLibraryInternal::SubstituteOtlGlyphsWithFallback'),
                         ('POSITION_LOOKUPS', 'RePositionOtlGlyphs')]
    for label, name in points:
        command += f'bp TextShaping!{name} ".echo HIT_{label}; kb 5; gc"; '
    command += 'g; q\n'
    commands = root/'commands.txt'; commands.write_text(command, encoding='ascii')
    sympath = 'srv*'+str(symbols)+('*https://msdl.microsoft.com/download/symbols' if args.fetch_symbols else '')
    executable = args.reference.resolve(); font = args.font.resolve()
    rows = []; result = dict(status='running', scope=__doc__, scalars=scalars,
        reference=identity(executable), font=identity(font), debugger=identity(cdb), objdump=identity(objdump),
        modules=[identity('C:/Windows/System32/'+name+'.dll') for name in ('DWrite', 'TextShaping')])
    (root/'result.json').write_text(json.dumps(result, indent=2))
    try:
        rows.append(run_check('provider-cdb', [str(cdb), '-y', sympath, '-cf', str(commands), str(executable),
            str(font), args.text, 'default', '1148846080', '0', '0', 'en-US'], Path.cwd(), root,
            12 if args.fetch_symbols else 5, threading.Event()))
        log = (root/'provider-cdb.log').read_text(errors='replace')
        for label, _ in points:
            if '\nHIT_'+label+'\n' not in log:
                raise RuntimeError('Missing runtime breakpoint: '+label)
        module = re.search(r'^([0-9a-f]+`[0-9a-f]+)\s+[0-9a-f`]+\s+TextShaping\s+\(pdb symbols\)', log, re.M)
        if not module: raise RuntimeError('TextShaping public PDB was not loaded')
        loaded_base = int(module[1].replace('`', ''), 16)
        rows.append(run_check('provider-headers', [str(objdump), '-p', 'C:/Windows/System32/TextShaping.dll'],
                              Path.cwd(), root, 5, threading.Event()))
        headers = (root/'provider-headers.log').read_text()
        preferred_base = int(re.search(r'^ImageBase\s+([0-9a-fA-F]+)', headers, re.M)[1], 16)
        result['ranges'] = []
        for label, _ in functions:
            body = log.split('\nDISASM_'+label+'_BEGIN\n', 1)[1].split('\nDISASM_'+label+'_END\n', 1)[0]
            addresses = [int(x.replace('`', ''), 16) for x in re.findall(r'^([0-9a-f]+`[0-9a-f]+)\s+[0-9a-f]+\s+', body, re.M)]
            if not addresses: raise RuntimeError('Missing disassembly: '+label)
            start, end = min(addresses)-loaded_base, max(addresses)-loaded_base+16
            if not 0 <= start < end < 0x1000000: raise RuntimeError('Invalid provider RVA range')
            result['ranges'].append(dict(label=label, start_rva=hex(start), stop_rva=hex(end)))
            rows.append(run_check('llvm-'+label.lower(), [str(objdump), '-d',
                '--start-address='+hex(preferred_base+start), '--stop-address='+hex(preferred_base+end),
                'C:/Windows/System32/TextShaping.dll'], Path.cwd(), root, 5, threading.Event()))
        result['public_symbols'] = [identity(p) for p in symbols.glob('**/*.pdb') if p.is_file() and p.name.lower() in ('dwrite.pdb', 'textshaping.pdb')]
        result['status'] = 'passed'
    except CheckFailure as error:
        rows.append(error.row); result['status'] = error.row['status']
    except Exception as error:
        result['status'] = 'failed'; result['error'] = str(error)
    result['steps'] = rows
    result['artifacts'] = [identity(p) for p in sorted(root.iterdir()) if p.suffix in ('.log', '.txt')]
    (root/'result.json').write_text(json.dumps(result, indent=2))
    print(result['status'], result.get('error', ''))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
