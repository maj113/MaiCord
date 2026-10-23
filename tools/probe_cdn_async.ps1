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
    cmake --build --preset x64-release --target MaiCordCdnClientProbe
    if ($LASTEXITCODE -ne 0) {
        throw 'MaiCord asynchronous CDN probe build failed.'
    }

    $binary = Join-Path (Split-Path -Parent $repository) `
        'MaiCord-build\Release\MaiCordCdnClientProbe.exe'
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary
    $measurements = @()
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) {
            throw "MaiCord asynchronous CDN probe failed: $($output -join ' ')"
        }
        $timing = $output | Select-String -Pattern `
            'first_us=(\d+) reused_us=(\d+) reused_status=(\d+) first_body=(\d+) webp_body=(\d+) webp_width=(\d+) webp_height=(\d+) connections=(\d+) runtime_bytes=(\d+)'
        if (-not $timing) {
            throw 'MaiCord asynchronous CDN probe output was malformed.'
        }
        $measurements += [pscustomobject]@{
            Sample = $sample
            FirstRequestMicroseconds = [uint64]$timing.Matches[0].Groups[1].Value
            ReusedRequestMicroseconds = [uint64]$timing.Matches[0].Groups[2].Value
            ReusedStatus = [uint64]$timing.Matches[0].Groups[3].Value
            FirstBodyBytes = [uint64]$timing.Matches[0].Groups[4].Value
            WebpBodyBytes = [uint64]$timing.Matches[0].Groups[5].Value
            WebpWidth = [uint64]$timing.Matches[0].Groups[6].Value
            WebpHeight = [uint64]$timing.Matches[0].Groups[7].Value
            Connections = [uint64]$timing.Matches[0].Groups[8].Value
            RuntimeBytes = [uint64]$timing.Matches[0].Groups[9].Value
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
        WebpBodyBytesMaximum = ($measurements.WebpBodyBytes | Measure-Object -Maximum).Maximum
        WebpDimensionsValid =
            @($measurements | Where-Object {
                $_.WebpWidth -ne 64 -or $_.WebpHeight -ne 64
            }).Count -eq 0
        RuntimeBytesMaximum = ($measurements.RuntimeBytes | Measure-Object -Maximum).Maximum
    } | Format-List
} finally {
    Pop-Location
}
