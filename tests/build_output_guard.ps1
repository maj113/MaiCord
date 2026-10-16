#requires -Version 5.1
$ErrorActionPreference = 'Stop'

# Extract the real lock, ownership and candidate-selection code. Never execute
# cleanup, enumerate live processes, or access a real output directory.
$tokens = $null
$parseErrors = $null
$source = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\maintain_build_outputs.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$helpers = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -in @('Enter-ReleaseCleanupLock', 'Test-ProtectedBuildProcess')
}, $true))
if ($helpers.Count -ne 2) { throw 'Expected cleanup lock and ownership helpers.' }
foreach ($helper in $helpers) { Invoke-Expression $helper.Extent.Text }
$conditions = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.IfStatementAst] -and
    $node.Clauses[0].Item1.Extent.Text.Contains('Test-ProtectedBuildProcess')
}, $true))
if ($conditions.Count -ne 1) { throw 'Expected one cleanup candidate condition.' }
$skipCandidate = [scriptblock]::Create($conditions[0].Clauses[0].Item1.Extent.Text)
$selection = @{}
foreach ($name in @('$builder', '$target')) {
    $assignments = @($conditions[0].Parent.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left.Extent.Text -eq $name
    }, $true))
    if ($assignments.Count -ne 1) { throw "Expected one cleanup candidate assignment for $name." }
    $selection[$name] = [scriptblock]::Create($assignments[0].Right.Extent.Text)
}

