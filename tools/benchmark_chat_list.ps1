#requires -Version 5.1
param(
    # Items per channel, comma separated; BUI's sizes by default.
    [string]$Sizes = '100,10000,1000000,10000000',
    [ValidateRange(1, 20)]
    [int]$Reps = 3,
    # Recorded seconds of the scroll stage (the repaint stage runs half of it, at least two); the warm-up is two more.
    [double]$Seconds = 5,
    # A process is ended, printing how far it got, when it holds more than this above its starting working set...
    [int]$CapMb = 4096,
    # ...or runs longer than this.
    [int]$LimitSeconds = 150,
    # Where results.json and results.md go; the build directory stays clean.
    [string]$OutDirectory = (Join-Path ([IO.Path]::GetTempPath()) 'maicord-chat-list-bench'),
    # Run this image instead of building the target (the Linux runner's counterpart is tools/benchmark_chat_list.py).
    [string]$Binary,
    # One profiled run of the first size instead of the suite.
    [switch]$Profile,
    # Appended to every process's command line (for example "--marks 0").
    [string]$ExtraArguments = '',
    # Validation instead of measurement: every process runs on WARP with the Direct3D 11 debug layer (never on the
    # hardware adapter) and fails on any debug-layer message but NULL_TILE_MAPPING_ACCESS_WARNING. Needs the Graphics
    # Tools optional feature. Its frame figures are WARP's, not results.
    [switch]$Warp
)

# Builds MaiCordChatListBenchmark (tools/benchmark_session.ps1 owns the build, mutex and cleanup) and runs it
# once per process, library size and repetition, like BUI's bench/run.mjs: every run is one process printing one
# JSON line, and the median of each numeric field over the runs that finished goes to results.json and results.md in
# BUI's columns (docs: tools/perf/README.md, "Chat list benchmark").
$ErrorActionPreference = 'Stop'
$SizeList = @($Sizes -split ',' | ForEach-Object { [long]$_.Trim() })
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')

function Get-Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    return $sorted[[math]::Floor(($sorted.Count - 1) / 2)]
}

function Invoke-ChatListRun([string]$Image, [long]$Size) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Image
    $info.Arguments = "--n $Size --seconds $Seconds --cap-mb $CapMb --limit-seconds $LimitSeconds $(if ($Warp) { '--warp 1 ' })$ExtraArguments"
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $process = [Diagnostics.Process]::Start($info)
    $output = $process.StandardOutput.ReadToEndAsync()
    $errors = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit(($LimitSeconds + 120) * 1000)) { $process.Kill(); $process.WaitForExit() }
    $lines = $output.Result -split "`r?`n"
    $lines | Where-Object { $_.StartsWith('D3D11 ') } | ForEach-Object { Write-Host "  $_" }
    $line = ($lines | Where-Object { $_.StartsWith('{') } | Select-Object -First 1)
    if (-not $line) { return [pscustomobject]@{ failed = "no output (exit $($process.ExitCode))" } }
    $result = $line | ConvertFrom-Json
    if ($process.ExitCode -eq 40) { $result | Add-Member -NotePropertyName failed -NotePropertyValue 'debug layer messages' -Force }
    return $result
}

