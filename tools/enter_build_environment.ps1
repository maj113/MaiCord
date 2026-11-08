#requires -Version 5.1
# The Windows build environment, dot-sourced by tools/build.ps1 and tools/target_session.ps1, without the developer
# shell: the components .vsconfig lists, found where VsDevCmd finds them. Sets $llvm to the LLVM toolchain's bin directory
# (-LlvmBin, else MAICORD_LLVM_BIN, else the pinned release: resolve_llvm_toolchain.ps1) and puts it first on PATH, with
# CC and CXX naming its clang-cl (a fresh cache configures with it); then the Build Tools' MSVC x64 toolset (ml64, and the
# C and C++ library headers of the units compiled against the SDK headers, MAICORD_SDK_INCLUDES) and the newest Windows
# SDK (its headers, import libraries, rc, mt and fxc). CMake and Ninja come from PATH.
param([string]$LlvmBin = '')
$llvm = & (Join-Path $PSScriptRoot 'resolve_llvm_toolchain.ps1') -LlvmBin $LlvmBin
$installation = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'The Visual Studio Build Tools with the MSVC x64 toolset (.vsconfig) were not found.' }
$msvc = "$installation\VC\Tools\MSVC\$((Get-Content -LiteralPath "$installation\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt" -Raw).Trim())"
$kits = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10.TrimEnd('\')
$sdk = @(Get-ChildItem -LiteralPath "$kits\Include" -Directory | Where-Object {
    (Test-Path -LiteralPath "$($_.FullName)\um\winsdkver.h") -and (Test-Path -LiteralPath "$kits\Lib\$($_.Name)\um\x64\kernel32.lib")
} | Sort-Object { [version]$_.Name })
if (!$sdk) { throw "No Windows SDK with x64 libraries in $kits (.vsconfig)." }
$sdk = $sdk[-1].Name
$env:PATH = "$llvm;$msvc\bin\Hostx64\x64;$kits\bin\$sdk\x64;$env:PATH"
$env:INCLUDE = (@("$msvc\include") + ('ucrt', 'um', 'shared', 'winrt', 'cppwinrt' | ForEach-Object { "$kits\Include\$sdk\$_" })) -join ';'
# The toolset's libraries serve CMake's compiler checks, which link a default C program; MaiCord links none of them.
$env:LIB = "$msvc\lib\x64;$kits\Lib\$sdk\ucrt\x64;$kits\Lib\$sdk\um\x64"
$env:CC = $env:CXX = Join-Path $llvm 'clang-cl.exe'
