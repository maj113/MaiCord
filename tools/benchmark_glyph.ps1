#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 7
)

# Builds the opt-in glyph benchmark (tools/benchmark_session.ps1 owns the build,
# mutex and cleanup) and prints the per-sample and median metrics.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')

Invoke-BenchmarkTarget -Target 'MaiCordGlyphBenchmark' -Label 'MaiCord glyph benchmark' -Run {
    param($binary)
    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) { throw "MaiCord glyph benchmark execution failed with code $LASTEXITCODE." }
        $values = @{}
        foreach ($line in $output) {
            if ($line -match '^([a-z_]+)=(\d+)$') { $values[$Matches[1]] = [uint64]$Matches[2] }
        }
        $required = @('atlas_full_resident_lookup_ns', 'atlas_full_missing_lookup_ns', 'atlas_full_churn_allocate_ns',
            'glyph_dispatch_cpu_batch_us', 'glyph_dispatch_gpu_batch_us')
        foreach ($name in $required) {
            if (-not $values.ContainsKey($name)) { throw "MaiCord glyph benchmark output lacks $name." }
        }
        $measurements += [pscustomobject]@{
            Sample = $sample
            CpuBatchUs = $values['glyph_dispatch_cpu_batch_us']
            GpuBatchUs = $values['glyph_dispatch_gpu_batch_us']
            ResidentLookupNs = $values['atlas_full_resident_lookup_ns']
            MissingLookupNs = $values['atlas_full_missing_lookup_ns']
            ChurnAllocateNs = $values['atlas_full_churn_allocate_ns']
            LinearResidentNs = $values['atlas_linear_resident_lookup_ns']
            LinearMissingNs = $values['atlas_linear_missing_lookup_ns']
            LinearChurnNs = $values['atlas_linear_churn_allocate_ns']
        }
    }
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    function Get-Median([string]$Property) { @($measurements.$Property | Sort-Object)[$middle] }
    $measurements | Format-Table -AutoSize
    [pscustomobject]@{
        Samples = $Samples
        CpuNanosecondsPerDispatchMedian = [math]::Round((Get-Median 'CpuBatchUs') * 1000 / 256, 1)
        GpuNanosecondsPerDispatchMedian = [math]::Round((Get-Median 'GpuBatchUs') * 1000 / 256, 1)
        FullAtlasResidentLookupNsMedian = Get-Median 'ResidentLookupNs'
        FullAtlasMissingLookupNsMedian = Get-Median 'MissingLookupNs'
        FullAtlasChurnAllocateNsMedian = Get-Median 'ChurnAllocateNs'
        LinearAtlasResidentLookupNsMedian = Get-Median 'LinearResidentNs'
        LinearAtlasMissingLookupNsMedian = Get-Median 'LinearMissingNs'
        LinearAtlasChurnAllocateNsMedian = Get-Median 'LinearChurnNs'
    } | Format-List
}
