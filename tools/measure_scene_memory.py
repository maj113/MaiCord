"""Bounded whole-process memory snapshots at existing UI scene checkpoints.

Default runs add no capture images or GPU waits, and make no timing claim.
The explicit --drain-picker diagnostic waits once, at most 250 ms, for preceding
immediate-context commands and records committed private mapping categories.
The default full fixture includes prior test activity; it is not a clean app
startup or a text-only allocation measurement. Each UI process has a 5 s cap.
--font-residency queries resident pages of mapped .ttf/.ttc/.otf/.otc files
without reading their contents. This diagnostic changes the sampling schedule;
it excludes anonymous font caches and files with other names.
Two native-only atlas oracle checks run in ordinary integration, outside this
paired fixture. The remaining recorded scene and atlas-work sequence must match.
"""
import argparse
import json
import os
import re
from pathlib import Path
import statistics
import struct
import threading

from benchmark_native_text_performance import identity
from verify_native_text import run_check, CheckFailure

FIELDS = ('width height dpi local_usage nonlocal_usage local_budget working_set '
          'private_bytes textures buffers builds mono_dispatches color_dispatches '
          'mono_pixels mono_commands title_quads').split()
BATCH_FIELDS = ('color_submissions', 'color_mask_submissions', 'color_canceled_jobs', 'color_batching')
WORK = ('width', 'height', 'dpi', 'builds', 'mono_dispatches', 'color_dispatches',
        'mono_pixels', 'mono_commands', 'title_quads')
MEMORY = ('local_usage', 'nonlocal_usage', 'working_set', 'private_bytes', 'textures', 'buffers')
SOURCES = ('tests/integration/ui_probe.cpp', 'tests/integration/scene_memory_measurement.hpp',
           'tests/integration/emoji_gpu_measurement.hpp','tests/integration/colr_binding_probe.hpp',
           'tests/integration/probe_present.hpp', 'src/render/d3d11_glyph_text.cpp',
           'include/maicord/render/d3d11_glyph_text.hpp')


def decode(path, side):
    data = path.read_bytes()
    if len(data) < 64 or data[:8] not in (b'MCMSCN01',b'MCMSCN02'):
        raise ValueError('Missing scene memory header')
    version=2 if data[:8]==b'MCMSCN02' else 1
    fields=FIELDS+list(BATCH_FIELDS) if version==2 else FIELDS
    _, stride, flags, vendor, device, luid, count, reserved = struct.unpack_from('<8Q', data)
    if stride != 128+len(fields)*8 or flags & ~8 != (3 if side == 'native' else 0) or reserved or not count or len(data) != 64 + count * stride:
        raise ValueError('Invalid scene dimensions or provider ownership')
    scenes = []
    for offset in range(64, len(data), stride):
        name = data[offset:offset+128].decode('utf-16-le').split('\0', 1)[0]
        if not name:
            raise ValueError('Empty scene name')
        scene=dict(name=name, **dict(zip(fields, struct.unpack_from('<'+str(len(fields))+'Q', data, offset+128))))
        if version==2 and (scene['color_batching'] not in (0,1) or (not scene['color_batching'] and any(scene[k] for k in BATCH_FIELDS[:-1]))):
            raise ValueError('Invalid batch accounting')
        scenes.append(scene)
    if scenes[0]['name'] != 'renderer-initialized' or scenes[-1]['name'] != 'probe-scope-end':
        raise ValueError('Incomplete scene sequence')
    return dict(adapter=dict(vendor=vendor, device=device, luid=luid), flags=flags, schema=version, batch_enabled=bool(flags&8), scenes=scenes,
                maxima={key: max(scene[key] for scene in scenes) for key in MEMORY})


