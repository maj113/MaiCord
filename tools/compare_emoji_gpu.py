"""Compare bounded picker sweeps with matching sources, device, viewport and work counters."""
import argparse
import json
from pathlib import Path
import statistics
from benchmark_native_text_performance import identity

WORK=('row','builds','mono_dispatches','color_dispatches','mono_pixels','mono_commands')

def summarize(report):
    samples=[r['measurement'] for r in report['runs']]
    return dict(runs=len(samples),pages=len(samples[0]['pages']),
        **{key:{field:statistics.median(m[key][field] for m in samples) for field in ('total','median','p95','maximum')} for key in ('cpu_us','gpu_interval_us')},
        totals={key:statistics.median(m['totals'][key] for m in samples) for key in samples[0]['totals']},
        maxima={key:statistics.median(m['maxima'][key] for m in samples) for key in samples[0]['maxima']},
        baseline={key:statistics.median(m['baseline'][key] for m in samples) for key in samples[0]['baseline']})

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native',type=Path,action='append',required=True)
    parser.add_argument('--reference',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--credit-canceled-jobs',action='store_true',help='Require equal color attempts (submitted plus canceled) on every page')
    args=parser.parse_args()
    if args.output.exists():parser.error('Preserve existing comparisons')
    files=args.native+[args.reference];reports=[json.loads(p.read_text()) for p in files]
    anchor=reports[-1];first=anchor['runs'][0]['measurement']
    if anchor['side']!='reference':raise ValueError('Reference side required')
    def signature_of(m):
        if args.credit_canceled_jobs and m.get('schema',1)!=2:raise ValueError('Cancellation credit requires version 2 page records')
        return [tuple(p[k]+(p.get('color_canceled_jobs',0) if k=='color_dispatches' and args.credit_canceled_jobs else 0) for k in WORK) for p in m['pages']]
    signature=signature_of(first)
    for i,report in enumerate(reports):
        if report['status']!='passed' or report['source']!=anchor['source'] or report['side']!=('reference' if i==len(reports)-1 else 'native'):
            raise ValueError('Unmatched source or failed side')
        for run in report['runs']:
            m=run['measurement']
            if m['adapter']!=first['adapter'] or m['viewport']!=first['viewport'] or signature_of(m)!=signature:
                raise ValueError('Device, viewport or actual page work differs')
            if identity(Path(run['binary']['path']))!=run['binary']:raise ValueError('Measurement binary changed')
    result=dict(status='matched-work-observations',adapter=first['adapter'],viewport=first['viewport'],
                canceled_jobs_credited=args.credit_canceled_jobs,
                inputs=[identity(p) for p in files],native=[summarize(r) for r in reports[:-1]],reference=summarize(anchor),
                scope='Sequential build-side groups with native repeats. CPU durations exclude memory sampling; GPU intervals include atlas generation, drawing and submission gaps. Same recorded work is not a new pixel comparison. Sampled memory is whole-process, not isolated text residency. P95 is a finite 72-page fixture statistic, not interactive tail-latency acceptance.'+
                      (' Color submissions plus explicitly counted cancellations must match at every page.' if args.credit_canceled_jobs else ''))
    args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(result['status'],len(signature),'pages;',args.output)

if __name__=='__main__':main()
