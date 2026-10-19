#requires -Version 5.1
# Builds MaiCord.exe and MaiCordAccount.dll in ..\MaiCord-build\Release, audits
# them and, with -Test, builds and runs the temporary test suite. Every run
# ends with the build directory holding only the client and its runtime assets.
[CmdletBinding()]
param(
    [switch]$Test,
    # Tests use the MaiCord.exe and MaiCordAccount.dll already in the build
    # directory (e.g. a running client) instead of building and auditing them.
    [switch]$SkipClient,
    # -SkipClient, waiting up to -ClientWaitSeconds for both files before the
    # tests run (CI copies them in while the tests build).
    [switch]$PrebuiltClient,
    [ValidateRange(0, 3600)][int]$ClientWaitSeconds = 0,
    [string[]]$TestNames = @(),
    # CI shards (maicord-tests.txt, written by CMake): app = tests whose
    # executable links the application core or a shipping library or runs the
    # shipping pair; core = the rest.
    [string[]]$TestShard = @(),
    # CTest labels to skip, e.g. tests that need a hardware GPU or a display.
    [string[]]$ExcludeTestLabels = @(),
    # The local workflow requires 20 s; slower CI runners pass a longer deadline.
    [ValidateRange(1, 600)][int]$TestDeadlineSeconds = 20,
    # Multiplies every per-test CTest timeout (slower CI runners).
    [ValidateRange(1, 20)][int]$TestTimeoutScale = 1,
    # UI probes write their BMP frames here; by default captures stay off.
    [string]$ProbeCaptureDirectory = '',
    # Adds the WASAPI playback and capture contracts (needs audio endpoints).
    [switch]$AudioPlaybackProbe,
    # tools/perf/run_perf.ps1 calls these: -PerfProbe builds the offline
    # MaiCordPerfProbe, -StartupProbe a startup-measuring client (restored
    # afterwards); both run run_perf.ps1 -Execute with -PerfArguments.
    [switch]$PerfProbe,
    [switch]$StartupProbe,
    [hashtable]$PerfArguments = @{}
)

$ErrorActionPreference = 'Stop'
$split = { param($values) @($values | ForEach-Object { $_ -split ',' } | Where-Object { $_ }) }
$TestNames = & $split $TestNames
$TestShard = & $split $TestShard
$ExcludeTestLabels = & $split $ExcludeTestLabels
if (!$Test -and ($TestNames -or $TestShard -or $ProbeCaptureDirectory -or $AudioPlaybackProbe -or $PrebuiltClient)) {
    throw '-TestNames, -TestShard, -ProbeCaptureDirectory, -AudioPlaybackProbe and -PrebuiltClient require -Test.'
}
if ($SkipClient -and !$Test -and !$PerfProbe) { throw '-SkipClient requires -Test or -PerfProbe.' }
if (@($Test, $PerfProbe, $StartupProbe | Where-Object { $_ }).Count -gt 1) { throw '-Test, -PerfProbe and -StartupProbe are separate runs.' }
if ($StartupProbe -and $SkipClient) { throw '-StartupProbe builds its own client.' }
if ($TestShard | Where-Object { $_ -cnotin 'app', 'core' }) { throw "Unknown test shard in: $($TestShard -join ', ')" }
if ($TestNames | Where-Object { $_ -notmatch '^[A-Za-z0-9_.+-]+$' }) { throw "Invalid test name in: $($TestNames -join ', ')" }
if ($PrebuiltClient) { $SkipClient = [switch]::Present }
if ($AudioPlaybackProbe -and $TestNames) { $TestNames += 'maicord_account_audio_playback' }

$repository = Split-Path -Parent $PSScriptRoot
$build = [IO.Path]::GetFullPath("$repository\..\MaiCord-build\Release")
$client = Join-Path $build 'MaiCord.exe'
$account = Join-Path $build 'MaiCordAccount.dll'
$cleanup = Join-Path $PSScriptRoot 'maintain_build_outputs.ps1'
$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
$testProfile = $null
$restoreClient = $false