function Invoke-ChatListSuite([string]$Image) {
    # A validation run (-Warp) measures nothing, so it may share the machine.
    if (-not $Warp -and (Get-Process -Name MaiCord -ErrorAction SilentlyContinue)) {
        throw 'MaiCord.exe is running: close it before measuring (the benchmark shares the display and the CPU).'
    }
    [void](New-Item -ItemType Directory -Force -Path $OutDirectory)
    $results = New-Object Collections.Generic.List[object]
    $raw = New-Object Collections.Generic.List[object]
    $alive = $true
    foreach ($size in $SizeList) {
        if (-not $alive) {
            $results.Add([pscustomobject]@{ lib = 'MaiCord timeline'; n = $size; finished = 0; failed = 0; skipped = 'failed at a smaller size' })
            continue
        }
        $runs = @(); $fails = @()
        for ($i = 1; $i -le $Reps; $i++) {
            $run = Invoke-ChatListRun $Image $size
            if ($run.failed) {
                $fails += $run
                Write-Host ("MaiCord {0} run {1}: FAILED {2} in {3} after {4}s, {5} MB" -f $size, $i, $run.failed, $run.stage, $run.elapsed_s, $run.ram_above_data_mb)
                continue
            }
            $runs += $run; $raw.Add($run)
            Write-Host ("MaiCord {0} run {1}: p50 {2} ms, first {3} ms" -f $size, $i, $run.frame_ms_p50, $run.first_frame_ms)
        }
        $row = [ordered]@{ lib = 'MaiCord timeline'; n = $size; finished = $runs.Count; failed = $fails.Count; failures = $fails }
        if ($runs.Count) {
            foreach ($property in $runs[0].PSObject.Properties) {
                if ($property.Value -is [ValueType] -and -not $row.Contains($property.Name)) {
                    $values = @($runs | ForEach-Object { [double]$_.($property.Name) })
                    $row[$property.Name] = Get-Median $values
                    $row[$property.Name + '__min'] = ($values | Measure-Object -Minimum).Minimum
                    $row[$property.Name + '__max'] = ($values | Measure-Object -Maximum).Maximum
                }
            }
        }
        $results.Add([pscustomobject]$row)
        $alive = $runs.Count -gt 0
    }
    $results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutDirectory 'results.json') -Encoding UTF8
    $raw | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutDirectory 'results_raw.json') -Encoding UTF8

    $culture = [Globalization.CultureInfo]::InvariantCulture
    $columns = @(
        @('setup_ms', 'Setup ms'), @('first_frame_ms', 'First frame ms'), @('frame_ms_p50', 'Scroll p50 ms'), @('frame_ms_p95', 'p95'),
        @('frame_ms_p99', 'p99'), @('cpu_ms_per_frame', 'CPU ms/frame'), @('main_allocs_per_frame', 'Allocs/frame'),
        @('still_frame_ms_p50', 'Repaint p50 ms'), @('jump_ms_p50', 'Jump p50 ms'), @('ram_above_data_mb', 'RAM above data MB'),
        @('peak_above_data_mb', 'Peak MB'))
    $markdown = "| Library | Items | Finished | " + (($columns | ForEach-Object { $_[1] }) -join ' | ') + " |`n"
    $markdown += "|---|---:|---:|" + (($columns | ForEach-Object { '---:' }) -join '|') + "|`n"
    foreach ($row in $results) {
        $items = ([long]$row.n).ToString('N0', $culture)
        if ($row.skipped) { $markdown += "| $($row.lib) | $items | skipped ($($row.skipped)) |" + (' |' * $columns.Count) + "`n"; continue }
        if (-not $row.finished) {
            $first = $row.failures | Select-Object -First 1
            $markdown += "| $($row.lib) | $items | 0/${Reps}: $($first.failed) in $($first.stage) after $($first.elapsed_s) s, $($first.ram_above_data_mb) MB |" + (' |' * $columns.Count) + "`n"
            continue
        }
        $cells = foreach ($column in $columns) {
            $digits = if ($column[0] -match 'allocs|_mb') { 'F0' } else { 'F2' }
            ([double]$row.($column[0])).ToString($digits, $culture)
        }
        $markdown += "| $($row.lib) | $items | $($row.finished)/$Reps | " + ($cells -join ' | ') + " |`n"
    }
    Set-Content -LiteralPath (Join-Path $OutDirectory 'results.md') -Value $markdown -Encoding UTF8
    Write-Host $markdown
    Write-Host "Results: $OutDirectory"
}

# One sampled run (-Profile): the UI thread's instruction pointer at 1 kHz through the scroll stage, resolved against the
# linker map into functions by tools/perf/profile_chat_list.py. Its frame figures are not results (each sample suspends
# the thread).
function Invoke-ChatListProfile([string]$Image) {
    [void](New-Item -ItemType Directory -Force -Path $OutDirectory)
    $samples = Join-Path $OutDirectory 'profile-samples.txt'
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Image
    $info.Arguments = "--n $($SizeList[0]) --seconds $Seconds --profile 1"
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $process = [Diagnostics.Process]::Start($info)
    $output = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    Set-Content -LiteralPath $samples -Value $output -Encoding ASCII
    $map = Join-Path $cacheRoot 'MaiCordChatListBenchmark.map'
    if (Test-Path -LiteralPath $map) { Copy-Item -LiteralPath $map -Destination (Join-Path $OutDirectory 'MaiCordChatListBenchmark.map') -Force }
    python (Join-Path $PSScriptRoot 'perf\profile_chat_list.py') $samples (Join-Path $OutDirectory 'MaiCordChatListBenchmark.map') | Tee-Object -FilePath (Join-Path $OutDirectory 'profile.txt')
}

$run = if ($Profile) { { param($image) Invoke-ChatListProfile $image } } else { { param($image) Invoke-ChatListSuite $image } }
if ($Binary) {
    & $run (Resolve-Path -LiteralPath $Binary).ProviderPath
} else {
    Invoke-BenchmarkTarget -Target MaiCordChatListBenchmark -Label 'Chat list benchmark' -Run $run `
        -ExtraOutputs @((Join-Path $cacheRoot 'MaiCordChatListBenchmark.map'))
}
