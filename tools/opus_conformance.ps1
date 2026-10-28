#requires -Version 5.1
param(
    # A local copy of opus_testvectors-rfc8251.tar.gz; downloaded into a
    # temporary directory (and deleted afterwards) when omitted.
    [string]$Archive
)

# Decodes the official Opus test vectors with the shipping decoder: range coder
# state, opus_compare quality and pinned PCM hashes (benchmarks/opus_conformance.cpp).
# The archive is checked against its pinned SHA-256 and parsed in memory, never
# extracted. tools/benchmark_session.ps1 owns the build, mutex and cleanup.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')
$url = 'https://opus-codec.org/docs/opus_testvectors-rfc8251.tar.gz'
$sha256 = '6B26A22F9BA87B2B836906A9BB7AFEC5F8E54D49553B1200382520EE6FEDFA55'
$download = $null
if (-not $Archive) {
    $download = Join-Path ([IO.Path]::GetTempPath()) ('maicord-opus-vectors-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $download | Out-Null
    $Archive = Join-Path $download 'opus_testvectors-rfc8251.tar.gz'
}

Invoke-BenchmarkTarget -Target 'MaiCordOpusConformance' -Label 'Opus conformance' -Prepare {
    if ($download) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $url -OutFile $Archive -UseBasicParsing
    }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $sha256) { throw 'Test vector archive SHA-256 mismatch.' }
} -Run {
    param($binary)
    $env:MAICORD_OPUS_VECTORS = (Resolve-Path -LiteralPath $Archive).ProviderPath
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "Opus conformance failed with code $LASTEXITCODE." }
} -Cleanup {
    Remove-Item Env:MAICORD_OPUS_VECTORS -ErrorAction SilentlyContinue
    if ($download -and (Test-Path -LiteralPath $download)) { Remove-Item -LiteralPath $download -Recurse -Force }
}
