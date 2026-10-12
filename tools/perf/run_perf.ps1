#requires -Version 5.1
# Offline performance benchmark. Without -Execute this builds MaiCordPerfProbe
# through tools/build.ps1 (single Release cache, temporary outputs removed
# afterwards) and build.ps1 calls back here with -Execute to run it.
[CmdletBinding()]
param(
    [ValidateRange(1, 10)][int]$Runs = 2,
    # Skip the fresh-profile (cold) launches; every run reuses one profile.
    [switch]$WarmOnly,
    # Also print each probe's raw report.
    [switch]$Raw,
    [switch]$Execute,
    [string]$Probe,
    [string]$ProfileRoot,
    [ValidateRange(10, 600)][int]$RunTimeoutSeconds = 120,
    # Run each launch under cdb and print the faulting stack (diagnosis only).
    [switch]$Debugger,
    # 1 kHz in-process sampling of the main thread; prints hot functions per
    # phase. It perturbs the timing columns, so profile runs are separate.
    [switch]$Sampling
)

$ErrorActionPreference = 'Stop'
if (!$Execute) {
    $build = Join-Path (Split-Path -Parent $PSScriptRoot) 'build.ps1'
    $arguments = @{ PerfProbe = $true; SkipClient = $true; PerfRuns = $Runs }
    if ($WarmOnly) { $arguments.PerfWarmOnly = $true }
    if ($Raw) { $arguments.PerfRaw = $true }
    if ($Debugger) { $arguments.PerfDebugger = $true }
    if ($Sampling) { $arguments.PerfProfile = $true }
    & $build @arguments
    return
}

if (!(Test-Path -LiteralPath $Probe -PathType Leaf)) { throw "Perf probe not found: $Probe" }
if (!(Test-Path -LiteralPath (Join-Path $ProfileRoot '.maicord-test-profile') -PathType Leaf)) {
    throw 'The perf profile root is not a build runner test profile.'
}
$marker = Get-Content -LiteralPath (Join-Path $ProfileRoot '.maicord-test-profile') -Raw

function Invoke-Probe([string]$RunProfile) {
    $start = [Diagnostics.ProcessStartInfo]::new($Probe)
    if ($Debugger) {
        $cdb = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Debuggers\x64\cdb.exe'
        $start = [Diagnostics.ProcessStartInfo]::new($cdb)
        $start.Arguments = '-g -G -c ".lines -d; .ecxr; r; k 40; lm o; q" "' + $Probe + '"'
    }
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.WorkingDirectory = Split-Path -Parent $Probe
    $temp = Join-Path $RunProfile 'Temp'
    [void](New-Item -ItemType Directory -Force -Path $temp)
    $start.EnvironmentVariables['LOCALAPPDATA'] = $RunProfile
    $start.EnvironmentVariables['TEMP'] = $temp
    $start.EnvironmentVariables['TMP'] = $temp
    if ($Sampling) { $start.EnvironmentVariables['MAICORD_PERF_PROFILE'] = '1' }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    [void]$process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (!$process.WaitForExit($RunTimeoutSeconds * 1000)) {
        try { $process.Kill() } catch {}
        $process.WaitForExit()
        throw "The perf probe exceeded $RunTimeoutSeconds seconds."
    }
    $text = $stdout.GetAwaiter().GetResult()
    [void]$stderr.GetAwaiter().GetResult()
    $code = $process.ExitCode
    $process.Dispose()
    # Startup stages (load_trace, microseconds since application_run) of this launch.
    $log = Join-Path $RunProfile 'MaiCord\logs\maicord.log'
    $stages = [Collections.Generic.List[string]]::new()
    if (Test-Path -LiteralPath $log) {
        $lines = @(Get-Content -LiteralPath $log)
        $begin = 0
        for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i] -match '\] process_entry') { $begin = $i; break } }
        $seen = @{}
        for ($i = $begin; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '\[load_trace\] ([a-z_]+) .*startup_us=(\d+)' -and !$seen.ContainsKey($Matches[1])) {
                $seen[$Matches[1]] = $true
                $stages.Add("stage name=$($Matches[1]) us=$($Matches[2])")
            }
        }
    }
    $text = $text + "`n" + ($stages -join "`n")
    if ($Raw) { Write-Host $text }
    if ($text -notmatch '(?m)^END') { throw "The perf probe report is incomplete (exit $code).`n$text" }
    return $text
}

