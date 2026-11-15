#requires -Version 5.1
# Prints one line for each file of a checkout: "<last write time in ticks> <bytes> <path>", the path relative to -Root
# with "/". tools/build_linux.sh calls it from WSL for a checkout on a Windows drive: Windows lists the 5,400 files in
# one second, and rsync needs 4 to 10 seconds to examine them through the 9p mount.
# It does not list what the copy does not take: entries named .git, Analysis, docs or .claude, and *.log files.
param([Parameter(Mandatory)][string]$Root)
$ErrorActionPreference = 'Stop'
$Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
$out = [IO.StreamWriter]::new([Console]::OpenStandardOutput(), [Text.UTF8Encoding]::new($false))
$out.NewLine = "`n"
$skipped = [Collections.Generic.HashSet[string]]::new([string[]]('.git', 'Analysis', 'docs', '.claude'))
$pending = [Collections.Generic.Stack[IO.DirectoryInfo]]::new()
$pending.Push([IO.DirectoryInfo]::new($Root))
while ($pending.Count) {
    foreach ($entry in $pending.Pop().EnumerateFileSystemInfos()) {
        if ($skipped.Contains($entry.Name)) { continue }
        if ($entry -is [IO.DirectoryInfo]) { $pending.Push($entry) }
        elseif (!$entry.Name.EndsWith('.log')) {
            $out.WriteLine("$($entry.LastWriteTimeUtc.Ticks) $($entry.Length) $($entry.FullName.Substring($Root.Length + 1).Replace('\', '/'))")
        }
    }
}
$out.Flush()
