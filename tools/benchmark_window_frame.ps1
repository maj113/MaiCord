#requires -Version 5.1
# Builds the opt-in window frame benchmark (benchmarks/window_frame_benchmark.cpp)
# inside the single Release cache under the shared build mutex, runs it once,
# then removes the binary and objects and restores the shipping configuration.
# Its windows open behind every other window and never activate.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$target = 'MaiCordWindowFrameBenchmark'
$binary = Join-Path $cacheRoot "$target.exe"
$objects = Join-Path $cacheRoot "CMakeFiles\$target.dir"
. (Join-Path $PSScriptRoot 'enter_build_environment.ps1')

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
Push-Location $repository
try {
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
    cmake --build $cacheRoot --target $target
    if ($LASTEXITCODE -ne 0) { throw 'Window frame benchmark build failed.' }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "Window frame benchmark failed with code $LASTEXITCODE." }
} finally {
    if ($locked) {
        & (Join-Path $PSScriptRoot 'clear_benchmark_outputs.ps1') -CacheRoot $cacheRoot -Paths @($binary, $objects)
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
