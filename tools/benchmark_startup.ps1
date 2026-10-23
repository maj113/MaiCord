#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 15,
    # When non-zero, one extra launch idles this long after the 5 s settle
    # window and reports main-thread wakes per minute from the shutdown log.
    [ValidateRange(0, 3600)]
    [int]$IdleSeconds = 0,
    [ValidateSet('Normal', 'Minimized')]
    [string]$IdleWindowStyle = 'Minimized'
)

# Builds the client inside the single Release cache under the shared build
# mutex and launches it against a temporary, account-free LOCALAPPDATA that is
# removed afterwards. The mutex stays held so no build relinks a running image.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$binary = Join-Path $cacheRoot 'MaiCord.exe'
. (Join-Path $PSScriptRoot 'enter_build_environment.ps1')

function Read-StartupLog([string]$Path) {
    $values = @{}
    if (-not (Test-Path -LiteralPath $Path)) { return $values }
    $lines = @(Get-Content -LiteralPath $Path)
    $start = 0
    for ($index = $lines.Count - 1; $index -ge 0; $index--) {
        if ($lines[$index] -match '\[startup\] process_entry') { $start = $index; break }
    }
    for ($index = $start; $index -lt $lines.Count; $index++) {
        $line = $lines[$index]
        if ($line -match '\[load_trace\] ([a-z_]+) .*startup_us=(\d+)') {
            if (-not $values.ContainsKey($Matches[1])) { $values[$Matches[1]] = [uint64]$Matches[2] }
        } elseif ($line -match '\] ([a-z_]+)=(\d+)$') {
            $values[$Matches[1]] = [uint64]$Matches[2]
        }
    }
    return $values
}

function Invoke-Client([string]$Style, [int]$HoldMilliseconds, [string]$Log) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $binary -PassThru -WindowStyle $Style
    try {
        $null = $process.WaitForInputIdle(5000)
        $inputIdle = $timer.Elapsed.TotalMilliseconds
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        while ($deadline.ElapsedMilliseconds -lt 5000 -and -not $process.HasExited) {
            if ((Read-StartupLog $Log).ContainsKey('first_present_us')) { break }
            Start-Sleep -Milliseconds 50
        }
        if ($HoldMilliseconds -gt 0) { Start-Sleep -Milliseconds $HoldMilliseconds }
        $process.Refresh()
        $memory = @($process.WorkingSet64, $process.PrivateMemorySize64)
    } finally {
        if (-not $process.HasExited) {
            $null = $process.CloseMainWindow()
            if (-not $process.WaitForExit(5000)) {
                $process.Kill()
                $process.WaitForExit()
            }
        }
        $process.Dispose()
    }
    return @($inputIdle, $memory)
}

function Get-Median([object[]]$Values) {
    $ordered = @($Values | Where-Object { $null -ne $_ } | Sort-Object)
    if (-not $ordered.Count) { return $null }
    return $ordered[[int][math]::Floor(($ordered.Count - 1) * 0.5)]
}

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
$profile = Join-Path ([IO.Path]::GetTempPath()) ('maicord-startup-' + [guid]::NewGuid().ToString('N'))
$previous = @{}
foreach ($name in @('LOCALAPPDATA', 'TEMP', 'TMP')) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
Push-Location $repository
try {
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    foreach ($client in Get-Process -Name MaiCord -ErrorAction SilentlyContinue) {
        if ($client.Path -and [IO.Path]::GetFullPath($client.Path).Equals($binary, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Close MaiCord before benchmarking $binary."
        }
    }
    cmake --preset x64-release -DMAICORD_TEST_AUDIO_PLAYBACK=OFF -DMAICORD_TEST_PROFILE_DIRECTORY= -DMAICORD_TEST_PROFILE_NAMES= | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Release configuration failed.' }
    cmake --build $cacheRoot --target MaiCord MaiCordAccount
    if ($LASTEXITCODE -ne 0) { throw 'MaiCord Release build failed.' }

    [void](New-Item -ItemType Directory -Path $profile)
    $temporary = Join-Path $profile 'Temp'
    [void](New-Item -ItemType Directory -Path $temporary)
    $env:LOCALAPPDATA = $profile
    $env:TEMP = $temporary
    $env:TMP = $temporary
    $log = Join-Path $profile 'MaiCord\logs\maicord.log'

    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        # A visible top-level window is required for a real first Present.
        $result = Invoke-Client 'Normal' 0 $log
        $values = Read-StartupLog $log
        $us = { param($key) if ($values.ContainsKey($key)) { [math]::Round($values[$key] / 1000.0, 3) } else { $null } }
        $measurements += [pscustomobject]@{
            Sample = $sample
            InputIdleMs = [math]::Round($result[0], 3)
            AccountStartMs = & $us 'account_start'
            WindowMs = & $us 'window_created'
            RendererMs = & $us 'renderer_ready'
            UiMs = & $us 'ui_ready'
            ShownMs = & $us 'window_shown'
            LoopMs = & $us 'loop_entered'
            FirstPresentMs = & $us 'first_present_us'
            WorkingSetMiB = [math]::Round($result[1][0] / 1MB, 3)
            PrivateMiB = [math]::Round($result[1][1] / 1MB, 3)
        }
    }
    $measurements | Format-Table -AutoSize
    $summary = [ordered]@{ Samples = $Samples }
    foreach ($property in @('InputIdleMs', 'AccountStartMs', 'WindowMs', 'RendererMs', 'UiMs', 'ShownMs', 'LoopMs', 'FirstPresentMs', 'WorkingSetMiB', 'PrivateMiB')) {
        $summary[$property + 'Median'] = Get-Median @($measurements.$property)
    }
    $summary['InputIdleP95Ms'] = @($measurements.InputIdleMs | Sort-Object)[[int][math]::Floor(($Samples - 1) * 0.95)]

    if ($IdleSeconds -gt 0) {
        $null = Invoke-Client $IdleWindowStyle (5000 + $IdleSeconds * 1000) $log
        $values = Read-StartupLog $log
        $window = if ($values.ContainsKey('steady_wake_window_ms')) { $values['steady_wake_window_ms'] } else { 0 }
        if ($window -eq 0) { throw 'The idle launch did not log its steady wake window.' }
        $perMinute = { param($key) [math]::Round($values[$key] * 60000.0 / $window, 2) }
        $summary['IdleWindowStyle'] = $IdleWindowStyle
        $summary['IdleWindowSeconds'] = [math]::Round($window / 1000.0, 1)
        $summary['IdleWakesPerMinute'] = & $perMinute 'steady_wakes'
        $summary['IdleTimeoutWakesPerMinute'] = & $perMinute 'steady_wake_timeouts'
        $summary['IdleMessageWakesPerMinute'] = & $perMinute 'steady_wake_messages'
        $summary['IdleHandleWakesPerMinute'] = & $perMinute 'steady_wake_handles'
        $summary['IdleFrameWakesPerMinute'] = & $perMinute 'steady_wake_frames'
    }
    [pscustomobject]$summary | Format-List
} finally {
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    if (Test-Path -LiteralPath $profile) { Remove-Item -LiteralPath $profile -Recurse -Force }
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
    Pop-Location
}
