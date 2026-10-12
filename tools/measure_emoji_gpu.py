"""Bounded real-picker GPU timeline and memory observations; one build side at a time.

The phase inherits earlier UI-probe atlas contents. It is neither an empty-atlas
benchmark nor hardware busy time. DXGI samples describe the whole probe process;
resource payload sizes are not driver allocation sizes or physical residency.
--page-control adds an explicit flush or GPU wait after every page. Submission
CPU samples exclude this diagnostic overhead, which is reported separately.
The present mode uses the normal renderer Present path instead of the fixture's
resize path. Its CPU samples include any renderer frame-latency wait.
"""
import argparse
import json
import math
import os
import re
from pathlib import Path
import statistics
import struct
import threading
from benchmark_native_text_performance import identity
from verify_native_text import run_check, CheckFailure

FIELDS = ('cpu_ticks thread_cycles gpu_ticks local_usage nonlocal_usage local_budget '
          'working_set private_bytes textures buffers builds mono_dispatches '
          'color_dispatches mono_pixels mono_commands row').split()
BATCH_FIELDS = ('color_submissions','color_mask_submissions','color_canceled_jobs')

def decode(path, side, require_color=True):
    data = path.read_bytes()
    if len(data) < 224 or data[:8] not in (b'MCETGPU1',b'MCETGPU2'):
        raise ValueError('Missing measurement header')
    version=2 if data[:8]==b'MCETGPU2' else 1
    fields=FIELDS+list(BATCH_FIELDS) if version==2 else FIELDS
    h = struct.unpack_from('<12Q', data)
    _, cpu_frequency, gpu_frequency, count, stride, vendor, device, luid, width, height, dpi, flags = h
    if not cpu_frequency or not gpu_frequency or not 0 < count <= 512 or stride != len(fields)*8 or len(data) != 96+(count+1)*stride:
        raise ValueError('Invalid measurement dimensions')
    if flags & ~56 != (3 if side == 'native' else 0) or flags >> 4 > 3:
        raise ValueError('Unexpected renderer or native ownership')
    rows = [dict(zip(fields, struct.unpack_from('<'+str(len(fields))+'Q', data, 96+i*stride))) for i in range(count+1)]
    if version==2 and not flags&8 and any(r[k] for r in rows for k in BATCH_FIELDS):raise ValueError('Batch counters in an immediate build')
    baseline, pages = rows[0], rows[1:]
    if any(a['row'] >= b['row'] for a,b in zip(pages,pages[1:])) or not any(p['color_dispatches'] or (not require_color and p['mono_dispatches']) for p in pages):
        raise ValueError('Incomplete or empty color-glyph sweep')
    for row in pages:
        row['cpu_us'] = row['cpu_ticks']*1e6/cpu_frequency
        row['gpu_interval_us'] = row['gpu_ticks']*1e6/gpu_frequency
    def summary(key):
        values = [p[key] for p in pages]
        return dict(total=sum(values), median=statistics.median(values),
                    p95=sorted(values)[math.ceil(.95*len(values))-1], maximum=max(values))
    return dict(adapter=dict(vendor=vendor,device=device,luid=luid),viewport=[width,height,dpi],
                cpu_frequency=cpu_frequency,gpu_frequency=gpu_frequency,flags=flags,schema=version,batch_enabled=bool(flags&8),page_control=('queued','flush','wait','present')[flags>>4],baseline=baseline,pages=pages,
                cpu_us=summary('cpu_us'),gpu_interval_us=summary('gpu_interval_us'),
                totals={k:sum(p[k] for p in pages) for k in ('thread_cycles','builds','mono_dispatches','color_dispatches','mono_pixels','mono_commands')},
                maxima={k:max(p[k] for p in rows) for k in ('local_usage','nonlocal_usage','working_set','private_bytes','textures','buffers')})

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--side',choices=('native','reference'),required=True)
    parser.add_argument('--exe',type=Path,default=Path('D:/MaiCord-build/Release/MaiCordUiProbe.exe'))
    parser.add_argument('--runs',type=int,default=3)
    parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--page-control',choices=('queued','flush','wait','present'),default='queued')
    args=parser.parse_args()
    if not 1<=args.runs<=5:parser.error('Use 1-5 bounded processes')
    root=Path(__file__).resolve().parents[1];out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    destination=out/'measurement.json'
    if destination.exists():parser.error('Choose a new evidence directory')
    exe=args.exe.resolve();binary=identity(exe);runs=[]
    report=dict(status='running',side=args.side,page_control=args.page_control,executable=binary,scope=__doc__,runs=runs,
                source=[identity(root/p) for p in ('tests/integration/ui_probe.cpp','tests/integration/emoji_gpu_measurement.hpp','src/render/d3d11_glyph_text.cpp','include/maicord/render/d3d11_glyph_text.hpp')])
    report['implementation']=[identity(root/p) for p in ('src/render/colr_paint.cpp','src/render/colr_paint_batch.inc','include/maicord/render/colr_paint.hpp','src/render/d3d11_renderer_text.cpp','src/render/colr_program.cpp','include/maicord/render/colr_program.hpp','src/render/shaders/colr_paint.hlsl')]
    def save():destination.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    try:
        for i in range(args.runs):
            data=out/f'data-{i}';data.mkdir();metrics=out/f'pages-{i}.bin'
            env={k:v for k,v in os.environ.items() if not k.startswith('MAICORD_')}
            env.update(LOCALAPPDATA=str(data),MAICORD_UI_PROBE_ONCE='1',MAICORD_GPU_METRICS='1',MAICORD_EMOJI_GPU_METRICS=str(metrics))
            if args.side=='native':env['MAICORD_NATIVE_BOOTSTRAP_PROBE']='1'
            if args.page_control!='queued':env['MAICORD_EMOJI_PAGE_CONTROL']=str(('queued','flush','wait','present').index(args.page_control))
            entry=dict(run=i);runs.append(entry)
            try:entry['process']=run_check('sweep-'+str(i),[str(exe)],root,out,5,threading.Event(),env=env)
            except CheckFailure as error:entry['process']=error.row;raise
            log=data/'MaiCord/logs/maicord.log';raw=log.read_text(encoding='utf-8')
            if 'application_renderer_release_passed' not in raw or 'emoji_gpu_failure_line=' in raw:
                raise ValueError('Incomplete probe or GPU observations')
            entry.update(measurement=decode(metrics,args.side),binary=identity(metrics),log=identity(log))
            if entry['measurement']['page_control']!=args.page_control:raise ValueError('Requested page control was not applied')
            if args.page_control!='queued':
                values=[re.findall(r'\bemoji_page_control_'+name+r'=(\d+)',raw) for name in ('count','us')]
                if any(len(v)!=1 for v in values) or int(values[0][0])!=len(entry['measurement']['pages']):raise ValueError('Incomplete page-control accounting')
                entry['page_control_us']=int(values[1][0])
            names=('emoji_batch_submissions','emoji_batch_mask_submissions','emoji_batch_canceled_jobs')
            found=[re.findall(r'\b'+name+r'=(\d+)',raw) for name in names]
            if any(found):
                if any(len(values)!=1 for values in found):raise ValueError('Incomplete batch counters')
                entry['batch']={name:int(values[0]) for name,values in zip(names,found)}
                if entry['measurement']['schema']==2:
                    for name,key in zip(names,BATCH_FIELDS):
                        if entry['batch'][name]!=sum(p[key] for p in entry['measurement']['pages']):raise ValueError('Batch totals disagree with page records')
            if entry['measurement']['batch_enabled'] and 'batch' not in entry:raise ValueError('Missing batch counters')
            if identity(exe)!=binary:raise ValueError('Executable changed during measurement')
            save()
        report['status']='passed'
    except Exception as error:report.update(status='failed',error=str(error));raise
    finally:save()
    print(args.side,report['status'],len(runs),'processes;',destination)

if __name__=='__main__':main()
