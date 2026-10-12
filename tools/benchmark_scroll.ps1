#requires -Version 5.1
param(
    [ValidateRange(1, 50)]
    [int]$Samples = 5
)

# Builds MaiCordUiProbe inside the single Release cache under the shared build
# mutex, runs its opt-in 200-message scroll benchmark in a temporary profile,
# then removes the probe, its objects and the profile and restores the
# shipping-only configuration.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$binary = Join-Path $cacheRoot 'MaiCordUiProbe.exe'
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
function Get-BuildFiles { Get-ChildItem -LiteralPath $cacheRoot -File -Recurse -Force | ForEach-Object FullName }

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
$configured = $false
$profile = Join-Path $cacheRoot ('test-profile-' + [guid]::NewGuid().ToString('N'))
$previous = @{}
foreach ($name in @('LOCALAPPDATA', 'MAICORD_UI_PROBE_ONCE', 'MAICORD_SCROLL_BENCHMARK', 'MAICORD_GPU_METRICS')) {
    $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location $repository
try {
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    $before = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in Get-BuildFiles) { [void]$before.Add($path) }
    [void](New-Item -ItemType Directory -Path $profile)
    [IO.File]::WriteAllText((Join-Path $profile '.maicord-test-profile'), $repository)
    cmake --preset x64-release -DMAICORD_BUILD_TESTS=ON "-DMAICORD_TEST_PROFILE_DIRECTORY=$profile" `
        -DMAICORD_TEST_PROFILE_NAMES=maicord_ui_probe | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Probe configuration failed.' }
    $configured = $true
    cmake --build $cacheRoot --target MaiCordUiProbe
    if ($LASTEXITCODE -ne 0) { throw 'MaiCordUiProbe build failed.' }

    $values = @{}
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $run = Join-Path $profile "run-$sample"
        [void](New-Item -ItemType Directory -Path $run)
        $env:LOCALAPPDATA = $run
        $env:MAICORD_UI_PROBE_ONCE = '1'
        $env:MAICORD_SCROLL_BENCHMARK = '1'
        $env:MAICORD_GPU_METRICS = '1'
        $process = Start-Process -FilePath $binary -PassThru -Wait
        $log = Get-Content -LiteralPath (Join-Path $run 'MaiCord\logs\maicord.log') -Raw
        if ($process.ExitCode -ne 0) {
            @($log -split "`r?`n" | Where-Object { $_ -match 'error' } | Select-Object -Last 20) | Write-Host
            throw "Scroll benchmark failed with code $($process.ExitCode)."
        }
        foreach ($match in [regex]::Matches($log, '(scroll_[a-z0-9_]+|content_text_[a-z0-9_]+|shaped_text_[a-z0-9_]+)=(\d+)')) {
            $key = $match.Groups[1].Value
            if (-not $values.ContainsKey($key)) { $values[$key] = @() }
            $values[$key] += [double]$match.Groups[2].Value
        }
    }
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    $values.Keys | Sort-Object | ForEach-Object {
        [pscustomobject]@{ Metric = $_; Median = @($values[$_] | Sort-Object)[[math]::Min($middle, $values[$_].Count - 1)]; Samples = $values[$_].Count }
    } | Format-Table -AutoSize
} finally {
    foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    if ($locked) {
        try {
            if ($configured) {
                $newFiles = @(Get-BuildFiles | Where-Object { !$before.Contains($_) })
                & (Join-Path $PSScriptRoot 'maintain_build_outputs.ps1') -Apply -NewFiles $newFiles -TestProfile $profile
            }
        } finally {
            if (Test-Path -LiteralPath $profile) { Remove-Item -LiteralPath $profile -Recurse -Force }
            cmake --preset x64-release -DMAICORD_BUILD_TESTS=OFF -DMAICORD_TEST_AUDIO_PLAYBACK=OFF `
                -DMAICORD_TEST_PROFILE_DIRECTORY= -DMAICORD_TEST_PROFILE_NAMES= | Out-Null
            & (Join-Path $PSScriptRoot 'maintain_build_outputs.ps1') -Apply -ConfigureScratch
            $mutex.ReleaseMutex()
        }
    }
    $mutex.Dispose()
    Pop-Location
}
