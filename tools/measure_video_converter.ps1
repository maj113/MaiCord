param(
    [Parameter(Mandatory)][string[]]$InputFiles,
    [Parameter(Mandatory)][string]$PixelExecutable,
    [Parameter(Mandatory)][string]$TiledExecutable,
    [ValidateRange(1, 5)][int]$Repeats = 3,
    [ValidateRange(1, 15)][int]$TimeoutSeconds = 15
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'mp4_process.ps1')
$executables = @((Resolve-Path -LiteralPath $PixelExecutable).Path,
                 (Resolve-Path -LiteralPath $TiledExecutable).Path)
foreach ($file in $InputFiles) {
    $inputPath = (Resolve-Path -LiteralPath $file).Path
    for ($repeat = 0; $repeat -lt $Repeats; ++$repeat) {
        $order = if (($repeat % 2) -eq 0) { @(0, 1) } else { @(1, 0) }
        foreach ($backend in $order) {
            $output = Invoke-BoundedProcess -Path $executables[$backend] -Arguments @() -EnvironmentOverrides @{
                MAICORD_MP4_BENCHMARK_INPUT = $inputPath
                MAICORD_MP4_SKIP_DEMUX_REPEAT = '1'
                MAICORD_MP4_GPU = '1'
                MAICORD_MP4_GPU_FIRST_ONLY = '0'
                MAICORD_MP4_GPU_NATIVE_IDR = '0'
                MAICORD_MP4_GPU_NATIVE_STREAM = '0'
                MAICORD_MP4_GPU_VERIFY = '0'
                MAICORD_MP4_GPU_DEBUG = '0'
                MAICORD_MP4_GPU_CPU_BASELINE = '0'
                MAICORD_MP4_GPU_MF_BASELINE = '0'
                MAICORD_MP4_GPU_FFMPEG_BASELINE = '0'
                MAICORD_MP4_GPU_TRACE = '0'
                MAICORD_MP4_INSPECT = '0'
                MAICORD_MP4_DECODE_TRACE = '0'
                MAICORD_MP4_GPU_STRESS = '0'
                MAICORD_MP4_GPU_WARM = '0'
                MAICORD_MP4_LOW_LATENCY = '0'
                MAICORD_MP4_SHADER_READABLE = '0'
                MAICORD_MP4_CONVERTER_ONLY = '1'
                MAICORD_MP4_WORKER_THREADS = '0'
            }
            $row = [ordered]@{ Input = [IO.Path]::GetFileName($inputPath); Repeat = $repeat; Tiled = $backend }
            foreach ($line in $output -split '[\r\n]+') {
                if ($line -match '^(gpu_converter_[^=]+)=(\d+)$') { $row[$Matches[1]] = [long]$Matches[2] }
            }
            if ($row.gpu_converter_samples -ne 128) { throw "Incomplete timing probe: $output" }
            [pscustomobject]$row
        }
    }
}
