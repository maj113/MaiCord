param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [switch]$RequireD3D11,
    [switch]$RequireD3D11Only,
    [switch]$ForbidMediaFoundation,
    [switch]$ForbidDirectWrite,
    [switch]$RequireNoDebugInfo,
    # File bytes allowed in the writable .data section (-1: unchecked). One
    # nonzero default member initializer moves a whole zero-filled global
    # (e.g. app_detail::g_app) from .bss into the file.
    [int]$MaximumInitializedDataBytes = -1
)

$ErrorActionPreference = 'Stop'
$resolved = (Resolve-Path -LiteralPath $Executable).Path
$dumpbinCommand = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($dumpbinCommand) {
    $dumpbin = $dumpbinCommand.Source
} else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'dumpbin.exe was not on PATH and vswhere.exe was unavailable.'
    }
    $installation = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $installation) {
        $visualStudioRoot = Join-Path $env:ProgramFiles 'Microsoft Visual Studio'
        if (Test-Path -LiteralPath $visualStudioRoot) {
            $installation = Get-ChildItem -LiteralPath $visualStudioRoot -Directory |
                ForEach-Object {
                    Get-ChildItem -LiteralPath $_.FullName -Directory
                } |
                Where-Object {
                    Test-Path -LiteralPath (Join-Path $_.FullName `
                        'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt')
                } |
                Sort-Object FullName -Descending |
                Select-Object -First 1 -ExpandProperty FullName
        }
        if (-not $installation) {
            throw 'No Visual Studio installation with x64 C++ tools was found.'
        }
    }
    $versionFile = Join-Path $installation 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    $toolVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    $dumpbin = Join-Path $installation "VC\Tools\MSVC\$toolVersion\bin\Hostx64\x64\dumpbin.exe"
    if (-not (Test-Path -LiteralPath $dumpbin)) {
        throw "Visual Studio's x64 dumpbin.exe was not found at $dumpbin"
    }
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

$forbidden = @(
    'ucrtbase.dll',
    'vcruntime',
    'msvcp',
    'api-ms-win-crt',
    'd3dcompiler',
    'dxcompiler',
    'webauthn.dll',
    'libcrypto',
    'libssl',
    'libcef.dll',
    'chrome_elf.dll',
    'WebView2Loader.dll',
    'electron.dll'
)
if ($ForbidMediaFoundation) {
    $forbidden += @('mf.dll', 'mfplat.dll', 'mfreadwrite.dll', 'msmpg2vdec.dll',
                    'MFStartup', 'MFCreate', 'MFTEnum')
}
if ($ForbidDirectWrite) {
    $forbidden += @('dwrite.dll', 'dwritecore.dll', 'DWriteCreateFactory', 'DWriteCoreCreateFactory')
}

foreach ($name in $forbidden) {
    if ($imports -match [regex]::Escape($name)) {
        throw "Forbidden runtime import found: $name"
    }
}

$required = @('KERNEL32.dll')
if ($RequireD3D11) {
    $required += @('USER32.dll', 'D3D11.dll')
} elseif ($RequireD3D11Only) {
    $required += @('D3D11.dll')
}
foreach ($name in $required) {
    if ($imports -notmatch [regex]::Escape($name)) {
        throw "Expected platform import missing: $name"
    }
}

if ($headers -notmatch '8664 machine \(x64\)') {
    throw 'MaiCord is not an x64 PE image.'
}
if ($headers -notmatch '[1-9A-F][0-9A-F]* entry point') {
    throw 'The PE has no direct executable entry point.'
}
foreach ($mitigation in @('High Entropy Virtual Addresses', 'Dynamic base', 'NX compatible', 'CET compatible')) {
    if ($headers -notmatch [regex]::Escape($mitigation)) {
        throw "Required PE mitigation missing: $mitigation"
    }
}

$size = (Get-Item -LiteralPath $resolved).Length
[pscustomobject]@{
    Executable = $resolved
    Bytes = $size
    KiB = [math]::Round($size / 1KB, 2)
    ImportedDlls = @([regex]::Matches($imports, '(?im)^\s+([^\s]+\.dll)\s*$') |
        ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique) -join ', '
    CrtFree = $true
    DirectEntry = $true
    Mitigations = 'ASLR, HighEntropyVA, NX, CET-compatible'
    HasHardwareD3D11Import = if ($RequireD3D11 -or $RequireD3D11Only) { $true } else { 'not required' }
} | Format-List
