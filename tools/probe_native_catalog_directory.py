"""Bounded offline test of directory metadata freshness; no runtime policy change."""
import argparse,ctypes as c,json,os,struct,sys,threading,time
from pathlib import Path
from ctypes import wintypes as w

class Basic(c.Structure):
    _fields_=[(n,c.c_longlong) for n in ('created','accessed','written','changed')]+[('attributes',w.DWORD)]
class Stat(c.Structure):
    _fields_=[(n,c.c_longlong) for n in ('id','created','accessed','written','changed','allocated','size')]+[(n,w.DWORD) for n in ('attributes','reparse','links','device','characteristics','reserved')]+[('volume',c.c_ulonglong),('identity',c.c_ubyte*16)]
assert c.sizeof(Stat)==104
k=c.WinDLL('kernel32',use_last_error=True)
k.CreateFileW.argtypes=[w.LPCWSTR,w.DWORD,w.DWORD,c.c_void_p,w.DWORD,w.DWORD,w.HANDLE];k.CreateFileW.restype=w.HANDLE
k.CloseHandle.argtypes=[w.HANDLE];k.CloseHandle.restype=w.BOOL
k.GetFileInformationByHandleEx.argtypes=[w.HANDLE,c.c_int,c.c_void_p,w.DWORD];k.GetFileInformationByHandleEx.restype=w.BOOL
k.SetFileInformationByHandle.argtypes=[w.HANDLE,c.c_int,c.c_void_p,w.DWORD];k.SetFileInformationByHandle.restype=w.BOOL
k.GetFileInformationByName.argtypes=[w.LPCWSTR,c.c_int,c.c_void_p,w.DWORD];k.GetFileInformationByName.restype=w.BOOL
k.WriteFile.argtypes=[w.HANDLE,c.c_void_p,w.DWORD,c.POINTER(w.DWORD),c.c_void_p];k.WriteFile.restype=w.BOOL

def checked(ok):
    if not ok:raise c.WinError(c.get_last_error())

def handle(path,access,flags=0):
    h=k.CreateFileW(str(path),access,7,None,3,flags,None)
    if h==c.c_void_p(-1).value:raise c.WinError(c.get_last_error())
    return h

def stat(path):
    s=Stat();checked(k.GetFileInformationByName(str(path),3,c.byref(s),c.sizeof(s)))
    return dict(size=s.size,written=s.written,changed=s.changed,identity=bytes(s.identity).hex(),links=s.links)

def directory(path):
    h=handle(path,1,0x02000000);buffer=c.create_string_buffer(65536);result={};calls=0
    try:
        for batch in range(1024):
            calls+=1
            if not k.GetFileInformationByHandleEx(h,20 if not batch else 19,buffer,len(buffer)):
                error=c.get_last_error()
                if error==18:return result,calls
                raise c.WinError(error)
            raw=buffer.raw;at=0
            while True:
                if at+88>len(raw):raise ValueError('Truncated directory record')
                step=struct.unpack_from('<I',raw,at)[0];units=struct.unpack_from('<I',raw,at+60)[0]
                if units%2 or units>len(raw)-at-88:raise ValueError('Invalid directory name')
                name=raw[at+88:at+88+units].decode('utf-16-le')
                written,changed,size=struct.unpack_from('<qqq',raw,at+24)
                result[name]=dict(size=size,written=written,changed=changed,identity=raw[at+72:at+88].hex())
                if not step:break
                if step<88+units or step%8 or step>len(raw)-at:raise ValueError('Invalid directory offset')
                at+=step
        raise ValueError('Directory work limit')
    finally:checked(k.CloseHandle(h))

def worker(out):
    primary=out/'primary.bin';alias=out/'alias.bin';primary.write_bytes(b'abc');os.link(primary,alias)
    h=handle(primary,0x40000000)
    try:
        old=Basic();checked(k.GetFileInformationByHandleEx(h,0,c.byref(old),c.sizeof(old)))
        old.changed-=86400*10000000
        checked(k.SetFileInformationByHandle(h,0,c.byref(old),c.sizeof(old)))
    finally:checked(k.CloseHandle(h))
    before,initial_calls=directory(out);baseline=stat(primary)
    h=handle(alias,0x40000000)
    try:
        count=w.DWORD();checked(k.WriteFile(h,c.c_char_p(b'z'),1,c.byref(count),None));assert count.value==1
        keep=Basic();keep.written=baseline['written'];checked(k.SetFileInformationByHandle(h,0,c.byref(keep),c.sizeof(keep)))
        opened,_=directory(out);live=stat(primary)
    finally:checked(k.CloseHandle(h))
    after,final_calls=directory(out);closed=stat(primary)
    rows=dict(before=before,baseline=baseline,writer_open_directory=opened,writer_open_stat=live,
        writer_closed_directory=after,writer_closed_stat=closed)
    fields=('size','written','changed','identity')
    rows['stale_primary_after_close']=any(after[primary.name][f]!=closed[f] for f in fields)
    rows['stale_primary_while_open']=any(opened[primary.name][f]!=live[f] for f in fields)
    rows['directory_would_reuse_old_primary']=all(after[primary.name][f]==baseline[f] for f in fields)
    rows['fresh_stamp_changed']=any(closed[f]!=baseline[f] for f in fields)
    rows['calls']=[initial_calls,final_calls]
    (out/'result.json').write_text(json.dumps(dict(status='observed',fixture=rows,scope='Directory freshness only; no performance or runtime acceptance'),indent=2))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output-dir',type=Path,required=True);p.add_argument('--worker',action='store_true');args=p.parse_args()
    out=args.output_dir.resolve()
    if args.worker:worker(out);return 0
    if out.exists():p.error('Preserve existing evidence')
    out.mkdir(parents=True);(out/'result.json').write_text('{"status":"running"}')
    from verify_native_text import run_check,CheckFailure
    try:r=run_check('directory-freshness',[sys.executable,'-X','utf8',str(Path(__file__).resolve()),'--worker','--output-dir',str(out)],Path.cwd(),out,2,threading.Event())
    except CheckFailure as e:r=e.row
    saved=json.loads((out/'result.json').read_text());saved['execution']=r
    if r['exit']:saved['status']=r['status']
    (out/'result.json').write_text(json.dumps(saved,indent=2));print(saved);return r['exit']
if __name__=='__main__':raise SystemExit(main())
