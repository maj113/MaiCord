#requires -Version 5.1
# Prints the bin directory of the LLVM toolchain (clang-cl, lld-link, llvm-lib).
# An explicit -LlvmBin, else MAICORD_LLVM_BIN, wins; otherwise the release
# pinned in tools/llvm_toolchain.json is fetched once into -Root (default
# %LOCALAPPDATA%\MaiCord-toolchain\<tag>), its SHA-256 checked, and reused.
# CI calls this too, so local builds and CI use the same pin.
param(
    [string]$LlvmBin = '',
    [string]$Root = ''
)
$ErrorActionPreference = 'Stop'
$required = 'clang-cl.exe', 'lld-link.exe', 'llvm-lib.exe'
function Test-Toolchain([string]$Bin) {
    foreach ($tool in $required) { if (!(Test-Path -LiteralPath (Join-Path $Bin $tool) -PathType Leaf)) { return $false } }
    return $true
}
$explicit = if ($LlvmBin) { $LlvmBin } else { $env:MAICORD_LLVM_BIN }
if ($explicit) {
    $bin = [IO.Path]::GetFullPath($explicit)
    if (!(Test-Toolchain $bin)) { throw "The build needs $($required -join ', ') in $bin (-LlvmBin or MAICORD_LLVM_BIN)." }
    return $bin
}
$pin = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'llvm_toolchain.json') -Raw | ConvertFrom-Json
if ($pin.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'tools/llvm_toolchain.json has no valid sha256.' }
if (!$Root) { $Root = Join-Path $env:LOCALAPPDATA "MaiCord-toolchain\$($pin.tag)" }
$Root = [IO.Path]::GetFullPath($Root)
$bin = Join-Path $Root 'bin'
if (Test-Toolchain $bin) { return $bin }

$staging = "$Root.download"
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
[void](New-Item -ItemType Directory -Path $staging)
try {
    $archive = Join-Path $staging $pin.asset
    $url = "https://github.com/$($pin.repository)/releases/download/$($pin.tag)/$($pin.asset)"
    Write-Host "Fetching the pinned LLVM toolchain $($pin.tag)..."
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $progress = $ProgressPreference
    $ProgressPreference = 'SilentlyContinue'
    try { Invoke-WebRequest -Uri $url -OutFile $archive -UseBasicParsing } finally { $ProgressPreference = $progress }
    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
    if ($hash -ne $pin.sha256.ToUpperInvariant()) {
        throw "$($pin.asset) from $($pin.tag) hashes to $hash, not the pinned $($pin.sha256)."
    }
    $expanded = Join-Path $staging 'toolchain'
    Expand-Archive -LiteralPath $archive -DestinationPath $expanded
    if (!(Test-Toolchain (Join-Path $expanded 'bin'))) { throw "$($pin.asset) lacks $($required -join ', ') in bin." }
    if (!(Get-ChildItem -Path (Join-Path $expanded 'lib\clang\*\include\stddef.h') -ErrorAction SilentlyContinue)) {
        throw "$($pin.asset) has no clang resource headers (lib\clang\<major>\include)."
    }
    if (Test-Path -LiteralPath $Root) { Remove-Item -LiteralPath $Root -Recurse -Force }
    Move-Item -LiteralPath $expanded -Destination $Root
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
}
return $bin
