"""Hidden offline main-process startup; first submitted Present, not visible latency.

Use isolated executable/font copies without the account DLL. Each worker gets a
fresh app-data directory, optionally seeded with a catalog from an untimed primer.
Memory/cycles are sampled 100 ms after observing the first Present log. Module
sampling can miss transient loads and affect residency; Windows caches are uncontrolled.
"""
import argparse,ctypes as c,json,os,re,shutil,statistics,subprocess,time
from pathlib import Path
from benchmark_native_text_performance import identity

k=c.WinDLL('kernel32',use_last_error=True);u=c.WinDLL('user32',use_last_error=True)
callback=c.WINFUNCTYPE(c.c_int,c.c_void_p,c.c_ssize_t)
u.EnumWindows.argtypes=[callback,c.c_ssize_t];u.EnumWindows.restype=c.c_int
u.GetWindowThreadProcessId.argtypes=[c.c_void_p,c.POINTER(c.c_ulong)];u.GetWindowThreadProcessId.restype=c.c_ulong
u.GetClassNameW.argtypes=[c.c_void_p,c.c_wchar_p,c.c_int];u.GetClassNameW.restype=c.c_int
u.IsWindowVisible.argtypes=[c.c_void_p];u.IsWindowVisible.restype=c.c_int
u.PostMessageW.argtypes=[c.c_void_p,c.c_uint,c.c_size_t,c.c_ssize_t];u.PostMessageW.restype=c.c_int
k.K32EnumProcessModules.argtypes=[c.c_void_p,c.c_void_p,c.c_ulong,c.POINTER(c.c_ulong)];k.K32EnumProcessModules.restype=c.c_int
k.K32GetModuleFileNameExW.argtypes=[c.c_void_p,c.c_void_p,c.c_wchar_p,c.c_ulong];k.K32GetModuleFileNameExW.restype=c.c_ulong
class Memory(c.Structure):
    _fields_=[('cb',c.c_ulong),('faults',c.c_ulong)]+[(x,c.c_size_t) for x in ['peak_ws','ws','peak_paged','paged','peak_nonpaged','nonpaged','pagefile','peak_pagefile','private']]
k.K32GetProcessMemoryInfo.argtypes=[c.c_void_p,c.POINTER(Memory),c.c_ulong];k.K32GetProcessMemoryInfo.restype=c.c_int
k.QueryProcessCycleTime.argtypes=[c.c_void_p,c.POINTER(c.c_ulonglong)];k.QueryProcessCycleTime.restype=c.c_int

def window(pid):
    found=[]
    @callback
    def visit(hwnd,_):
        owner=c.c_ulong();u.GetWindowThreadProcessId(hwnd,c.byref(owner))
        if owner.value==pid:
            name=c.create_unicode_buffer(128);u.GetClassNameW(hwnd,name,128)
            if name.value=='MaiCordMainWindow':found.append(hwnd)
        return 1
    if not u.EnumWindows(visit,0):raise OSError(c.get_last_error(),'EnumWindows')
    return found[0] if found else None

