#requires -Version 5.1
param(
    # A local copy of opus_testvectors-rfc8251.tar.gz; downloaded into a
    # temporary directory (and deleted afterwards) when omitted.
    [string]$Archive
)

# Decodes the official Opus test vectors with the shipping decoder: range coder
# state, opus_compare quality and pinned PCM hashes (benchmarks/opus_conformance.cpp).
# The archive is checked against its pinned SHA-256 and parsed in memory, never
# extracted. Builds inside the single Release cache under the shared build mutex
# and removes the binary and objects afterwards.
$ErrorActionPreference = 'Stop'
$url = 'https://opus-codec.org/docs/opus_testvectors-rfc8251.tar.gz'
$sha256 = '6B26A22F9BA87B2B836906A9BB7AFEC5F8E54D49553B1200382520EE6FEDFA55'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
$target = 'MaiCordOpusConformance'
$binary = Join-Path $cacheRoot "$target.exe"
$objects = Join-Path $cacheRoot "CMakeFiles\$target.dir"
$download = $null
if (-not $Archive) {
    $download = Join-Path ([IO.Path]::GetTempPath()) ('maicord-opus-vectors-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $download | Out-Null
    $Archive = Join-Path $download 'opus_testvectors-rfc8251.tar.gz'
}
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
    if ($download) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Invoke-WebRequest -Uri $url -OutFile $Archive -UseBasicParsing
    }
    if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $sha256) { throw 'Test vector archive SHA-256 mismatch.' }
    try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
    cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
    cmake --build $cacheRoot --target $target
    if ($LASTEXITCODE -ne 0) { throw 'Opus conformance build failed.' }
    $env:MAICORD_OPUS_VECTORS = (Resolve-Path -LiteralPath $Archive).ProviderPath
    & $binary
    if ($LASTEXITCODE -ne 0) { throw "Opus conformance failed with code $LASTEXITCODE." }
} finally {
    Remove-Item Env:MAICORD_OPUS_VECTORS -ErrorAction SilentlyContinue
    if ($download -and (Test-Path -LiteralPath $download)) { Remove-Item -LiteralPath $download -Recurse -Force }
    if ($locked) {
        foreach ($path in @($binary, $objects)) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
        }
        cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=OFF | Out-Null
        Get-ChildItem -LiteralPath (Join-Path $cacheRoot 'CMakeFiles') -Directory -Filter '*Benchmark.dir' |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Recurse -File) } |
            Remove-Item -Recurse -Force
        $mutex.ReleaseMutex()
    }
    $mutex.Dispose()
    Pop-Location
}