function ConvertFrom-Report([string]$Text) {
    $report = @{ phases = @{}; memory = @{}; detail = @{}; modules = @(); sites = @(); hot = @(); threads = @(); fields = @(); stages = @(); lines = @{}; st = @{} }
    foreach ($line in $Text -split "`r?`n") {
        if (!$line) { continue }
        $parts = $line -split ' '
        $values = @{}
        foreach ($part in $parts | Select-Object -Skip 1) {
            $eq = $part.IndexOf('=')
            if ($eq -gt 0) { $values[$part.Substring(0, $eq)] = $part.Substring($eq + 1) }
        }
        switch ($parts[0]) {
            'phase' { $report.phases[$values.name] = $values }
            'memory' { $report.memory["$($values.snapshot)/$($values.category)"] = $values }
            'memory_detail' { $report.detail[$values.snapshot] = $values }
            'module' { $report.modules += , $values }
            'site' { $report.sites += , $values }
            'hot' { $report.hot += , $values }
            'ui_field' { $report.fields += , $values }
            'stage' { $report.stages += , $values }
            'thread' { $report.threads += , $values }
            # Startup attribution (tools/perf/perf_startup.cpp): repeated line kinds.
            { $_ -in @('st_mark', 'st_module', 'st_dll', 'st_api', 'st_d3d', 'st_present_call', 'st_event') } {
                if (!$report.st.ContainsKey($parts[0])) { $report.st[$parts[0]] = @() }
                $report.st[$parts[0]] += , $values
            }
            default { $report.lines[$parts[0]] = $values }
        }
    }
    return $report
}

function Get-Median([double[]]$Values) {
    if (!$Values.Count) { return 0 }
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count % 2) { return $sorted[[int][Math]::Floor($sorted.Count / 2)] }
    return ($sorted[$sorted.Count / 2 - 1] + $sorted[$sorted.Count / 2]) / 2
}
function Format-Number([double]$Value, [int]$Digits = 0) {
    return $Value.ToString("F$Digits", [Globalization.CultureInfo]::InvariantCulture)
}

# Runs: a fresh profile each time (cold: no font catalog, caches or
# preferences), then the same profile again (warm).
function New-RunProfile([string]$Name) {
    $path = Join-Path $ProfileRoot $Name
    [void](New-Item -ItemType Directory -Force -Path $path)
    [IO.File]::WriteAllText((Join-Path $path '.maicord-test-profile'), $marker)
    return $path
}
$cold = @()
$warm = @()
if ($WarmOnly) {
    # One priming launch builds the caches; every measured launch reuses them.
    $runProfile = New-RunProfile 'perf-1'
    [void](Invoke-Probe $runProfile)
    for ($run = 1; $run -le $Runs; $run++) { $warm += , (ConvertFrom-Report (Invoke-Probe $runProfile)) }
} else {
    for ($run = 1; $run -le $Runs; $run++) {
        $runProfile = New-RunProfile "perf-$run"
        $cold += , (ConvertFrom-Report (Invoke-Probe $runProfile))
        $warm += , (ConvertFrom-Report (Invoke-Probe $runProfile))
    }
}

