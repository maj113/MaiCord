param(
    [ValidateRange(1, 20)]
    [int]$Samples = 1
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot

. (Join-Path $PSScriptRoot 'enter_build_environment.ps1') -SkipNinjaFallback

Push-Location $repository
try {
    cmake --preset x64-release -DMAICORD_BUILD_INTEGRATION_TESTS=ON
    cmake --build --preset x64-release --target MaiCordGatewayProbe
    if ($LASTEXITCODE -ne 0) {
        throw 'MaiCord Gateway probe build failed.'
    }

    $binary = Join-Path (Split-Path -Parent $repository) `
        'MaiCord-build\Release\MaiCordGatewayProbe.exe'
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary
    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) {
            throw "MaiCord Gateway probe failed: $($output -join ' ')"
        }
        $timing = $output | Select-String -Pattern 'connect_us=(\d+)'
        if (-not $timing) {
            throw 'MaiCord Gateway probe output was malformed.'
        }
        $measurements += [pscustomobject]@{
            Sample = $sample
            ConnectThroughHelloMicroseconds = [uint64]$timing.Matches[0].Groups[1].Value
        }
    }
    $ordered = @($measurements.ConnectThroughHelloMicroseconds | Sort-Object)
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    $measurements | Format-Table -AutoSize
    [pscustomobject]@{
        Samples = $Samples
        ConnectThroughHelloMicrosecondsMedian = $ordered[$middle]
    } | Format-List
} finally {
    Pop-Location
}
