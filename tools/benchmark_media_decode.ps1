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
    [switch]$Raw
)

# Builds the opt-in decode benchmark inside the single Release cache under the
# shared build mutex, decodes a generated synthetic corpus (images, animations,
# local thumbnails, Ogg Opus, MP3), then removes the binary, objects and corpus
# and restores the shipping-only configuration.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$binary = Join-Path $cacheRoot 'MaiCordMediaDecodeBenchmark.exe'
$objects = Join-Path $cacheRoot 'CMakeFiles\MaiCordMediaDecodeBenchmark.dir'
$ownedCorpus = !$CorpusPath
$corpus = if ($CorpusPath) { (Resolve-Path -LiteralPath $CorpusPath).ProviderPath } else {
    Join-Path ([IO.Path]::GetTempPath()) ('maicord-decode-' + [guid]::NewGuid().ToString('N') + '.bin')
}
. (Join-Path $PSScriptRoot 'enter_build_environment.ps1')

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
Push-Location $repository
try {
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
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
    cmake --build $cacheRoot --target MaiCordMediaDecodeBenchmark
    if ($LASTEXITCODE -ne 0) { throw 'MaiCord decode benchmark build failed.' }

    $env:MAICORD_DECODE_CORPUS = $corpus
    $env:MAICORD_DECODE_FILTER = $Filter
    $env:MAICORD_DECODE_BUDGET_MS = [string]$BudgetMs
    $env:MAICORD_DECODE_STAGES = if ($Stages) { '1' } else { '0' }
    $output = @(& $binary)
    $exit = $LASTEXITCODE
    if ($Raw) { $output | ForEach-Object { Write-Host $_ } }
    $rows = foreach ($line in $output) {
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
} finally {
    foreach ($name in @('MAICORD_DECODE_CORPUS', 'MAICORD_DECODE_FILTER', 'MAICORD_DECODE_BUDGET_MS', 'MAICORD_DECODE_STAGES')) {
        Remove-Item "Env:$name" -ErrorAction SilentlyContinue
    }
    if ($ownedCorpus -and (Test-Path -LiteralPath $corpus)) { Remove-Item -LiteralPath $corpus -Force }
    if ($locked) {
        & (Join-Path $PSScriptRoot 'clear_benchmark_outputs.ps1') -CacheRoot $cacheRoot -Paths @($binary, $objects)
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