$first = $warm[0].lines['MAICORD_PERF']
if ($first.failure) { Write-Host "Probe failure: $($first.failure)" }
Write-Host ''
Write-Host "MaiCord perf ($Runs run(s); medians; client $($first.width)x$($first.height) at $($first.dpi) dpi; TSC $($first.tsc_mhz) MHz)"
Write-Host ''
$startup = @()
if ($cold.Count) { $startup += , @('cold', $cold) }
$startup += , @('warm', $warm)
Write-Host ('{0,-8}{1,16}{2,12}{3,14}' -f 'startup', 'first_present_ms', 'ready_ms', 'history_ms')
foreach ($row in $startup) {
    $values = $row[1] | ForEach-Object { $_.lines['startup'] }
    Write-Host ('{0,-8}{1,16}{2,12}{3,14}' -f $row[0],
        (Format-Number (Get-Median @($values | ForEach-Object { [double]$_.first_present_ms }))),
        (Format-Number (Get-Median @($values | ForEach-Object { [double]$_.ready_ms }))),
        (Format-Number (Get-Median @($values | ForEach-Object { [double]$_.history_ms }))))
}
$stageNames = [Collections.Generic.List[string]]::new()
foreach ($stage in $warm[0].stages) { if (!$stageNames.Contains($stage.name)) { $stageNames.Add($stage.name) } }
if ($stageNames.Count) {
    Write-Host ''
    Write-Host 'startup stages (ms since application_run; medians):'
    foreach ($name in $stageNames) {
        $line = '  {0,-34}' -f $name
        foreach ($row in $startup) {
            $values = @($row[1] | ForEach-Object { $_.stages | Where-Object { $_.name -eq $name } } | ForEach-Object { [double]$_.us / 1000 })
            if ($values.Count) { $line += '{0,8} {1}' -f (Format-Number (Get-Median $values) 1), $row[0] }
        }
        Write-Host $line
    }
}
# Startup attribution: named main-thread points (ms since process creation,
# main-thread CPU ms), then the first run's module loads, timed system calls,
# D3D11/DXGI/DirectComposition calls and long main-thread waits until the
# first presented frame.
if ($warm[0].st['st_mark']) {
    Write-Host ''
    Write-Host 'startup timeline (ms since process creation / main-thread CPU ms; medians):'
    $entryLine = '  {0,-26}' -f 'process_entry'
    foreach ($row in $startup) {
        $at = @($row[1] | ForEach-Object { [double]$_.lines['st_entry'].at_us / 1000 })
        $cpu = @($row[1] | ForEach-Object { [double]$_.lines['st_entry'].main_cpu_us / 1000 })
        $entryLine += '{0,8} /{1,6} {2}' -f (Format-Number (Get-Median $at) 1), (Format-Number (Get-Median $cpu) 1), $row[0]
    }
    Write-Host $entryLine
    $markNames = [Collections.Generic.List[string]]::new()
    foreach ($mark in $warm[0].st['st_mark']) { if (!$markNames.Contains($mark.name)) { $markNames.Add($mark.name) } }
    foreach ($name in $markNames) {
        $line = '  {0,-26}' -f $name
        foreach ($row in $startup) {
            $marks = @($row[1] | ForEach-Object { $_.st['st_mark'] | Where-Object { $_.name -eq $name } })
            if ($marks.Count) {
                $line += '{0,8} /{1,6} {2}' -f (Format-Number (Get-Median @($marks | ForEach-Object { [double]$_.at_us / 1000 })) 1),
                    (Format-Number (Get-Median @($marks | ForEach-Object { [double]$_.cpu_us / 1000 })) 1), $row[0]
            }
        }
        Write-Host $line
    }
    foreach ($row in $startup) {
        $present = @($row[1] | ForEach-Object { $_.lines['st_present'] })
        Write-Host ('  {0}: first present {1} ms, main CPU {2} ms, process CPU {3} ms; {4} modules at entry, {5} loaded before the first present' -f $row[0],
            (Format-Number (Get-Median @($present | ForEach-Object { [double]$_.at_us / 1000 })) 1),
            (Format-Number (Get-Median @($present | ForEach-Object { [double]$_.main_cpu_us / 1000 })) 1),
            (Format-Number (Get-Median @($present | ForEach-Object { [double]$_.process_cpu_us / 1000 })) 1),
            (Format-Number (Get-Median @($row[1] | ForEach-Object { [double]$_.lines['st_entry'].modules }))),
            (Format-Number (Get-Median @($row[1] | ForEach-Object { @($_.st['st_dll']).Count }))))
        $memory = @($row[1] | ForEach-Object { $_.lines['st_memory'] } | Where-Object { $_ })
        if ($memory.Count) {
            Write-Host ('  {0}: at the first present: private {1} KiB, PWS {2} KiB, WS {3} KiB, GPU local {4} KiB non-local {5} KiB, {6} handles, {7} modules (synthetic module included)' -f $row[0],
                (Format-Number (Get-Median @($memory | ForEach-Object { [double]$_.private_kb }))),
                (Format-Number (Get-Median @($memory | ForEach-Object { [double]$_.pws_kb }))),
                (Format-Number (Get-Median @($memory | ForEach-Object { [double]$_.ws_kb }))),
                (Format-Number (Get-Median @($memory | ForEach-Object { [double]$_.gpu_local_kb }))),
                (Format-Number (Get-Median @($memory | ForEach-Object { [double]$_.gpu_nonlocal_kb }))),
                (Format-Number (Get-Median @($memory | ForEach-Object { [double]$_.handles }))),
                (Format-Number (Get-Median @($memory | ForEach-Object { [double]$_.modules }))))
        }
    }
}
Write-Host ''
$columns = @(
    @('ms', 'ms', 0), @('frames', 'frames', 0), @('main_cpu_ms', 'main ms', 1), @('proc_cpu_ms', 'proc ms', 1),
    @('frame_p50_us', 'f50 us', 0), @('frame_p95_us', 'f95 us', 0), @('frame_p99_us', 'f99 us', 0),
    @('wakes', 'wakes', 0), @('ctxsw', 'ctxsw', 0), @('va_calls', 'VA', 0), @('va_commit_kb', 'VA KiB', 0),
    @('heap_calls', 'heap', 0), @('pws_kb', 'PWS KiB', 0), @('private_kb', 'priv KiB', 0)
)
$header = '{0,-12}' -f 'phase'
foreach ($column in $columns) { $header += '{0,10}' -f $column[1] }
Write-Host $header
foreach ($phase in @('startup', 'history', 'idle_static', 'scroll', 'switch', 'member_list', 'churn', 'settings', 'idle_anim', 'idle_final', 'idle_hidden')) {
    $rows = @($warm | ForEach-Object { $_.phases[$phase] } | Where-Object { $_ })
    if (!$rows.Count) { continue }
    $line = '{0,-12}' -f $phase
    foreach ($column in $columns) {
        $line += '{0,10}' -f (Format-Number (Get-Median @($rows | ForEach-Object { [double]$_[$column[0]] })) $column[2])
    }
    Write-Host $line
}
Write-Host ''
# Retained work per phase: Markdown parser runs vs cache hits, content text
# streams built (and those that kept the shell's glyphs), paint streams built.
$work = @(@('md_parses', 'md parses'), @('md_hits', 'md hits'), @('text_builds', 'text'), @('shell_reuses', 'shell kept'),
    @('paint_builds', 'paint'))
