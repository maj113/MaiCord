#requires -Version 5.1
# Dot-sourced by opus_conformance.ps1 and probe_network.ps1. Sets $repository and $cacheRoot and
# defines Invoke-OptInTarget, which builds one opt-in probe or conformance target of
# cmake/integration.cmake inside the single Release cache under the shared build mutex, runs it,
# then removes the binary and objects and restores the shipping-only configuration.
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))

# -Run receives the executable path. -Prepare runs before the build mutex is taken and
# -Cleanup in the final step, both reading the caller's variables.
function Invoke-OptInTarget {
    param(
        [Parameter(Mandatory)][string]$Target,
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][scriptblock]$Run,
        [scriptblock]$Prepare,
        [scriptblock]$Cleanup
    )
    $binary = Join-Path $cacheRoot "$Target.exe"
    $outputs = @($binary, (Join-Path $cacheRoot "CMakeFiles\$Target.dir"))
    . (Join-Path $PSScriptRoot 'enter_build_environment.ps1')

    $mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
    $locked = $false
    Push-Location $repository
    try {
        if ($Prepare) { & $Prepare }
        try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
        if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
        cmake --preset x64-release -DMAICORD_BUILD_INTEGRATION_TESTS=ON | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "$Label configuration failed." }
        cmake --build $cacheRoot --target $Target
        if ($LASTEXITCODE -ne 0) { throw "$Label build failed." }
        & $Run $binary
    } finally {
        if ($Cleanup) { & $Cleanup }
        if ($locked) {
            & (Join-Path $PSScriptRoot 'clear_target_outputs.ps1') -CacheRoot $cacheRoot -Paths $outputs
            $mutex.ReleaseMutex()
        }
        $mutex.Dispose()
        Pop-Location
    }
}
