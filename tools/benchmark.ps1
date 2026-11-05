#requires -Version 5.1
param(
    # A target of cmake/benchmarks.cmake, e.g. MaiCordCoreBenchmark. The names without a tools/benchmark_*.ps1 of their own run here.
    [Parameter(Mandatory = $true)]
    [string]$Target,
    [ValidateRange(1, 100)]
    [int]$Samples = 1
)

# Builds one opt-in benchmark (tools/benchmark_session.ps1 owns the build, mutex and cleanup), audits its image and prints what
# each run writes: lines of name=value that the benchmark's source defines. Drivers with their own parsing and medians
# (benchmark_text.ps1, benchmark_glyph.ps1, ...) stay the way to run those targets.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')

if ($Target -notmatch '^MaiCord\w+(Benchmark|Probe)$') { throw "$Target is not a benchmark target of cmake/benchmarks.cmake." }
Invoke-BenchmarkTarget -Target $Target -Label $Target -Run {
    param($binary)
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary
    for ($sample = 1; $sample -le $Samples; $sample++) {
        if ($Samples -gt 1) { Write-Host "--- sample $sample of $Samples" }
        & $binary
        if ($LASTEXITCODE -ne 0) { throw "$Target failed with code $LASTEXITCODE." }
    }
}
