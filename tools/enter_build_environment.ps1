#requires -Version 5.1
# Dot-source to preserve the caller's Visual Studio setup variables and environment.
param(
    [switch]$SkipNinjaFallback,
    [switch]$AddInstallerToPath
)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
$installation = & $vswhere -latest -prerelease -products * -property installationPath
if (-not $installation) { throw 'A Visual Studio C++ installation was not found.' }
# Some callers need VsDevCmd to find vswhere.exe by name.
if ($AddInstallerToPath) {
    $env:PATH = (Split-Path -Parent $vswhere) + ';' + $env:PATH
}
Import-Module (Join-Path $installation 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
if (-not $SkipNinjaFallback -and -not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $env:PATH = (Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
}
# A fresh Release cache configures with the LLVM toolchain tools/build.ps1 uses
# (MAICORD_LLVM_BIN, else the pinned release; see resolve_llvm_toolchain.ps1):
# CMake takes the compiler from CC/CXX and finds lld-link and llvm-lib beside it on PATH.
$llvm = & (Join-Path $PSScriptRoot 'resolve_llvm_toolchain.ps1')
$env:PATH = "$llvm;$env:PATH"
$env:CC = $env:CXX = Join-Path $llvm 'clang-cl.exe'
