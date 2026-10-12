param(
    [Parameter(Mandatory)][string[]]$InputFiles,
    [string]$Executable = 'D:/MaiCord-build/Release/MaiCordMp4Benchmark.exe',
    [ValidateSet('NativeIdr','NativeGpu','DirectGpu','MfGpu','FfmpegGpu')]
    [string[]]$Backends = @('NativeIdr','DirectGpu','MfGpu','FfmpegGpu'),
    [ValidateRange(1,5)][int]$Repeats = 5,
    [ValidateRange(1,15)][int]$TimeoutSeconds = 15,
    [switch]$WarmRenderer,
    [switch]$Verify
)
$ErrorActionPreference = 'Stop'
if ($Verify -and $WarmRenderer) { throw 'Correctness verification is separate from warm performance runs.' }
. (Join-Path $PSScriptRoot 'mp4_process.ps1')
$Executable = (Resolve-Path -LiteralPath $Executable).Path
foreach ($inputFile in $InputFiles) {
    $inputPath = (Resolve-Path -LiteralPath $inputFile).Path
    for ($repeat = 0; $repeat -lt $Repeats; ++$repeat) {
        # Rotate backend order; identical first-completed-frame boundary, no
        # full-clip throughput claim from the incomplete native IDR probe.
        for ($slot = 0; $slot -lt $Backends.Count; ++$slot) {
            $backend = $Backends[($slot + $repeat) % $Backends.Count]
            $output = Invoke-BoundedProcess -Path $Executable -Arguments @() -EnvironmentOverrides @{
                MAICORD_MP4_BENCHMARK_INPUT = $inputPath
                MAICORD_MP4_SKIP_DEMUX_REPEAT = '1'
                MAICORD_MP4_GPU = '1'
                MAICORD_MP4_GPU_FIRST_ONLY = '1'
                MAICORD_MP4_GPU_NATIVE_IDR = [string][int]($backend -eq 'NativeIdr')
                MAICORD_MP4_GPU_NATIVE_STREAM = [string][int]($backend -eq 'NativeGpu')
                MAICORD_MP4_GPU_MF_BASELINE = [string][int]($backend -eq 'MfGpu')
                MAICORD_MP4_GPU_FFMPEG_BASELINE = [string][int]($backend -eq 'FfmpegGpu')
                MAICORD_MP4_GPU_CPU_BASELINE = '0'
                MAICORD_MP4_GPU_WARM = [string][int][bool]$WarmRenderer
                MAICORD_MP4_GPU_VERIFY = [string][int][bool]$Verify
                MAICORD_MP4_GPU_DEBUG = [string][int][bool]$Verify
                MAICORD_MP4_GPU_STRESS = '0'
                MAICORD_MP4_GPU_QUEUE_DEPTH = '1'
                MAICORD_MP4_GPU_TRACE = '0'
                MAICORD_MP4_DECODE_TRACE = '0'
                MAICORD_MP4_INSPECT = '0'
                MAICORD_MP4_LOW_LATENCY = '0'
                MAICORD_MP4_SHADER_READABLE = '0'
                MAICORD_MP4_CONVERTER_ONLY = '0'
                MAICORD_MP4_WORKER_THREADS = '0'
            }
            $row = [ordered]@{Input=[IO.Path]::GetFileName($inputPath);Backend=$backend;Repeat=$repeat}
            foreach ($line in $output -split '[\r\n]+') {
                if ($line -match '^((?:gpu_|h264_state_)[^=]+)=(\d+)$') { $row[$Matches[1]] = [long]$Matches[2] }
            }
            if ($row.gpu_frames -ne 1 -or $row.gpu_first_only -ne 1) { throw 'Invalid first-picture probe result.' }
            [pscustomobject]$row
        }
    }
}