$header = '{0,-12}' -f 'work'
foreach ($column in $work) { $header += '{0,11}' -f $column[1] }
Write-Host $header
foreach ($phase in @('history', 'idle_static', 'scroll', 'switch', 'member_list', 'churn', 'settings', 'idle_anim', 'idle_final')) {
    $rows = @($warm | ForEach-Object { $_.phases[$phase] } | Where-Object { $_ -and $_.md_parses })
    if (!$rows.Count) { continue }
    $line = '{0,-12}' -f $phase
    foreach ($column in $work) { $line += '{0,11}' -f (Format-Number (Get-Median @($rows | ForEach-Object { [double]$_[$column[0]] }))) }
    Write-Host $line
}
Write-Host ''
# GPU instance bytes uploaded per presented frame (paint quads, glyph quads,
# media quads), member list rows resolved for media (cache misses) and shell
# segments replayed instead of built (timeline-only rebuilds).
Write-Host ('{0,-12}{1,11}{2,11}{3,11}{4,11}{5,11}{6,11}' -f 'upload', 'paint B/f', 'text B/f', 'media B/f', 'total KiB', 'member res', 'shell rep')
foreach ($phase in @('scroll', 'switch', 'member_list', 'churn', 'settings', 'idle_anim')) {
    $rows = @($warm | ForEach-Object { $_.phases[$phase] } | Where-Object { $_ -and $_.up_paint_b -ne $null })
    if (!$rows.Count) { continue }
    $perFrame = { param($row, $key) [double]$row[$key] / [Math]::Max(1, [double]$row.frames) }
    Write-Host ('{0,-12}{1,11}{2,11}{3,11}{4,11}{5,11}{6,11}' -f $phase,
        (Format-Number (Get-Median @($rows | ForEach-Object { & $perFrame $_ 'up_paint_b' }))),
        (Format-Number (Get-Median @($rows | ForEach-Object { & $perFrame $_ 'up_text_b' }))),
        (Format-Number (Get-Median @($rows | ForEach-Object { & $perFrame $_ 'up_media_b' }))),
        (Format-Number (Get-Median @($rows | ForEach-Object { ([double]$_.up_paint_b + [double]$_.up_text_b + [double]$_.up_media_b) / 1024 }))),
        (Format-Number (Get-Median @($rows | ForEach-Object { [double]$_.member_resolves }))),
        (Format-Number (Get-Median @($rows | ForEach-Object { [double]$_.shell_replays }))))
}
Write-Host ''
# Immediate-context work per painted frame: draws (renderer count), state
# binds without scissors, scissor rects and those repeating the previous one,
# buffer maps, copies and their kilopixels, compute dispatches and binds; then
# draw-stream GPU time per frame, replayed offscreen after each present (full
# frames and damage-clipped frames separately), the replayed damage area and
# the main-thread CPU of one replayed draw stream (submission alone).
$gpuColumns = @(@('draws', 'draws', 1), @('binds', 'binds', 1), @('g_scissor', 'scissor', 1),
    @('g_scissor_same', 'same sc', 1), @('g_maps', 'maps', 2), @('g_copies', 'copies', 2), @('copy_kpx', 'copy Kpx', 1),
    @('g_dispatch', 'dispatch', 2), @('g_cs', 'cs binds', 1), @('gpu_full', 'full us', 1), @('gpu_partial', 'part us', 1),
    @('partial_kpx', 'part Kpx', 1), @('submit_us', 'submit us', 1))
