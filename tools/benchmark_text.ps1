#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 5
)

# Builds the opt-in LTCG text benchmark (benchmarks/text_benchmark.cpp), runs it
# -Samples times and prints the median of each metric (tools/benchmark_session.ps1
# owns the build, mutex and cleanup). cycles_* metrics are thread cycles per
# uncached text_shape_utf8 call.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')

Invoke-BenchmarkTarget -Target 'MaiCordTextBenchmark' -Label 'MaiCord text benchmark' -AddInstallerToPath -Run {
    param($binary)
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
}
