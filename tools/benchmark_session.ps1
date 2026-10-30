#requires -Version 5.1
# Dot-sourced by tools/benchmark_*.ps1, opus_conformance.ps1 and probe_network.ps1. Sets $repository and
# $cacheRoot and defines Invoke-BenchmarkTarget, which builds one opt-in benchmark or probe inside
# the single Release cache under the shared build mutex, runs it, then removes the binary
# and objects and restores the shipping-only configuration.
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$cacheRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))

# -Run receives the executable path. -Prepare runs before the build mutex is taken and
# -Cleanup in the final step, both reading the caller's variables. -ExtraOutputs are
# removed with the binary; -CMakeArguments extend the benchmark configuration.
function Invoke-BenchmarkTarget {
    param(
        [Parameter(Mandatory)][string]$Target,
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][scriptblock]$Run,
        [scriptblock]$Prepare,
        [scriptblock]$Cleanup,
        [string[]]$CMakeArguments = @(),
        [string[]]$ExtraOutputs = @(),
        [switch]$AddInstallerToPath
    )
    $binary = Join-Path $cacheRoot "$Target.exe"
    $outputs = @($binary) + $ExtraOutputs + (Join-Path $cacheRoot "CMakeFiles\$Target.dir")
    if ($AddInstallerToPath) { . (Join-Path $PSScriptRoot 'enter_build_environment.ps1') -AddInstallerToPath }
    else { . (Join-Path $PSScriptRoot 'enter_build_environment.ps1') }

    $mutex = [Threading.Mutex]::new($false, 'Local\MaiCordReleaseBuild')
    $locked = $false
    Push-Location $repository
    try {
        if ($Prepare) { & $Prepare }
        try { $locked = $mutex.WaitOne([TimeSpan]::FromMinutes(45)) } catch [Threading.AbandonedMutexException] { $locked = $true }
        if (-not $locked) { throw 'The MaiCord Release build mutex was not acquired.' }
        cmake --preset x64-release -DMAICORD_BUILD_BENCHMARKS=ON @CMakeArguments | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Benchmark configuration failed.' }
        cmake --build $cacheRoot --target $Target
        if ($LASTEXITCODE -ne 0) { throw "$Label build failed." }
        & $Run $binary
    } finally {
        if ($Cleanup) { & $Cleanup }
        if ($locked) {
            & (Join-Path $PSScriptRoot 'clear_benchmark_outputs.ps1') -CacheRoot $cacheRoot -Paths $outputs
            $mutex.ReleaseMutex()
        }
        $mutex.Dispose()
        Pop-Location
    }
}
