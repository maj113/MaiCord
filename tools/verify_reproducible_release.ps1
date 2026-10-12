[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $FirstBuild,

    [Parameter(Mandatory = $true)]
    [string] $SecondBuild,

    [ValidateNotNullOrEmpty()]
    [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$firstRoot = (Resolve-Path -LiteralPath $FirstBuild).Path
$secondRoot = (Resolve-Path -LiteralPath $SecondBuild).Path
if ([StringComparer]::OrdinalIgnoreCase.Equals($firstRoot, $secondRoot)) {
    throw 'Independent build roots are required.'
}

$firstOutput = Join-Path $firstRoot $Configuration
$secondOutput = Join-Path $secondRoot $Configuration
$artifacts = @(
    'MaiCord.exe',
    'MaiCordAccount.dll',
    'MaiCordTests.exe',
    'MaiCordUiProbe.exe'
)

$mismatches = 0
foreach ($artifact in $artifacts) {
    $firstPath = Join-Path $firstOutput $artifact
    $secondPath = Join-Path $secondOutput $artifact
    if (!(Test-Path -LiteralPath $firstPath -PathType Leaf) -or
        !(Test-Path -LiteralPath $secondPath -PathType Leaf)) {
        throw "Missing reproducibility artifact: $artifact"
    }

    $firstFile = Get-Item -LiteralPath $firstPath
    $secondFile = Get-Item -LiteralPath $secondPath
    $firstHash = (Get-FileHash -LiteralPath $firstPath -Algorithm SHA256).Hash
    $secondHash = (Get-FileHash -LiteralPath $secondPath -Algorithm SHA256).Hash
    $equal = $firstFile.Length -eq $secondFile.Length -and $firstHash -eq $secondHash
    if (!$equal) {
        ++$mismatches
    }
    Write-Output ('{0} bytes={1} equal={2} sha256={3}' -f
        $artifact, $firstFile.Length, $equal, $firstHash)
}

if ($mismatches -ne 0) {
    throw "Release reproducibility failed: $mismatches artifact(s) differ."
}

Write-Output ('Release reproducibility passed: {0} byte-identical artifacts.' -f
    $artifacts.Count)