$header = '{0,-12}' -f 'gpu / frame'
foreach ($column in $gpuColumns) { $header += '{0,10}' -f $column[1] }
Write-Host ($header + '{0,10}{1,10}{2,10}' -f 'painted', 'full n', 'part n')
foreach ($phase in @('history', 'idle_static', 'scroll', 'switch', 'member_list', 'churn', 'settings', 'idle_anim', 'idle_final')) {
    $rows = @($warm | ForEach-Object { $_.phases[$phase] } | Where-Object { $_ -and $_.g_scissor -ne $null })
    if (!$rows.Count) { continue }
    $line = '{0,-12}' -f $phase
    foreach ($column in $gpuColumns) {
        $values = @($rows | ForEach-Object {
            $row = $_
            $frames = [Math]::Max(1, [double]$row.painted)
            switch ($column[0]) {
                'binds' { ([double]$row.g_shader + [double]$row.g_srv + [double]$row.g_sampler + [double]$row.g_cb +
                    [double]$row.g_ia + [double]$row.g_om + [double]$row.g_rs) / $frames }
                'copy_kpx' { [double]$row.g_copy_px / 1000 / $frames }
                'gpu_full' { [double]$row.gpu_full_us / [Math]::Max(1, [double]$row.gpu_full_n) }
                'gpu_partial' { [double]$row.gpu_partial_us / [Math]::Max(1, [double]$row.gpu_partial_n) }
                'partial_kpx' { [double]$row.gpu_partial_px / 1000 / [Math]::Max(1, [double]$row.gpu_partial_n) }
                'submit_us' { [double]$row.replay_cpu_us / [Math]::Max(1, [double]$row.replays) }
                default { [double]$row[$column[0]] / $frames }
            }
        })
        $line += '{0,10}' -f (Format-Number (Get-Median $values) $column[2])
    }
    $line += '{0,10}{1,10}{2,10}' -f (Format-Number (Get-Median @($rows | ForEach-Object { [double]$_.painted }))),
        (Format-Number (Get-Median @($rows | ForEach-Object { [double]$_.gpu_full_n }))),
        (Format-Number (Get-Median @($rows | ForEach-Object { [double]$_.gpu_partial_n })))
    Write-Host $line
}
Write-Host ''
Write-Host ('{0,-18}{1,11}{2,11}{3,11}{4,11}{5,11}{6,11}' -f 'memory (end)', 'commit KiB', 'PWS KiB', 'resident', 'regions', 'shell PWS', 'scroll PWS')
foreach ($category in @('app_virtualalloc', 'heaps', 'stacks', 'write_combined', 'other_private', 'image_exe',
        'image_graphics', 'image_system', 'mapped_files', 'mapped_sections', 'synthetic_module', 'image_synthetic', 'probe')) {
    $end = @($warm | ForEach-Object { $_.memory["end/$category"] } | Where-Object { $_ })
    $shell = @($warm | ForEach-Object { $_.memory["shell/$category"] } | Where-Object { $_ })
    $scroll = @($warm | ForEach-Object { $_.memory["after_scroll/$category"] } | Where-Object { $_ })
    if (!$end.Count) { continue }
    Write-Host ('{0,-18}{1,11}{2,11}{3,11}{4,11}{5,11}{6,11}' -f $category,
        (Format-Number (Get-Median @($end | ForEach-Object { [double]$_.commit_kb }))),
        (Format-Number (Get-Median @($end | ForEach-Object { [double]$_.pws_kb }))),
        (Format-Number (Get-Median @($end | ForEach-Object { [double]$_.resident_kb }))),
        (Format-Number (Get-Median @($end | ForEach-Object { [double]$_.regions }))),
        (Format-Number (Get-Median @($shell | ForEach-Object { [double]$_.pws_kb }))),
        (Format-Number (Get-Median @($scroll | ForEach-Object { [double]$_.pws_kb }))))
}
$clientPws = Get-Median @($warm | ForEach-Object {
    $report = $_
    $sum = 0.0
    foreach ($key in $report.memory.Keys) {
        if ($key -like 'end/*' -and $key -notmatch '/(synthetic_module|image_synthetic|probe)$') { $sum += [double]$report.memory[$key].pws_kb }
    }
    $sum
})
Write-Host ("client private working set at end (excluding synthetic module and probe): {0} KiB" -f (Format-Number $clientPws))
$detail = $warm[0].detail['end']
if ($detail) {
    Write-Host ("heaps {0} committed {1} KiB busy {2} KiB in {3} blocks; stacks {4} threads reserve {5} KiB commit {6} KiB" -f
        $detail.heap_count, $detail.heap_committed_kb, $detail.heap_busy_kb, $detail.heap_blocks,
        $detail.stack_threads, $detail.stack_reserved_kb, $detail.stack_committed_kb)
    Write-Host ("app VirtualAlloc regions {0} ({1} <= 16 KiB) reserving {2} KiB; GPU local {3} KiB non-local {4} KiB" -f
        $detail.app_regions, $detail.app_small_regions, $detail.app_reserved_kb, $detail.gpu_local_kb, $detail.gpu_nonlocal_kb)
}
$alloc = $warm[0].lines['alloc_total']
if ($alloc) {
    Write-Host ("client allocation calls: VirtualAlloc {0} ({1} KiB committed, {2} KiB rounding), VirtualFree {3}, heap {4} ({5} KiB)" -f
        $alloc.va_calls, $alloc.va_commit_kb, $alloc.va_commit_waste_kb, $alloc.vf_calls, $alloc.heap_calls, $alloc.heap_kb)
}
$synthetic = $warm[0].lines['synthetic']
if ($synthetic) {
    Write-Host ("synthetic session: READY {0} KiB -> {1} events; {2} gateway events; {3} history messages; {4} media frames; {5} member list syncs; {6} stub calls" -f
        $synthetic.ready_json_kb, $synthetic.ready_events, $synthetic.gateway_events, $synthetic.history_messages,
        $synthetic.media_frames, $synthetic.member_syncs, $synthetic.stub_calls)
}

