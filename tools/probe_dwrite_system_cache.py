"""Bounded, read-only DWrite system-catalog ownership probe in fresh QA children.

Private member offsets are pinned to the investigated Windows DLL. This tool
does not implement its protocol, attach to the app, alter a service, or measure
performance under a debugger. No symbols are downloaded during a probe.
"""
import argparse
import hashlib
import json
import re
import struct
import threading
from pathlib import Path

from verify_native_text import CheckFailure, run_check


DW_SHA256 = '4a83f1e02c5558c50ab9c0e94b095a14b4188962a64abd495a79b607d50773ef'
FUNCTIONS = {
    'try-system-cache': 0x1819c8,
    'map-server-cache': 0x180a90,
    'get-server-cache': 0x181f2c,
    'get-cache-section': 0x182008,
    'cache-reader-constructor': 0x1833ec,
    'initialize-system-font-set': 0x1814f8,
}
SCRIPTS = {
    'mapping': '''uf DWrite!CacheReader::GetBasePtrImpl
bp /1 DWrite!ClientSideCacheContext::TryGetSystemCache
g
r @$t0=@rdx
bp /1 poi(@rsp)
g
r @$t2=poi(@$t0)
.if (@$t2==0) {.echo CACHE_MISS;q}
.echo CACHE_READER
dps @$t2 Lc
r @$t3=dwo(@$t2+28)
r @$t3
.echo CACHE_MAPPING
!vprot poi(@$t2+10)
.echo EXPIRATION_EVENT
!handle poi(@$t2+20) f
q
''',
    'section': '''bp /1 ntdll!NtAlpcConnectPort ".echo ALPC_CONNECT;du poi(@rdx+8);k 5;gc"
bp /1 DWrite!ClientSideConnection::GetCacheMemorySectionWithRetry
g
.echo SECTION_REQUEST
r @edx
r @$t0=@r8
r @$t1=@r9
bp /1 poi(@rsp)
g
.echo SECTION_RESULT
r @eax
!handle poi(@$t0) f
.echo SECTION_EVENT
!handle poi(@$t1) f
q
''',
    'phase': '''bp /1 DWrite!DWriteFactory::InitializeSystemFontSet
g
r @$t0=0
r @$t1=0
r @$t2=0
r @$t3=0
bp ntdll!NtCreateFile "r @$t0=@$t0+1;.echo FILE_OPEN;du poi(poi(@r8+10)+8);k 6;gc"
bp KERNELBASE!RegEnumValueW "r @$t1=@$t1+1;gc"
bp ntdll!NtOpenFile "r @$t2=@$t2+1;.echo EXISTING_FILE_OPEN;du poi(poi(@r8+10)+8);k 6;gc"
bp ntdll!NtEnumerateValueKey "r @$t3=@$t3+1;gc"
bp /1 poi(@rsp)
g
.echo FONT_SET_COUNTS
r @$t0
r @$t1
r @$t2
r @$t3
q
''',
}


