#requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$ConfigureScratch,
    [switch]$RuntimeOnly
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$releaseRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
if (!(Test-Path -LiteralPath $releaseRoot)) { Write-Output 'No Release build exists.'; return }

function Enter-ReleaseCleanupLock([string]$Name = 'Local\MaiCordReleaseBuild') {
    $mutex = [Threading.Mutex]::new($false, $Name)
    try {
        try { $locked = $mutex.WaitOne(0) }
        catch [Threading.AbandonedMutexException] { $locked = $true }
        if (!$locked) { throw 'Another MaiCord Release build, test or cleanup invocation is active; temporary outputs were retained.' }
        return $mutex
    } catch {
        $mutex.Dispose()
        throw
    }
}
$cleanupMutex = $null
try {
    # The supported wrapper holds this same mutex through build/test/cleanup.
    # Its synchronous script calls acquire it reentrantly; standalone cleanup
    # cannot classify or remove outputs while another wrapper owns the cache.
    if ($Apply) { $cleanupMutex = Enter-ReleaseCleanupLock }

# Whether an existing file or directory is a reparse point (missing: -1, false).
function Test-ReparsePoint([string]$Path) {
    $attributes = [int][IO.FileInfo]::new($Path).Attributes
    return $attributes -ne -1 -and ($attributes -band [int][IO.FileAttributes]::ReparsePoint)
}
$validatedAncestors = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Assert-ReleasePath([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if (!$absolute.Equals($releaseRoot, [StringComparison]::OrdinalIgnoreCase) -and
        !$absolute.StartsWith($releaseRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path outside the single Release build: $absolute"
    }
    $ancestor = $absolute
    while ($ancestor -and !$validatedAncestors.Contains($ancestor)) {
        if (Test-ReparsePoint $ancestor) { throw "Reparse point in build path: $ancestor" }
        [void]$validatedAncestors.Add($ancestor)
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $absolute
}
[void](Assert-ReleasePath $releaseRoot)
$scratchDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$targets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$ownedTestRoots = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
# Registers every directory under $Root as removable once empty and every file $Keep accepts as a target; reparse points are
# rejected, never followed.
function Add-Tree([string]$Root, [scriptblock]$Keep = { $true }) {
    $rootPath = Assert-ReleasePath $Root
    if (![IO.Directory]::Exists($rootPath)) { return }
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($rootPath)
    while ($pending.Count) {
        $directory = Assert-ReleasePath $pending.Dequeue()
        [void]$scratchDirectories.Add($directory)
        foreach ($child in ([IO.DirectoryInfo]$directory).EnumerateFileSystemInfos()) {
            $childPath = Assert-ReleasePath $child.FullName
            if ($child -is [IO.DirectoryInfo]) { $pending.Enqueue($childPath) }
            elseif (& $Keep $child) { [void]$targets.Add($childPath) }
        }
    }
}
function Add-File([string]$Path) {
    $absolute = Assert-ReleasePath $Path
    if (Test-Path -LiteralPath $absolute -PathType Leaf) { [void]$targets.Add($absolute) }
}
function Get-CMakeVersionDirectories {
    Get-ChildItem -LiteralPath (Join-Path $releaseRoot 'CMakeFiles') -Directory -Force | Where-Object { $_.Name -match '^\d+\.\d+(?:\.\d+)?$' }
}
# CTest overwrites these files, so they are cleaned by directory, not as new outputs.
$testingRoot = Assert-ReleasePath (Join-Path $releaseRoot 'Testing')
if (Test-Path -LiteralPath $testingRoot -PathType Container) {
    [void]$ownedTestRoots.Add($testingRoot)
    Add-Tree $testingRoot
}
# Strict GUID sandbox names are explicitly authorized runner-owned cleanup.
# Any surviving marker must still match this repository, including old runs.
foreach ($profile in Get-ChildItem -LiteralPath $releaseRoot -Directory -Force) {
    if ($profile.Name -notmatch '^\.?test-profile-[0-9a-f]{32}$') { continue }
    $candidateProfile = Assert-ReleasePath $profile.FullName
    $marker = Assert-ReleasePath (Join-Path $candidateProfile '.maicord-test-profile')
    if ((Test-Path -LiteralPath $marker) -and
        ((Get-Content -LiteralPath $marker -Raw).Trim() -ne $repository)) {
        throw "Stale profile ownership marker differs: $candidateProfile"
    }
    [void]$ownedTestRoots.Add($candidateProfile)
    Add-Tree $candidateProfile
}
# Test-only files the ownership manifest omits: configure_file() resources (found in the build definition, removed even
# when a failed run left them) and the showcase plugin's stamped manifest, which every link rewrites.
foreach ($match in Select-String -Path "$repository\CMakeLists.txt", "$repository\cmake\*.cmake" -Pattern 'configure_file\(tests/[^\s")]+\.rc\.in\s+"\$\{CMAKE_CURRENT_BINARY_DIR\}/([^"/\\]+\.rc)"') {
    Add-File (Join-Path $releaseRoot $match.Matches[0].Groups[1].Value)
}
Add-File (Join-Path $releaseRoot 'showcase.maicord-plugin.json')
if ($RuntimeOnly) {
    $ConfigureScratch = $true
    $ownership = Assert-ReleasePath (Join-Path $releaseRoot 'maicord-build-outputs.txt')
    if (!(Test-Path -LiteralPath $ownership -PathType Leaf)) {
        throw 'Runtime-only cleanup requires the CMake build-output ownership manifest.'
    }
    $records = @(Get-Content -LiteralPath $ownership)
    if (!$records.Count -or $records[0] -ne ('repository|' + $repository.Replace('\', '/'))) {
        throw 'Build-output ownership manifest belongs to a different repository.'
    }
    foreach ($record in $records | Select-Object -Skip 1) {
        if (!$record) { continue }
        if ($record -notmatch '^(file|objects)\|(.+)$') { throw 'Invalid build-output ownership record.' }
        $kind = $Matches[1]
        $path = Assert-ReleasePath $Matches[2]
        if ($kind -eq 'file') {
            if ($path -eq (Join-Path $releaseRoot 'MaiCord.exe')) { continue }
            # Runtime directories can never become disposable through a manifest.
            if ($path.StartsWith((Join-Path $releaseRoot 'fonts') + '\', [StringComparison]::OrdinalIgnoreCase) -or
                $path.StartsWith((Join-Path $releaseRoot 'licenses') + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Runtime asset appeared in the disposable build-output manifest.'
            }
            if ([IO.File]::Exists($path)) { [void]$targets.Add($path) }
            if ([IO.Path]::GetExtension($path) -in @('.exe', '.dll', '.lib')) {
                foreach ($extension in @('.lib', '.exp', '.pdb', '.map', '.ilk', '.iobj', '.ipdb')) {
                    $sidecar = [IO.Path]::ChangeExtension($path, $extension)
                    if ([IO.File]::Exists($sidecar)) { [void]$targets.Add((Assert-ReleasePath $sidecar)) }
                }
            }
        } else {
            if ($path -notmatch '\\CMakeFiles\\[A-Za-z0-9_.+-]+\.dir$') { throw 'Invalid build object directory.' }
            Add-Tree $path { param($file)
                $file.Extension -match '^\.(obj|res|rsp|pdb|idb|iobj|ipdb|ilk|exp|lib|pch|i)$' -or $file.Name -cmatch '^application_pch_O(?:2|s)\.cpp$'
            }
        }
    }
    foreach ($name in @('CMakeCache.txt', 'build.ninja', 'cmake_install.cmake', 'compile_commands.json', '.ninja_deps', '.ninja_log', '.ninja_lock',
            'CTestTestfile.cmake', 'maicord-test-targets.txt', 'maicord-tests.txt', 'notification_sounds.rc',
            'CMakeFiles\cmake.check_cache', 'CMakeFiles\cmake.verify_globs', 'CMakeFiles\InstallScripts.json',
            'CMakeFiles\rules.ninja', 'CMakeFiles\TargetDirectories.txt', 'CMakeFiles\VerifyGlobs.cmake',
            'CMakeFiles\ShowIncludes\foo.h', 'CMakeFiles\ShowIncludes\main.c', 'CMakeFiles\ShowIncludes\main.obj')) {
        Add-File (Join-Path $releaseRoot $name)
    }
    foreach ($version in Get-CMakeVersionDirectories) {
        foreach ($name in @('CMakeASM_MASMCompiler.cmake', 'CMakeCCompiler.cmake', 'CMakeCXXCompiler.cmake',
                'CMakeRCCompiler.cmake', 'CMakeSystem.cmake', 'CMakeDetermineCompilerABI_C.bin', 'CMakeDetermineCompilerABI_CXX.bin')) {
            Add-File (Join-Path $version.FullName $name)
        }
    }
    [void]$targets.Add($ownership)
    # Include empty configure-created directories without treating unknown
    # files in them as disposable products.
    foreach ($name in @('generated', 'CMakeFiles', 'third_party')) { Add-Tree (Join-Path $releaseRoot $name) { $false } }
}
if ($ConfigureScratch) {
    # The resource editor's precompiled cache is not a build input or runtime asset.
    Add-File (Join-Path $releaseRoot 'notification_sounds.aps')
    # Interrupted atomic configuration writes are disposable only after the
    # canonical replacement exists and is at least as new. Never scan other
    # locations or classify arbitrary .tmp files as configure output.
    foreach ($location in @($releaseRoot, (Join-Path $releaseRoot 'CMakeFiles'))) {
        $location = Assert-ReleasePath $location
        if (!(Test-Path -LiteralPath $location -PathType Container)) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $location -File -Force) {
            $pattern = if ($location -eq $releaseRoot) {
                '^(build\.ninja|CMakeCache\.txt)\.tmp[A-Za-z0-9]+$'
            } else { '^(rules\.ninja)\.tmp[A-Za-z0-9]+$' }
            if ($file.Name -cnotmatch $pattern) { continue }
            $canonical = Assert-ReleasePath (Join-Path $location $Matches[1])
            $temporary = Assert-ReleasePath $file.FullName
            if (!(Test-Path -LiteralPath $canonical -PathType Leaf)) { continue }
            if ((Get-Item -LiteralPath $canonical -Force).LastWriteTimeUtc -lt $file.LastWriteTimeUtc) { continue }
            [void]$targets.Add($temporary)
        }
    }
    Add-File (Join-Path $releaseRoot 'CMakeFiles\CMakeConfigureLog.yaml')
    foreach ($version in Get-CMakeVersionDirectories) {
        foreach ($identity in @('CompilerIdC', 'CompilerIdCXX', 'CompilerIdASM_MASM')) {
            $scratch = Assert-ReleasePath (Join-Path $version.FullName $identity)
            if (!(Test-Path -LiteralPath $scratch -PathType Container)) { continue }
            [void]$scratchDirectories.Add($scratch)
            foreach ($directory in Get-ChildItem -LiteralPath $scratch -Directory -Recurse -Force) {
                [void]$scratchDirectories.Add((Assert-ReleasePath $directory.FullName))
            }
            foreach ($file in Get-ChildItem -LiteralPath $scratch -File -Recurse -Force) {
                [void]$targets.Add((Assert-ReleasePath $file.FullName))
            }
        }
    }
}
if ($Apply) {
    function Test-ProtectedBuildProcess($ProcessRecord, $SnapshotById) {
        # Unknown processes in other projects do not own this fixed workspace.
        # Explicit MaiCord paths also catch surviving producers after a wrapper
        # exits; their children inherit ownership only through valid identities.
        $visited = [Collections.Generic.HashSet[int]]::new()
        $child = $ProcessRecord
        while ($child) {
            $command = ([string]$child.CommandLine).Replace('/', '\')
            foreach ($protected in @($repository, $releaseRoot)) {
                $path = $protected.TrimEnd('\')
                if ([regex]::IsMatch($command, [regex]::Escape($path) + '(?=[\\\s"]|$)',
                        [Text.RegularExpressions.RegexOptions]::IgnoreCase)) { return $true }
                if ($child.ExecutablePath) {
                    $executable = [IO.Path]::GetFullPath($child.ExecutablePath)
                    if ($executable.Equals($path, [StringComparison]::OrdinalIgnoreCase) -or
                        $executable.StartsWith($path + '\', [StringComparison]::OrdinalIgnoreCase)) { return $true }
                }
            }
            if (!$child.CreationDate -or !$visited.Add([int]$child.ProcessId)) { return $false }
            $parentId = [int]$child.ParentProcessId
            if (!$SnapshotById.ContainsKey($parentId)) { return $false }
            $parent = $SnapshotById[$parentId]
            # Parent IDs survive process exit. Reject a replacement process
            # that started after this child, including through shell wrappers.
            if (!$parent.CreationDate -or
                $parent.CreationDate.ToUniversalTime() -gt $child.CreationDate.ToUniversalTime()) { return $false }
            $child = $parent
        }
        return $false
    }
    $processes = @(Get-CimInstance Win32_Process)
    $snapshotById = @{}
    foreach ($process in $processes) { $snapshotById.Add([int]$process.ProcessId, $process) }
    $processHandles = [Collections.Generic.List[Diagnostics.Process]]::new()
    $exitGrace = [Diagnostics.Stopwatch]::StartNew()
    try {
        foreach ($process in $processes) {
            # The supported build wrapper selects the fixed Ninja preset.
            # MSBuild hosts are outside this graph, including reusable workers.
            # Exact cleanup-target executables still block regardless of name.
            $builder = $process.Name -in @('cl.exe', 'link.exe', 'ninja.exe', 'cmake.exe', 'ctest.exe')
            $target = $process.ExecutablePath -and $targets.Contains([IO.Path]::GetFullPath($process.ExecutablePath))
            if (!$target -and !($builder -and (Test-ProtectedBuildProcess $process $snapshotById))) { continue }
            # A CIM snapshot can still describe a process that exited before
            # this guard runs. Only a live, matching process blocks cleanup.
            try {
                $live = [Diagnostics.Process]::GetProcessById([int]$process.ProcessId)
            } catch {
                if ($_.Exception.GetBaseException() -is [ArgumentException]) { continue }
                throw
            }
            $processHandles.Add($live)
            # Query/access failures deliberately propagate: an unverified
            # candidate is not permission to remove its possible outputs.
            try {
                $null = $live.Handle
                if ($live.HasExited) { continue }
                $started = $live.StartTime.ToUniversalTime()
            } catch {
                $queryFailure = $_
                # Exit can also race handle acquisition or the start-time
                # query. Ignore it only when exit is positively confirmed.
                $exited = $false
                try { $exited = $live.HasExited } catch {}
                if ($exited) { continue }
                throw $queryFailure
            }
            if (!$process.CreationDate -or
                [Math]::Abs(($started - $process.CreationDate.ToUniversalTime()).Ticks) -ge 10) {
                throw "Build or test process identity changed ($($process.Name), PID $($process.ProcessId)); temporary outputs were retained."
            }
            # Briefly tolerate a verified process finishing at the snapshot
            # boundary. All candidates share one second, outside test timing.
            $remainingGrace = [Math]::Max(0, 1000 - [int]$exitGrace.ElapsedMilliseconds)
            if ($live.WaitForExit($remainingGrace)) { continue }
            if ($builder) {
                throw "Build or test process is still active ($($process.Name), PID $($process.ProcessId)); temporary outputs were retained."
            }
            throw "A temporary test executable is still running ($($process.Name), PID $($process.ProcessId))."
        }
    } finally {
        foreach ($live in $processHandles) { $live.Dispose() }
    }
}
$parents = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($directory in $scratchDirectories) { [void]$parents.Add($directory) }
$removedBytes = 0L
function Assert-CurrentPlain([string]$Path) {
    [void](Assert-ReleasePath $Path)
    foreach ($current in @($Path, [IO.Path]::GetDirectoryName($Path))) {
        if (Test-ReparsePoint $current) { throw "Reparse point appeared during cleanup: $current" }
    }
}
$orderedTargets = @($targets | Where-Object { !$RuntimeOnly -or $_ -ne $ownership })
foreach ($path in $orderedTargets) {
    Assert-CurrentPlain $path
    $file = [IO.FileInfo]::new($path)
    $removedBytes += $file.Length
    if ($Apply) {
        if ($file.IsReadOnly) { $file.IsReadOnly = $false }
        $file.Delete()
    }
    $parent = [IO.Path]::GetDirectoryName($path)
    while ($parent -and !$parent.Equals($releaseRoot, [StringComparison]::OrdinalIgnoreCase)) {
        [void]$parents.Add($parent)
        $parent = [IO.Path]::GetDirectoryName($parent)
    }
}
if ($Apply) {
    foreach ($parent in $parents | Sort-Object Length -Descending) {
        Assert-CurrentPlain $parent
        if ([IO.Directory]::Exists($parent) -and ![IO.Directory]::GetFileSystemEntries($parent).Length) {
            [IO.Directory]::Delete($parent)
        }
    }
    foreach ($root in $ownedTestRoots) {
        if (Test-Path -LiteralPath $root) { throw "Owned temporary test directory remains: $root" }
    }
}
# Keep retry authority through every file/directory operation and assertion.
if ($RuntimeOnly) {
    Assert-CurrentPlain $ownership
    $removedBytes += (Get-Item -LiteralPath $ownership -Force).Length
    if ($Apply) { Remove-Item -LiteralPath $ownership -Force }
}
[pscustomobject]@{ TemporaryFiles = $targets.Count; TemporaryBytes = $removedBytes; Removed = [bool]$Apply }
} finally {
    if ($cleanupMutex) {
        try { $cleanupMutex.ReleaseMutex() }
        finally { $cleanupMutex.Dispose() }
    }
}
