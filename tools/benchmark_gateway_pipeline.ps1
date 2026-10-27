#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Runs = 5,
    # Repeats the replay process; every sample prints its own table.
    [ValidateRange(1, 20)]
    [int]$Samples = 1,
    # large: ~3 MiB READY; small: a READY within the 128 KiB compressed-message limit.
    [ValidateSet('large', 'small')]
    [string[]]$Preset = @('small', 'large'),
    # Samples the producer thread in-process and prints its hottest functions per phase.
    [switch]$Profile,
    # The compressor's level and window; Discord's frames declare a 64 KiB window.
    [int]$ZstdLevel = 6,
    [ValidateRange(10, 23)]
    [int]$ZstdWindowLog = 16
)

# Builds the opt-in Gateway pipeline benchmark inside the single Release cache
# under the shared build mutex, emits its synthetic fixture, encodes each
# payload as the Gateway's ETF term and compresses them as one zstd-stream
# (shared context, a flush per payload), replays it and removes the binary,
# objects and fixture files.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$binary = Join-Path $cacheRoot 'MaiCordGatewayPipelineBenchmark.exe'
$map = Join-Path $cacheRoot 'MaiCordGatewayPipelineBenchmark.map'
$objects = Join-Path $cacheRoot 'CMakeFiles\MaiCordGatewayPipelineBenchmark.dir'
$work = Join-Path ([IO.Path]::GetTempPath()) ('maicord-gateway-pipeline-' + [guid]::NewGuid().ToString('N'))
. (Join-Path $PSScriptRoot 'enter_build_environment.ps1')
$python = (Get-Command python -ErrorAction Stop).Source
$compress = @'
import json, struct, sys
from compression.zstd import ZstdCompressor, CompressionParameter as P

