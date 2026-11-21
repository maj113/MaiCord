#requires -Version 5.1
# Prints the name of the mutex that makes the runs on one build directory wait for each other: tools/build.ps1 and
# tools/maintain_build_outputs.ps1 take it. The name comes from the directory's path, so
# runs on the build directories of other checkouts do not wait.
param([Parameter(Mandatory)][string]$Directory)
$path = [IO.Path]::GetFullPath($Directory).TrimEnd('\').ToLowerInvariant()
$sha = [Security.Cryptography.SHA256]::Create()
try { 'Local\MaiCordReleaseBuild-' + [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($path)), 0, 8).Replace('-', '') }
finally { $sha.Dispose() }