def identity(path):
    path = Path(path).resolve()
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def pe_functions(data):
    pe = struct.unpack_from('<I', data, 60)[0]
    count = struct.unpack_from('<H', data, pe+6)[0]
    optional = pe+24
    assert struct.unpack_from('<H', data, optional)[0] == 0x20b
    base = struct.unpack_from('<Q', data, optional+24)[0]
    sections = optional+struct.unpack_from('<H', data, pe+20)[0]
    regions = [struct.unpack_from('<IIII', data, sections+i*40+8) for i in range(count)]
    def file_offset(rva):
        for virtual_size, start, size, raw in regions:
            if start <= rva < start+size:
                return raw+rva-start
        raise ValueError('RVA outside file-backed section')
    rva, size = struct.unpack_from('<II', data, optional+112+3*8)
    at = file_offset(rva)
    assert size % 12 == 0
    return base, {start: end for start, end, unwind in
                  struct.iter_unpack('<III', data[at:at+size])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--exe', type=Path, default=Path('D:/MaiCord-build/Release/MaiCordNativeAppLineTests.exe'))
    parser.add_argument('--symbols', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve(); out.mkdir(parents=True, exist_ok=True)
    result_path = out/'result.json'
    if result_path.exists():
        parser.error('Preserve existing evidence; choose an unused output directory')
    dll = Path('C:/Windows/System32/DWrite.dll')
    module = identity(dll)
    if module['sha256'] != DW_SHA256:
        parser.error('Uninvestigated DWrite build: establish its ABI before using private offsets')
    symbols = []
    for name in ('DWrite', 'ntdll'):
        cached = list(args.symbols.glob(name+'.pdb/*/'+name+'.pdb'))
        if not cached:
            parser.error('Missing cached '+name+' symbols; no network fallback is used')
        symbols.extend(str(p.parent.resolve()) for p in cached)
    cdb = Path('C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe')
    llvm = Path('C:/Program Files/LLVM/bin/llvm-objdump.exe')
    binary = identity(args.exe)
    report = dict(status='running', scope=__doc__, module=module, executable=binary,
                  debugger=identity(cdb), llvm=identity(llvm), executions=[], functions=[])
    def save():
        result_path.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    save()
    try:
        logs = {}
        for name, commands in SCRIPTS.items():
            script = out/(name+'.cdb'); script.write_text(commands, encoding='ascii')
            row = run_check(name, [str(cdb), '-hd', '-G', '-y', ';'.join(symbols),
                '-cf', str(script), str(args.exe.resolve()), '--fresh-engine', '0', '1'],
                Path.cwd(), out, 5, threading.Event())
            report['executions'].append(row); save()
            log = Path(row['log']).read_text(errors='replace'); logs[name] = log
            if "Couldn't resolve" in log or 'Memory access error' in log:
                raise ValueError('Debugger command did not establish '+name)
        mapping = logs['mapping'].split('\nCACHE_MAPPING\n', 1)[1]
        if not re.search(r'^Type:\s+[0-9a-f]+\s+MEM_MAPPED\s*$', mapping, re.M) or not re.search(
                r'^Protect:\s+[0-9a-f]+\s+PAGE_READONLY\s*$', mapping, re.M):
            raise ValueError('System cache is not a read-only mapping')
        if "DWrite!CacheReader::`vftable'" not in logs['mapping']:
            raise ValueError('Returned wrapper does not identify a CacheReader')
        extent = re.findall(r'^\$t3=([0-9a-f]+)', logs['mapping'], re.M)
        assert len(extent) == 1
        report['cache_bytes'] = int(extent[0], 16)
        region = re.search(r'^RegionSize:\s+([0-9a-f]+)', mapping, re.M)
        if not region or int(region[1], 16) != report['cache_bytes']:
            raise ValueError('Reader extent and mapped region differ')
        section = logs['section'].split('\nSECTION_RESULT\n', 1)[1]
        if not re.search(r'^eax=0\s*$', section, re.M) or 'MapRead' not in section:
            raise ValueError('Read-only section acquisition was not observed')
        if '\\BaseNamedObjects\\FontCachePort' not in logs['section']:
            raise ValueError('Expected FontCache ALPC connection was not observed')
        event = section.split('\nSECTION_EVENT\n', 1)[1]
        if not re.search(r'^\s*Type\s+Event\s*$', event, re.M) or not re.search(
                r'^\s*GrantedAccess\s+0x100000:', event, re.M):
            raise ValueError('Synchronization-only expiration event was not observed')
        for key, register in [('ntcreatefile_calls', 't0'), ('regenumvalue_calls', 't1'),
                              ('ntopenfile_calls', 't2'), ('ntenumeratevalue_calls', 't3')]:
            values = re.findall(r'^\$'+register+r'=([0-9a-f]+)', logs['phase'], re.M)
            assert len(values) == 1
            report[key] = int(values[0], 16)
        base, ranges = pe_functions(dll.read_bytes())
        for name, start in FUNCTIONS.items():
            end = ranges[start]
            report['functions'].append(dict(name=name, start_rva=hex(start), end_rva=hex(end), bytes=end-start))
            report['executions'].append(run_check('llvm-'+name, [str(llvm), '-d',
                '--start-address='+hex(base+start), '--stop-address='+hex(base+end), str(dll)],
                Path.cwd(), out, 5, threading.Event()))
            save()
        if identity(args.exe) != binary or identity(dll) != module:
            raise ValueError('An inspected executable changed')
        report['status'] = 'observed'
    except CheckFailure as error:
        report['executions'].append(error.row); report['status'] = 'failed'
        raise
    except Exception as error:
        report['status'] = 'failed'; report['error'] = str(error)
        raise
    finally:
        save()
    print(json.dumps({key: report[key] for key in ('status', 'cache_bytes', 'ntcreatefile_calls',
        'regenumvalue_calls', 'ntopenfile_calls', 'ntenumeratevalue_calls')}))


if __name__ == '__main__':
    main()
