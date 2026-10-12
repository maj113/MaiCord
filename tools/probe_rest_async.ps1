param(
    [ValidateRange(1, 20)]
    [int]$Samples = 5
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}
$installation = & $vswhere -latest -prerelease -products * -property installationPath
if (-not $installation) {
    throw 'A Visual Studio C++ installation was not found.'
}

Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

Push-Location $repository
try {
    cmake --preset x64-release -DMAICORD_BUILD_INTEGRATION_TESTS=ON
    cmake --build --preset x64-release --target MaiCordRestClientProbe
    if ($LASTEXITCODE -ne 0) {
        throw 'MaiCord asynchronous REST probe build failed.'
    }

    $binary = Join-Path (Split-Path -Parent $repository) `
        'MaiCord-build\Release\MaiCordRestClientProbe.exe'
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary
    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) {
            throw "MaiCord asynchronous REST probe failed: $($output -join ' ')"
        }
        $timing = $output | Select-String -Pattern `
            'first_us=(\d+) reused_us=(\d+) connections=(\d+) runtime_bytes=(\d+)'
        if (-not $timing) {
            throw 'MaiCord asynchronous REST probe output was malformed.'
        }
        $measurements += [pscustomobject]@{
            Sample = $sample
            FirstRequestMicroseconds = [uint64]$timing.Matches[0].Groups[1].Value
            ReusedRequestMicroseconds = [uint64]$timing.Matches[0].Groups[2].Value
            Connections = [uint64]$timing.Matches[0].Groups[3].Value
            RuntimeBytes = [uint64]$timing.Matches[0].Groups[4].Value
        }
    }
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    $first = @($measurements.FirstRequestMicroseconds | Sort-Object)
    $reused = @($measurements.ReusedRequestMicroseconds | Sort-Object)
    $measurements | Format-Table -AutoSize
    [pscustomobject]@{
        Samples = $Samples
        FirstRequestMicrosecondsMedian = $first[$middle]
        ReusedRequestMicrosecondsMedian = $reused[$middle]
        ConnectionsMaximum = ($measurements.Connections | Measure-Object -Maximum).Maximum
        RuntimeBytesMaximum = ($measurements.RuntimeBytes | Measure-Object -Maximum).Maximum
    } | Format-List
} finally {
    Pop-Location
}
