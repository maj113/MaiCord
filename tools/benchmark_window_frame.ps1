#requires -Version 5.1
# Builds the opt-in window frame benchmark (benchmarks/window_frame_benchmark.cpp)
# and runs it once (tools/benchmark_session.ps1 owns the build, mutex and cleanup).
# Its windows open behind every other window and never activate.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')

Invoke-BenchmarkTarget -Target 'MaiCordWindowFrameBenchmark' -Label 'Window frame benchmark' -Run {
    param($binary)
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "Window frame benchmark failed with code $LASTEXITCODE." }
}
