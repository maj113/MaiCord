#requires -Version 5.1
# Call while holding the Release build mutex, from the repository directory.
param([string]$CacheRoot, [string[]]$Paths)

foreach ($path in $Paths) {
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
}
cmake --preset x64-release -DMAICORD_BUILD_INTEGRATION_TESTS=OFF | Out-Null
# Generating with probes enabled creates an empty directory per such target.
Get-ChildItem -LiteralPath (Join-Path $CacheRoot 'CMakeFiles') -Directory |
    Where-Object { $_.Name -match '(Probe|Conformance)\.dir$' -and -not (Get-ChildItem -LiteralPath $_.FullName -Recurse -File) } |
    Remove-Item -Recurse -Force
