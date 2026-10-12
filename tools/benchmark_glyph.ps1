#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 7
)

# Builds the opt-in benchmark inside the single Release cache under the shared
# build mutex, then removes its binary/objects and restores the shipping-only
# configuration.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$binary = Join-Path $cacheRoot 'MaiCordGlyphBenchmark.exe'
$objects = Join-Path $cacheRoot 'CMakeFiles\MaiCordGlyphBenchmark.dir'
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

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
Push-Location $repository
try {
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
    cmake --build $cacheRoot --target MaiCordGlyphBenchmark
    if ($LASTEXITCODE -ne 0) { throw 'MaiCord glyph benchmark build failed.' }

    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) { throw "MaiCord glyph benchmark execution failed with code $LASTEXITCODE." }
        $values = @{}
        foreach ($line in $output) {
            if ($line -match '^([a-z_]+)=(\d+)$') { $values[$Matches[1]] = [uint64]$Matches[2] }
        }
        $required = @('atlas_full_resident_lookup_ns', 'atlas_full_missing_lookup_ns', 'atlas_full_churn_allocate_ns')
        if (-not $values.ContainsKey('glyph_dispatch_skipped')) {
            $required += @('glyph_dispatch_cpu_batch_us', 'glyph_dispatch_gpu_batch_us')
        }
        foreach ($name in $required) {
            if (-not $values.ContainsKey($name)) { throw "MaiCord glyph benchmark output lacks $name." }
        }
        $measurements += [pscustomobject]@{
            Sample = $sample
            CpuBatchUs = $values['glyph_dispatch_cpu_batch_us']
            GpuBatchUs = $values['glyph_dispatch_gpu_batch_us']
            ResidentLookupNs = $values['atlas_full_resident_lookup_ns']
            MissingLookupNs = $values['atlas_full_missing_lookup_ns']
            ChurnAllocateNs = $values['atlas_full_churn_allocate_ns']
            LinearResidentNs = $values['atlas_linear_resident_lookup_ns']
            LinearMissingNs = $values['atlas_linear_missing_lookup_ns']
            LinearChurnNs = $values['atlas_linear_churn_allocate_ns']
        }
    }
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    function Get-Median([string]$Property) { @($measurements.$Property | Sort-Object)[$middle] }
    $measurements | Format-Table -AutoSize
    $dispatch = $null -ne (Get-Median 'CpuBatchUs')
    [pscustomobject]@{
        Samples = $Samples
        CpuNanosecondsPerDispatchMedian = if ($dispatch) { [math]::Round((Get-Median 'CpuBatchUs') * 1000 / 256, 1) } else { 'skipped (native fonts)' }
        GpuNanosecondsPerDispatchMedian = if ($dispatch) { [math]::Round((Get-Median 'GpuBatchUs') * 1000 / 256, 1) } else { 'skipped (native fonts)' }
        FullAtlasResidentLookupNsMedian = Get-Median 'ResidentLookupNs'
        FullAtlasMissingLookupNsMedian = Get-Median 'MissingLookupNs'
        FullAtlasChurnAllocateNsMedian = Get-Median 'ChurnAllocateNs'
        LinearAtlasResidentLookupNsMedian = Get-Median 'LinearResidentNs'
        LinearAtlasMissingLookupNsMedian = Get-Median 'LinearMissingNs'
        LinearAtlasChurnAllocateNsMedian = Get-Median 'LinearChurnNs'
    } | Format-List
} finally {
    if ($locked) {
        foreach ($path in @($binary, $objects)) {
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