# Top modules and allocation sites (first warm run), sites resolved through the link map.
Write-Host ''
Write-Host 'modules (end, by resident KiB):'
foreach ($module in $warm[0].modules | Select-Object -First 12) {
    Write-Host ('  {0,-28}{1,9} commit{2,9} resident{3,9} private' -f $module.name, $module.commit_kb, $module.resident_kb, $module.pws_kb)
}
$map = [IO.Path]::ChangeExtension($Probe, '.map')
$symbols = $null
if (Test-Path -LiteralPath $map) {
    $entries = [Collections.Generic.List[object]]::new()
    $publics = $false
    foreach ($line in [IO.File]::ReadLines($map)) {
        if (!$publics) { if ($line -match 'Publics by Value') { $publics = $true }; continue }
        if ($line -match '^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s') {
            $address = [Convert]::ToUInt64($Matches[2], 16)
            if ($address -ge 0x140000000) { $entries.Add([pscustomobject]@{ Rva = $address - 0x140000000; Name = $Matches[1] }) }
        }
    }
    $symbols = @($entries | Sort-Object Rva)
}
function Resolve-Rva([UInt64]$Rva) {
    if (!$symbols -or !$Rva) { return ('0x{0:x}' -f $Rva) }
    $low = 0; $high = $symbols.Count - 1; $best = $null
    while ($low -le $high) {
        $mid = [int][Math]::Floor(($low + $high) / 2)
        if ($symbols[$mid].Rva -le $Rva) { $best = $symbols[$mid]; $low = $mid + 1 } else { $high = $mid - 1 }
    }
    if (!$best) { return ('0x{0:x}' -f $Rva) }
    $name = $best.Name
    if ($name -match '^\?([^@]+)@') { $name = $Matches[1] }
    return "$name+0x{0:x}" -f ($Rva - $best.Rva)
}
Write-Host ''
Write-Host 'client VirtualAlloc sites (end, by committed KiB):'
foreach ($site in $warm[0].sites | Select-Object -First 25) {
    $frames = @($site.frames -split '/' | ForEach-Object { Resolve-Rva ([Convert]::ToUInt64($_.Substring(2), 16)) })
    Write-Host ('  {0,8} commit{1,8} PWS{2,5}x  {3}' -f $site.commit_kb, $site.pws_kb, $site.regions, ($frames -join ' < '))
}

