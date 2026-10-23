#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 5
)

# Builds the opt-in LTCG text benchmark (benchmarks/text_benchmark.cpp) inside
# the single Release cache under the shared build mutex, runs it -Samples times
# and prints the median of each metric, then removes the binary and objects and
# restores the shipping configuration. cycles_* metrics are thread cycles per
# uncached text_shape_utf8 call.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$target = 'MaiCordTextBenchmark'
$binary = Join-Path $cacheRoot "$target.exe"
$objects = Join-Path $cacheRoot "CMakeFiles\$target.dir"
. (Join-Path $PSScriptRoot 'enter_build_environment.ps1') -AddInstallerToPath

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
Push-Location $repository
try {
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
    cmake --build $cacheRoot --target $target
    if ($LASTEXITCODE -ne 0) { throw 'MaiCord text benchmark build failed.' }

    $values = @{}
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) { throw "MaiCord text benchmark execution failed with code $LASTEXITCODE." }
        foreach ($line in $output) {
            if ($line -match '^([a-z_]+)=(\d+)') {
                if (-not $values.ContainsKey($Matches[1])) { $values[$Matches[1]] = @() }
                $values[$Matches[1]] += [uint64]$Matches[2]
            }
        }
    }
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    foreach ($name in $values.Keys | Sort-Object) {
        [pscustomobject]@{ Metric = $name; Median = @($values[$name] | Sort-Object)[$middle] }
    }
} finally {
    if ($locked) {
        & (Join-Path $PSScriptRoot 'clear_benchmark_outputs.ps1') -CacheRoot $cacheRoot -Paths @($binary, $objects)
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
