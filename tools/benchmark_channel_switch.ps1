#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 5
)

# Builds the opt-in channel switch benchmark and reports per-metric medians
# (tools/benchmark_session.ps1 owns the build, mutex and cleanup).
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')

Invoke-BenchmarkTarget -Target 'MaiCordChannelSwitchBenchmark' -Label 'MaiCord channel switch benchmark' -Run {
    param($binary)
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
}