if ($warm[0].st['st_mark']) {
    foreach ($row in $startup) {
        $first = $row[1][0]
        Write-Host ''
        Write-Host ("startup detail, first {0} run:" -f $row[0])
        Write-Host ('  modules at entry: {0}' -f ((@($first.st['st_module']) | ForEach-Object { $_.name }) -join ' '))
        Write-Host '  modules loaded before the first present (ms, KiB, thread):'
        foreach ($dll in @($first.st['st_dll'])) {
            Write-Host ('    {0,8} {1,-34}{2,7} KiB  {3}' -f (Format-Number ([double]$dll.at_us / 1000) 1), $dll.name, $dll.kb,
                $(if ($dll.main -eq '1') { 'main' } else { 'worker' }))
        }
        Write-Host ('  {0,-34}{1,6}{2,10}{3,8}{4,10}{5,9}{6,10}{7,10}{8,7}' -f 'system call', 'calls', 'total ms', 'main', 'main ms', 'max ms', 'max at', 'first ms', 'later')
        foreach ($api in @($first.st['st_api']) + @($first.st['st_d3d'])) {
            Write-Host ('  {0,-34}{1,6}{2,10}{3,8}{4,10}{5,9}{6,10}{7,10}{8,7}' -f $api.name, $api.calls,
                (Format-Number ([double]$api.dur_us / 1000) 2), $api.main_calls, (Format-Number ([double]$api.main_us / 1000) 2),
                (Format-Number ([double]$api.max_us / 1000) 2), (Format-Number ([double]$api.max_at_us / 1000) 1),
                (Format-Number ([double]$api.first_us / 1000) 1), $api.later)
        }
        foreach ($call in @($first.st['st_present_call'])) {
            Write-Host ('  present {0}: at {1} ms, {2} ms{3}' -f $call.index, (Format-Number ([double]$call.at_us / 1000) 1),
                (Format-Number ([double]$call.dur_us / 1000) 2), $(if ($call.main -eq '1') { '' } else { ' (worker)' }))
        }
        Write-Host '  main-thread waits and module loads over 0.2 ms (at ms, duration ms, caller):'
        foreach ($event in @($first.st['st_event'])) {
            $caller = if ($event.rva -and $event.rva -ne '0x0') { Resolve-Rva ([Convert]::ToUInt64($event.rva.Substring(2), 16)) } else { '' }
            Write-Host ('    {0,8} {1,8}  {2,-28} {3} {4}' -f (Format-Number ([double]$event.at_us / 1000) 1),
                (Format-Number ([double]$event.dur_us / 1000) 2), $event.api, $event.module, $caller)
        }
    }
}
Write-Host ''
Write-Host 'AccountUiRuntime members (end, largest resident first):'
foreach ($field in $warm[0].fields | Sort-Object { -[double]$_.resident_kb } | Select-Object -First 14) {
    Write-Host ('  {0,-40}{1,8} KiB{2,8} KiB resident' -f $field.name, $field.kb, $field.resident_kb)
}
Write-Host ''
Write-Host 'threads (whole run; idle = context switches in the final idle phase; stack = reserve/commit KiB at last sample):'
foreach ($thread in $warm[0].threads | Sort-Object { -[double]$_.cpu_ms }) {
    $name = $thread.start
    if ($name -match '^MaiCordPerfProbe\.exe\+0x([0-9a-f]+)$') { $name = Resolve-Rva ([Convert]::ToUInt64($Matches[1], 16)) }
    Write-Host ('  {0,-52}{1,10} ms{2,8} ctxsw{3,7} idle{4,7}/{5,-5} stack' -f $name, $thread.cpu_ms, $thread.ctxsw, $thread.idle_ctxsw,
        $thread.stack_reserve_kb, $thread.stack_commit_kb)
}
if ($Sampling) {
    foreach ($phase in @('startup', 'history', 'scroll', 'switch', 'member_list', 'churn', 'settings', 'idle_anim')) {
        $hot = @($warm[0].hot | Where-Object { $_.phase -eq $phase })
        if (!$hot.Count) { continue }
        foreach ($kind in @('self', 'inclusive')) {
            Write-Host ''
            Write-Host "hot functions ($phase, $kind samples; callees = time in other modules charged to the nearest client caller):"
            foreach ($entry in $hot | Where-Object { $_.kind -eq $kind } | Select-Object -First 30) {
                $name = if ($entry.module) { "[$($entry.module)]" } else { Resolve-Rva ([Convert]::ToUInt64($entry.rva.Substring(2), 16)) }
                Write-Host ('  {0,6} (own {1,5}, callees {2,5}, incl {3,6})  {4}' -f $entry.weight, $entry.own, $entry.callees, $entry.incl, $name)
            }
        }
    }
}
