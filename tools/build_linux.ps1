#requires -Version 5.1
# Builds, audits and tests the Linux port of MaiCord inside WSL by running tools/build_linux.sh (read its header for
# the layout and the decisions). One command for Windows-side callers; tools/build.ps1 remains the Windows build.
[CmdletBinding()]
param(
    # Also build and run the Linux tests (ctest inside WSL).
    [switch]$Test,
    # Only these ctest cases; requires -Test.
    [string[]]$TestNames = @(),
    # Keep objects, the ninja state and compile_commands.json in ~/mc-build for incremental builds.
    [switch]$KeepIntermediates,
    # Configure and build only: no audit and no tests.
    [switch]$Check,
    # The WSL distribution with clang and lld 23 (default: $env:MAICORD_WSL_DISTRO, else Ubuntu-Preview) and its user
    # (default: $env:MAICORD_WSL_USER, else the distribution's default user).
    [string]$Distro = $(if ($env:MAICORD_WSL_DISTRO) { $env:MAICORD_WSL_DISTRO } else { 'Ubuntu-Preview' }),
    [string]$User = $env:MAICORD_WSL_USER
)

$ErrorActionPreference = 'Stop'
$TestNames = @($TestNames | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($TestNames -and !$Test) { throw '-TestNames requires -Test.' }
if ($Check -and $Test) { throw '-Check builds only; it cannot run tests.' }
if ($TestNames | Where-Object { $_ -notmatch '^[A-Za-z0-9_.+-]+$' }) { throw "Invalid test name in: $($TestNames -join ', ')" }

$repository = Split-Path -Parent $PSScriptRoot
$wsl = @('-d', $Distro) + @(if ($User) { '-u', $User })
$script = (& wsl.exe @wsl -- wslpath -a ("$repository/tools/build_linux.sh" -replace '\\', '/') | Out-String).Trim()
if ($LASTEXITCODE -or !$script) { throw "wslpath failed in WSL distribution $Distro." }

$arguments = @()
if ($Test) { $arguments += '--test'; $arguments += $TestNames }
if ($KeepIntermediates) { $arguments += '--keep' }
if ($Check) { $arguments += '--check' }
& wsl.exe @wsl -- bash -lc "bash '$script' $($arguments -join ' ')"
if ($LASTEXITCODE) { throw "build_linux.sh failed ($LASTEXITCODE)." }
