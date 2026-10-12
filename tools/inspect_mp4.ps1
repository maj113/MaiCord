param(
    [Parameter(Mandatory)][string]$InputFile,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Executable = 'D:\MaiCord-build\Release\MaiCordMp4Benchmark.exe',
    [ValidateRange(1, 15)][int]$TimeoutSeconds = 15
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'mp4_process.ps1')
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$inputPath = (Resolve-Path -LiteralPath $InputFile).Path
[void][IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($OutputDirectory))
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
foreach ($mf in @('0', '1')) {
    $backend = if ($mf -eq '0') { 'DirectGpu' } else { 'MfGpu' }
    $log = Join-Path $OutputDirectory "$([IO.Path]::GetFileName($inputPath))-$backend.txt"
    if (Test-Path -LiteralPath $log) { throw "Snapshot already exists: $log" }
    $output = Invoke-BoundedProcess -Path $Executable -Arguments @() -EnvironmentOverrides @{
        'MAICORD_MP4_BENCHMARK_INPUT' = $inputPath
        'MAICORD_MP4_SKIP_DEMUX_REPEAT' = '1'
        'MAICORD_MP4_GPU' = '1'
        'MAICORD_MP4_GPU_FIRST_ONLY' = '0'
        'MAICORD_MP4_GPU_NATIVE_IDR' = '0'
        'MAICORD_MP4_GPU_NATIVE_STREAM' = '0'
        'MAICORD_MP4_GPU_MF_BASELINE' = $mf
        'MAICORD_MP4_GPU_FFMPEG_BASELINE' = '0'
        'MAICORD_MP4_INSPECT' = '1'
        'MAICORD_MP4_DECODE_TRACE' = '0'
        'MAICORD_MP4_GPU_VERIFY' = '0'
        'MAICORD_MP4_GPU_DEBUG' = '0'
        'MAICORD_MP4_GPU_TRACE' = '0'
        'MAICORD_MP4_GPU_CPU_BASELINE' = '0'
        'MAICORD_MP4_GPU_WARM' = '0'
        'MAICORD_MP4_GPU_STRESS' = '0'
        'MAICORD_MP4_GPU_QUEUE_DEPTH' = '1'
        'MAICORD_MP4_LOW_LATENCY' = '0'
        'MAICORD_MP4_SHADER_READABLE' = '0'
        'MAICORD_MP4_CONVERTER_ONLY' = '0'
        'MAICORD_MP4_WORKER_THREADS' = '0'
    }
    # Keep inspection output separate from performance data: attribute queries
    # and debugger probes are not representative latency measurements.
    $snapshot = @($output -split '[\r\n]+' | Where-Object { $_ -match '^inspect_' })
    if (@($snapshot | Where-Object { $_ -eq 'inspect_transform_available=1' }).Count -ne 2) {
        throw "Transform snapshot failed for $backend"
    }
    $snapshot | Out-File -LiteralPath $log -Encoding utf8
    [pscustomobject]@{ Backend = $backend; Snapshot = $log }
}
