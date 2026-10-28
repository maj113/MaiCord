#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 5
)

# Builds the opt-in inflate benchmark (tools/benchmark_session.ps1 owns the
# build, mutex and cleanup), decodes a generated gzip/zlib corpus, then removes
# the corpus as well.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')
$corpus = Join-Path ([IO.Path]::GetTempPath()) ('maicord-compression-' + [guid]::NewGuid().ToString('N') + '.bin')

Invoke-BenchmarkTarget -Target 'MaiCordHttpCompressionBenchmark' -Label 'MaiCord compression benchmark' -Prepare {
    & python (Join-Path $PSScriptRoot 'build_compression_corpus.py') $corpus | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Corpus generation failed.' }
} -Run {
    param($binary)
    $env:MAICORD_COMPRESSION_CORPUS = $corpus
    $rows = @{}
    for ($sample = 1; $sample -le $Samples; $sample++) {
        $output = @(& $binary)
        if ($LASTEXITCODE -ne 0) { throw "MaiCord compression benchmark failed with code $LASTEXITCODE." }
        foreach ($line in $output) {
            $entry = $line | ConvertFrom-Json
            if (-not $rows.ContainsKey($entry.index)) { $rows[$entry.index] = @{ Entry = $entry; Elapsed = @() } }
            $rows[$entry.index].Elapsed += [double]$entry.elapsed_us
        }
    }
    $kinds = @('gzip', 'zlib')
    $middle = [int][math]::Floor(($Samples - 1) * 0.5)
    $rows.Keys | Sort-Object | ForEach-Object {
        $row = $rows[$_]
        $median = @($row.Elapsed | Sort-Object)[$middle]
        [pscustomobject]@{
            Index = $_
            Kind = $kinds[$row.Entry.kind]
            RawBytes = $row.Entry.raw_bytes
            EncodedBytes = $row.Entry.encoded_bytes
            MedianUsPerDecode = [math]::Round($median / $row.Entry.iterations, 1)
            MedianMBps = [math]::Round($row.Entry.raw_bytes * $row.Entry.iterations / $median, 1)
        }
    } | Format-Table -AutoSize
} -Cleanup {
    Remove-Item Env:MAICORD_COMPRESSION_CORPUS -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $corpus) { Remove-Item -LiteralPath $corpus -Force }
}
