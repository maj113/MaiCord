#requires -Version 5.1
[CmdletBinding()]
param(
    [Alias('Tests')][switch]$Test,
    # Emit link maps for in-run size analysis; finalization removes them.
    [switch]$LinkMaps,
    [switch]$AudioPlaybackProbe,
    # Build and run tests without relinking the client (e.g. while it is running):
    # client-driven tests use the MaiCord.exe/MaiCordAccount.dll already present.
    [switch]$SkipClient,
    [string[]]$TestNames = @(),
    # Ninja's own default (logical processors + 2), still capped by available commit below.
    [ValidateRange(1, 32)][int]$Jobs = [Math]::Min(32, [Environment]::ProcessorCount + 2),
    [ValidateRange(1, 64)][int]$TestJobs = [Math]::Max(1, [Math]::Min(16, [Environment]::ProcessorCount)),
    # The local workflow requires 20 s; smaller CI runners pass a longer deadline.
    [ValidateRange(1, 600)][int]$TestDeadlineSeconds = 20,
    # Multiplies every per-test CTest timeout (slower CI runners).
    [ValidateRange(1, 20)][int]$TestTimeoutScale = 1,
    # CTest labels to skip, e.g. tests that need a hardware GPU or a live display.
    [string[]]$ExcludeTestLabels = @(),
    # CI shards of the suite, by the executables' libraries (CMake writes
    # maicord-test-shards.txt): app = tests linking the application core or a
    # shipping library, core = everything else.
    [string[]]$TestShard = @(),
    # Tests use the MaiCord.exe and MaiCordAccount.dll already in the build
    # directory (e.g. an open client, or CI's client job output) instead of
    # building them; implies -SkipClient. The suite waits up to
    # -ClientWaitSeconds for both files before it starts.
    [switch]$PrebuiltClient,
    [ValidateRange(0, 3600)][int]$ClientWaitSeconds = 0,
    # Builds MaiCordPerfProbe (tools/perf) in its own temporary configuration,
    # runs tools/perf/run_perf.ps1 against it and removes it with the other
    # temporary outputs. Not combinable with -Test.
    [switch]$PerfProbe,
    [ValidateRange(1, 10)][int]$PerfRuns = 2,
    [switch]$PerfWarmOnly,
    [switch]$PerfRaw,
    [switch]$PerfDebugger,
    [switch]$PerfProfile,
    # Measure real-backend first-present startup using isolated temporary profiles.
    [switch]$StartupProbe,
    [ValidateRange(1, 10)][int]$StartupProbeRuns = 3
)

