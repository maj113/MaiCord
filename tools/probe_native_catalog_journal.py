"""Read-only, bounded availability probe for unprivileged journal checkpoints."""
import argparse,ctypes as c,json,sys,threading
from pathlib import Path
from ctypes import wintypes as w
from probe_native_catalog_directory import k,handle,checked
class Journal(c.Structure):
    _fields_=[('id',c.c_ulonglong)]+[(n,c.c_longlong) for n in ('first','next','lowest','maximum')]+[(n,c.c_ulonglong) for n in ('size','delta')]+[('minimum_version',w.WORD),('maximum_version',w.WORD),('flags',w.DWORD),('range_chunk',c.c_ulonglong),('range_threshold',c.c_longlong)]
class Read(c.Structure):
    _fields_=[('start',c.c_longlong),('reasons',w.DWORD),('close_only',w.DWORD),('timeout',c.c_ulonglong),('wait_bytes',c.c_ulonglong),('journal',c.c_ulonglong),('minimum',w.WORD),('maximum',w.WORD)]
k.DeviceIoControl.argtypes=[w.HANDLE,w.DWORD,c.c_void_p,w.DWORD,c.c_void_p,w.DWORD,c.POINTER(w.DWORD),c.c_void_p];k.DeviceIoControl.restype=w.BOOL

def worker(out):
    result=dict(status='observed',scope='Read-only journal availability; no names/records retained and no runtime acceptance')
    try:h=handle(r'\\.\C:',0)
    except OSError as e:result['open_error']=e.winerror
    else:
        try:
            journal=Journal();done=w.DWORD();ok=k.DeviceIoControl(h,0x900f4,None,0,c.byref(journal),c.sizeof(journal),c.byref(done),None)
            result['query']=dict(ok=bool(ok),error=0 if ok else c.get_last_error(),bytes=done.value)
            if ok:
                result['checkpoint']={n:getattr(journal,n) for n in ['id','first','next','lowest','minimum_version','maximum_version']}
                request=Read(journal.next,0xffffffff,0,0,0,journal.id,2,3);buffer=c.create_string_buffer(65536)
                ok=k.DeviceIoControl(h,0x903ab,c.byref(request),c.sizeof(request),buffer,len(buffer),c.byref(done),None)
                result['unprivileged_read']=dict(ok=bool(ok),error=0 if ok else c.get_last_error(),bytes=done.value)
        finally:checked(k.CloseHandle(h))
    (out/'result.json').write_text(json.dumps(result,indent=2))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output-dir',type=Path,required=True);p.add_argument('--worker',action='store_true');a=p.parse_args();out=a.output_dir.resolve()
    if a.worker:worker(out);return 0
    if out.exists():p.error('Preserve existing evidence')
    out.mkdir(parents=True);(out/'result.json').write_text('{"status":"running"}')
    from verify_native_text import run_check,CheckFailure
    try:r=run_check('journal-availability',[sys.executable,'-X','utf8',str(Path(__file__).resolve()),'--worker','--output-dir',str(out)],Path.cwd(),out,2,threading.Event())
    except CheckFailure as e:r=e.row
    s=json.loads((out/'result.json').read_text());s['execution']=r
    if r['exit']:s['status']=r['status']
    (out/'result.json').write_text(json.dumps(s,indent=2));print(s);return r['exit']
if __name__=='__main__':raise SystemExit(main())
