param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [switch]$RequireD3D11Only,
    [switch]$ForbidMediaFoundation,
    [switch]$ForbidDirectWrite,
    [switch]$RequireNoDebugInfo,
    # File bytes allowed in the writable .data section (-1: unchecked). One
    # nonzero default member initializer moves a whole zero-filled global
    # (e.g. app_detail::g_app) from .bss into the file.
    [int]$MaximumInitializedDataBytes = -1,
    # The exact set of delay-loaded DLLs (case-insensitive). Each must be
    # absent from the static imports, which the loader maps before the entry
    # point; an empty list requires no delay-load imports at all.
    [string[]]$DelayLoad = @()
)

$ErrorActionPreference = 'Stop'
$resolved = (Resolve-Path -LiteralPath $Executable).Path
$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
if (!$dumpbin) {
    # Outside a developer shell: the x64 tools of the newest Visual Studio.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere)) { throw 'dumpbin.exe was not on PATH and vswhere.exe was unavailable.' }
    $installation = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$installation) { throw 'No Visual Studio installation with x64 C++ tools was found.' }
    $toolVersion = (Get-Content -LiteralPath (Join-Path $installation 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
    $dumpbin = Join-Path $installation "VC\Tools\MSVC\$toolVersion\bin\Hostx64\x64\dumpbin.exe"
    if (!(Test-Path -LiteralPath $dumpbin)) { throw "Visual Studio's x64 dumpbin.exe was not found at $dumpbin" }
}
$imports = & $dumpbin /nologo /imports $resolved | Out-String
if ($LASTEXITCODE -ne 0) { throw 'dumpbin import inspection failed.' }
$headers = & $dumpbin /nologo /headers $resolved | Out-String
if ($LASTEXITCODE -ne 0) { throw 'dumpbin header inspection failed.' }

if ($RequireNoDebugInfo) {
    if ($headers -match '(?im)^\s+[0-9A-F]+\s+(cv|coff|coffgrp|fpo|vcfeat|pogo|repro)\s') {
        throw 'Release PE contains symbol/debug records.'
    }
    if (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($resolved, '.pdb'))) {
        throw 'Release output has a PDB beside the executable.'
    }
}

if ($MaximumInitializedDataBytes -ge 0) {
    $dataSection = [regex]::Match($headers, '(?ms)^SECTION HEADER #\d+\s+\.data name\b.*?^\s*([0-9A-F]+) size of raw data')
    if (!$dataSection.Success) { throw 'dumpbin reported no .data section.' }
    $initializedData = [Convert]::ToInt64($dataSection.Groups[1].Value, 16)
    if ($initializedData -gt $MaximumInitializedDataBytes) {
        throw ("Initialized .data is $initializedData bytes (limit $MaximumInitializedDataBytes). A global with a " +
               'nonzero default member initializer was likely serialized into the file; zero it in its definition ' +
               'and apply the default at startup.')
    }
}

# dumpbin lists static imports first, then a delay-load section. A DLL name
# line is followed by the descriptor's Import Address Table (static) or
# Characteristics (delay-load) line. The loader appends ".dll" to a name
# without an extension, so the image may omit it; names compare with it.
$importParts = $imports -split 'Section contains the following delay load imports:', 2
$dllNames = {
    param($text)
    [regex]::Matches($text, '(?im)^\s+(\S+)\s*\r?\n\s+[0-9A-F]+ (Import Address Table|Characteristics)\s*$') |
        ForEach-Object { $name = $_.Groups[1].Value; if ($name -match '\.[^.\\/]*$') { $name } else { "$name.dll" } }
}
$staticDlls = @(& $dllNames $importParts[0])
$delayDlls = @(if ($importParts.Count -gt 1) { & $dllNames $importParts[1] })
$expectedDelay = @($DelayLoad | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object -Unique)
$actualDelay = @($delayDlls | ForEach-Object { $_.ToLowerInvariant() } | Sort-Object -Unique)
if (($expectedDelay -join ',') -ne ($actualDelay -join ',')) {
    throw "Delay-loaded DLLs are [$($actualDelay -join ', ')], expected [$($expectedDelay -join ', ')]."
}
foreach ($name in $expectedDelay) {
    if ($staticDlls -contains $name) { throw "$name is delay-loaded but still a static import (mapped before the entry point)." }
}

$forbiddenDlls = @('ucrtbase.dll', 'webauthn.dll', 'libcef.dll', 'chrome_elf.dll', 'WebView2Loader.dll', 'electron.dll')
$forbiddenText = @('vcruntime', 'msvcp', 'api-ms-win-crt', 'd3dcompiler', 'dxcompiler', 'libcrypto', 'libssl')
if ($ForbidMediaFoundation) {
    $forbiddenDlls += @('mf.dll', 'mfplat.dll', 'mfreadwrite.dll', 'msmpg2vdec.dll')
    $forbiddenText += @('MFStartup', 'MFCreate', 'MFTEnum')
}
if ($ForbidDirectWrite) {
    $forbiddenDlls += @('dwrite.dll', 'dwritecore.dll')
    $forbiddenText += @('DWriteCreateFactory', 'DWriteCoreCreateFactory')
}

foreach ($name in $forbiddenDlls) {
    if (($staticDlls + $delayDlls) -contains $name) { throw "Forbidden runtime import found: $name" }
}
foreach ($name in $forbiddenText) {
    if ($imports -match [regex]::Escape($name)) { throw "Forbidden runtime import found: $name" }
}

foreach ($name in @('KERNEL32.dll') + @(if ($RequireD3D11Only) { 'D3D11.dll' })) {
    if ($staticDlls -notcontains $name) { throw "Expected static platform import missing: $name" }
}

if ($headers -notmatch '8664 machine \(x64\)') { throw 'MaiCord is not an x64 PE image.' }
if ($headers -notmatch '[1-9A-F][0-9A-F]* entry point') { throw 'The PE has no direct executable entry point.' }
foreach ($mitigation in 'High Entropy Virtual Addresses', 'Dynamic base', 'NX compatible', 'CET compatible') {
    if ($headers -notmatch [regex]::Escape($mitigation)) { throw "Required PE mitigation missing: $mitigation" }
}

[pscustomobject]@{
    Executable = $resolved
    Bytes = (Get-Item -LiteralPath $resolved).Length
    ImportedDlls = @($staticDlls | Sort-Object -Unique) -join ', '
    DelayLoadedDlls = @($delayDlls | Sort-Object -Unique) -join ', '
} | Format-List