# The Gateway's term layout: atom keys, nil/true/false atoms, binaries for
# strings, integers by size, snowflake strings as 64-bit integers.
def term(value, out):
    if value is None:
        out += b'\x64\x00\x03nil'
    elif value is True:
        out += b'\x64\x00\x04true'
    elif value is False:
        out += b'\x64\x00\x05false'
    elif isinstance(value, int):
        if 0 <= value <= 255:
            out += struct.pack('>BB', 97, value)
        elif -2**31 <= value < 2**31:
            out += struct.pack('>Bi', 98, value)
        else:
            magnitude = abs(value)
            digits = magnitude.to_bytes((magnitude.bit_length() + 7) // 8, 'little')
            out += struct.pack('>BBB', 110, len(digits), 1 if value < 0 else 0) + digits
    elif isinstance(value, float):
        out += struct.pack('>Bd', 70, value)
    elif isinstance(value, str):
        if len(value) >= 17 and value.isdigit() and value[0] != '0' and int(value) < 2**64:
            term(int(value), out)
        else:
            data = value.encode('utf-8', 'surrogatepass')
            out += struct.pack('>BI', 109, len(data)) + data
    elif isinstance(value, list):
        if not value:
            out += b'\x6a'
        else:
            out += struct.pack('>BI', 108, len(value))
            for item in value:
                term(item, out)
            out += b'\x6a'
    elif isinstance(value, dict):
        out += struct.pack('>BI', 116, len(value))
        for key, item in value.items():
            data = key.encode('latin-1')
            out += struct.pack('>BH', 100, len(data)) + data
            term(item, out)
    else:
        raise TypeError(type(value))

raw = open(sys.argv[1], 'rb').read()
out = bytearray()
stream = ZstdCompressor(options={P.compression_level: int(sys.argv[3]), P.window_log: int(sys.argv[4])})
cursor = 0
while cursor < len(raw):
    phase = raw[cursor]
    size = struct.unpack_from('<I', raw, cursor + 1)[0]
    payload = raw[cursor + 5:cursor + 5 + size]
    cursor += 5 + size
    etf = bytearray(b'\x83')
    term(json.loads(payload), etf)
    chunk = stream.compress(bytes(etf), mode=ZstdCompressor.FLUSH_BLOCK)
    out += struct.pack('<BII', phase, len(etf), len(chunk)) + chunk
open(sys.argv[2], 'wb').write(out)
'@
$resolve = @'
import re, sys
symbols = []
base = 0x140000000
for line in open(sys.argv[1], encoding='latin-1'):
    m = re.match(r'\s*Preferred load address is ([0-9a-fA-F]+)', line)
    if m: base = int(m.group(1), 16)
    m = re.match(r'\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f\b', line)
    if m and int(m.group(2), 16) >= base: symbols.append((int(m.group(2), 16) - base, m.group(1)))
symbols.sort()
starts = [rva for rva, _ in symbols]
import bisect
def name(rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i < 0: return hex(rva)
    raw = symbols[i][1]
    parts = re.match(r'\?([^@]+)@((?:[^@]+@)*)@', raw)
    text = (parts.group(1) + ' [' + parts.group(2).rstrip('@').replace('@', '<') + ']') if parts else raw
    return text if rva == symbols[i][0] else text + '+' + hex(rva - symbols[i][0])
phases = ['ready', 'supplemental', 'burst']
samples = {}
hot = {}
for line in sys.stdin:
    fields = dict(f.split('=', 1) for f in line.split()[1:] if '=' in f)
    if line.startswith('profile '):
        samples[int(fields['phase'])] = (int(fields['samples']), int(fields['outside']))
    elif line.startswith('hot '):
        hot.setdefault(int(fields['phase']), []).append((int(fields['rva']), int(fields['self']), int(fields['inclusive'])))
    else:
        sys.stdout.write(line)
for phase, (count, outside) in sorted(samples.items()):
    if not count: continue
    entries = hot.get(phase, [])
    print()
    print(f'{phases[phase]}: {count} samples, {100.0 * outside / count:.1f}% outside this image')
    for title, key in (('self', 1), ('inclusive', 2)):
        print(f'  {title}:')
        for rva, own, incl in sorted(entries, key=lambda e: -e[key])[:22]:
            value = own if key == 1 else incl
            if value * 200 < count: break
            print(f'    {100.0 * value / count:5.1f}%  {name(rva)}')
'@

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
Push-Location $repository
try {
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    # The profiler walks the producer's stack through unwind tables.
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON "-DMAICORD_UNWIND_TABLES=$(if ($Profile) { 'ON' } else { 'OFF' })" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
    cmake --build $cacheRoot --target MaiCordGatewayPipelineBenchmark
    if ($LASTEXITCODE -ne 0) { throw 'Gateway pipeline benchmark build failed.' }

    [void](New-Item -ItemType Directory -Path $work)
    $raw = Join-Path $work 'fixture.raw'
    $compressed = Join-Path $work 'fixture.z'
    $script = Join-Path $work 'compress.py'
    [IO.File]::WriteAllText($script, $compress)
    $resolver = Join-Path $work 'resolve.py'
    [IO.File]::WriteAllText($resolver, $resolve)
    foreach ($name in $Preset) {
        Write-Host "preset=$name"
        & $binary emit $raw $name
        if ($LASTEXITCODE -ne 0) { throw "Fixture emission failed with code $LASTEXITCODE." }
        & $python -I $script $raw $compressed $ZstdLevel $ZstdWindowLog
        if ($LASTEXITCODE -ne 0) { throw 'Fixture compression failed.' }
        for ($sample = 1; $sample -le $Samples; $sample++) {
            if ($Profile) {
                & $binary replay $compressed $Runs $name profile | & $python -I $resolver $map
            } else {
                & $binary replay $compressed $Runs $name
            }
            if ($LASTEXITCODE -ne 0) { Write-Host "Gateway pipeline replay ($name) failed with code $LASTEXITCODE." }
        }
    }
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    if ($locked) {
        & (Join-Path $PSScriptRoot 'clear_benchmark_outputs.ps1') -CacheRoot $cacheRoot -Paths @($binary, $map, $objects)
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
