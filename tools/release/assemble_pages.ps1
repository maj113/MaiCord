# Assembles the GitHub Pages artifact: the website from site/ plus the signed
# channel manifests (updates/) from an update-channels checkout. Every Pages
# deploy replaces the whole site, so release.yml, manifest-refresh.yml and
# site.yml all deploy this same union; a deploy missing either half would take
# the website or the update channels offline.
param(
    # A checkout of the update-channels branch.
    [Parameter(Mandatory = $true)][string]$Channels,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Stop'
$site = Join-Path $PSScriptRoot '../../site'
if (!(Test-Path (Join-Path $site 'index.html'))) { throw "site/index.html is missing." }
if (Test-Path (Join-Path $site 'updates')) { throw "site/updates would collide with the update channels." }
$updates = Join-Path $Channels 'updates'
foreach ($channel in 'stable', 'preview') {
    foreach ($suffix in '', '.sig') {
        $file = Join-Path $updates "maicord-$channel.manifest.json$suffix"
        if (!(Test-Path $file)) { throw "Refusing to deploy without $file." }
    }
}
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Path $Out | Out-Null
Copy-Item -Recurse -Path (Join-Path $site '*') -Destination $Out
Copy-Item -Recurse -Path $updates -Destination $Out
Get-ChildItem -Recurse -File $Out | ForEach-Object { Write-Host $_.FullName }
