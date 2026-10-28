#requires -Version 5.1
param(
    # Substring of corpus item names to run (e.g. jpeg, gif_480).
    [string]$Filter = '',
    # Timed decode budget per item; each item runs at least three times.
    [ValidateRange(10, 60000)][int]$BudgetMs = 400,
    # Half-size images and short audio, for a quick pass.
    [switch]$Quick,
    # Write name -> output hash for every item, or compare against such a file.
    [string]$SaveGolden = '',
    [string]$CompareGolden = '',
    # Reuse a corpus written by generate_media_decode_benchmark_corpus.py (kept afterwards).
    [string]$CorpusPath = '',
    # Also print per-stage cycles where the benchmark measures them (lossy WebP).
    [switch]$Stages,
    [switch]$Raw,
    # Runs the built benchmark this many times back to back; each item reports
    # its fastest run (one build, min-of-N).
    [ValidateRange(1, 20)][int]$Repeat = 1
)

# Builds the opt-in decode benchmark (tools/benchmark_session.ps1 owns the build,
# mutex and cleanup), decodes a generated synthetic corpus (images, animations,
# local thumbnails, Ogg Opus, MP3), then removes the corpus as well.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'benchmark_session.ps1')
$ownedCorpus = !$CorpusPath
$corpus = if ($CorpusPath) { (Resolve-Path -LiteralPath $CorpusPath).ProviderPath } else {
    Join-Path ([IO.Path]::GetTempPath()) ('maicord-decode-' + [guid]::NewGuid().ToString('N') + '.bin')
}

Invoke-BenchmarkTarget -Target 'MaiCordMediaDecodeBenchmark' -Label 'MaiCord decode benchmark' -Prepare {
    if ($ownedCorpus) {
        $generator = @('-I', (Join-Path $PSScriptRoot 'generate_media_decode_benchmark_corpus.py'), $corpus)
        if ($Quick) { $generator += '--quick' }
        # Python reports progress on stderr; Windows PowerShell 5.1 turns redirected
        # stderr into a terminating error under 'Stop', so judge by exit code only.
        $ErrorActionPreference = 'Continue'
        & python @generator 2>$null
        $ErrorActionPreference = 'Stop'
        if ($LASTEXITCODE -ne 0) { throw 'Corpus generation failed.' }
    }
} -Run {
    param($binary)
    $env:MAICORD_DECODE_CORPUS = $corpus
    $env:MAICORD_DECODE_FILTER = $Filter
    $env:MAICORD_DECODE_BUDGET_MS = [string]$BudgetMs
    $env:MAICORD_DECODE_STAGES = if ($Stages) { '1' } else { '0' }
    $output = @()
    $exit = 0
    $runs = for ($attempt = 1; $attempt -le $Repeat; $attempt++) {
        $pass = @(& $binary)
        if ($LASTEXITCODE -ne 0) { $exit = $LASTEXITCODE }
        $output += $pass
        foreach ($line in $pass) {
            if ($line -notmatch '^decode name=') { continue }
            $fields = @{}
            foreach ($pair in ($line -split ' ' | Select-Object -Skip 1)) {
                $key, $value = $pair -split '=', 2
                $fields[$key] = $value
            }
            $pixels = [double]$fields.pixels
            [pscustomobject]@{
                Name = $fields.name
                Ok = $fields.ok
                Frames = [int]$fields.frames
                MinUs = [int64]$fields.min_us
                CyclesPerPixel = if ($pixels) { [math]::Round([double]$fields.min_cycles / $pixels, 2) } else { 0 }
                MedianVsMin = if ([double]$fields.min_cycles) { [math]::Round([double]$fields.median_cycles / [double]$fields.min_cycles, 3) } else { 0 }
                OutputKiB = [math]::Round([double]$fields.output_bytes / 1024, 1)
                ScratchKiB = [math]::Round([double]$fields.scratch_bytes / 1024, 1)
                # Animations: container parse cycles as a share of one average frame decode.
                ParsePerFrame = if ($fields.parse_cycles -and [int]$fields.frames) { [math]::Round([double]$fields.parse_cycles * [int]$fields.frames / [double]$fields.min_cycles, 3) } else { '' }
                Runs = [int]$fields.runs
                Hash = $fields.hash
            }
        }
    }
    if ($Raw) { $output | ForEach-Object { Write-Host $_ } }
    # With -Repeat, each item keeps its fastest process (groups keep corpus order).
    $rows = foreach ($group in ($runs | Group-Object Name)) {
        $group.Group | Sort-Object MinUs | Select-Object -First 1
    }
    $hashes = @($runs | Group-Object Name | Where-Object { @($_.Group.Hash | Sort-Object -Unique).Count -ne 1 })
    if ($hashes.Count) { throw "Decode outputs differ between repeats: $($hashes.Name -join ', ')." }
    $rows | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
    $output | Where-Object { $_ -match '^decode_(header|memory|stages)' } | ForEach-Object { Write-Host $_ }
    if ($SaveGolden) {
        $golden = [ordered]@{}
        foreach ($row in $rows) { $golden[$row.Name] = $row.Hash }
        $golden | ConvertTo-Json | Set-Content -LiteralPath $SaveGolden -Encoding utf8
    }
    if ($CompareGolden) {
        $golden = Get-Content -LiteralPath $CompareGolden -Raw | ConvertFrom-Json
        $mismatches = @($rows | Where-Object { $golden.($_.Name) -and $golden.($_.Name) -ne $_.Hash })
        if ($mismatches.Count) {
            $mismatches | ForEach-Object { Write-Host "Golden mismatch: $($_.Name) $($_.Hash) (expected $($golden.($_.Name)))" }
            throw "$($mismatches.Count) decode outputs differ from $CompareGolden."
        }
        Write-Host "All $(@($rows).Count) decode outputs match $CompareGolden."
    }
    if ($exit -ne 0) { throw "MaiCord decode benchmark failed with code $exit." }
} -Cleanup {
    foreach ($name in @('MAICORD_DECODE_CORPUS', 'MAICORD_DECODE_FILTER', 'MAICORD_DECODE_BUDGET_MS', 'MAICORD_DECODE_STAGES')) {
        Remove-Item "Env:$name" -ErrorAction SilentlyContinue
    }
    if ($ownedCorpus -and (Test-Path -LiteralPath $corpus)) { Remove-Item -LiteralPath $corpus -Force }
}
