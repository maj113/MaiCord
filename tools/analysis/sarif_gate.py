"""Gate releases on new static-analysis (CodeQL) SARIF findings.

check: fails on gated findings that the baseline does not list. A finding is gated
when its rule is in --rules or its security severity is at least --min-severity.
Baseline entries are rule|path|message (no line numbers, which move with edits);
--write-baseline records the current gated findings.
"""
import argparse
import json
import sys
from pathlib import Path
from urllib.parse import unquote, urlparse


def sarif_files(inputs):
    for item in inputs:
        path = Path(item)
        yield from sorted(path.rglob('*.sarif')) if path.is_dir() else [path]


def relative_uri(run, location, root):
    artifact = location.get('physicalLocation', {}).get('artifactLocation', {})
    if 'uri' not in artifact and 'index' in artifact:  # An index into the run's artifacts.
        artifact = run.get('artifacts', [])[artifact['index']].get('location', {})
    uri = artifact.get('uri', '')
    if artifact.get('uriBaseId') or not uri:
        return uri.replace('\\', '/')
    parsed = urlparse(uri)
    path = Path(unquote(parsed.path).lstrip('/') if parsed.scheme == 'file' else uri)
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        return None  # Outside the repository.


def rule_table(run):
    rules = {}
    driver = run.get('tool', {}).get('driver', {})
    for component in [driver] + run.get('tool', {}).get('extensions', []):
        for rule in component.get('rules', []):
            rules.setdefault(rule.get('id'), rule)
    return rules


def findings(files, root, ignore):
    """Yields (run, rules, result, path) for every kept result, deduplicated."""
    seen = set()
    for file in files:
        for run in json.loads(file.read_text(encoding='utf-8-sig')).get('runs', []):
            rules = rule_table(run)
            for result in run.get('results', []):
                if result.get('ruleId') in ignore or not result.get('locations'):
                    continue
                path = relative_uri(run, result['locations'][0], root)
                if path is None:
                    continue
                region = result['locations'][0].get('physicalLocation', {}).get('region', {})
                key = (result.get('ruleId'), path, region.get('startLine'), message(result))
                if key not in seen:
                    seen.add(key)
                    yield run, rules, result, path


def message(result):
    return ' '.join(result.get('message', {}).get('text', '').split())


def fingerprint(result, path):
    return f"{result.get('ruleId')}|{path}|{message(result)}"


def check(args, root):
    gated = {}
    for _, rules, result, path in findings(sarif_files(args.input), root, set(args.ignore)):
        severity = rules.get(result.get('ruleId'), {}).get('properties', {}).get('security-severity')
        if result.get('ruleId') in args.rules or (
                args.min_severity is not None and severity is not None and float(severity) >= args.min_severity):
            region = result['locations'][0].get('physicalLocation', {}).get('region', {})
            gated.setdefault(fingerprint(result, path), (path, region.get('startLine', 1), message(result)))
    baseline_path = Path(args.baseline)
    if args.write_baseline:
        baseline_path.write_text(json.dumps(sorted(gated), indent=1) + '\n', encoding='utf-8')
        print(f'{len(gated)} gated findings recorded in {baseline_path}')
        return 0
    baseline = set(json.loads(baseline_path.read_text(encoding='utf-8'))) if baseline_path.exists() else set()
    new = [key for key in gated if key not in baseline]
    for key in new:
        path, line, text = gated[key]
        print(f'::error file={path},line={line}::{key.split("|")[0]}: {text}')
    print(f'{len(gated)} gated findings, {len(gated) - len(new)} in the baseline, {len(new)} new')
    return 1 if new else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    gate = commands.add_parser('check')
    gate.add_argument('--input', nargs='+', required=True)
    gate.add_argument('--ignore', nargs='*', default=[])
    gate.add_argument('--root', default='.')
    gate.add_argument('--baseline', required=True)
    gate.add_argument('--rules', nargs='*', default=[])
    gate.add_argument('--min-severity', type=float)
    gate.add_argument('--write-baseline', action='store_true')
    args = parser.parse_args()
    return check(args, Path(args.root).resolve())


if __name__ == '__main__':
    sys.exit(main())
