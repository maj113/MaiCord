#requires -Version 5.1
$ErrorActionPreference = 'Stop'

# Load only the proof helpers and their exemption condition. Never execute the
# cleanup script, enumerate live processes, or access a real output directory.
$tokens = $null
$parseErrors = $null
$source = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\maintain_build_outputs.ps1'
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$helpers = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -in @('Test-ExplicitOtherBuild', 'Test-CMakeLinkHelper', 'Test-OtherBuildDescendant')
}, $true))
if ($helpers.Count -ne 3) { throw 'Expected all three cleanup proof helpers.' }
foreach ($helper in $helpers) { Invoke-Expression $helper.Extent.Text }
$conditions = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.IfStatementAst] -and
    $node.Clauses[0].Item1.Extent.Text.Contains('Test-OtherBuildDescendant')
}, $true))
if ($conditions.Count -ne 1) { throw 'Expected one cleanup exemption condition.' }
$exemption = [scriptblock]::Create($conditions[0].Clauses[0].Item1.Extent.Text)
$candidateBlock = $conditions[0].Parent
$selection = @{}
foreach ($name in @('$builder', '$target')) {
    $assignments = @($candidateBlock.FindAll({ param($node)
        $node -is [Management.Automation.Language.AssignmentStatementAst] -and
        $node.Left.Extent.Text -eq $name
    }, $true))
    if ($assignments.Count -ne 1) { throw "Expected one cleanup candidate assignment for $name." }
    $selection[$name] = [scriptblock]::Create($assignments[0].Right.Extent.Text)
}
$skipConditions = @($candidateBlock.FindAll({ param($node)
    $node -is [Management.Automation.Language.IfStatementAst] -and
    $node.Clauses[0].Item1.Extent.Text -eq '!$builder -and !$target'
}, $true))
if ($skipConditions.Count -ne 1) { throw 'Expected one cleanup candidate filter.' }
$skipCandidate = [scriptblock]::Create($skipConditions[0].Clauses[0].Item1.Extent.Text)

