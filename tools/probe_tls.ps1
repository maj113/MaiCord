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
    cmake --build --preset x64-release --target MaiCordTlsProbe
    if ($LASTEXITCODE -ne 0) {
        throw 'MaiCord TLS probe build failed.'
    }

    $binary = Join-Path (Split-Path -Parent $repository) `
        'MaiCord-build\Release\MaiCordTlsProbe.exe'
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary
    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) {
            throw 'MaiCord TLS probe failed.'
        }
        $handshake = $output | Select-String -Pattern 'handshake_us=(\d+)'
        if (-not $handshake) {
            throw 'MaiCord TLS probe output was malformed.'
        }
        $measurements += [pscustomobject]@{
            Sample = $sample
            HandshakeMicroseconds = [uint64]$handshake.Matches[0].Groups[1].Value
        }
    }
    $ordered = @($measurements.HandshakeMicroseconds | Sort-Object)
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    $measurements | Format-Table -AutoSize
    [pscustomobject]@{
        Samples = $Samples
        HandshakeMicrosecondsMedian = $ordered[$middle]
    } | Format-List
} finally {
    Pop-Location
}
