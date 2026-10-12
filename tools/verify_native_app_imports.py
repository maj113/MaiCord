"""Bounded LLVM PE import audit for the native app and its account module.

Static and delay-load imports; tools/audit_pe.ps1 -DelayLoad checks which DLLs
are delay-loaded. Runtime module observation and feature acceptance are separate.
QA comparison executables intentionally import DirectWrite and should fail here.
"""
from pathlib import Path
import argparse, json, re, shutil, threading
from verify_native_text import run_check, CheckFailure
from verify_hangul_render import identity


def inspect_headers(text):
    if 'file format coff-x86-64' not in text or '(PE32+)' not in text:
        raise ValueError('Expected an AMD64 PE32+ image')
    if not re.search(r'^Entry d ([0-9a-f]+) ([0-9a-f]+) Delay Import Directory$', text, re.M | re.I):
        raise ValueError('Missing delay-import directory entry')
    names = re.findall(r'^\s*DLL Name:\s*(\S+)', text, re.M)
    if not names:
        raise ValueError('No import descriptors were decoded')
    if any('dwrite' in name.lower() for name in names) or re.search(r'\bDWrite(?:Core)?CreateFactory\b', text, re.I):
        raise ValueError('DirectWrite dependency remains')
    return names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact', type=Path, action='append', required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--objdump', default=shutil.which('llvm-objdump') or 'C:/Program Files/LLVM/bin/llvm-objdump.exe')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    result = dict(status='running', scope=__doc__, artifacts=[])
    try:
        for index, path in enumerate(args.artifact):
            path = path.resolve(); before = identity(path)
            row = dict(artifact=before); result['artifacts'].append(row)
            row['process'] = run_check('imports-' + str(index), [args.objdump, '-p', str(path)], root, out, 5, threading.Event())
            row['imports'] = inspect_headers(Path(row['process']['log']).read_text(encoding='utf-8'))
            if identity(path) != before:
                raise RuntimeError('Artifact changed during inspection')
        result['status'] = 'passed'
    except CheckFailure as error:
        result.update(status='failed', process=error.row, error=str(error))
    except Exception as error:
        result.update(status='failed', error=str(error))
    (out / 'imports-result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(result['status'], result.get('error', ''), out / 'imports-result.json')
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
