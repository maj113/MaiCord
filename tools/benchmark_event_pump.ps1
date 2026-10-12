#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 7
)

# Builds the opt-in benchmark inside the single Release cache under the shared
# build mutex, then removes its binary/objects and restores the shipping-only
# configuration.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$binary = Join-Path $cacheRoot 'MaiCordEventPumpBenchmark.exe'
$objects = Join-Path $cacheRoot 'CMakeFiles\MaiCordEventPumpBenchmark.dir'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
$installation = & $vswhere -latest -prerelease -products * -property installationPath
if (-not $installation) { throw 'A Visual Studio C++ installation was not found.' }
Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $env:PATH = (Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
}

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
Push-Location $repository
try {
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
    cmake --build $cacheRoot --target MaiCordEventPumpBenchmark
    if ($LASTEXITCODE -ne 0) { throw 'MaiCord event-pump benchmark build failed.' }
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary

    $names = [ordered]@{
        UpdatesPerSecond = 'message_event_pump_update'
        UnknownPerSecond = 'message_event_pump_unknown'
        PresenceEventsPerMinute = 'presence_events_per_minute'
        LegacyTextRebuildsPerMinute = 'presence_legacy_text_rebuilds_per_minute'
        TextRebuildsPerMinute = 'presence_text_rebuilds_per_minute'
        LegacyProjectionRebuildsPerMinute = 'presence_legacy_projection_rebuilds_per_minute'
        ProjectionRebuildsPerMinute = 'presence_projection_rebuilds_per_minute'
        LegacyModelUsPerMinute = 'presence_legacy_model_us_per_minute'
        ModelUsPerMinute = 'presence_model_us_per_minute'
        MemberUpdatesPerMinute = 'member_list_update_events_per_minute'
        MemberLegacyTextRebuildsPerMinute = 'member_list_legacy_text_rebuilds_per_minute'
        MemberTextRebuildsPerMinute = 'member_list_text_rebuilds_per_minute'
        MemberLegacyProjectionRebuildsPerMinute = 'member_list_legacy_projection_rebuilds_per_minute'
        MemberProjectionRebuildsPerMinute = 'member_list_projection_rebuilds_per_minute'
        MemberLegacyModelUsPerMinute = 'member_list_legacy_model_us_per_minute'
        MemberModelUsPerMinute = 'member_list_model_us_per_minute'
    }
    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) { throw "MaiCord event-pump benchmark execution failed with code $LASTEXITCODE." }
        $row = [ordered]@{ Sample = $sample }
        foreach ($property in $names.Keys) {
            $match = $output | Select-String -Pattern ('^' + $names[$property] + '=(\d+)') | Select-Object -First 1
            if (-not $match) { throw "MaiCord event-pump benchmark output lacks $($names[$property])." }
            $row[$property] = [uint64]$match.Matches[0].Groups[1].Value
        }
        $measurements += [pscustomobject]$row
    }
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    $measurements | Format-Table -AutoSize
    $summary = [ordered]@{ Samples = $Samples }
    foreach ($property in $names.Keys) { $summary[$property + 'Median'] = @($measurements.$property | Sort-Object)[$middle] }
    [pscustomobject]$summary | Format-List
} finally {
    if ($locked) {
        foreach ($path in @($binary, $objects)) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        }
        cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=OFF | Out-Null
        # Generating with benchmarks enabled creates an empty directory per benchmark target.
        Get-ChildItem -LiteralPath (Join-Path $cacheRoot 'CMakeFiles') -Directory -Filter '*Benchmark.dir' |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Recurse -File) } |
            Remove-Item -Recurse -Force
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
