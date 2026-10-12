"""Audit picker tails by matching page identity; retain the existing p95 definition.

Reads saved observations only. It does not run the app or infer blocked time from
thread cycles. p95 is a finite-fixture statistic, not interactive tail acceptance.
"""
import argparse
import collections
import json
import math
from pathlib import Path
import statistics
from benchmark_native_text_performance import identity
from measure_emoji_gpu import decode

WORK = ('row', 'builds', 'mono_dispatches', 'color_dispatches', 'mono_pixels', 'mono_commands')
METRICS = ('cpu_us', 'thread_cycles', 'gpu_interval_us')


def p95(values):
    return sorted(values)[math.ceil(.95 * len(values)) - 1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--group', nargs=2, action='append', metavar=('LABEL', 'REPORT'), required=True)
    parser.add_argument('--credit-canceled-jobs', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 2 <= len(args.group) <= 8 or len({x[0] for x in args.group}) != len(args.group):
        parser.error('Use 2-8 uniquely named groups')
    if args.output.exists():
        parser.error('Preserve prior evidence; choose a new output')
    reports = [(label, Path(path), json.loads(Path(path).read_text(encoding='utf-8'))) for label, path in args.group]
    anchor = reports[0][2]
    if not anchor.get('runs'):
        raise ValueError('Empty anchor group')
    first = anchor['runs'][0]['measurement']

    def work(m):
        if args.credit_canceled_jobs and m.get('schema') != 2:
            raise ValueError('Cancellation credit requires schema 2')
        return [tuple(row[k] + (row.get('color_canceled_jobs', 0) if k == 'color_dispatches' and args.credit_canceled_jobs else 0)
                      for k in WORK) for row in m['pages']]

    signature = work(first)
    result = dict(status='matched-page-tail-audit', scope=__doc__, inputs=[], groups=[],
                  canceled_jobs_credited=args.credit_canceled_jobs)
    for label, path, report in reports:
        if report.get('status') != 'passed' or report.get('side') not in ('native', 'reference') or report['source'] != anchor['source'] or not report['runs']:
            raise ValueError('Unmatched source, failed or empty group')
        samples = []
        for run in report['runs']:
            if identity(Path(run['binary']['path'])) != run['binary']:
                raise ValueError('Recorded measurement binary changed')
            m = decode(Path(run['binary']['path']), report['side'])
            if m != run['measurement'] or m['adapter'] != first['adapter'] or m['viewport'] != first['viewport'] or work(m) != signature:
                raise ValueError('Decoded observations, device, viewport or page work differ')
            samples.append(m)
        group = dict(label=label, runs=len(samples), pages=[], per_run=[])
        for m in samples:
            ordered = sorted(m['pages'], key=lambda x: (x['cpu_us'], x['row']))
            threshold = ordered[math.ceil(.95 * len(ordered)) - 1]
            group['per_run'].append(dict(cpu_p95_us=threshold['cpu_us'], threshold_row=threshold['row'],
                                        tail_rows=[x['row'] for x in ordered if x['cpu_us'] >= threshold['cpu_us']]))
        members = collections.Counter(row for run in group['per_run'] for row in run['tail_rows'])
        for i, page in enumerate(first['pages']):
            row = dict(row=page['row'], tail_membership=members[page['row']])
            for metric in METRICS:
                values = [m['pages'][i][metric] for m in samples]
                row[metric] = dict(median=statistics.median(values), minimum=min(values), maximum=max(values))
            group['pages'].append(row)
        group['median_of_run_cpu_p95_us'] = statistics.median(x['cpu_p95_us'] for x in group['per_run'])
        group['p95_of_page_cpu_medians_us'] = p95([x['cpu_us']['median'] for x in group['pages']])
        if result['groups']:
            base = result['groups'][0]
            group['deltas_to_first_group'] = [dict(row=y['row'],
                cpu_us=y['cpu_us']['median'] - x['cpu_us']['median'],
                thread_cycle_percent=100 * (y['thread_cycles']['median'] / x['thread_cycles']['median'] - 1) if x['thread_cycles']['median'] else None,
                gpu_interval_us=y['gpu_interval_us']['median'] - x['gpu_interval_us']['median'])
                for x, y in zip(base['pages'], group['pages'])]
        result['inputs'].append(dict(label=label, report=identity(path)))
        result['groups'].append(group)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(result['status'], len(signature), 'pages;', args.output)


if __name__ == '__main__':
    main()
