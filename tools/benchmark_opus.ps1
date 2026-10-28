#requires -Version 5.1
param(
    # Print the bit-exactness digest matrix instead of timing.
    [switch]$Digests
)

# Builds the opt-in Opus benchmark (shipping code generation) and runs it
# (tools/benchmark_session.ps1 owns the build, mutex and cleanup).
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')

Invoke-BenchmarkTarget -Target 'MaiCordOpusBenchmark' -Label 'Opus benchmark' -Run {
    param($binary)
    if ($Digests) { $env:MAICORD_OPUS_BENCH = 'digests' }
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "Opus benchmark failed with code $LASTEXITCODE." }
} -Cleanup {
    Remove-Item Env:MAICORD_OPUS_BENCH -ErrorAction SilentlyContinue
}
