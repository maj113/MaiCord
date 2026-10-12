param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [ValidateRange(1, 100)]
    [int]$Samples = 7
)

$ErrorActionPreference = 'Stop'
$resolved = (Resolve-Path -LiteralPath $Executable).Path
$logPath = Join-Path $env:LOCALAPPDATA 'MaiCord\logs\maicord.log'
$previousMetrics = [Environment]::GetEnvironmentVariable('MAICORD_GPU_METRICS', 'Process')

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class MaiCordGpuBenchmarkWindow {
    [DllImport("user32.dll")]
    public static extern bool MoveWindow(IntPtr window, int x, int y, int width, int height,
                                         bool repaint);
}
'@

$sizes = @(
    @(480, 600), @(640, 480), @(800, 700), @(1024, 640), @(1280, 800),
    @(520, 420), @(1440, 900), @(960, 720), @(400, 320), @(1280, 800)
)

$measurements = @()
try {
    $env:MAICORD_GPU_METRICS = '1'
    for ($sample = 1; $sample -le $Samples; $sample++) {
        # This benchmark needs real Present/resize work, so visible windows are intentional.
        $process = Start-Process -FilePath $resolved -PassThru -WindowStyle Normal
        try {
            $null = $process.WaitForInputIdle(5000)
            $process.Refresh()
            if ($process.MainWindowHandle -eq 0) {
                throw 'MaiCord did not create a top-level window.'
            }
            foreach ($size in $sizes) {
                if (-not [MaiCordGpuBenchmarkWindow]::MoveWindow(
                    $process.MainWindowHandle, 40, 40, $size[0], $size[1], $true)) {
                    throw 'MoveWindow failed during GPU benchmark.'
                }
                Start-Sleep -Milliseconds 40
            }
        } finally {
            if (-not $process.HasExited) {
                $null = $process.CloseMainWindow()
                if (-not $process.WaitForExit(2000)) {
                    $process.Kill()
                    $process.WaitForExit()
                }
            }
            $process.Dispose()
        }

        $tail = Get-Content -LiteralPath $logPath -Tail 48
        function Read-MaiCordMetric([string]$name) {
            $match = $tail | Select-String -Pattern "$name=(\d+)" | Select-Object -Last 1
            if (-not $match) {
                throw "Missing GPU benchmark metric: $name"
            }
            return [uint64]$match.Matches[0].Groups[1].Value
        }
        $enabled = Read-MaiCordMetric 'gpu_timestamps_enabled'
        $gpuSamples = Read-MaiCordMetric 'gpu_timestamp_samples'
        if ($enabled -ne 1 -or $gpuSamples -eq 0) {
            throw 'GPU timestamp collection was not active or produced no samples.'
        }
        $measurements += [pscustomobject]@{
            Sample = $sample
            GpuAverageUs = Read-MaiCordMetric 'gpu_paint_average_us'
            GpuMaximumUs = Read-MaiCordMetric 'gpu_paint_max_us'
            GpuSamples = $gpuSamples
            GpuSkipped = Read-MaiCordMetric 'gpu_timestamp_skipped'
            GpuDisjoint = Read-MaiCordMetric 'gpu_timestamp_disjoint'
            GpuErrors = Read-MaiCordMetric 'gpu_timestamp_errors'
            CpuAverageUs = Read-MaiCordMetric 'paint_cpu_average_us'
            CpuMaximumUs = Read-MaiCordMetric 'paint_cpu_max_us'
        }
    }
} finally {
    if ($null -eq $previousMetrics) {
        Remove-Item Env:MAICORD_GPU_METRICS -ErrorAction SilentlyContinue
    } else {
        $env:MAICORD_GPU_METRICS = $previousMetrics
    }
}

$gpuAverage = @($measurements.GpuAverageUs | Sort-Object)
$cpuAverage = @($measurements.CpuAverageUs | Sort-Object)
$middle = [int][math]::Floor(($Samples - 1) * 0.5)
$measurements | Format-Table -AutoSize
[pscustomobject]@{
    Samples = $Samples
    GpuAverageUsMedian = $gpuAverage[$middle]
    GpuMaximumUsMaximum = ($measurements.GpuMaximumUs | Measure-Object -Maximum).Maximum
    GpuTimestampSamplesTotal = ($measurements.GpuSamples | Measure-Object -Sum).Sum
    GpuSkippedTotal = ($measurements.GpuSkipped | Measure-Object -Sum).Sum
    GpuDisjointTotal = ($measurements.GpuDisjoint | Measure-Object -Sum).Sum
    GpuErrorsTotal = ($measurements.GpuErrors | Measure-Object -Sum).Sum
    CpuAverageUsMedian = $cpuAverage[$middle]
    CpuMaximumUsMaximum = ($measurements.CpuMaximumUs | Measure-Object -Maximum).Maximum
} | Format-List