$repository = 'P:\MaiCord'
$releaseRoot = 'P:\MaiCord-build\Release'
$targets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
[void]$targets.Add('P:\MaiCord-build\Release\temporary.exe')
$epoch = [DateTime]::new(2020, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
function New-Record($Id, $Parent, $Name, $Seconds, $Command = '', $Executable = '') {
    return [pscustomobject]@{
        ProcessId = $Id; ParentProcessId = $Parent; Name = $Name
        CreationDate = $epoch.AddSeconds($Seconds); CommandLine = $Command; ExecutablePath = $Executable
    }
}
function New-Snapshot([object[]]$Items) {
    $map = @{}
    foreach ($item in $Items) { $map.Add([int]$item.ProcessId, $item) }
    return $map
}
$script:guardChecks = 0
function Assert-Blocked($Label, [bool]$Expected, $Leaf, $SnapshotById) {
    # Snapshot fixtures stand for live, identity-validated processes.
    $process = $Leaf
    $builder = & $selection['$builder']
    $target = & $selection['$target']
    $actual = !(& $skipCandidate)
    if ($actual -ne $Expected) { throw "$Label returned $actual, expected $Expected." }
    $script:guardChecks++
}

$anchor = New-Record 10 0 'cmake.exe' 0 'cmake --build .tools/openmac-shared-aggregate'
$shell = New-Record 11 10 'cmd.exe' 1
$ninja = New-Record 12 11 'ninja.exe' 2
$compiler = New-Record 13 12 'cl.exe' 3
$snapshot = New-Snapshot @($anchor, $shell, $ninja, $compiler)
Assert-Blocked 'Unrelated relative CMake does not own MaiCord outputs' $false $anchor $snapshot
Assert-Blocked 'Unrelated Ninja descendant does not own MaiCord outputs' $false $ninja $snapshot
Assert-Blocked 'Unrelated compiler descendant does not own MaiCord outputs' $false $compiler $snapshot
$anchor.CommandLine = 'cmake --fresh -S Q:\other\source -B Q:\other\build -G Ninja'
Assert-Blocked 'Unrelated configure graph does not own MaiCord outputs' $false $compiler $snapshot
$anchor.CommandLine = 'cmake --build P:\MaiCord-other'
Assert-Blocked 'Adjacent project is not a protected-path match' $false $anchor $snapshot
$anchor.CommandLine = 'cmake --build P:\MaiCord-build\Release-other'
Assert-Blocked 'Adjacent output tree is not a protected-path match' $false $anchor $snapshot
$anchor.CommandLine = 'cmake --build "p:/maicord-build/release"'
Assert-Blocked 'Protected build path accepts slash and case variants' $true $anchor $snapshot
Assert-Blocked 'Protected ownership crosses shell and Ninja' $true $compiler $snapshot
$anchor.CommandLine = 'cmake --preset x64-release -S P:\MaiCord'
Assert-Blocked 'Protected source configure remains a producer' $true $anchor $snapshot
$anchor.CommandLine = 'ctest --test-dir P:\MaiCord-build\Release'
$anchor.Name = 'ctest.exe'
Assert-Blocked 'Protected CTest graph remains a producer' $true $compiler $snapshot
$anchor.Name = 'cmake.exe'
$anchor.CommandLine = 'cmake --build P:\MaiCord-build\Release'

$ninja.ParentProcessId = 999
Assert-Blocked 'Missing parent cannot attribute unrelated compiler' $false $compiler $snapshot
$ninja.ParentProcessId = 11
$anchor.CreationDate = $epoch.AddSeconds(2)
Assert-Blocked 'Reused ancestor PID cannot attribute an older wrapper' $false $compiler $snapshot
$anchor.CreationDate = $epoch
$shell.CreationDate = $null
Assert-Blocked 'Missing ancestor time cannot establish ownership' $false $compiler $snapshot
$shell.CreationDate = $epoch.AddSeconds(1)
$compiler.CreationDate = $null
Assert-Blocked 'Missing candidate time cannot establish ancestry' $false $compiler $snapshot
$compiler.CreationDate = $epoch.AddSeconds(3)
$shell.CreationDate = $epoch
$ninja.CreationDate = $epoch
$shell.ParentProcessId = 12
Assert-Blocked 'Equal-time ancestry cycle terminates without ownership' $false $compiler $snapshot
$shell.ParentProcessId = 10
$shell.CreationDate = $epoch.AddSeconds(1)
$ninja.CreationDate = $epoch.AddSeconds(2)
$ninja.CommandLine = 'ninja -CP:\MaiCord-build\Release'
$ninja.ParentProcessId = 999
Assert-Blocked 'Explicit orphan Ninja remains protected' $true $ninja $snapshot
Assert-Blocked 'Orphan Ninja compiler inherits protected ownership' $true $compiler $snapshot
$ninja.CommandLine = ''
$shell.ExecutablePath = 'P:\MaiCord-build\Release\temporary.exe'
$ninja.ParentProcessId = 11
Assert-Blocked 'Protected executable ancestor establishes ownership' $true $compiler $snapshot
$shell.ExecutablePath = ''
$compiler.CommandLine = 'cl /c "P:\MaiCord\src\app.cpp"'
$compiler.ParentProcessId = 999
Assert-Blocked 'Explicit orphan compiler remains protected' $true $compiler $snapshot
$compiler.CommandLine = ''
$compiler.Name = 'MSBuild.exe'
Assert-Blocked 'MSBuild worker remains outside supported Ninja producers' $false $compiler $snapshot
$compiler.Name = 'unrelated.exe'
$compiler.ExecutablePath = 'P:\MaiCord-build\Release\temporary.exe'
Assert-Blocked 'Exact cleanup executable blocks regardless of process name' $true $compiler $snapshot
$compiler.CreationDate = $null
Assert-Blocked 'Exact cleanup target still requires live identity verification' $true $compiler $snapshot

# A unique kernel mutex has no filesystem output. Separate runspaces exercise
# real thread contention without launching processes or touching another build.
$lockDefinition = ($helpers | Where-Object Name -eq 'Enter-ReleaseCleanupLock').Extent.Text
$lockName = 'Local\MaiCordGuardFixture-' + [Guid]::NewGuid().ToString('N')
$owner = Enter-ReleaseCleanupLock $lockName
$nested = $null
$runspace = [Management.Automation.Runspaces.RunspaceFactory]::CreateRunspace()
$runspace.ThreadOptions = [Management.Automation.Runspaces.PSThreadOptions]::ReuseThread
$runspace.Open()
$worker = [PowerShell]::Create()
$worker.Runspace = $runspace
try {
    $nested = Enter-ReleaseCleanupLock $lockName
    $nested.ReleaseMutex(); $nested.Dispose(); $nested = $null
    $script:guardChecks++
    [void]$worker.AddScript({ param($Definition, $Name)
        Invoke-Expression $Definition
        try {
            $mutex = Enter-ReleaseCleanupLock $Name
            try { 'acquired' } finally { $mutex.ReleaseMutex(); $mutex.Dispose() }
        } catch { 'blocked' }
    }).AddArgument($lockDefinition).AddArgument($lockName)
    $result = @($worker.Invoke())
    if ($worker.Streams.Error.Count -or $result.Count -ne 1 -or $result[0] -ne 'blocked') {
        throw "Standalone cleanup did not reject a concurrent wrapper owner: result=$result errors=$($worker.Streams.Error)."
    }
    $script:guardChecks++
    $owner.ReleaseMutex(); $owner.Dispose(); $owner = $null
    $result = @($worker.Invoke())
    if ($worker.Streams.Error.Count -or $result.Count -ne 1 -or $result[0] -ne 'acquired') {
        throw 'Standalone cleanup did not acquire the released workspace lock.'
    }
    $script:guardChecks++
} finally {
    if ($nested) { $nested.ReleaseMutex(); $nested.Dispose() }
    if ($owner) { $owner.ReleaseMutex(); $owner.Dispose() }
    $worker.Dispose()
    $runspace.Dispose()
}
Write-Output "PASS build output guard checks=$script:guardChecks"