def modules(handle):
    values=(c.c_void_p*1024)();needed=c.c_ulong()
    if not k.K32EnumProcessModules(handle,values,c.sizeof(values),c.byref(needed)):return None
    if needed.value>c.sizeof(values):raise ValueError('Module inventory overflow')
    paths=[]
    for value in values[:needed.value//c.sizeof(c.c_void_p)]:
        name=c.create_unicode_buffer(32768)
        if not k.K32GetModuleFileNameExW(handle,value,name,len(name)):return None
        paths.append(name.value)
    return paths

def run(exe,out,side,seed=None):
    out.mkdir();data=out/'data';data.mkdir();log=data/'MaiCord/logs/maicord.log'
    if (exe.parent/'MaiCordAccount.dll').exists():raise ValueError('Use an isolated offline executable copy without MaiCordAccount.dll')
    if seed:
        (data/'MaiCord').mkdir();shutil.copy2(seed,data/'MaiCord/native-fonts-v1.bin')
    env={key:value for key,value in os.environ.items() if not key.startswith('MAICORD_')};env['LOCALAPPDATA']=str(data)
    startup=subprocess.STARTUPINFO();startup.dwFlags=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
    start=time.perf_counter();proc=subprocess.Popen([str(exe)],cwd=exe.parent,env=env,startupinfo=startup,creationflags=subprocess.CREATE_NO_WINDOW)
    observed=set();snapshots=0;hwnd=None;first_seen=None;row={}
    try:
        while time.perf_counter()-start<3:
            if proc.poll() is not None:raise ValueError(f'Main process exited before observation: {proc.returncode}')
            current=modules(int(proc._handle))
            if current is not None:observed.update(current);snapshots+=1
            hwnd=window(proc.pid)
            if hwnd and u.IsWindowVisible(hwnd):raise ValueError('Benchmark window unexpectedly visible')
            raw=log.read_text(encoding='utf-8') if log.exists() else ''
            if 'first_present_us=' in raw:
                if first_seen is None:first_seen=time.perf_counter()
                if time.perf_counter()-first_seen>=.1:
                    memory=Memory();memory.cb=c.sizeof(memory);cycles=c.c_ulonglong()
                    if not k.K32GetProcessMemoryInfo(int(proc._handle),c.byref(memory),c.sizeof(memory)) or not k.QueryProcessCycleTime(int(proc._handle),c.byref(cycles)):raise OSError(c.get_last_error(),'Process metrics')
                    row.update(observed_us=(first_seen-start)*1e6,working_set=memory.ws,private_bytes=memory.private,process_cycles=cycles.value);break
            time.sleep(.01)
        else:raise TimeoutError('No first submitted frame within three seconds')
    finally:
        if proc.poll() is None:
            hwnd=hwnd or window(proc.pid)
            if hwnd:u.PostMessageW(hwnd,0x0010,0,0)
            try:proc.wait(timeout=2)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=1)
    if proc.returncode!=0:raise ValueError(f'Unclean main exit: {proc.returncode}')
    raw=log.read_text(encoding='utf-8');keys=['first_present_us','first_frame_cpu_us','first_frame_quads','first_frame_draw_calls','first_frame_glyph_quads']
    if 'normal_user_profile_absent' not in raw or 'host_load_native_error=126' not in raw:raise ValueError('Unexpected account state in offline fixture')
    for key in keys:
        values=re.findall(r'\b'+key+r'=(\d+)',raw)
        if len(values)!=1:raise ValueError('Missing/ambiguous '+key)
        row[key]=int(values[0])
    if side=='native':
        for key in ['native_catalog_cache_hits','native_catalog_cache_writes','native_catalog_failures','native_line_prepare_failures','native_line_execute_failures','native_outline_failures']:
            values=re.findall(r'\b'+key+r'=(\d+)',raw)
            if len(values)!=1:raise ValueError('Missing/ambiguous '+key)
            row[key]=int(values[0])
        if row['native_catalog_cache_hits']!=int(seed is not None) or any(row[key] for key in ['native_catalog_failures','native_line_prepare_failures','native_line_execute_failures','native_outline_failures']):raise ValueError('Cache state or native text failure')
    forbidden=[x for x in observed if Path(x).name.lower() in ['dwrite.dll','dwritecore.dll']]
    if not snapshots or not hwnd or not row['first_frame_glyph_quads']:raise ValueError('Incomplete native-window observation')
    if side=='native' and forbidden:raise ValueError('Native main loaded DirectWrite: '+str(forbidden))
    if side=='native' and any(Path(x).name.lower()=='textshaping.dll' for x in observed):raise ValueError('Native main loaded TextShaping')
    if side=='reference' and not forbidden:raise ValueError('DWrite reference was not observed')
    row.update(modules=sorted(observed),module_snapshots=snapshots,hidden=True,exit=proc.returncode,log=identity(log))
    return row

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--native',type=Path,required=True);parser.add_argument('--reference',type=Path,required=True);parser.add_argument('--output-dir',type=Path,required=True);parser.add_argument('--runs',type=int,default=3);parser.add_argument('--cache',choices=('create','hit'),default='create');args=parser.parse_args()
    if not 1<=args.runs<=5:parser.error('Use 1–5 workers per provider')
    out=args.output_dir.resolve();out.mkdir(parents=True,exist_ok=False);executables={side:getattr(args,side).resolve() for side in ['native','reference']}
    report=dict(status='running',scope=__doc__,cache=args.cache,source=identity(Path(__file__)),executables={side:identity(exe) for side,exe in executables.items()},runs=[])
    try:
        seed=None
        if args.cache=='hit':
            report['primer']=run(executables['native'],out/'primer','native');seed=out/'primer/data/MaiCord/native-fonts-v1.bin'
            if not seed.is_file():raise ValueError('No native catalog created by primer')
        for index in range(args.runs):
            for side in (['native','reference'] if index%2==0 else ['reference','native']):
                row=run(executables[side],out/f'{index}-{side}',side,seed);row.update(side=side,trial=index);report['runs'].append(row)
        anchor=report['runs'][0]
        for row in report['runs']:
            for key in ['first_frame_quads','first_frame_draw_calls','first_frame_glyph_quads']:assert row[key]==anchor[key],key
        report['medians']={side:{key:statistics.median(x[key] for x in report['runs'] if x['side']==side) for key in ['first_present_us','first_frame_cpu_us','working_set','private_bytes','process_cycles']} for side in executables}
        for side,exe in executables.items():assert report['executables'][side]==identity(exe)
        report['status']='passed'
    except Exception as error:report.update(status='failed',error=str(error));raise
    finally:(out/'measurement.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report['medians'],indent=2))

if __name__=='__main__':main()