def measure(args):
    if args.drain_picker and args.fixture != 'full':
        raise ValueError('The completion diagnostic requires the full picker fixture')
    root = Path(__file__).resolve().parents[1]
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    destination = out / 'measurement.json'
    if destination.exists():
        raise ValueError('Choose a new evidence directory')
    exe = args.exe.resolve()
    report = dict(status='running', side=args.side, fixture=args.fixture, drain_picker=args.drain_picker, font_residency=args.font_residency, executable=identity(exe),
                  source=[identity(root / p) for p in SOURCES], scope=__doc__, runs=[])
    try:
        for i in range(args.runs):
            data = out / f'data-{i}'
            data.mkdir()
            metrics = out / f'scenes-{i}.bin'
            env = {k: v for k, v in os.environ.items() if not k.startswith('MAICORD_')}
            env.update(LOCALAPPDATA=str(data), MAICORD_UI_PROBE_ONCE='1', MAICORD_GPU_METRICS='1',
                       MAICORD_SCENE_MEMORY=str(metrics))
            if args.side == 'native':
                env['MAICORD_NATIVE_BOOTSTRAP_PROBE'] = '1'
            if args.drain_picker:
                env['MAICORD_SCENE_DRAIN_PICKER'] = '1'
            if args.font_residency:
                env['MAICORD_SCENE_FONT_RESIDENCY'] = '1'
            if args.fixture == 'extended':
                env['MAICORD_EXTENDED_STANDARD_RENDER_PROBE'] = '1'
            entry = dict(run=i)
            report['runs'].append(entry)
            try:
                entry['process'] = run_check('scenes-'+str(i), [str(exe)], root, out, 5, threading.Event(), env=env)
            except CheckFailure as error:
                entry['process'] = error.row
                raise
            log = data / 'MaiCord/logs/maicord.log'
            raw = log.read_text(encoding='utf-8')
            marker = 'extended_standard_render_probe_passed' if args.fixture == 'extended' else 'application_renderer_release_passed'
            if marker not in raw or 'scene_memory_passed' not in raw or 'scene_memory_failed' in raw or 'session_has_live_handles' in raw:
                raise ValueError('Incomplete probe or memory snapshots')
            entry.update(measurement=decode(metrics, args.side), binary=identity(metrics), log=identity(log))
            if args.font_residency:
                fields = ('mapped', 'resident', 'private_resident')
                values = {key: [int(v) for v in re.findall(r'\bfont_file_'+key+r'_bytes=(\d+)', raw)] for key in fields}
                scenes = entry['measurement']['scenes']
                if any(len(v) != len(scenes) for v in values.values()):
                    raise ValueError('Incomplete font-file residency snapshots')
                entry['font_pages'] = [dict(name=s['name'], **{key: values[key][i] for key in fields}) for i, s in enumerate(scenes)]
                if any(not 0 <= v['private_resident'] <= v['resident'] <= v['mapped'] for v in entry['font_pages']):
                    raise ValueError('Invalid font-file residency counts')
                detail = re.findall(r'font_page_file=([^\r\n]+).*?font_page_mapping_bytes=(\d+).*?font_page_resident_bytes=(\d+).*?font_page_private_bytes=(\d+)', raw, re.S)
                entry['font_details'] = [dict(file=name, mapped=int(mapped), resident=int(resident), private_resident=int(private)) for name, mapped, resident, private in detail]
                if args.fixture == 'full':
                    end = next(v for v in entry['font_pages'] if v['name'] == 'font-16.bmp')
                    if any(sum(v[k] for v in entry['font_details']) != end[k] for k in fields):
                        raise ValueError('Font detail totals differ from their scene snapshot')
            if args.drain_picker:
                names = [f'picker_{stage}_{kind}_bytes' for stage in ('begin', 'end', 'complete')
                         for kind in ('wc', 'other_private')] + ['picker_context_completion_us']
                entry['completion'] = {}
                for name in names:
                    found = re.findall(r'\b'+name+r'=(\d+)', raw)
                    if len(found) != 1:
                        raise ValueError('Incomplete completion diagnostic: '+name)
                    entry['completion'][name] = int(found[0])
                if sum(s['name'] == 'picker-context-complete' for s in entry['measurement']['scenes']) != 1:
                    raise ValueError('Missing context-complete snapshot')
            if identity(exe) != report['executable']:
                raise ValueError('Executable changed during measurement')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        destination.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(args.side, args.fixture, 'passed;', destination)