$ErrorActionPreference = 'Stop'
if ($AudioPlaybackProbe -and !$Test) { throw '-AudioPlaybackProbe requires -Test so its temporary output is removed.' }
if ($TestNames.Count -and !$Test) { throw '-TestNames requires -Test.' }
if ($SkipClient -and !$Test -and !$PerfProbe) { throw '-SkipClient requires -Test or -PerfProbe.' }
if ($Test -and $PerfProbe) { throw '-PerfProbe is a separate temporary configuration; run it without -Test.' }
if ($StartupProbe -and ($Test -or $PerfProbe -or $SkipClient)) { throw '-StartupProbe requires its own client build.' }
$TestShard = @($TestShard | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (@($TestShard | Where-Object { $_ -cnotin @('app', 'core') }).Count) { throw "Unknown test shard in: $($TestShard -join ', ')" }
if ($TestShard.Count -and !$Test) { throw '-TestShard requires -Test.' }
if ($PrebuiltClient -and !$Test) { throw '-PrebuiltClient requires -Test.' }
if ($PrebuiltClient) { $SkipClient = [switch]::Present }
$selectedTestNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($name in @($TestNames | ForEach-Object { $_ -split "," } | Where-Object { $_ })) {
    if ($name -notmatch '^[A-Za-z0-9_.+-]+$' -or !$selectedTestNames.Add($name)) { throw "Invalid or duplicate test name: $name" }
}
if ($AudioPlaybackProbe -and $selectedTestNames.Count) { [void]$selectedTestNames.Add('maicord_account_audio_playback') }
$excludedLabels = @($ExcludeTestLabels | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
$pushed = $false
$invocationFailure = $null
$startupRestorePending = $false
$previousBytecode = $env:PYTHONDONTWRITEBYTECODE

function Invoke-CMake([string[]]$Arguments) {
    & cmake @Arguments
    if ($LASTEXITCODE -ne 0) {
        $description = (@($Arguments | Select-Object -First 6) -join ' ')
        if ($Arguments.Count -gt 6) { $description += ' ...' }
        throw "CMake failed ($LASTEXITCODE): $description"
    }
}
function Invoke-ClientBuild([string]$Label = 'Client compile/link') {
    $clientBuildTimer = [Diagnostics.Stopwatch]::StartNew()
    Invoke-CMake -Arguments @('--build', $cacheRoot, '--target', 'MaiCord', 'MaiCordAccount', '--parallel', [string]$compileJobs)
    $clientBuildTimer.Stop()
    Write-Host ('{0}: {1:F2} seconds ({2} workers).' -f $Label, $clientBuildTimer.Elapsed.TotalSeconds, $compileJobs)
    # Both images keep large zero-filled globals in .bss; 16 KiB of initialized
    # .data leaves room for small tables but not for a serialized aggregate.
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary -RequireD3D11Only -ForbidMediaFoundation -ForbidDirectWrite -RequireNoDebugInfo -MaximumInitializedDataBytes 16384 -DelayLoad dcomp.dll, shell32.dll, ws2_32.dll, secur32.dll, ole32.dll, advapi32.dll, combase.dll
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable (Join-Path $cacheRoot 'MaiCordAccount.dll') -ForbidMediaFoundation -ForbidDirectWrite -RequireNoDebugInfo -MaximumInitializedDataBytes 16384
    foreach ($name in @('MaiCord.exe', 'MaiCordAccount.dll')) {
        $image = Get-Item -LiteralPath (Join-Path $cacheRoot $name)
        Write-Host ('Client image: {0} bytes={1}' -f $image.Name, $image.Length)
    }
}
function Get-BuildFiles {
    if (Test-Path -LiteralPath $cacheRoot) {
        Get-ChildItem -LiteralPath $cacheRoot -File -Recurse -Force | ForEach-Object FullName
    }
}
# Windows PowerShell 5.1 runs on .NET Framework, which lacks
# ProcessStartInfo.ArgumentList and Process.Kill(bool).
function ConvertTo-ProcessArgument([string]$Argument) {
    if ($Argument -and $Argument -notmatch '[\s"]') { return $Argument }
    return '"' + ($Argument -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}
function Stop-ProcessTree([Diagnostics.Process]$Process) {
    # Retain real handles before termination: waiting for CTest alone does not
    # mean its test processes have finished closing their files.
    $owned = [Collections.Generic.List[Diagnostics.Process]]::new()
    $identities = [Collections.Generic.Dictionary[int, datetime]]::new()
    $identities[$Process.Id] = $Process.StartTime.ToUniversalTime()
    $snapshot = @(Get-CimInstance Win32_Process)
    try {
        do {
            $added = $false
            foreach ($entry in $snapshot) {
                $processId = [int]$entry.ProcessId
                $parentId = [int]$entry.ParentProcessId
                if ($identities.ContainsKey($processId) -or !$identities.ContainsKey($parentId)) { continue }
                $created = $entry.CreationDate.ToUniversalTime()
                if ($created -lt $identities[$parentId]) { continue }
                $child = $null
                try {
                    $child = [Diagnostics.Process]::GetProcessById($processId)
                    # Force handle acquisition and reject a reused snapshot PID.
                    $null = $child.Handle
                    if ([Math]::Abs(($child.StartTime.ToUniversalTime() - $created).TotalMilliseconds) -ge 1) {
                        $child.Dispose()
                        continue
                    }
                } catch {
                    if ($child) { $child.Dispose() }
                    continue
                }
                $identities[$processId] = $created
                $owned.Add($child)
                $added = $true
            }
        } while ($added)
        # Kill only retained process identities, never a newly reused PID or
        # unrelated compiler/client. Stop parents before their captured children.
        foreach ($member in (@($Process) + @($owned))) {
            if (!$member.HasExited) { try { $member.Kill() } catch {} }
        }
        $shutdown = [Diagnostics.Stopwatch]::StartNew()
        $survivors = [Collections.Generic.List[int]]::new()
        foreach ($member in (@($Process) + @($owned))) {
            $remaining = [Math]::Max(0, 5000 - [int]$shutdown.ElapsedMilliseconds)
            if (!$member.WaitForExit($remaining)) { $survivors.Add($member.Id) }
        }
        if ($survivors.Count) {
            throw "CTest shutdown did not complete; temporary outputs must be retained (PIDs: $($survivors -join ', '))."
        }
    } finally {
        foreach ($child in $owned) { $child.Dispose() }
    }
}
function Write-AudioPlaybackDiagnostics {
    if (!$AudioPlaybackProbe) { return }
    $log = Join-Path $cacheRoot 'Testing\Temporary\LastTest.log'
    if (!(Test-Path -LiteralPath $log -PathType Leaf)) { return }
    $contents = Get-Content -LiteralPath $log -Raw
    $match = [regex]::Match($contents,
        '(?ms)^\d+/\d+ Testing: maicord_account_audio_playback\r?\n.*?^Output:\r?\n-+\r?\n(.*?)^<end of output>')
    if ($match.Success) { Write-Host $match.Groups[1].Value.TrimEnd() }
}
function Read-TestCatalog([string]$Context) {
    $catalogText = & ctest --test-dir $cacheRoot --show-only=json-v1
    if ($LASTEXITCODE -ne 0) { throw "Cannot read the complete CTest catalog$Context." }
    $catalog = ($catalogText -join "`n") | ConvertFrom-Json
    if (!$catalog.tests.Count) { throw 'The complete CTest catalog is empty.' }
    return $catalog
}
function Invoke-WholeTestSuite {
    $catalog = Read-TestCatalog
    $executedCount = if ($selectedTestNames.Count) { $selectedTestNames.Count } else { $catalog.tests.Count }
    $start = [Diagnostics.ProcessStartInfo]::new((Get-Command ctest -ErrorAction Stop).Source)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $arguments = [Collections.Generic.List[string]]::new()
    foreach ($argument in @('--test-dir', $cacheRoot, '--output-on-failure', '--parallel', [string]$TestJobs)) {
        $arguments.Add($argument)
    }
    if ($VerbosePreference -eq 'Continue') {
        $arguments.Add('--verbose')
    }
    if ($excludedLabels.Count) {
        $arguments.Add('--label-exclude')
        $arguments.Add('^(' + ((@($excludedLabels) | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$')
    }
    if ($selectedTestNames.Count) {
        $arguments.Add('--tests-regex')
        $arguments.Add('^(' + ((@($selectedTestNames | Sort-Object) | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$')
    }
    $start.Arguments = (@($arguments | ForEach-Object { ConvertTo-ProcessArgument $_ }) -join ' ')
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $started = $false
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        [void]$process.Start()
        $started = $true
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $completed = $process.WaitForExit([Math]::Max(1, $TestDeadlineSeconds * 1000 - [int]$timer.ElapsedMilliseconds))
        if (!$completed) { Stop-ProcessTree $process }
        $timer.Stop()
        $outputText = $stdout.GetAwaiter().GetResult()
        Write-Host $outputText
        Write-AudioPlaybackDiagnostics
        $errorText = $stderr.GetAwaiter().GetResult()
        if ($errorText) { Write-Host $errorText }
        if (!$completed -or $timer.Elapsed.TotalSeconds -gt $TestDeadlineSeconds) {
            $unfinished = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
            foreach ($line in $outputText -split "`r?`n") {
                if ($line -match '^\s*Start\s+\d+:\s+(\S+)') { [void]$unfinished.Add($Matches[1]) }
                elseif ($line -match 'Test\s+#\s*\d+:\s+(\S+)\s+\.') { [void]$unfinished.Remove($Matches[1]) }
            }
            if ($unfinished.Count) { Write-Host "Incomplete tests at deadline: $(@($unfinished | Sort-Object) -join ', ')" }
            throw "The whole CTest suite exceeded $TestDeadlineSeconds seconds ($($timer.Elapsed.TotalSeconds.ToString('F2')) seconds)."
        }
        if ($process.ExitCode -ne 0) { throw "The whole CTest suite failed ($($process.ExitCode))." }
        $scope = if ($selectedTestNames.Count) { ' selected' } else { '' }
        $ran = [regex]::Match($outputText, 'tests passed[^\r\n]*out of (\d+)')
        if ($ran.Success) { $executedCount = [int]$ran.Groups[1].Value }
        if ($excludedLabels.Count) { $scope += " (labels excluded: $($excludedLabels -join ', '))" }
        $skipped = @([regex]::Matches($outputText, '(?m)^\s*\d+\s+-\s+(\S+)\s+\(Skipped\)') | ForEach-Object { $_.Groups[1].Value })
        $skippedNote = if ($skipped.Count) { " Skipped: $($skipped -join ', ')." } else { '' }
        Write-Host "All $executedCount$scope tests passed in $($timer.Elapsed.TotalSeconds.ToString('F2')) seconds.$skippedNote"
    } finally {
        if ($started -and !$process.HasExited) { Stop-ProcessTree $process }
        $process.Dispose()
    }
}

try {
    # Concurrent invocations queue for the single Release cache.
    try {
        $locked = $mutex.WaitOne(0)
        if (!$locked) {
            Write-Host 'Waiting for another MaiCord Release build or test invocation...'
            $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45))
        }
    } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (!$locked) { throw 'Another MaiCord Release build or test invocation is still active after 45 minutes.' }
    $compileJobs = [Math]::Min($Jobs, 4)
    try {
        $memory = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
        $availableCommit = [long]$memory.FreeVirtualMemory * 1024L
        if ($availableCommit -le 0) { throw 'Available commit was not reported.' }
        $memoryJobs = [Math]::Max(1, [int][Math]::Floor(($availableCommit - 1GB) / 1GB))
        $compileJobs = [Math]::Min($Jobs, $memoryJobs)
        Write-Host "Compiler workers: $compileJobs (requested $Jobs; available commit $(([double]$availableCommit / 1GB).ToString('F2')) GiB)."
    } catch {
        Write-Host "Compiler workers: $compileJobs (requested $Jobs; memory information unavailable, conservative limit 4)."
    }
    $ancestor = $cacheRoot
    while ($ancestor) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point in the canonical Release path: $ancestor"
            }
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
    $installation = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$installation) { throw 'A Visual Studio C++ installation was not found.' }
    Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    if (!(Get-Command ninja -ErrorAction SilentlyContinue)) {
        $ninjaDirectory = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
        if (!(Test-Path -LiteralPath (Join-Path $ninjaDirectory 'ninja.exe'))) { throw 'Ninja was not found.' }
        $env:PATH = $ninjaDirectory + ';' + $env:PATH
    }
    $binary = Join-Path $cacheRoot 'MaiCord.exe'
    foreach ($client in @(if (!$SkipClient) { Get-Process -Name MaiCord -ErrorAction SilentlyContinue })) {
        if (!$client.Path) { throw 'Cannot determine the running MaiCord path before linking.' }
        if ([IO.Path]::GetFullPath($client.Path).Equals($binary, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Close MaiCord before rebuilding $binary."
        }
    }
    Push-Location $repository
    $pushed = $true
    $env:PYTHONDONTWRITEBYTECODE = '1'
    $mapOption = if ($LinkMaps) { 'ON' } else { 'OFF' }
    $startupOption = if ($StartupProbe) { 'ON' } else { 'OFF' }
    Invoke-CMake -Arguments @('--preset', 'x64-release', "-DMAICORD_GENERATE_LINK_MAPS=$mapOption", '-DMAICORD_TEST_AUDIO_PLAYBACK=OFF',
        "-DMAICORD_STARTUP_PROBE=$startupOption", '-DMAICORD_TEST_PROFILE_DIRECTORY=', '-DMAICORD_TEST_PROFILE_NAMES=', '-DMAICORD_BUILD_PERF_PROBE=OFF')
    $startupRestorePending = $StartupProbe.IsPresent
    if ($SkipClient) {
        Write-Host 'Skipping the client build and PE audits (-SkipClient).'
    } else {
        Invoke-ClientBuild
    }
    & (Join-Path $PSScriptRoot 'maintain_build_outputs.ps1') -Apply -ConfigureScratch
    if ($StartupProbe) {
        $startupProfile = Join-Path $cacheRoot ('test-profile-' + [guid]::NewGuid().ToString('N'))
        try {
            [void](New-Item -ItemType Directory -Path $startupProfile)
            [IO.File]::WriteAllText((Join-Path $startupProfile '.maicord-test-profile'), $repository)
            & (Join-Path $PSScriptRoot 'perf\run_perf.ps1') -ProductionStartup -Execute -Probe $binary -ProfileRoot $startupProfile -Runs $StartupProbeRuns
        } finally {
            if (Test-Path -LiteralPath $startupProfile -PathType Container) {
                & (Join-Path $PSScriptRoot 'maintain_build_outputs.ps1') -Apply -TestProfile $startupProfile
            }
        }
    }
    if ($Test -or $PerfProbe) {
        $before = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($path in Get-BuildFiles) { [void]$before.Add($path) }
        $profile = $null
        $testsConfigured = $false
        $primaryFailure = $null
        $previousEnvironment = @{}
        foreach ($name in @('LOCALAPPDATA', 'TEMP', 'TMP', 'MAICORD_UI_PROBE_CAPTURE_DIR')) { $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
        try {
            $profile = Join-Path $cacheRoot ('test-profile-' + [guid]::NewGuid().ToString('N'))
            [void](New-Item -ItemType Directory -Path $profile)
            [IO.File]::WriteAllText((Join-Path $profile '.maicord-test-profile'), $repository)
            $temporary = Join-Path $profile 'Temp'
            [void](New-Item -ItemType Directory -Path $temporary)
            $env:LOCALAPPDATA = $profile
            $env:TEMP = $temporary
            $env:TMP = $temporary
            [Environment]::SetEnvironmentVariable('MAICORD_UI_PROBE_CAPTURE_DIR', $null, 'Process')
            $audioOption = if ($AudioPlaybackProbe) { 'ON' } else { 'OFF' }
            $profileNames = $selectedTestNames -join ';'
            # Without this, test targets that drive the client would relink it.
            $prebuiltOption = if ($SkipClient) { 'ON' } else { 'OFF' }
            if ($PerfProbe) {
                # The perf probe is its own temporary configuration (see cmake/perf_probe.cmake).
                Invoke-CMake -Arguments @('--preset', 'x64-release', '-DMAICORD_BUILD_TESTS=OFF', '-DMAICORD_BUILD_PERF_PROBE=ON')
            } else {
                Invoke-CMake -Arguments @('--preset', 'x64-release', '-DMAICORD_BUILD_TESTS=ON', "-DMAICORD_TEST_TIMEOUT_SCALE=$TestTimeoutScale",
                    "-DMAICORD_TEST_AUDIO_PLAYBACK=$audioOption", "-DMAICORD_TEST_PROFILE_DIRECTORY=$profile",
                    "-DMAICORD_TEST_PROFILE_NAMES=$profileNames", "-DMAICORD_TEST_PREBUILT_CLIENT=$prebuiltOption")
            }
            $testsConfigured = $true
            $targets = @(Get-Content -LiteralPath (Join-Path $cacheRoot 'maicord-test-targets.txt') | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() })
            if (!$targets.Count -or @($targets | Where-Object { $_ -notmatch '^[A-Za-z0-9_][A-Za-z0-9_.+-]*$' -or $_ -in @('MaiCord', 'MaiCordAccount') }).Count) {
                throw 'The CMake temporary test target manifest is invalid.'
            }
            if ($PerfProbe) {
                if ($targets -notcontains 'MaiCordPerfProbe') { throw 'The temporary target manifest lacks MaiCordPerfProbe.' }
                Invoke-CMake -Arguments @('--build', $cacheRoot, '--parallel', [string]$compileJobs, '--target', 'MaiCordPerfProbe')
                $perfArguments = @{ Execute = $true; Probe = (Join-Path $cacheRoot 'MaiCordPerfProbe.exe'); ProfileRoot = $profile; Runs = $PerfRuns }
                if ($PerfWarmOnly) { $perfArguments.WarmOnly = $true }
                if ($PerfRaw) { $perfArguments.Raw = $true }
                if ($PerfDebugger) { $perfArguments.Debugger = $true }
                if ($PerfProfile) { $perfArguments.Sampling = $true }
                & (Join-Path $PSScriptRoot 'perf\run_perf.ps1') @perfArguments
            }
            if ($Test) {
            $catalog = Read-TestCatalog ' before building tests'
            foreach ($name in $selectedTestNames) {
                if (!@($catalog.tests | Where-Object name -CEQ $name).Count) { throw "Unknown registered test: $name" }
            }
            # Map each registered test to the temporary test executables its command
            # runs. CTest omits commands from the catalog until an executable exists,
            # so read the generated CTestTestfile.cmake: only its add_test() names and
            # quoted or bracketed executable paths, never evaluating CMake text.
            $testExecutables = [Collections.Generic.Dictionary[string, string[]]]::new([StringComparer]::Ordinal)
            $allowed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($target in $targets) { [void]$allowed.Add($target) }
            $testText = [IO.File]::ReadAllText((Join-Path $cacheRoot 'CTestTestfile.cmake'))
            foreach ($call in @([regex]::Split($testText, '(?m)^[ \t]*add_test[ \t]*\(') | Select-Object -Skip 1)) {
                $command = [regex]::Split($call, '(?m)^[ \t]*[A-Za-z_]+[ \t]*\(')[0]
                $nameMatch = [regex]::Match($command, '^\s*(?:\[(=*)\[(?<name>.*?)\]\1\]|"(?<name>[^"]*)"|(?<name>[^\s()"]+))')
                if (!$nameMatch.Success) { throw 'Cannot read a test name from CTestTestfile.cmake.' }
                $executables = [Collections.Generic.List[string]]::new()
                foreach ($match in [regex]::Matches($command, '(?:"|\[=*\[|\s)(?<path>[A-Za-z]:[\\/][^"\r\n\]]*?\.exe)(?="|\]=*\]|\s|\))', [Text.RegularExpressions.RegexOptions]::IgnoreCase)) {
                    $candidate = [IO.Path]::GetFullPath($match.Groups['path'].Value.Replace('/', '\'))
                    if (!$candidate.StartsWith($cacheRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { continue }
                    $executable = [IO.Path]::GetFileNameWithoutExtension($candidate)
                    if ($allowed.Contains($executable)) { $executables.Add($executable) }
                }
                $testExecutables[$nameMatch.Groups['name'].Value] = $executables.ToArray()
            }
            foreach ($entry in $catalog.tests) {
                if (!$testExecutables.ContainsKey($entry.name)) {
                    $sample = $testText.Substring(0, [Math]::Min(800, $testText.Length))
                    throw "CTestTestfile.cmake has no readable command for test $($entry.name). It begins:`n$sample"
                }
            }
            if ($TestShard.Count) {
                $shards = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
                foreach ($line in Get-Content -LiteralPath (Join-Path $cacheRoot 'maicord-test-shards.txt')) {
                    if (!$line.Trim()) { continue }
                    if ($line -notmatch '^([A-Za-z0-9_][A-Za-z0-9_.+-]*) (app|core)$') { throw 'The CMake test shard manifest is invalid.' }
                    $shards[$Matches[1]] = $Matches[2]
                }
                # A test belongs to the shard of the first test executable it runs.
                $shardTests = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
                foreach ($entry in $catalog.tests) {
                    if ($selectedTestNames.Count -and !$selectedTestNames.Contains($entry.name)) { continue }
                    $shard = 'core'
                    foreach ($executable in $testExecutables[$entry.name]) {
                        if ($shards.ContainsKey($executable)) { $shard = $shards[$executable]; break }
                    }
                    if ($TestShard -contains $shard) { [void]$shardTests.Add($entry.name) }
                }
                if (!$shardTests.Count) { throw "Test shard $($TestShard -join ', ') selects no tests." }
                $selectedTestNames = $shardTests
            }
            # Tests carrying an excluded label never run, so their executables are
            # not built (CTest matches labels exactly and case-sensitively).
            $referenced = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach ($entry in $catalog.tests) {
                if ($selectedTestNames.Count -and !$selectedTestNames.Contains($entry.name)) { continue }
                $labels = @($entry.properties | Where-Object { $_.name -eq 'LABELS' } | ForEach-Object { $_.value })
                if (@($labels | Where-Object { $excludedLabels -ccontains $_ }).Count) { continue }
                foreach ($executable in $testExecutables[$entry.name]) { [void]$referenced.Add($executable) }
            }
            if (!$referenced.Count) { throw 'No CTest executable maps to the temporary target manifest.' }

            # One Ninja invocation keeps every worker busy. Ninja still stops at the
            # first failing edge; the broad shared drivers are listed first.
            $drivers = @('MaiCordNativeAppLineTests', 'MaiCordUiProbe', 'MaiCordNativeExtensionTests' | Where-Object { $referenced.Contains($_) })
            $remaining = @($referenced | Where-Object { $_ -notin $drivers } | Sort-Object)
            Invoke-CMake -Arguments (@('--build', $cacheRoot, '--parallel', [string]$compileJobs, '--target') + $drivers + $remaining)
            if ($PrebuiltClient) {
                $deadline = [Diagnostics.Stopwatch]::StartNew()
                while (!(Test-Path -LiteralPath $binary -PathType Leaf) -or
                       !(Test-Path -LiteralPath (Join-Path $cacheRoot 'MaiCordAccount.dll') -PathType Leaf)) {
                    if ($deadline.Elapsed.TotalSeconds -ge $ClientWaitSeconds) {
                        throw "-PrebuiltClient needs MaiCord.exe and MaiCordAccount.dll in $cacheRoot."
                    }
                    Start-Sleep -Seconds 5
                }
            }
            Invoke-WholeTestSuite
            }

        } catch { $primaryFailure = $_ } finally {
            foreach ($name in $previousEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process') }
            try {
                if ($testsConfigured -or $profile) {
                    $newFiles = if ($testsConfigured) {
                        @(Get-BuildFiles | Where-Object { !$before.Contains($_) })
                    } else {
                        @(Get-ChildItem -LiteralPath $profile -File -Recurse -Force | ForEach-Object FullName)
                    }
                    & (Join-Path $PSScriptRoot 'maintain_build_outputs.ps1') -Apply -NewFiles $newFiles -TestProfile $profile
                }
            } catch { if (!$primaryFailure) { $primaryFailure = $_ } else { Write-Warning "Temporary test cleanup failed: $_" } }
            # Windows Error Reporting writes crashed test executables' dumps under
            # the real profile, outside the redirected test LOCALAPPDATA. MaiCord.exe
            # dumps (the real client) are kept.
            $crashDumps = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'CrashDumps'
            if (Test-Path -LiteralPath $crashDumps) {
                Get-ChildItem -LiteralPath $crashDumps -Filter 'MaiCord?*.exe.*.dmp' -File -ErrorAction SilentlyContinue |
                    Remove-Item -Force -ErrorAction SilentlyContinue
            }
        }
        if ($primaryFailure) { throw $primaryFailure }
    }
} catch {
    $invocationFailure = $_
    throw
} finally {
    try {
        # Cover candidate build/audit failures as well as measurement and profile
        # cleanup failures. Never leave the gated candidate as the normal client.
        if ($startupRestorePending) {
            Invoke-CMake -Arguments @('--preset', 'x64-release', '-DMAICORD_STARTUP_PROBE=OFF')
            Invoke-ClientBuild -Label 'Normal client restoration'
        }
    } catch {
        if ($invocationFailure) { Write-Warning "Normal client restoration failed: $_" }
        else { $invocationFailure = $_; throw }
    } finally {
        try {
            # Keep runtime assets only. The next invocation regenerates the one
            # Release cache; configuring tests OFF here would recreate discarded files.
            if ($locked -and $pushed -and (Test-Path -LiteralPath (Join-Path $cacheRoot 'maicord-build-outputs.txt'))) {
                & (Join-Path $PSScriptRoot 'maintain_build_outputs.ps1') -Apply -RuntimeOnly
            }
        } catch {
            if ($invocationFailure) { Write-Warning "Runtime-only finalization failed: $_" }
            else { throw }
        } finally {
            $env:PYTHONDONTWRITEBYTECODE = $previousBytecode
            if ($pushed) { Pop-Location }
            if ($locked) { $mutex.ReleaseMutex() }
            $mutex.Dispose()
        }
    }
}
