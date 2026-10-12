"""Fresh-process empty-atlas and bounded repeated-render observations.

Initialization includes the renderer/device. First draw includes text discovery,
shaping, atlas generation and drawing. Reset preserves font/GPU owners. Warm
shape/draw and retained-draw phases each issue 64 calls without Present. Device
intervals include CPU feed gaps; no OS-cold, GPU busy-time or display-latency claim.
Optional prewarm completes 4096 retained draws before each warm/reset phase,
outside its timing interval; first draw remains unchanged.
"""
import argparse,json,os,re,threading
from pathlib import Path
from benchmark_native_text_performance import identity
from verify_native_text import run_check,CheckFailure
from measure_emoji_gpu import decode

PHASES=('first_draw','warm_shape_draw','reset_draw','rewarm_shape_draw','retained_draw')
REPEATS=(1,64,1,64,64)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--side',choices=('native','reference'),required=True)
    p.add_argument('--exe',type=Path,default=Path('D:/MaiCord-build/Release/MaiCordGlyphCacheProbe.exe'))
    p.add_argument('--output-dir',type=Path,required=True)
    p.add_argument('--runs',type=int,default=3)
    p.add_argument('--capture',action='store_true')
    p.add_argument('--fixture',choices=('mixed','mono','color'),default='mixed')
    p.add_argument('--prewarm',action='store_true',help='Diagnostic: complete 4096 retained draws before each warm/reset phase; excludes first draw.')
    args=p.parse_args()
    if not 1<=args.runs<=5:p.error('Use one to five bounded workers')
    root=Path(__file__).resolve().parents[1];out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=True)
    report_path=out/'measurement.json'
    if report_path.exists():p.error('Preserve existing evidence')
    exe=args.exe.resolve();binary=identity(exe)
    report=dict(scope=__doc__,side=args.side,fixture=args.fixture,prewarm=args.prewarm,status='running',executable=binary,runs=[],source=[identity(root/f) for f in ['tests/integration/glyph_cache_probe.cpp','tests/integration/ui_probe.cpp','tests/integration/glyph_cache_measurement.hpp','tests/integration/emoji_gpu_measurement.hpp','tools/measure_glyph_cache.py','tools/measure_emoji_gpu.py']])
    def save():report_path.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    save()
    try:
        for i in range(args.runs):
            data=out/f'data-{i}';data.mkdir();metrics=out/f'phases-{i}.bin'
            env={k:v for k,v in os.environ.items() if not k.startswith('MAICORD_')}
            env.update(LOCALAPPDATA=str(data),MAICORD_UI_PROBE_ONCE='1',MAICORD_GPU_METRICS='1',MAICORD_GLYPH_CACHE_METRICS=str(metrics))
            env['MAICORD_GLYPH_CACHE_KIND']=str(('mixed','mono','color').index(args.fixture))
            if args.prewarm:env['MAICORD_GLYPH_CACHE_PREWARM']='1'
            if args.side=='native':env['MAICORD_NATIVE_BOOTSTRAP_PROBE']='1'
            if args.capture:
                captures=out/f'captures-{i}';captures.mkdir();env['MAICORD_UI_PROBE_CAPTURE_DIR']=str(captures)
            entry=dict(index=i);report['runs'].append(entry)
            try:entry['process']=run_check('glyph-cache-'+str(i),[str(exe)],root,out,5,threading.Event(),env=env)
            except CheckFailure as error:entry['process']=error.row;raise
            log=data/'MaiCord/logs/maicord.log';raw=log.read_text(encoding='utf-8')
            if 'glyph_cache_passed' not in raw:raise ValueError('Incomplete glyph cache probe')
            fixture=re.findall(r'\bglyph_fixture=(\d+)',raw)
            if fixture!=[env['MAICORD_GLYPH_CACHE_KIND']] and not (args.fixture=='mixed' and not fixture):raise ValueError('Missing/mismatched glyph fixture')
            prewarm=re.findall(r'\bglyph_prewarm_draws=(\d+)',raw)
            if prewarm!=['16384' if args.prewarm else '0'] and not (not args.prewarm and not prewarm):raise ValueError('Missing/mismatched GPU prewarm')
            m=decode(metrics,args.side,require_color=args.fixture!='mono')
            if [row['row'] for row in m['pages']]!=list(range(5)) or any(m['baseline'][k] for k in ['builds','mono_dispatches','color_dispatches']):raise ValueError('Missing phases or nonempty atlas')
            phases={}
            for j,(name,repeats,row) in enumerate(zip(PHASES,REPEATS,m['pages'])):
                if j in (0,2):
                    if not row['builds'] or (args.fixture!='color' and not row['mono_dispatches']) or (args.fixture!='mono' and not row['color_dispatches']):raise ValueError('Missing glyph generation')
                    if (args.fixture=='color' and row['mono_dispatches']) or (args.fixture=='mono' and row['color_dispatches']):raise ValueError('Mixed glyph generation in isolated fixture')
                elif any(row[k] for k in ['builds','mono_dispatches','color_dispatches','color_canceled_jobs']):raise ValueError('Warm atlas regenerated glyphs')
                phases[name]=dict(repeats=repeats,cpu_us_per_call=row['cpu_us']/repeats,device_us_per_call=row['gpu_interval_us']/repeats,cycles_per_call=row['thread_cycles']/repeats,raw=row)
            values={}
            keys=['renderer_init_us','renderer_init_cycles','geometry_hash','glyphs','clusters','quads']
            for key in keys:
                matches=re.findall(r'\b'+key+r'=(\d+)',raw)
                if len(matches)!=1:raise ValueError('Missing/ambiguous '+key)
                values[key]=int(matches[0])
            cold_keys=['cold_shape_ticks','cold_shape_cycles','cold_append_ticks','cold_append_cycles']
            quad_hash=re.findall(r'\bquad_hash=(\d+)',raw)
            if quad_hash:
                if len(quad_hash)!=1:raise ValueError('Ambiguous quad hash')
                values['quad_hash']=int(quad_hash[0])
            if any(key+'=' in raw for key in cold_keys):
                for key in cold_keys:
                    matches=re.findall(r'\b'+key+r'=(\d+)',raw)
                    if len(matches)!=1:raise ValueError('Incomplete cold-phase attribution')
                    values[key]=int(matches[0])
            if 'mono_submissions=' in raw:
                for key in ['mono_rasters','mono_submissions']:
                    matches=re.findall(r'\b'+key+r'=(\d+)',raw)
                    if len(matches)!=1:raise ValueError('Incomplete monochrome submission counters')
                    values[key]=int(matches[0])
            if args.capture:
                images=list(captures.glob('*.bmp'))
                if len(images)!=5 or len({identity(x)['sha256'] for x in images})!=1:raise ValueError('Cold/warm/reset images differ')
                entry['captures']=[identity(x) for x in images]
            if identity(exe)!=binary:raise ValueError('Executable changed')
            save()
        report['status']='passed'
    except Exception as e:report.update(status='failed',error=str(e));raise
    finally:save()
    print(report_path)

if __name__=='__main__':main()
