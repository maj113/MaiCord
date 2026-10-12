param(
    [Parameter(Mandatory)][string[]]$InputFiles,
    [string]$Executable = 'D:\MaiCord-build\Release\MaiCordMp4Benchmark.exe',
    [ValidateRange(1, 5)][int]$Repeats = 3,
    [ValidateRange(1, 15)][int]$TimeoutSeconds = 15,
    [ValidateSet(1, 2, 4, 8)][int]$QueueDepth = 1,
    [switch]$LowLatency,
    [switch]$WarmRenderer,
    [switch]$MfBaseline,
    [switch]$FfmpegBaseline,
    [switch]$NativeGpu,
    [ValidateSet(0, 2, 4, 8)][int]$WorkerThreads = 0,
    [string]$TraceDirectory = ''
)
$ErrorActionPreference = 'Stop'
if ($MfBaseline -and $FfmpegBaseline) { throw 'Select one comparison backend.' }
if ($NativeGpu -and ($MfBaseline -or $FfmpegBaseline)) { throw 'Select one comparison backend.' }
. (Join-Path $PSScriptRoot 'mp4_process.ps1')
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if ($TraceDirectory) {
    if ($QueueDepth -ne 1) { throw 'Frame traces require queue depth one.' }
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($TraceDirectory))
    $TraceDirectory = (Resolve-Path -LiteralPath $TraceDirectory).Path
}
foreach ($inputFile in $InputFiles) {
    $inputPath = (Resolve-Path -LiteralPath $inputFile).Path
    for ($repeat = 0; $repeat -lt $Repeats; ++$repeat) {
        # Alternate order in separate fresh processes; never benchmark while
        # correctness readbacks, the D3D debug layer or another decoder runs.
        $order = if (($repeat % 2) -eq 0) { @('0', '1') } else { @('1', '0') }
        foreach ($cpu in $order) {
            $output = Invoke-BoundedProcess -Path $Executable -Arguments @() -EnvironmentOverrides @{
                'MAICORD_MP4_BENCHMARK_INPUT' = $inputPath
                'MAICORD_MP4_SKIP_DEMUX_REPEAT' = '1'
                'MAICORD_MP4_GPU' = '1'
                'MAICORD_MP4_GPU_FIRST_ONLY' = '0'
                'MAICORD_MP4_GPU_NATIVE_IDR' = '0'
                'MAICORD_MP4_GPU_NATIVE_STREAM' = if ($NativeGpu) { $cpu } else { '0' }
                'MAICORD_MP4_GPU_VERIFY' = '0'
                'MAICORD_MP4_GPU_DEBUG' = '0'
                'MAICORD_MP4_GPU_CPU_BASELINE' = if ($MfBaseline -or $FfmpegBaseline -or $NativeGpu) { '0' } else { $cpu }
                'MAICORD_MP4_GPU_MF_BASELINE' = if ($MfBaseline) { $cpu } else { '0' }
                'MAICORD_MP4_GPU_FFMPEG_BASELINE' = if ($FfmpegBaseline) { $cpu } else { '0' }
                'MAICORD_MP4_GPU_STRESS' = '0'
                'MAICORD_MP4_GPU_QUEUE_DEPTH' = [string]$QueueDepth
                'MAICORD_MP4_LOW_LATENCY' = [string][int][bool]$LowLatency
                'MAICORD_MP4_GPU_WARM' = [string][int][bool]$WarmRenderer
                'MAICORD_MP4_SHADER_READABLE' = '0'
                'MAICORD_MP4_CONVERTER_ONLY' = '0'
                'MAICORD_MP4_WORKER_THREADS' = [string]$WorkerThreads
                'MAICORD_MP4_GPU_TRACE' = [string][int][bool]$TraceDirectory
                'MAICORD_MP4_INSPECT' = '0'
                'MAICORD_MP4_DECODE_TRACE' = '0'
            }
            $backend = if ($cpu -eq '0') { 'DirectGpu' } elseif ($MfBaseline) { 'MfGpu' } elseif ($FfmpegBaseline) { 'FfmpegGpu' } elseif ($NativeGpu) { 'NativeGpu' } else { 'DirectCpu' }
            $row = [ordered]@{ Input = [IO.Path]::GetFileName($inputPath); Repeat = $repeat; Backend = $backend }
            foreach ($line in $output -split '[\r\n]+') {
                if ($line -match '^(gpu_[^=]+)=(\d+)$') { $row[$Matches[1]] = [long]$Matches[2] }
            }
            if ($TraceDirectory) {
                $traceRows = [Collections.Generic.List[object]]::new()
                $trace = $null
                foreach ($line in $output -split '[\r\n]+') {
                    if ($line -match '^trace_([^=]+)=(\d+)$') {
                        if ($Matches[1] -eq 'frame') {
                            if ($trace) { $traceRows.Add([pscustomobject]$trace) }
                            $trace = [ordered]@{ Frame = [long]$Matches[2] }
                        } else { $trace[$Matches[1]] = [long]$Matches[2] }
                    }
                }
                if ($trace) { $traceRows.Add([pscustomobject]$trace) }
                if ($traceRows.Count -ne $row.gpu_frames) { throw 'Incomplete frame trace.' }
                $traceRows | Export-Csv (Join-Path $TraceDirectory "$($row.Input)-$backend-$repeat.csv") -NoTypeInformation
            }
            [pscustomobject]$row
        }
    }
}
