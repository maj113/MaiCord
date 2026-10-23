#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 5
)

# Builds the opt-in channel switch benchmark inside the single Release cache
# under the shared build mutex, reports per-metric medians, then removes the
# binary and objects and restores the shipping-only configuration.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$binary = Join-Path $cacheRoot 'MaiCordChannelSwitchBenchmark.exe'
$objects = Join-Path $cacheRoot 'CMakeFiles\MaiCordChannelSwitchBenchmark.dir'
. (Join-Path $PSScriptRoot 'enter_build_environment.ps1')

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
Push-Location $repository
try {
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
    cmake --build $cacheRoot --target MaiCordChannelSwitchBenchmark
    if ($LASTEXITCODE -ne 0) { throw 'MaiCord channel switch benchmark build failed.' }

    $values = @{}
    $units = @{}
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) { throw "MaiCord channel switch benchmark failed with code $LASTEXITCODE." }
        foreach ($line in $output) {
            if ($line -notmatch '^(\w+)=(\d+) (\w+)$') { throw "Malformed benchmark output: $line" }
            if (-not $values.ContainsKey($Matches[1])) { $values[$Matches[1]] = @() }
            $values[$Matches[1]] += [uint64]$Matches[2]
            $units[$Matches[1]] = $Matches[3]
        }
    }
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    $values.Keys | Sort-Object | ForEach-Object {
        [pscustomobject]@{ Metric = $_; Median = @($values[$_] | Sort-Object)[$middle]; Unit = $units[$_] }
    } | Format-Table -AutoSize
} finally {
    if ($locked) {
        & (Join-Path $PSScriptRoot 'clear_benchmark_outputs.ps1') -CacheRoot $cacheRoot -Paths @($binary, $objects)
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
