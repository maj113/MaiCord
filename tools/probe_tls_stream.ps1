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
    cmake --build --preset x64-release --target MaiCordTlsStreamProbe
    if ($LASTEXITCODE -ne 0) {
        throw 'MaiCord TLS stream probe build failed.'
    }

    $binary = Join-Path (Split-Path -Parent $repository) `
        'MaiCord-build\Release\MaiCordTlsStreamProbe.exe'
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary
    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) {
            throw "MaiCord TLS stream probe failed: $($output -join ' ')"
        }
        $timing = $output | Select-String -Pattern `
            'connect_us=(\d+) first_us=(\d+) reused_us=(\d+) stream_bytes=(\d+)'
        if (-not $timing) {
            throw 'MaiCord TLS stream probe output was malformed.'
        }
        $measurements += [pscustomobject]@{
            Sample = $sample
            ConnectMicroseconds = [uint64]$timing.Matches[0].Groups[1].Value
            FirstRequestMicroseconds = [uint64]$timing.Matches[0].Groups[2].Value
            ReusedRequestMicroseconds = [uint64]$timing.Matches[0].Groups[3].Value
            StreamBytes = [uint64]$timing.Matches[0].Groups[4].Value
        }
    }
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    $connect = @($measurements.ConnectMicroseconds | Sort-Object)
    $first = @($measurements.FirstRequestMicroseconds | Sort-Object)
    $reused = @($measurements.ReusedRequestMicroseconds | Sort-Object)
    $measurements | Format-Table -AutoSize
    [pscustomobject]@{
        Samples = $Samples
        ConnectMicrosecondsMedian = $connect[$middle]
        FirstRequestMicrosecondsMedian = $first[$middle]
        ReusedRequestMicrosecondsMedian = $reused[$middle]
        StreamBytesMaximum = ($measurements.StreamBytes | Measure-Object -Maximum).Maximum
    } | Format-List
} finally {
    Pop-Location
}
