param(
    [Parameter(Mandatory)][string]$InputFile,
    [ValidateSet('Native','Mft','Ffmpeg')][string]$Backend = 'Native',
    [ValidateSet('Playback','HoldFirst')][string]$Mode = 'Playback',
    [string]$Executable = 'D:/MaiCord-build/Release/MaiCordMp4Benchmark.exe',
    [switch]$AmdTelemetry
)
$ErrorActionPreference = 'Stop'
$info = [Diagnostics.ProcessStartInfo]::new((Resolve-Path -LiteralPath $Executable).Path)
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$settings = @{
    MAICORD_MP4_BENCHMARK_INPUT = (Resolve-Path -LiteralPath $InputFile).Path
    MAICORD_MP4_SKIP_DEMUX_REPEAT = '1'; MAICORD_MP4_GPU = '1'
    MAICORD_MP4_GPU_PACED = [string][int]($Mode -eq 'Playback')
    MAICORD_MP4_GPU_IDLE_AFTER_FRAME = [string][int]($Mode -eq 'HoldFirst')
    MAICORD_MP4_GPU_NATIVE_STREAM = [string][int]($Backend -eq 'Native')
    MAICORD_MP4_GPU_FFMPEG_BASELINE = [string][int]($Backend -eq 'Ffmpeg')
    MAICORD_MP4_GPU_VERIFY = '0'; MAICORD_MP4_GPU_DEBUG = '0'; MAICORD_MP4_GPU_STRESS = '0'
    MAICORD_MP4_GPU_FIRST_ONLY = [string][int]($Mode -eq 'HoldFirst'); MAICORD_MP4_GPU_NATIVE_IDR = '0'
    MAICORD_MP4_GPU_CPU_BASELINE = '0'; MAICORD_MP4_GPU_MF_BASELINE = '0'
    MAICORD_MP4_GPU_QUEUE_DEPTH = '1'; MAICORD_MP4_GPU_WARM = '0'
    MAICORD_MP4_GPU_TRACE = '0'; MAICORD_MP4_INSPECT = '0'; MAICORD_MP4_DECODE_TRACE = '0'
    MAICORD_MP4_LOW_LATENCY = '0'; MAICORD_MP4_CONVERTER_ONLY = '0'
}
foreach ($name in $settings.Keys) { $info.Environment[$name] = $settings[$name] }
$child = [Diagnostics.Process]::new()
$child.StartInfo = $info
$started = $false
$telemetry = $null
$hardware = [Collections.Generic.List[object]]::new()
try {
    if ($AmdTelemetry) {
        if (-not ('AmdGpuTelemetry' -as [type])) { Add-Type -Path "$PSScriptRoot/diagnostics/AmdGpuTelemetry.cs" }
        $telemetry = [AmdGpuTelemetry]::new()
        $hardware.Add([pscustomobject]@{ Phase='Before'; Time=[DateTime]::UtcNow; Values=$telemetry.Read() })
    }
    $started = $child.Start()
    if (-not $started) { throw 'Cannot start paced playback probe.' }
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    $stdout = $child.StandardOutput.ReadToEndAsync()
    $stderr = $child.StandardError.ReadToEndAsync()
    # PDH expands wildcard instances when opening the query. Let the child
    # create its GPU context first or a new process can be silently omitted.
    Start-Sleep -Milliseconds 750
    $samples = Get-Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -MaxSamples 5 -ErrorAction SilentlyContinue | ForEach-Object {
        if ($telemetry) { $hardware.Add([pscustomobject]@{ Phase='Playback'; Time=[DateTime]::UtcNow; Values=$telemetry.Read() }) }
        $_
    }
    $remaining = [Math]::Max(0, 15000 - [int]$deadline.ElapsedMilliseconds)
    if (-not $child.WaitForExit($remaining)) { throw 'Paced playback exceeded 15 seconds.' }
    $output = $stdout.GetAwaiter().GetResult()
    if ($child.ExitCode -ne 0) { throw "Probe failed: $($stderr.GetAwaiter().GetResult()) $output" }
    $engines = @($samples.CounterSamples | Where-Object { $_.InstanceName -match "pid_$($child.Id)_" } |
        Group-Object InstanceName | ForEach-Object {
            [pscustomobject]@{ Engine=$_.Name; Samples=@($_.Group | Select-Object CookedValue,RawValue,SecondValue,Timestamp,Status) }
        })
    if ($engines.Count -eq 0) { throw 'No GPU engine samples: do not interpret missing counters as zero usage.' }
    [pscustomobject]@{ Backend=$Backend; Mode=$Mode; ProcessId=$child.Id; Engines=$engines; Hardware=$hardware.ToArray(); Metrics=$output }
} finally {
    if ($started -and -not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
    $child.Dispose()
    if ($telemetry) { $telemetry.Dispose() }
}
