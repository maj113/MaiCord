#requires -Version 5.1
param(
    [ValidateRange(1, 100)]
    [int]$Samples = 5
)

# Builds the opt-in inflate benchmark inside the single Release cache under the
# shared build mutex, decodes a generated gzip/zlib/zlib-stream corpus, then
# removes the binary, objects and corpus and restores the shipping-only configuration.
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$binary = Join-Path $cacheRoot 'MaiCordHttpCompressionBenchmark.exe'
$objects = Join-Path $cacheRoot 'CMakeFiles\MaiCordHttpCompressionBenchmark.dir'
$corpus = Join-Path ([IO.Path]::GetTempPath()) ('maicord-compression-' + [guid]::NewGuid().ToString('N') + '.bin')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
$installation = & $vswhere -latest -prerelease -products * -property installationPath
if (-not $installation) { throw 'A Visual Studio C++ installation was not found.' }
Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $env:PATH = (Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
}

$mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
$locked = $false
Push-Location $repository
try {
    & python (Join-Path $PSScriptRoot 'build_compression_corpus.py') $corpus | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Corpus generation failed.' }
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
    cmake --build $cacheRoot --target MaiCordHttpCompressionBenchmark
    if ($LASTEXITCODE -ne 0) { throw 'MaiCord compression benchmark build failed.' }

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
    $kinds = @('gzip', 'zlib', 'zlib-stream')
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
} finally {
    Remove-Item Env:MAICORD_COMPRESSION_CORPUS -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $corpus) { Remove-Item -LiteralPath $corpus -Force }
    if ($locked) {
        foreach ($path in @($binary, $objects)) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        }
        cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=OFF | Out-Null
        # Generating with benchmarks enabled creates an empty directory per benchmark target.
        Get-ChildItem -LiteralPath (Join-Path $cacheRoot 'CMakeFiles') -Directory -Filter '*Benchmark.dir' |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Recurse -File) } |
            Remove-Item -Recurse -Force
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