def compare(args):
    files = args.native + [args.reference]
    reports = [json.loads(p.read_text(encoding='utf-8')) for p in files]
    anchor = reports[-1]
    first = anchor['runs'][0]['measurement']
    def signature_of(measurement):
        if args.credit_canceled_jobs and measurement.get('schema',1)!=2:
            raise ValueError('Cancellation credit requires version 2 counters')
        return [(s['name'], *(s[k]+(s.get('color_canceled_jobs',0) if k=='color_dispatches' and args.credit_canceled_jobs else 0) for k in WORK)) for s in measurement['scenes']]
    signature=signature_of(first)
    summaries = []
    for i, report in enumerate(reports):
        if report['status'] != 'passed' or report['source'] != anchor['source'] or report['fixture'] != anchor['fixture'] or report['side'] != ('reference' if i == len(reports)-1 else 'native'):
            raise ValueError('Unmatched source, fixture, or failed side')
        if report.get('drain_picker', False) != anchor.get('drain_picker', False):
            raise ValueError('Do not combine waiting and non-waiting fixtures')
        if report.get('font_residency', False) != anchor.get('font_residency', False):
            raise ValueError('Do not combine font-page diagnostics and ordinary snapshots')
        for run in report['runs']:
            m = run['measurement']
            if m['adapter'] != first['adapter'] or signature_of(m) != signature:
                raise ValueError('Adapter, scene sequence or recorded work differs')
            if identity(Path(run['binary']['path'])) != run['binary']:
                raise ValueError('Raw measurement changed')
        samples = [r['measurement'] for r in report['runs']]
        summaries.append(dict(side=report['side'], runs=len(samples),
            maxima={k: statistics.median(m['maxima'][k] for m in samples) for k in MEMORY},
            scenes=[dict(name=s['name'], **{k: statistics.median(m['scenes'][j][k] for m in samples) for k in MEMORY})
                    for j, s in enumerate(first['scenes'])]))
    result = dict(status='matched-scene-work-observations', fixture=anchor['fixture'], adapter=first['adapter'],
                  canceled_jobs_credited=args.credit_canceled_jobs,
                  drain_picker=anchor.get('drain_picker', False),
                  font_residency=anchor.get('font_residency', False),
                  inputs=[identity(p) for p in files], groups=summaries,
                  scope='Sequential build-side groups. Matching recorded work is not pixel equality. Sampled whole-process memory includes driver and fixture activity; payload is not physical residency. No CPU/GPU timing claim. '+
                        ('One bounded context-completion wait per process; this changes the schedule.' if anchor.get('drain_picker', False) else 'No GPU completion wait.')+
                        (' Color submission plus explicitly counted cancellation must match at every checkpoint.' if args.credit_canceled_jobs else ''))
    with args.output.open('x', encoding='utf-8') as file:
        json.dump(result, file, indent=2)
        file.write('\n')
    print(result['status'], len(signature), 'checkpoints;', args.output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    measurement = commands.add_parser('measure')
    measurement.add_argument('--side', choices=('native', 'reference'), required=True)
    measurement.add_argument('--fixture', choices=('full', 'extended'), default='full')
    measurement.add_argument('--drain-picker', action='store_true', help='QA-only: wait once for preceding picker context work, capped at 250 ms; record private mapping categories')
    measurement.add_argument('--font-residency', action='store_true', help='QA-only: query mapped and resident font-file pages at each scene')
    measurement.add_argument('--exe', type=Path, default=Path('D:/MaiCord-build/Release/MaiCordUiProbe.exe'))
    measurement.add_argument('--runs', type=int, choices=range(1, 6), default=3)
    measurement.add_argument('--output-dir', type=Path, required=True)
    comparison = commands.add_parser('compare')
    comparison.add_argument('--native', type=Path, action='append', required=True)
    comparison.add_argument('--reference', type=Path, required=True)
    comparison.add_argument('--output', type=Path, required=True)
    comparison.add_argument('--credit-canceled-jobs',action='store_true',help='Require equal color attempts as submitted plus canceled jobs at every scene; retains separate raw counts')
    args = parser.parse_args()
    (measure if args.command == 'measure' else compare)(args)


if __name__ == '__main__':
    main()
