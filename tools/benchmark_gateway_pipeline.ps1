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
    [switch]$Profile
)

# Builds the opt-in Gateway pipeline benchmark inside the single Release cache
# under the shared build mutex, emits its synthetic fixture, compresses it as
# one zlib-stream (shared context, Z_SYNC_FLUSH per payload, as the Gateway
# does), replays it and removes the binary, objects and fixture files.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$binary = Join-Path $cacheRoot 'MaiCordGatewayPipelineBenchmark.exe'
$map = Join-Path $cacheRoot 'MaiCordGatewayPipelineBenchmark.map'
$objects = Join-Path $cacheRoot 'CMakeFiles\MaiCordGatewayPipelineBenchmark.dir'
$work = Join-Path ([IO.Path]::GetTempPath()) ('maicord-gateway-pipeline-' + [guid]::NewGuid().ToString('N'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
$installation = & $vswhere -latest -prerelease -products * -property installationPath
if (-not $installation) { throw 'A Visual Studio C++ installation was not found.' }
Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $env:PATH = (Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
}
$python = (Get-Command python -ErrorAction Stop).Source
$compress = @'
import json, struct, sys, zlib
raw = open(sys.argv[1], 'rb').read()
out = bytearray()
stream = zlib.compressobj()
cursor = 0
while cursor < len(raw):
    phase = raw[cursor]
    size = struct.unpack_from('<I', raw, cursor + 1)[0]
    payload = raw[cursor + 5:cursor + 5 + size]
    cursor += 5 + size
    json.loads(payload)
    chunk = stream.compress(payload) + stream.flush(zlib.Z_SYNC_FLUSH)
    out += struct.pack('<BII', phase, size, len(chunk)) + chunk
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
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON | Out-Null
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
        & $python -I $script $raw $compressed
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
        foreach ($path in @($binary, $map, $objects)) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        }
        cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=OFF | Out-Null
        # Generating with benchmarks enabled creates an empty directory per benchmark target.
        Get-ChildItem -LiteralPath (Join-Path $cacheRoot 'CMakeFiles') -Directory -Filter '*Benchmark.dir' |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Recurse -File) } |
            Remove-Item -Recurse -Force
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
