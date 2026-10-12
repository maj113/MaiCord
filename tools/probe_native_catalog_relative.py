"""Offline comparison of absolute and parent-relative fresh file-stat queries."""
import argparse,ctypes as c,json,sys,threading,time
from pathlib import Path
from ctypes import wintypes as w
from probe_native_catalog_directory import k,Stat,handle,checked,stat
class Unicode(c.Structure):
    _fields_=[('length',w.USHORT),('maximum',w.USHORT),('buffer',w.LPWSTR)]
class Attributes(c.Structure):
    _fields_=[('length',w.ULONG),('root',w.HANDLE),('name',c.POINTER(Unicode)),('attributes',w.ULONG),('security',c.c_void_p),('quality',c.c_void_p)]
class Io(c.Structure):
    _fields_=[('status',c.c_void_p),('information',c.c_size_t)]
nt=c.WinDLL('ntdll').NtQueryInformationByName
nt.argtypes=[c.POINTER(Attributes),c.POINTER(Io),c.c_void_p,w.ULONG,c.c_int];nt.restype=c.c_long

def worker(out,fixture):
    paths=[p for p in Path('C:/Windows/Fonts').iterdir() if p.suffix.lower() in ('.ttf','.ttc','.otf')]
    queries=[];roots={}
    try:
        for p in paths+[fixture]:
            parent=str(p.parent)
            if parent not in roots:roots[parent]=handle(p.parent,0xa0,0x02000000)
            buffer=c.create_unicode_buffer(p.name);name=Unicode(len(p.name.encode('utf-16-le')),c.sizeof(buffer)-2,c.cast(buffer,w.LPWSTR))
            attr=Attributes(c.sizeof(Attributes),roots[parent],c.pointer(name),0x40,None,None)
            queries.append((p,buffer,name,attr))
        differences=[];samples=[]
        for batch in range(6):
            outputs=[[],[]];times=[0,0]
            for order in range(2):
                side=order^(batch&1);start=time.perf_counter_ns()
                for p,buffer,name,attr in queries:
                    s=Stat();io=Io()
                    if side:
                        status=nt(c.byref(attr),c.byref(io),c.byref(s),c.sizeof(s),77)
                        if status:raise RuntimeError(f'Nt status {status&0xffffffff:#x} for {p}')
                    else:checked(k.GetFileInformationByName(str(p),3,c.byref(s),c.sizeof(s)))
                    outputs[side].append((s.size,s.written,s.changed,s.volume,bytes(s.identity).hex()))
                times[side]=time.perf_counter_ns()-start
            differences += [str(queries[i][0]) for i,(a,b) in enumerate(zip(*outputs)) if a!=b]
            samples.append(times)
        result=dict(status='observed',files=len(queries),differences=differences,absolute_relative_ns=samples,
            fixture=str(fixture),fixture_relative=outputs[1][-1],scope='Python/ctypes diagnostic, six alternating batches; no native app speed claim')
        (out/'result.json').write_text(json.dumps(result,indent=2))
    finally:
        for h in roots.values():checked(k.CloseHandle(h))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output-dir',type=Path,required=True);p.add_argument('--fixture',type=Path,required=True);p.add_argument('--worker',action='store_true');a=p.parse_args();out=a.output_dir.resolve()
    if a.worker:worker(out,a.fixture.resolve());return 0
    if out.exists():p.error('Preserve existing evidence')
    out.mkdir(parents=True);(out/'result.json').write_text('{"status":"running"}')
    from verify_native_text import run_check,CheckFailure
    try:r=run_check('relative-stamps',[sys.executable,'-X','utf8',str(Path(__file__).resolve()),'--worker','--output-dir',str(out),'--fixture',str(a.fixture.resolve())],Path.cwd(),out,2,threading.Event())
    except CheckFailure as e:r=e.row
    s=json.loads((out/'result.json').read_text());s['execution']=r
    if r['exit']:s['status']=r['status']
    (out/'result.json').write_text(json.dumps(s,indent=2));print(s);return r['exit']
if __name__=='__main__':raise SystemExit(main())
