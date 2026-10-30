#requires -Version 5.1

# Builds the opt-in DAVE frame benchmark (shipping code generation) and runs it
# (tools/benchmark_session.ps1 owns the build, mutex and cleanup). It times Opus-sized
# seals, sender opens (audio and clear-range video) and per-sender setup and memory.
# The linker map gives per-function sizes: -KeepMap saves it before cleanup.
param(
    # Keep the linker map in this file instead of deleting it with the other outputs.
    [string]$KeepMap
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')

$map = Join-Path $cacheRoot 'MaiCordDaveFrameBenchmark.map'
Invoke-BenchmarkTarget -Target 'MaiCordDaveFrameBenchmark' -Label 'DAVE frame benchmark' -ExtraOutputs @($map) -Run {
    param($binary)
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "DAVE frame benchmark failed with code $LASTEXITCODE." }
    if ($KeepMap) { Copy-Item -Force -LiteralPath $map -Destination $KeepMap }
}
