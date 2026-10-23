#requires -Version 5.1
param(
    # Print the bit-exactness digest matrix instead of timing.
    [switch]$Digests
)

# Builds the opt-in Opus benchmark (shipping code generation) inside the single
# Release cache under the shared build mutex, runs it, then removes the binary
# and objects and restores the shipping-only configuration.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$target = 'MaiCordOpusBenchmark'
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
    if ($LASTEXITCODE -ne 0) { throw 'Opus benchmark build failed.' }
    if ($Digests) { $env:MAICORD_OPUS_BENCH = 'digests' }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "Opus benchmark failed with code $LASTEXITCODE." }
} finally {
    Remove-Item Env:MAICORD_OPUS_BENCH -ErrorAction SilentlyContinue
    if ($locked) {
        & (Join-Path $PSScriptRoot 'clear_benchmark_outputs.ps1') -CacheRoot $cacheRoot -Paths @($binary, $objects)
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
