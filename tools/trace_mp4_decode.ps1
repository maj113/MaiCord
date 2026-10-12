param(
    [Parameter(Mandatory)][string]$InputFile,
    [string]$Executable = 'D:/MaiCord-build/Release/MaiCordMp4Benchmark.exe',
    [ValidateRange(1, 15)][int]$TimeoutSeconds = 15,
    [switch]$WarmRenderer
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'mp4_process.ps1')
$output = Invoke-BoundedProcess -Path (Resolve-Path -LiteralPath $Executable).Path -Arguments @() -EnvironmentOverrides @{
    MAICORD_MP4_BENCHMARK_INPUT = (Resolve-Path -LiteralPath $InputFile).Path
    MAICORD_MP4_SKIP_DEMUX_REPEAT = '1'
    MAICORD_MP4_GPU = '1'
    MAICORD_MP4_GPU_FIRST_ONLY = '0'
    MAICORD_MP4_GPU_NATIVE_IDR = '0'
    MAICORD_MP4_GPU_NATIVE_STREAM = '0'
    MAICORD_MP4_DECODE_TRACE = '1'
    MAICORD_MP4_GPU_WARM = [string][int][bool]$WarmRenderer
    MAICORD_MP4_GPU_VERIFY = '0'
    MAICORD_MP4_GPU_DEBUG = '0'
    MAICORD_MP4_GPU_MF_BASELINE = '0'
    MAICORD_MP4_GPU_FFMPEG_BASELINE = '0'
    MAICORD_MP4_GPU_CPU_BASELINE = '0'
    MAICORD_MP4_GPU_STRESS = '0'
    MAICORD_MP4_GPU_QUEUE_DEPTH = '1'
    MAICORD_MP4_GPU_TRACE = '0'
    MAICORD_MP4_INSPECT = '0'
    MAICORD_MP4_LOW_LATENCY = '0'
    MAICORD_MP4_SHADER_READABLE = '0'
    MAICORD_MP4_CONVERTER_ONLY = '0'
    MAICORD_MP4_WORKER_THREADS = '0'
}
if ($output -notmatch '(?m)^decode_trace_dropped=0\r?$') { throw 'Decode trace overflowed.' }
$names = @('ProcessOutput', 'OutputType', 'OutputSample', 'InputPrepare', 'ProcessInput',
           'EndOfStream', 'Drain', 'Reset', 'ExtractFrame')
$row = $null
foreach ($line in $output -split '[\r\n]+') {
    if ($line -match '^decode_event=(\d+)$') {
        if ($row) { [pscustomobject]$row }
        $row = [ordered]@{ Event = [int]$Matches[1] }
    } elseif ($row -and $line -match '^decode_(operation|sample|begin_us|duration_us|status)=(\d+)$') {
        $row[$Matches[1]] = if ($Matches[1] -eq 'operation') { $names[[int]$Matches[2]] }
            elseif ($Matches[1] -eq 'status') { '0x{0:X8}' -f [long]$Matches[2] }
            else { [long]$Matches[2] }
    }
}
if ($row) { [pscustomobject]$row }