$repository = 'P:\MaiCord'
$releaseRoot = 'P:\MaiCord-build\Release'
$targets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
[void]$targets.Add('P:\MaiCord-build\Release\temporary.exe')
$directories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($path in @('Q:\', 'Q:\other project', 'Q:\other project\build', 'P:\', 'P:\MaiCord-other')) {
    [void]$directories.Add($path)
}
$reparsePaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Test-Path([string]$LiteralPath, [string]$PathType) {
    if ($PathType -ne 'Container') { throw 'Unexpected filesystem query.' }
    return $directories.Contains($LiteralPath)
}
function Get-Item([string]$LiteralPath, [switch]$Force) {
    if (!$directories.Contains($LiteralPath)) { throw "Unexpected directory metadata query: $LiteralPath" }
    $attributes = [IO.FileAttributes]::Directory
    if ($reparsePaths.Contains($LiteralPath)) { $attributes = $attributes -bor [IO.FileAttributes]::ReparsePoint }
    return [pscustomobject]@{ Attributes = $attributes }
}

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
function Assert-Exemption($Label, [bool]$Expected, $Leaf, $SnapshotById) {
    # These are builder candidates after live identity validation. Execute the
    # real outer condition too, so a cleanup target cannot inherit an exemption.
    $builder = $true
    $process = $Leaf
    $target = $process.ExecutablePath -and $targets.Contains([IO.Path]::GetFullPath($process.ExecutablePath))
    $actual = & $exemption
    if ($actual -ne $Expected) { throw "$Label returned $actual, expected $Expected." }
    $script:guardChecks++
}
function Assert-Blocked($Label, [bool]$Expected, $Leaf, $SnapshotById) {
    # Test the real name/target filter before the existing live-process check.
    # Snapshot records stand for verified processes that remain active.
    $process = $Leaf
    $builder = & $selection['$builder']
    $target = & $selection['$target']
    $actual = !(& $skipCandidate) -and !(& $exemption)
    if ($actual -ne $Expected) { throw "$Label returned $actual, expected $Expected." }
    $script:guardChecks++
}

$anchor = New-Record 10 0 'cmake.exe' 0 'cmake --build "Q:\other project\build"'
$shell = New-Record 11 10 'cmd.exe' 1
$ninja = New-Record 12 11 'ninja.exe' 2
$compiler = New-Record 13 12 'cl.exe' 3
$snapshot = New-Snapshot @($anchor, $shell, $ninja, $compiler)
Assert-Exemption 'Explicit outside build' $true $anchor $snapshot
Assert-Exemption 'Compiler through Ninja and shell' $true $compiler $snapshot

$anchor.CommandLine = 'cmake --build P:\MaiCord-other'
Assert-Exemption 'Adjacent directory shares only a name prefix' $true $compiler $snapshot
$anchor.CommandLine = 'cmake --build P:\MaiCord-build\Release\child'
Assert-Exemption 'Protected output child' $false $compiler $snapshot
$anchor.CommandLine = 'cmake --build P:\MaiCord'
Assert-Exemption 'Protected repository' $false $compiler $snapshot
$anchor.CommandLine = 'cmake --build P:\'
Assert-Exemption 'Protected path ancestor' $false $compiler $snapshot
$anchor.CommandLine = 'cmake --build relative\build'
Assert-Exemption 'Unknown working directory' $false $compiler $snapshot
$anchor.CommandLine = 'cmake --build Q:\missing'
Assert-Exemption 'Absent outside directory' $false $compiler $snapshot
$anchor.CommandLine = 'cmake --build "Q:\other project\build"'
[void]$reparsePaths.Add('Q:\other project')
Assert-Exemption 'Outside ancestor redirects' $false $compiler $snapshot
$reparsePaths.Clear()

$ninja.ParentProcessId = 999
Assert-Exemption 'Missing snapshot parent' $false $compiler $snapshot
$ninja.ParentProcessId = 11
$anchor.CreationDate = $epoch.AddSeconds(2)
Assert-Exemption 'Reused ancestor PID behind an older wrapper' $false $compiler $snapshot
$anchor.CreationDate = $epoch
$shell.CreationDate = $null
Assert-Exemption 'Missing ancestor creation time' $false $compiler $snapshot
$shell.CreationDate = $epoch.AddSeconds(1)
$compiler.CreationDate = $null
Assert-Exemption 'Missing candidate creation time' $false $compiler $snapshot
$compiler.CreationDate = $epoch.AddSeconds(3)

# Equal timestamps satisfy every age comparison and must still terminate when
# the snapshot contains a cycle.
$shell.CreationDate = $epoch
$ninja.CreationDate = $epoch
$shell.ParentProcessId = 12
Assert-Exemption 'Equal-time ancestry cycle' $false $compiler $snapshot
$shell.ParentProcessId = 10
$shell.CreationDate = $epoch.AddSeconds(1)
$ninja.CreationDate = $epoch.AddSeconds(2)

$shell.ExecutablePath = 'P:\MaiCord-build\Release\temporary.exe'
Assert-Exemption 'Cleanup target ancestor' $false $compiler $snapshot
$shell.ExecutablePath = ''
$compiler.ExecutablePath = 'P:\MaiCord-build\Release\temporary.exe'
Assert-Exemption 'Cleanup target candidate' $false $compiler $snapshot
$compiler.ExecutablePath = ''

$nested = New-Record 14 10 'cmake.exe' 1 'cmake --build P:\MaiCord-build\Release'
$snapshot.Add(14, $nested)
$ninja.ParentProcessId = 14
Assert-Exemption 'Nested protected CMake stops inheritance' $false $compiler $snapshot
$nested.Name = 'ctest.exe'
$nested.CommandLine = 'ctest --test-dir P:\MaiCord'
Assert-Exemption 'Nested protected CTest stops inheritance' $false $compiler $snapshot
$nested.CommandLine = 'ctest --test-dir .'
Assert-Exemption 'Nested relative build cannot borrow outside proof' $false $compiler $snapshot
$nested.Name = 'MSBuild.exe'
Assert-Exemption 'Unknown build system stops inheritance' $false $compiler $snapshot
$ninja.ParentProcessId = 11
$anchor.Name = 'ctest.exe'
$anchor.CommandLine = 'ctest --test-dir="Q:\other project\build"'
Assert-Exemption 'Explicit outside test directory' $true $compiler $snapshot

# The supported graph uses Ninja. A reusable MSBuild node is outside its
# producer set, regardless of parent lifetime or whether the node has IPC work.
$worker = New-Record 20 999 'MSBuild.exe' 1 '"Q:\MSBuild.exe" /noautoresponse /nologo /nodemode:1 /nodeReuse:true /low:false' 'Q:\MSBuild.exe'
$hostProcess = New-Record 21 0 'MSBuild.exe' 0 'MSBuild.exe project.proj' 'Q:\MSBuild.exe'
$snapshot = New-Snapshot @($worker, $hostProcess)
Assert-Blocked 'Reusable MSBuild node is outside the Ninja graph' $false $worker $snapshot
$worker.ParentProcessId = 21
Assert-Blocked 'MSBuild parent lifetime does not determine producer scope' $false $worker $snapshot
$worker.ExecutablePath = 'P:\MaiCord-build\Release\temporary.exe'
Assert-Blocked 'Exact MSBuild cleanup target still blocks' $true $worker $snapshot
$worker.ExecutablePath = 'Q:\MSBuild.exe'
$compiler.ParentProcessId = 20
$snapshot.Add(13, $compiler)
Assert-Blocked 'Compiler below MSBuild remains unknown' $true $compiler $snapshot
$ninja.ParentProcessId = 999
$snapshot.Add(12, $ninja)
Assert-Blocked 'Unknown active Ninja remains a producer' $true $ninja $snapshot
$anchor.Name = 'cmake.exe'
$anchor.CommandLine = 'cmake --build P:\MaiCord-build\Release'
$snapshot.Add(10, $anchor)
Assert-Blocked 'Own-tree CMake remains a producer' $true $anchor $snapshot
$ninja.ParentProcessId = 10
Assert-Blocked 'Own-tree Ninja remains a producer' $true $ninja $snapshot

$anchor.CommandLine = 'cmake --build "Q:\other project\build"'
$helper = New-Record 30 10 'cmake.exe' 1 '"Q:\cmake.exe" -E vs_link_exe --intdir=objects -- "Q:\tools\link.exe" /out:app.exe'
$compiler.ParentProcessId = 30
$snapshot = New-Snapshot @($anchor, $helper, $compiler)
Assert-Blocked 'Outside CMake linker helper inherits build ownership' $false $helper $snapshot
Assert-Blocked 'Linker helper does not interrupt descendant ownership' $false $compiler $snapshot
$helper.CommandLine = $helper.CommandLine.Replace('vs_link_exe', 'vs_link_dll')
Assert-Blocked 'Outside DLL linker helper inherits build ownership' $false $helper $snapshot
$anchor.CommandLine = 'cmake --build P:\MaiCord-build\Release'
Assert-Blocked 'Own-tree linker helper still blocks' $true $helper $snapshot
$anchor.CommandLine = 'cmake --build "Q:\other project\build"'
$helper.ExecutablePath = 'P:\MaiCord-build\Release\temporary.exe'
Assert-Blocked 'Exact cleanup target overrides helper ownership' $true $helper $snapshot
$helper.ExecutablePath = ''
$helper.CommandLine = 'cmake -P script.cmake -E vs_link_exe -- link.exe'
Assert-Blocked 'Script mode cannot impersonate a linker helper' $true $helper $snapshot
$helper.CommandLine = 'cmake --build . -E vs_link_exe -- link.exe'
Assert-Blocked 'Unknown nested build cannot impersonate a linker helper' $true $helper $snapshot
$helper.CommandLine = 'cmake -S source -B build -E vs_link_exe -- link.exe'
Assert-Blocked 'Configure mode cannot impersonate a linker helper' $true $helper $snapshot
$helper.CommandLine = 'cmake -E vs_link_exe -- cmake.exe --build "Q:\other project\build"'
Assert-Blocked 'Helper requires link.exe and cannot borrow an embedded build flag' $true $helper $snapshot
$helper.CommandLine = 'cmake -E copy source destination -- link.exe'
Assert-Blocked 'Other CMake helper modes remain unknown' $true $helper $snapshot

$ninja.CommandLine = 'ninja -C "Q:\other project\build" all'
$ninja.ParentProcessId = 10
$compiler.ParentProcessId = 12
$snapshot = New-Snapshot @($anchor, $ninja, $compiler)
Assert-Blocked 'Explicit outside Ninja directory is independent proof' $false $ninja $snapshot
$ninja.CommandLine = 'ninja "-C" P:\MaiCord-build\Release all'
Assert-Blocked 'Own-tree Ninja directory overrides outside ancestor' $true $ninja $snapshot
Assert-Blocked 'Compiler cannot inherit through protected Ninja directory' $true $compiler $snapshot
$ninja.CommandLine = 'ninja -C . all'
Assert-Blocked 'Relative Ninja directory cannot borrow ancestor proof' $true $ninja $snapshot
$ninja.CommandLine = 'ninja -C "Q:\other project\build" -CP:\MaiCord-build\Release all'
Assert-Blocked 'Repeated or joined Ninja directory cannot override proof' $true $ninja $snapshot

Write-Output "PASS build output guard checks=$script:guardChecks"
