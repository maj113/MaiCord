#requires -Version 5.1
# Builds one opt-in network probe (cmake/integration.cmake) under the shared build mutex, audits it, runs it -Samples
# times against the live endpoints and prints every sample with the medians and maxima. tools/benchmark_session.ps1
# owns the build, mutex and cleanup.
param(
    [Parameter(Mandatory)][ValidateSet('tls', 'tls-stream', 'rest', 'rest-async', 'cdn')][string]$Probe,
    [ValidateRange(1, 20)][int]$Samples = 5
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')

# Fields: the probe's key=value output names and the columns they fill. Median and Maximum: the columns summarised,
# as Column or SummaryName=Column.
$probes = @{
    'tls' = @{ Target = 'MaiCordTlsProbe'; Label = 'TLS probe'
        Fields = [ordered]@{ handshake_us = 'HandshakeMicroseconds' }
        Median = @('HandshakeMicroseconds'); Maximum = @() }
    'tls-stream' = @{ Target = 'MaiCordTlsStreamProbe'; Label = 'TLS stream probe'
        Fields = [ordered]@{ connect_us = 'ConnectMicroseconds'; first_us = 'FirstRequestMicroseconds'
            reused_us = 'ReusedRequestMicroseconds'; stream_bytes = 'StreamBytes' }
        Median = @('ConnectMicroseconds', 'FirstRequestMicroseconds', 'ReusedRequestMicroseconds')
        Maximum = @('StreamBytesMaximum=StreamBytes') }
    'rest' = @{ Target = 'MaiCordRestProbe'; Label = 'REST probe'
        Fields = [ordered]@{ first_us = 'FirstRequestMicroseconds'; reused_us = 'ReusedRequestMicroseconds'
            rate_fields = 'RateFieldMask' }
        Median = @('FirstRequestMicroseconds', 'ReusedRequestMicroseconds')
        Maximum = @('ObservedRateFieldMask=RateFieldMask') }
    'rest-async' = @{ Target = 'MaiCordRestClientProbe'; Label = 'asynchronous REST probe'
        Fields = [ordered]@{ first_us = 'FirstRequestMicroseconds'; reused_us = 'ReusedRequestMicroseconds'
            connections = 'Connections'; runtime_bytes = 'RuntimeBytes' }
        Median = @('FirstRequestMicroseconds', 'ReusedRequestMicroseconds')
        Maximum = @('ConnectionsMaximum=Connections', 'RuntimeBytesMaximum=RuntimeBytes') }
    'cdn' = @{ Target = 'MaiCordCdnClientProbe'; Label = 'asynchronous CDN probe'
        Fields = [ordered]@{ first_us = 'FirstRequestMicroseconds'; reused_us = 'ReusedRequestMicroseconds'
            reused_status = 'ReusedStatus'; first_body = 'FirstBodyBytes'; webp_body = 'WebpBodyBytes'
            webp_width = 'WebpWidth'; webp_height = 'WebpHeight'; connections = 'Connections'
            runtime_bytes = 'RuntimeBytes' }
        Median = @('FirstRequestMicroseconds', 'ReusedRequestMicroseconds')
        Maximum = @('ConnectionsMaximum=Connections', 'WebpBodyBytesMaximum=WebpBodyBytes', 'RuntimeBytesMaximum=RuntimeBytes') }
}
$spec = $probes[$Probe]

Invoke-BenchmarkTarget -Target $spec.Target -Label "MaiCord $($spec.Label)" -CMakeArguments '-DMAICORD_BUILD_INTEGRATION_TESTS=ON' -Run {
    param($binary)
    & (Join-Path $PSScriptRoot 'audit_pe.ps1') -Executable $binary
    $rows = @(for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) { throw "MaiCord $($spec.Label) failed: $($output -join ' ')" }
        $row = [ordered]@{ Sample = $sample }
        foreach ($key in $spec.Fields.Keys) {
            $match = [regex]::Match(($output -join ' '), "\b$key=(\d+)")
            if (!$match.Success) { throw "MaiCord $($spec.Label) output was malformed." }
            $row[$spec.Fields[$key]] = [uint64]$match.Groups[1].Value
        }
        [pscustomobject]$row
    })
    $rows | Format-Table -AutoSize
    $summary = [ordered]@{ Samples = $Samples }
    foreach ($column in $spec.Median) {
        $summary["${column}Median"] = @($rows.$column | Sort-Object)[[int][math]::Floor(($Samples - 1) * 0.5)]
    }
    foreach ($item in $spec.Maximum) {
        $name, $column = $item -split '=', 2
        $summary[$name] = ($rows.$column | Measure-Object -Maximum).Maximum
    }
    if ($Probe -eq 'cdn') {
        $summary['WebpDimensionsValid'] = @($rows | Where-Object { $_.WebpWidth -ne 64 -or $_.WebpHeight -ne 64 }).Count -eq 0
    }
    [pscustomobject]$summary | Format-List
}
