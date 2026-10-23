param(
    [ValidateRange(1, 20)]
    [int]$Samples = 5
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'enter_build_environment.ps1') -SkipNinjaFallback

Push-Location $repository
try {
    cmake --preset x64-release -DMAICORD_BUILD_INTEGRATION_TESTS=ON
    cmake --build --preset x64-release --target MaiCordRestProbe
    if ($LASTEXITCODE -ne 0) {
        throw 'MaiCord REST probe build failed.'
    }

    $binary = Join-Path (Split-Path -Parent $repository) `
        'MaiCord-build\Release\MaiCordRestProbe.exe'
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary
    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) {
            throw "MaiCord REST probe failed: $($output -join ' ')"
        }
        $timing = $output | Select-String -Pattern `
            'first_us=(\d+) reused_us=(\d+) rate_fields=(\d+)'
        if (-not $timing) {
            throw 'MaiCord REST probe output was malformed.'
        }
        $measurements += [pscustomobject]@{
            Sample = $sample
            FirstRequestMicroseconds = [uint64]$timing.Matches[0].Groups[1].Value
            ReusedRequestMicroseconds = [uint64]$timing.Matches[0].Groups[2].Value
            RateFieldMask = [uint32]$timing.Matches[0].Groups[3].Value
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
        ObservedRateFieldMask = ($measurements.RateFieldMask | Measure-Object -Maximum).Maximum
    } | Format-List
} finally {
    Pop-Location
}
