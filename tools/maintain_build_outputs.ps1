#requires -Version 5.1
[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$ConfigureScratch,
    [switch]$RuntimeOnly,
    [string[]]$NewFiles = @(),
    [string]$TestProfile
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).ProviderPath
$releaseRoot = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $repository) 'MaiCord-build\Release'))
if (!(Test-Path -LiteralPath $releaseRoot)) { Write-Output 'No Release build exists.'; return }

$validatedAncestors = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Assert-ReleasePath([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if (!$absolute.Equals($releaseRoot, [StringComparison]::OrdinalIgnoreCase) -and
        !$absolute.StartsWith($releaseRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path outside the single Release build: $absolute"
    }
    $ancestor = $absolute
    while ($ancestor -and !$validatedAncestors.Contains($ancestor)) {
        if (Test-Path -LiteralPath $ancestor) {
            $item = Get-Item -LiteralPath $ancestor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point in build path: $ancestor"
            }
        }
        [void]$validatedAncestors.Add($ancestor)
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $absolute
}
[void](Assert-ReleasePath $releaseRoot)
$allowedTargets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$manifest = Join-Path $releaseRoot 'maicord-test-targets.txt'
# The build definition: CMakeLists.txt and the cmake/*.cmake modules it includes.
$listFile = Get-Content -LiteralPath (Join-Path $repository 'CMakeLists.txt') -Raw
$modules = @{}
foreach ($module in Get-ChildItem -LiteralPath (Join-Path $repository 'cmake') -Filter '*.cmake' -File) {
    $modules['cmake/' + $module.Name] = Get-Content -LiteralPath $module.FullName -Raw
}
$source = (@($listFile) + @($modules.Values)) -join "`n"
$resourceNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($match in [regex]::Matches($source, 'configure_file\(tests/[^\s")]+\.rc\.in\s+"\$\{CMAKE_CURRENT_BINARY_DIR\}/([^"/\\]+\.rc)"')) {
    [void]$resourceNames.Add($match.Groups[1].Value)
}
if ($NewFiles.Count -and !(Test-Path -LiteralPath $manifest)) {
    $profileOnly = $TestProfile -and !@($NewFiles | Where-Object {
        ![IO.Path]::GetFullPath($_).StartsWith([IO.Path]::GetFullPath($TestProfile) + '\', [StringComparison]::OrdinalIgnoreCase)
    }).Count
    if (!$profileOnly) { throw 'CMake test target allowlist is missing.' }
}
if (Test-Path -LiteralPath $manifest) {
    [void](Assert-ReleasePath $manifest)
    # Production targets are defined before CMakeLists.txt includes cmake/tests.cmake:
    # in its own text up to that line and in the modules it includes there.
    $testBoundary = $listFile.IndexOf('include(cmake/tests.cmake)', [StringComparison]::Ordinal)
    if ($testBoundary -lt 0) { throw 'Cannot identify production/test target boundary.' }
    $production = @($listFile.Substring(0, $testBoundary))
    foreach ($match in [regex]::Matches($production[0], '(?m)^\s*include\((cmake/[A-Za-z0-9_]+\.cmake)\)')) {
        if (!$modules.ContainsKey($match.Groups[1].Value)) { throw "Missing CMake module: $($match.Groups[1].Value)" }
        $production += $modules[$match.Groups[1].Value]
    }
    $productionTargets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($match in [regex]::Matches(($production -join "`n"),
            '(?m)^\s*(?:add_library|add_executable|maicord_library|maicord_program)\(\s*([A-Za-z0-9_.+-]+)')) {
        [void]$productionTargets.Add($match.Groups[1].Value)
    }
    [void]$productionTargets.Add('MaiCord')
    [void]$productionTargets.Add('MaiCordAccount')
    foreach ($line in Get-Content -LiteralPath $manifest) {
        $target = $line.Trim()
        if (!$target) { continue }
        if ($target -notmatch '^[A-Za-z0-9_][A-Za-z0-9_.+-]*$' -or $productionTargets.Contains($target)) {
            throw "Invalid temporary test target: $target"
        }
        [void]$allowedTargets.Add($target)
    }
    if (!$allowedTargets.Count) { throw 'CMake test target allowlist is empty.' }
}

$profileRoot = $null
if ($TestProfile) {
    $profileRoot = Assert-ReleasePath $TestProfile
    if ([IO.Path]::GetDirectoryName($profileRoot) -ne $releaseRoot -or
        [IO.Path]::GetFileName($profileRoot) -notmatch '^test-profile-[0-9a-f]{32}$' -or
        (Get-Content -LiteralPath (Join-Path $profileRoot '.maicord-test-profile') -Raw).Trim() -ne $repository) {
        throw 'Temporary profile ownership marker differs.'
    }
}
$testingPrefix = (Join-Path $releaseRoot 'Testing') + '\'
$generatedPrefix = (Join-Path $releaseRoot 'generated') + '\'
$objectPrefix = (Join-Path $releaseRoot 'CMakeFiles') + '\'
# Constant-time target lookups: a test run classifies thousands of new files
# against hundreds of temporary targets.
function Is-TestFile([string]$Path) {
    if ($profileRoot -and $Path.StartsWith($profileRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { return $true }
    if ($Path.StartsWith($testingPrefix, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    if ($Path.StartsWith($generatedPrefix, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetExtension($Path) -match '^\.(h|hpp|inc)$') { return $true }
    if ([IO.Path]::GetDirectoryName($Path) -eq $releaseRoot) {
        if ($resourceNames.Contains([IO.Path]::GetFileName($Path))) { return $true }
        return $allowedTargets.Contains([IO.Path]::GetFileNameWithoutExtension($Path)) -and
            [IO.Path]::GetExtension($Path) -match '^\.(exe|dll|lib|exp|pdb|map|iobj|ipdb|ilk|idb)$'
    }
    if ($Path.StartsWith($objectPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        $relative = $Path.Substring($objectPrefix.Length)
        $separator = $relative.IndexOf('\')
        if ($separator -gt 4) {
            $directory = $relative.Substring(0, $separator)
            if ($directory.EndsWith('.dir', [StringComparison]::OrdinalIgnoreCase) -and
                $allowedTargets.Contains($directory.Substring(0, $directory.Length - 4))) { return $true }
        }
    }
    return $false
}
$scratchDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$targets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$ownedTestRoots = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Add-OwnedTree([string]$Root) {
    $rootPath = Assert-ReleasePath $Root
    if (!(Test-Path -LiteralPath $rootPath -PathType Container)) { return }
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($rootPath)
    while ($pending.Count) {
        $directory = Assert-ReleasePath $pending.Dequeue()
        [void]$scratchDirectories.Add($directory)
        # Validate each child before traversing it; never follow reparse points.
        foreach ($child in Get-ChildItem -LiteralPath $directory -Force) {
            $childPath = Assert-ReleasePath $child.FullName
            if ($child.PSIsContainer) { $pending.Enqueue($childPath) }
            else { [void]$targets.Add($childPath) }
        }
    }
}
# CTest overwrites these files, so their absence from NewFiles is not ownership.
$testingRoot = Assert-ReleasePath (Join-Path $releaseRoot 'Testing')
if (Test-Path -LiteralPath $testingRoot -PathType Container) {
    [void]$ownedTestRoots.Add($testingRoot)
    Add-OwnedTree $testingRoot
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
    Add-OwnedTree $candidateProfile
}
foreach ($path in $NewFiles) {
    $absolute = Assert-ReleasePath $path
    if ((Test-Path -LiteralPath $absolute -PathType Leaf) -and (Is-TestFile $absolute)) { [void]$targets.Add($absolute) }
}
# This configure-generated resource is test-only even if a previous failed run left it.
if ($resourceNames.Contains('indic_input_fixtures.rc')) {
    $resource = Assert-ReleasePath (Join-Path $releaseRoot 'indic_input_fixtures.rc')
    if (Test-Path -LiteralPath $resource -PathType Leaf) { [void]$targets.Add($resource) }
}
# The test-only showcase plugin's stamped manifest (rewritten by every link, so
# it is never a new file of the run that last wrote it).
if ($allowedTargets.Contains('MaiCordShowcasePlugin')) {
    $pluginManifest = Assert-ReleasePath (Join-Path $releaseRoot 'showcase.maicord-plugin.json')
    if (Test-Path -LiteralPath $pluginManifest -PathType Leaf) { [void]$targets.Add($pluginManifest) }
}
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
            if ($path -in @((Join-Path $releaseRoot 'MaiCord.exe'), (Join-Path $releaseRoot 'MaiCordAccount.dll'))) { continue }
            # Runtime directories can never become disposable through a manifest.
            if ($path.StartsWith((Join-Path $releaseRoot 'fonts') + '\', [StringComparison]::OrdinalIgnoreCase) -or
                $path.StartsWith((Join-Path $releaseRoot 'licenses') + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw 'Runtime asset appeared in the disposable build-output manifest.'
            }
            if (Test-Path -LiteralPath $path -PathType Leaf) { [void]$targets.Add($path) }
            if ([IO.Path]::GetExtension($path) -in @('.exe', '.dll', '.lib')) {
                foreach ($extension in @('.lib', '.exp', '.pdb', '.map', '.ilk', '.iobj', '.ipdb')) {
                    $sidecar = Assert-ReleasePath ([IO.Path]::ChangeExtension($path, $extension))
                    if (Test-Path -LiteralPath $sidecar -PathType Leaf) { [void]$targets.Add($sidecar) }
                }
            }
        } else {
            if ($path -notmatch '\\CMakeFiles\\[A-Za-z0-9_.+-]+\.dir$') { throw 'Invalid build object directory.' }
            if (!(Test-Path -LiteralPath $path -PathType Container)) { continue }
            $pending = [Collections.Generic.Queue[string]]::new()
            $pending.Enqueue($path)
            while ($pending.Count) {
                $directory = Assert-ReleasePath $pending.Dequeue()
                [void]$scratchDirectories.Add($directory)
                foreach ($child in Get-ChildItem -LiteralPath $directory -Force) {
                    $childPath = Assert-ReleasePath $child.FullName
                    if ($child.PSIsContainer) { $pending.Enqueue($childPath) }
                    elseif ($child.Extension -match '^\.(obj|res|rsp|pdb|idb|iobj|ipdb|ilk|exp|lib|pch|i)$' -or
                        $child.Name -cmatch '^application_pch_O(?:2|s)\.cpp$') { [void]$targets.Add($childPath) }
                }
            }
        }
    }
    foreach ($name in @('CMakeCache.txt', 'build.ninja', 'cmake_install.cmake', '.ninja_deps', '.ninja_log', '.ninja_lock',
            'CTestTestfile.cmake', 'maicord-test-targets.txt', 'maicord-test-shards.txt', 'notification_sounds.rc',
            'MaiCord.map', 'MaiCordAccount.map',
            'CMakeFiles\cmake.check_cache', 'CMakeFiles\cmake.verify_globs', 'CMakeFiles\InstallScripts.json',
            'CMakeFiles\rules.ninja', 'CMakeFiles\TargetDirectories.txt', 'CMakeFiles\VerifyGlobs.cmake',
            'CMakeFiles\ShowIncludes\foo.h', 'CMakeFiles\ShowIncludes\main.c', 'CMakeFiles\ShowIncludes\main.obj')) {
        $path = Assert-ReleasePath (Join-Path $releaseRoot $name)
        if (Test-Path -LiteralPath $path -PathType Leaf) { [void]$targets.Add($path) }
    }
    foreach ($version in Get-ChildItem -LiteralPath (Join-Path $releaseRoot 'CMakeFiles') -Directory -Force) {
        if ($version.Name -notmatch '^\d+\.\d+(?:\.\d+)?$') { continue }
        foreach ($name in @('CMakeASM_MASMCompiler.cmake', 'CMakeCCompiler.cmake', 'CMakeCXXCompiler.cmake',
                'CMakeRCCompiler.cmake', 'CMakeSystem.cmake', 'CMakeDetermineCompilerABI_C.bin', 'CMakeDetermineCompilerABI_CXX.bin')) {
            $path = Assert-ReleasePath (Join-Path $version.FullName $name)
            if (Test-Path -LiteralPath $path -PathType Leaf) { [void]$targets.Add($path) }
        }
    }
    [void]$targets.Add($ownership)
    # Include empty configure-created directories without treating unknown
    # files in them as disposable products.
    foreach ($name in @('generated', 'CMakeFiles', 'third_party')) {
        $root = Assert-ReleasePath (Join-Path $releaseRoot $name)
        if (!(Test-Path -LiteralPath $root -PathType Container)) { continue }
        $pending = [Collections.Generic.Queue[string]]::new()
        $pending.Enqueue($root)
        while ($pending.Count) {
            $directory = Assert-ReleasePath $pending.Dequeue()
            [void]$scratchDirectories.Add($directory)
            foreach ($child in Get-ChildItem -LiteralPath $directory -Force) {
                [void](Assert-ReleasePath $child.FullName)
                if ($child.PSIsContainer) { $pending.Enqueue($child.FullName) }
            }
        }
    }
}
if ($ConfigureScratch) {
    # The resource editor's precompiled cache is not a build input or runtime asset.
    $resourceCache = Assert-ReleasePath (Join-Path $releaseRoot 'notification_sounds.aps')
    if (Test-Path -LiteralPath $resourceCache -PathType Leaf) { [void]$targets.Add($resourceCache) }
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
    $log = Assert-ReleasePath (Join-Path $releaseRoot 'CMakeFiles\CMakeConfigureLog.yaml')
    if (Test-Path -LiteralPath $log -PathType Leaf) { [void]$targets.Add($log) }
    foreach ($version in Get-ChildItem -LiteralPath (Join-Path $releaseRoot 'CMakeFiles') -Directory -Force) {
        if ($version.Name -notmatch '^\d+\.\d+(?:\.\d+)?$') { continue }
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
    function Test-ExplicitOtherBuild($ProcessRecord) {
        $separator = '(?:=|\s+)'
        $option = if ($ProcessRecord.Name -eq 'ctest.exe') { '--test-dir' }
            elseif ($ProcessRecord.Name -eq 'cmake.exe') { '--build' }
            elseif ($ProcessRecord.Name -eq 'ninja.exe') {
                if ([regex]::Matches($ProcessRecord.CommandLine, '(?:^|\s)"?-C').Count -ne 1) { return $false }
                $separator = '\s+'
                '(?:"-C"|-C)'
            } else { return $false }
        $prefix = if ($ProcessRecord.Name -eq 'cmake.exe') {
            '^\s*(?:"[^"]+"|[^\s"]+)\s+'
        } else { '(?:^|\s)' }
        $match = [regex]::Match($ProcessRecord.CommandLine,
            $prefix + $option + $separator + '(?:"(?<path>[A-Za-z]:[\\/][^"]+)"|(?<path>[A-Za-z]:[\\/][^\s"]+))(?=\s|$)')
        if (!$match.Success) { return $false }
        $other = [IO.Path]::GetFullPath($match.Groups['path'].Value)
        foreach ($protected in @($repository, $releaseRoot)) {
            $otherPrefix = $other.TrimEnd('\') + '\'
            $protectedPrefix = $protected.TrimEnd('\') + '\'
            if ($otherPrefix.Equals($protectedPrefix, [StringComparison]::OrdinalIgnoreCase) -or
                $other.StartsWith($protectedPrefix, [StringComparison]::OrdinalIgnoreCase) -or
                $protected.StartsWith($otherPrefix, [StringComparison]::OrdinalIgnoreCase)) { return $false }
        }
        if (!(Test-Path -LiteralPath $other -PathType Container)) { return $false }
        # An explicit outside path is proof only if no ancestor redirects it.
        $ancestor = $other
        while ($ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { return $false }
            $ancestor = [IO.Path]::GetDirectoryName($ancestor)
        }
        return $true
    }
    function Test-CMakeLinkHelper($ProcessRecord) {
        if ($ProcessRecord.Name -ne 'cmake.exe') { return $false }
        # Ninja invokes CMake as a linker wrapper. Only these modes, with an
        # actual link.exe command, inherit the owning build's outside proof.
        $match = [regex]::Match($ProcessRecord.CommandLine,
            '^\s*(?:"[^"]+"|[^\s"]+)\s+-E\s+vs_link_(?:exe|dll)\s+(?:[^\r\n]*?\s+)?--\s+(?:"(?<tool>[^"]+)"|(?<tool>[^\s"]+))(?=\s|$)')
        return $match.Success -and
            [IO.Path]::GetFileName($match.Groups['tool'].Value).Equals('link.exe', [StringComparison]::OrdinalIgnoreCase)
    }
    function Test-OtherBuildDescendant($ProcessRecord, $SnapshotById) {
        # Only these tools inherit build ownership. A nested CMake/CTest or
        # MSBuild invocation must supply its own proof instead of borrowing it.
        if ($ProcessRecord.Name -notin @('cl.exe', 'link.exe', 'ninja.exe') -and
            !(Test-CMakeLinkHelper $ProcessRecord)) { return $false }
        if ($ProcessRecord.Name -eq 'ninja.exe' -and
            [regex]::IsMatch($ProcessRecord.CommandLine, '(?:^|\s)"?-C')) {
            return (Test-ExplicitOtherBuild $ProcessRecord)
        }
        $visited = [Collections.Generic.HashSet[int]]::new()
        $child = $ProcessRecord
        while ($child) {
            if (!$child.CreationDate -or !$visited.Add([int]$child.ProcessId)) { return $false }
            $parentId = [int]$child.ParentProcessId
            if (!$SnapshotById.ContainsKey($parentId)) { return $false }
            $parent = $SnapshotById[$parentId]
            # Parent IDs survive process exit. Reject a replacement process
            # that started after this child, including through shell wrappers.
            if (!$parent.CreationDate -or
                $parent.CreationDate.ToUniversalTime() -gt $child.CreationDate.ToUniversalTime()) { return $false }
            if ($parent.ExecutablePath -and
                $targets.Contains([IO.Path]::GetFullPath($parent.ExecutablePath))) { return $false }
            if ($parent.Name -in @('cmake.exe', 'ctest.exe')) {
                if (Test-ExplicitOtherBuild $parent) { return $true }
                if (!(Test-CMakeLinkHelper $parent)) { return $false }
            }
            if ($parent.Name -eq 'MSBuild.exe') { return $false }
            if ($parent.Name -eq 'ninja.exe' -and
                [regex]::IsMatch($parent.CommandLine, '(?:^|\s)"?-C')) {
                return (Test-ExplicitOtherBuild $parent)
            }
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
            if (!$builder -and !$target) { continue }
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
            if ($builder -and !$target -and
                ((Test-ExplicitOtherBuild $process) -or (Test-OtherBuildDescendant $process $snapshotById))) { continue }
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
# Seed owned directories even if a test wrote no files into them.
if ($profileRoot) {
    [void]$parents.Add($profileRoot)
    foreach ($directory in Get-ChildItem -LiteralPath $profileRoot -Directory -Recurse -Force) {
        [void]$parents.Add((Assert-ReleasePath $directory.FullName))
    }
}
$removedBytes = 0L
$removedDirectories = 0
function Assert-CurrentPlain([string]$Path) {
    [void](Assert-ReleasePath $Path)
    foreach ($current in @($Path, [IO.Path]::GetDirectoryName($Path))) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse point appeared during cleanup: $current"
            }
        }
    }
}
foreach ($target in $allowedTargets) {
    $objectRoot = Assert-ReleasePath (Join-Path $releaseRoot "CMakeFiles\$target.dir")
    if (Test-Path -LiteralPath $objectRoot -PathType Container) {
        [void]$parents.Add($objectRoot)
        foreach ($directory in Get-ChildItem -LiteralPath $objectRoot -Directory -Recurse -Force) {
            [void](Assert-ReleasePath $directory.FullName)
            [void]$parents.Add($directory.FullName)
        }
    }
}
foreach ($path in $targets) { [void](Assert-ReleasePath $path) }
$orderedTargets = @($targets | Where-Object { !$RuntimeOnly -or $_ -ne $ownership })
foreach ($path in $orderedTargets) {
    Assert-CurrentPlain $path
    $removedBytes += (Get-Item -LiteralPath $path -Force).Length
    if ($Apply) { Remove-Item -LiteralPath $path -Force }
    $parent = [IO.Path]::GetDirectoryName($path)
    while ($parent -and !$parent.Equals($releaseRoot, [StringComparison]::OrdinalIgnoreCase)) {
        [void]$parents.Add($parent)
        $parent = [IO.Path]::GetDirectoryName($parent)
    }
}
if ($Apply) {
    foreach ($parent in $parents | Sort-Object Length -Descending) {
        Assert-CurrentPlain $parent
        if ((Test-Path -LiteralPath $parent -PathType Container) -and
            !@(Get-ChildItem -LiteralPath $parent -Force).Count) {
            Remove-Item -LiteralPath $parent
            ++$removedDirectories
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
$bytes = [long]((Get-ChildItem -LiteralPath $releaseRoot -File -Recurse -Force | Measure-Object Length -Sum).Sum)
[pscustomobject]@{ ReleaseDirectory = $releaseRoot; Bytes = $bytes; TemporaryFiles = $targets.Count; TemporaryBytes = $removedBytes; RemovedDirectories = $removedDirectories; Removed = [bool]$Apply }
