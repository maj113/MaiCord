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
