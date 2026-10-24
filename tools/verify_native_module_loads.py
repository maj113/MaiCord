"""Record loader debug events from process creation through clean exit.

This checks DLL loads, including modules unloaded before a later snapshot.
Debugger runs are correctness evidence, not latency or memory measurements.
Each run uses a fresh app-data directory, so no account is signed in.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import os
from pathlib import Path
import subprocess
import time
from benchmark_native_text_performance import identity
from measure_native_startup import window

k=c.WinDLL('kernel32',use_last_error=True)
u=c.WinDLL('user32',use_last_error=True)

class ExceptionRecord(c.Structure):
    _fields_=[('code',w.DWORD),('flags',w.DWORD),('record',c.c_void_p),
              ('address',c.c_void_p),('count',w.DWORD),('information',c.c_size_t*15)]
class ExceptionInfo(c.Structure):
    _fields_=[('record',ExceptionRecord),('first_chance',w.DWORD)]
class CreateInfo(c.Structure):
    _fields_=[('file',w.HANDLE),('process',w.HANDLE),('thread',w.HANDLE),('base',c.c_void_p)]
class LoadInfo(c.Structure):
    _fields_=[('file',w.HANDLE),('base',c.c_void_p)]
class Payload(c.Union):
    _fields_=[('exception',ExceptionInfo),('create',CreateInfo),('load',LoadInfo),
              ('base',c.c_void_p),('exit_code',w.DWORD)]
class Event(c.Structure):
    _fields_=[('kind',w.DWORD),('pid',w.DWORD),('tid',w.DWORD),('data',Payload)]

assert c.sizeof(c.c_void_p)==8 and c.sizeof(Event)==176 and Event.data.offset==16
k.WaitForDebugEvent.argtypes=[c.POINTER(Event),w.DWORD];k.WaitForDebugEvent.restype=w.BOOL
k.ContinueDebugEvent.argtypes=[w.DWORD,w.DWORD,w.DWORD];k.ContinueDebugEvent.restype=w.BOOL
k.GetFinalPathNameByHandleW.argtypes=[w.HANDLE,w.LPWSTR,w.DWORD,w.DWORD];k.GetFinalPathNameByHandleW.restype=w.DWORD
k.K32GetMappedFileNameW.argtypes=[w.HANDLE,c.c_void_p,w.LPWSTR,w.DWORD];k.K32GetMappedFileNameW.restype=w.DWORD
k.CloseHandle.argtypes=[w.HANDLE];k.CloseHandle.restype=w.BOOL
u.IsWindowVisible.argtypes=[w.HWND];u.IsWindowVisible.restype=w.BOOL
u.PostMessageW.argtypes=[w.HWND,w.UINT,w.WPARAM,w.LPARAM];u.PostMessageW.restype=w.BOOL

def image_path(process,file,base):
    name=c.create_unicode_buffer(32768)
    try:
        size=k.GetFinalPathNameByHandleW(file,name,len(name),0) if file else 0
        if not size or size>=len(name):size=k.K32GetMappedFileNameW(process,base,name,len(name))
        if not size or size>=len(name):raise OSError(c.get_last_error(),'Unresolved image load')
        return name.value
    finally:
        if file:k.CloseHandle(file)

def trace(exe,out,mode,expect_dwrite,close_delay_ms=0):
    out.mkdir(parents=True,exist_ok=False);data=out/'data';data.mkdir()
    env={key:value for key,value in os.environ.items() if not key.startswith('MAICORD_')}
    env['LOCALAPPDATA']=str(data)
    if mode.startswith('glyph-'):
        env.update(MAICORD_GPU_METRICS='1',MAICORD_GLYPH_CACHE_METRICS=str(out/'phases.bin'),
                   MAICORD_GLYPH_CACHE_KIND=str(('mixed','mono','color').index(mode[6:])))
    if mode=='cjk':
        env.update(MAICORD_CJK_PIXEL_STREAM='1',MAICORD_CJK_PIXEL_FAMILY='Microsoft YaHei UI',
                   MAICORD_CJK_PIXEL_TEXT='中文消息测试',MAICORD_CJK_PIXEL_CASE='1 128 95 1')
    log=data/'MaiCord/logs/maicord.log'
    startup=subprocess.STARTUPINFO();startup.dwFlags=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
    report=dict(status='running',scope=__doc__,executable=identity(exe),source=identity(Path(__file__)),
                mode=mode,expect_dwrite=expect_dwrite,close_delay_ms=close_delay_ms,events=[],exceptions=[])
    proc=subprocess.Popen([str(exe)],cwd=exe.parent,env=env,startupinfo=startup,stdout=subprocess.DEVNULL,
                          creationflags=subprocess.CREATE_NO_WINDOW|0x2)
    report['pid']=proc.pid;start=time.monotonic();created=False;exited=False;initial_break=False;closing=False;present_at=None
    try:
        while not exited:
            if time.monotonic()-start>20:raise TimeoutError('Debugged fixture did not exit within 20 seconds')
            event=Event()
            if not k.WaitForDebugEvent(c.byref(event),50):
                if c.get_last_error()!=121:raise OSError(c.get_last_error(),'WaitForDebugEvent')
            else:
                if event.pid!=proc.pid:raise ValueError('Unexpected child debug event')
                disposition=0x00010002
                try:
                    if event.kind in (3,6):
                        info=event.data.create if event.kind==3 else event.data.load
                        path=image_path(int(proc._handle),info.file,info.base)
                        report['events'].append(dict(kind='process' if event.kind==3 else 'load',base=info.base,path=path))
                        if event.kind==3:
                            if created:raise ValueError('Duplicate process creation')
                            created=True
                    elif event.kind==7:report['events'].append(dict(kind='unload',base=event.data.base))
                    elif event.kind==1:
                        info=event.data.exception;code=info.record.code
                        if code==0x80000003 and info.first_chance and not initial_break:initial_break=True
                        else:
                            report['exceptions'].append(dict(code=code,first_chance=bool(info.first_chance)))
                            disposition=0x80010001
                    elif event.kind==5:
                        report['exit_code']=event.data.exit_code;exited=True
                finally:
                    if not k.ContinueDebugEvent(event.pid,event.tid,disposition):
                        raise OSError(c.get_last_error(),'ContinueDebugEvent')
            if mode=='fresh-main' and not closing and log.exists() and 'first_present_us=' in log.read_text(encoding='utf-8'):
                if present_at is None:present_at=time.monotonic()
                if (time.monotonic()-present_at)*1000<close_delay_ms:continue
                hwnd=window(proc.pid)
                if not hwnd or u.IsWindowVisible(hwnd):raise ValueError('Missing or visible offline window')
                if not u.PostMessageW(hwnd,0x0010,0,0):raise OSError(c.get_last_error(),'Close offline fixture')
                closing=True
        proc.wait(timeout=2)
        if not created or not initial_break or report['exit_code'] or proc.returncode:
            raise ValueError('Incomplete debug lifecycle or failed fixture')
        if mode=='fresh-main':
            raw=log.read_text(encoding='utf-8')
            markers=['normal_user_profile_absent','normal_exit']
            if not closing or any(marker not in raw for marker in markers):
                raise ValueError('Unexpected account state or incomplete offline fixture')
            report['app_log']=identity(log)
        if mode.startswith('glyph-'):
            from measure_emoji_gpu import decode
            raw=log.read_text(encoding='utf-8')
            if 'glyph_cache_passed' not in raw:raise ValueError('Incomplete glyph fixture')
            phases=decode(out/'phases.bin','reference' if expect_dwrite else 'native',mode!='glyph-mono')
            if len(phases['pages'])!=5:raise ValueError('Missing cold/warm/reset/retained phases')
            report['app_log']=identity(log);report['glyph_phases']=identity(out/'phases.bin')
        if mode=='cjk':
            if 'cjk_pixel_frames=16' not in log.read_text(encoding='utf-8'):
                raise ValueError('Incomplete CJK fixture')
            report['app_log']=identity(log)
        report['dwrite_loads']=[e for e in report['events'] if e['kind']=='load' and Path(e['path']).name.lower() in ('dwrite.dll','dwritecore.dll')]
        report['textshaping_loads']=[e for e in report['events'] if e['kind']=='load' and Path(e['path']).name.lower()=='textshaping.dll']
        if bool(report['dwrite_loads'])!=expect_dwrite:raise ValueError('DirectWrite load expectation failed')
        if not expect_dwrite and report['textshaping_loads']:raise ValueError('Native fixture loaded TextShaping')
        if identity(exe)!=report['executable']:raise ValueError('Executable changed during trace')
        report['status']='passed'
    except Exception as error:
        report.update(status='failed',error=str(error));raise
    finally:
        if not exited:
            proc.kill()
            while time.monotonic()-start<25:
                event=Event()
                if not k.WaitForDebugEvent(c.byref(event),100):continue
                k.ContinueDebugEvent(event.pid,event.tid,0x00010002)
                if event.kind==5:break
            proc.wait(timeout=2)
        report['seconds']=time.monotonic()-start
        (out/'trace.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    return report

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True);parser.add_argument('--output-dir',type=Path,required=True)
    parser.add_argument('--mode',choices=('exit','fresh-main','glyph-mixed','glyph-mono','glyph-color','cjk'),default='exit')
    parser.add_argument('--expect-dwrite',action='store_true')
    parser.add_argument('--close-delay-ms',type=int,default=0)
    args=parser.parse_args()
    if not 0<=args.close_delay_ms<=5000:parser.error('Use a close delay between 0 and 5000 ms')
    result=trace(args.exe.resolve(),args.output_dir.resolve(),args.mode,args.expect_dwrite,args.close_delay_ms)
    print(result['status'],len(result['events']),'image events; DirectWrite loads:',len(result['dwrite_loads']))

if __name__=='__main__':main()