function Invoke-Native([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE) { throw "$Command failed ($LASTEXITCODE): $($Arguments[0..3] -join ' ') ..." }
}
function Invoke-Configure([string[]]$Options) {
    Invoke-Native cmake (@('-S', $repository, '--preset', 'x64-release') + $Options)
}
function Invoke-Build([string[]]$Targets) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    Invoke-Native cmake (@('--build', $build, '--parallel', $jobs, '--target') + $Targets)
    Write-Host ('Build: {0:F2} seconds ({1} workers).' -f $timer.Elapsed.TotalSeconds, $jobs)
    if ($Targets -contains 'MaiCord') {
        # Both images keep large zero-filled globals in .bss; 16 KiB of initialized
        # .data leaves room for small tables but not for a serialized aggregate.
        & "$PSScriptRoot\audit_pe.ps1" -Executable $client -RequireD3D11Only -ForbidMediaFoundation -ForbidDirectWrite -RequireNoDebugInfo -MaximumInitializedDataBytes 16384 -DelayLoad dcomp.dll, shell32.dll, ws2_32.dll, secur32.dll, ole32.dll, advapi32.dll, combase.dll
        & "$PSScriptRoot\audit_pe.ps1" -Executable $account -ForbidMediaFoundation -ForbidDirectWrite -RequireNoDebugInfo -MaximumInitializedDataBytes 16384
        foreach ($image in Get-Item -LiteralPath $client, $account) { Write-Host "Client image: $($image.Name) bytes=$($image.Length)" }
    }
}
function Get-Regex([string[]]$Values) { '^(' + (($Values | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$' }

try {
    # Concurrent invocations queue for the single Release directory.
    try {
        if (!($locked = $mutex.WaitOne(0))) {
            Write-Host 'Waiting for another MaiCord Release build or test invocation...'
            $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45))
        }
    } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (!$locked) { throw 'Another MaiCord Release build or test invocation is still active after 45 minutes.' }
    if (!$SkipClient) {
        foreach ($running in Get-Process -Name MaiCord -ErrorAction SilentlyContinue) {
            if (!$running.Path) { throw 'Cannot determine the running MaiCord path before linking.' }
            if ([IO.Path]::GetFullPath($running.Path) -eq $client) { throw "Close MaiCord before rebuilding $client." }
        }
    }
    # Ninja's default worker count, capped at one per free GiB of commit past the first.
    $freeCommit = [long](Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory * 1KB
    $jobs = [string][Math]::Max(1, [Math]::Min([Math]::Min(32, [Environment]::ProcessorCount + 2), [Math]::Floor($freeCommit / 1GB) - 1))

    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    $installation = & "$installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$installation) { throw 'A Visual Studio C++ installation was not found.' }
    # VsDevCmd calls vswhere.exe by name.
    $env:PATH = "$installer;$env:PATH"
    Import-Module "$installation\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

    $targets = @(if (!$SkipClient) { 'MaiCord', 'MaiCordAccount' })
    if ($Test -or $PerfProbe -or $StartupProbe) {
        # A private LOCALAPPDATA/TEMP for the run, owned by this repository.
        $testProfile = Join-Path $build ('test-profile-' + [guid]::NewGuid().ToString('N'))
        [void](New-Item -ItemType Directory -Path "$testProfile\Temp")
        [IO.File]::WriteAllText("$testProfile\.maicord-test-profile", $repository)
    }
    $on = { param($value) if ($value) { 'ON' } else { 'OFF' } }
    Invoke-Configure @("-DMAICORD_BUILD_TESTS=$(& $on $Test)", "-DMAICORD_BUILD_PERF_PROBE=$(& $on $PerfProbe)",
        "-DMAICORD_STARTUP_PROBE=$(& $on $StartupProbe)", "-DMAICORD_TEST_AUDIO_PLAYBACK=$(& $on $AudioPlaybackProbe)",
        "-DMAICORD_TEST_PREBUILT_CLIENT=$(& $on $SkipClient)", "-DMAICORD_TEST_TIMEOUT_SCALE=$TestTimeoutScale",
        "-DMAICORD_TEST_PROFILE_DIRECTORY=$(if ($Test) { $testProfile })", "-DMAICORD_TEST_PROFILE_NAMES=$($TestNames -join ';')")
    $restoreClient = $StartupProbe.IsPresent

    if ($Test) {
        # Opt-in labels run only when -TestNames names their tests (showcase: website screenshots).
        $skippedLabels = @($ExcludeTestLabels) + @(if (!$TestNames) { 'showcase' })
        $catalog = foreach ($line in [IO.File]::ReadAllLines("$build\maicord-tests.txt")) {
            $name, $shard, $labels, $target = $line.Split("`t")
            [pscustomobject]@{ Name = $name; Shard = $shard; Labels = @($labels.Split(';') | Where-Object { $_ }); Target = $target }
        }
        foreach ($name in $TestNames) { if ($catalog.Name -cnotcontains $name) { throw "Unknown registered test: $name" } }
        $selected = @($catalog | Where-Object {
            (!$TestNames -or $TestNames -ccontains $_.Name) -and (!$TestShard -or $TestShard -ccontains $_.Shard)
        })
        if ($TestShard -and !$selected) { throw "Test shard $($TestShard -join ', ') selects no tests." }
        # Tests carrying a skipped label never run, so their executables are not built.
        $targets += @($selected | Where-Object { !($_.Labels | Where-Object { $skippedLabels -ccontains $_ }) -and $_.Target } |
            ForEach-Object Target | Sort-Object -Unique)
    }
    if ($PerfProbe) { $targets += 'MaiCordPerfProbe' }
    if ($targets) { Invoke-Build $targets }

    if ($StartupProbe) {
        & "$PSScriptRoot\perf\run_perf.ps1" -ProductionStartup -Execute -Probe $client -ProfileRoot $testProfile @PerfArguments
    }
    if ($PerfProbe) {
        & "$PSScriptRoot\perf\run_perf.ps1" -Execute -Probe "$build\MaiCordPerfProbe.exe" -ProfileRoot $testProfile @PerfArguments
    }
    if ($Test) {
        if ($PrebuiltClient) {
            $wait = [Diagnostics.Stopwatch]::StartNew()
            while (@(Test-Path -LiteralPath $client, $account -PathType Leaf) -contains $false) {
                if ($wait.Elapsed.TotalSeconds -ge $ClientWaitSeconds) { throw "-PrebuiltClient needs MaiCord.exe and MaiCordAccount.dll in $build." }
                Start-Sleep -Seconds 1
            }
        }
        $arguments = @('--test-dir', $build, '--output-on-failure', '--parallel', [Math]::Max(1, [Math]::Min(16, [Environment]::ProcessorCount)),
            # CTest stops every test at the deadline; the elapsed check below enforces it exactly.
            '--stop-time', (Get-Date).AddSeconds($TestDeadlineSeconds + 1).ToString('HH:mm:ss', [Globalization.CultureInfo]::InvariantCulture))
        if ($VerbosePreference -eq 'Continue') { $arguments += '--verbose' }
        if ($skippedLabels) { $arguments += '--label-exclude', (Get-Regex $skippedLabels) }
        if ($TestNames -or $TestShard) { $arguments += '--tests-regex', (Get-Regex $selected.Name) }
        $environment = @{ LOCALAPPDATA = $testProfile; TEMP = "$testProfile\Temp"; TMP = "$testProfile\Temp"; MAICORD_UI_PROBE_CAPTURE_DIR = $null }
        if ($ProbeCaptureDirectory) {
            $environment.MAICORD_UI_PROBE_CAPTURE_DIR = (New-Item -ItemType Directory -Force -Path $ProbeCaptureDirectory).FullName
        }
        $previous = @{}
        foreach ($name in $environment.Keys) {
            $previous[$name] = [Environment]::GetEnvironmentVariable($name)
            [Environment]::SetEnvironmentVariable($name, $environment[$name])
        }
        $timer = [Diagnostics.Stopwatch]::StartNew()
        try { & ctest @arguments; $failed = $LASTEXITCODE } finally {
            foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name]) }
        }
        $seconds = $timer.Elapsed.TotalSeconds
        if ($AudioPlaybackProbe) {
            # The playback probe's report, which passing tests do not print.
            $log = [IO.File]::ReadAllText("$build\Testing\Temporary\LastTest.log")
            $match = [regex]::Match($log, '(?ms)^\d+/\d+ Testing: maicord_account_audio_playback\r?\n.*?^Output:\r?\n-+\r?\n(.*?)^<end of output>')
            if ($match.Success) { Write-Host $match.Groups[1].Value.TrimEnd() }
        }
        if ($seconds -gt $TestDeadlineSeconds) { throw ('The test suite exceeded {0} seconds ({1:F2} seconds).' -f $TestDeadlineSeconds, $seconds) }
        if ($failed) { throw "The test suite failed ($failed)." }
        Write-Host ('Tests passed in {0:F2} seconds.' -f $seconds)
    }
} catch {
    $primary = $_
    throw
} finally {
    # Never leave the startup-measuring candidate as the normal client, nor
    # anything but the client and its runtime assets in the build directory
    # (without the CMake ownership manifest, after a failed configure, the
    # temporary profiles). Windows Error Reporting writes crashed test
    # executables' dumps under the real profile; MaiCord.exe dumps are kept.
    $late = $null
    foreach ($step in {
        if ($restoreClient) { Invoke-Configure @('-DMAICORD_STARTUP_PROBE=OFF'); Invoke-Build @('MaiCord', 'MaiCordAccount') }
    }, {
        if ($locked) { & $cleanup -Apply -RuntimeOnly:(Test-Path -LiteralPath "$build\maicord-build-outputs.txt") }
    }, {
        if ($locked -and $Test) {
            Get-ChildItem -LiteralPath "$([Environment]::GetFolderPath('LocalApplicationData'))\CrashDumps" -Filter 'MaiCord?*.exe.*.dmp' -File -ErrorAction SilentlyContinue |
                Remove-Item -Force -ErrorAction SilentlyContinue
        }
    }) {
        try { & $step } catch { if ($primary -or $late) { Write-Warning "Finalization failed: $_" } else { $late = $_ } }
    }
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
    if ($late) { throw $late }
}
