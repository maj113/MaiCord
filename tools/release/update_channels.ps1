# Updates the update-channels branch, which holds the GitHub Pages site that
# serves the signed channel manifests (updates/maicord-<channel>.manifest.json
# and .sig). Checks the branch out into -Site (an orphan branch on first use),
# runs `maicord_release.py channels`, commits and pushes when a manifest
# changed, and writes deploy=true|false to GITHUB_OUTPUT.
#
# Environment: GH_TOKEN (contents: write), GITHUB_REPOSITORY, PUBLIC_KEY and,
# when a manifest needs re-signing, MAICORD_UPDATE_SIGNING_KEY.
param(
    [Parameter(Mandatory = $true)][string]$Site,
    [string]$Publish,
    [int]$RefreshWithinDays = 30,
    [Parameter(Mandatory = $true)][string]$Message,
    # Deploy the current site even when nothing changed (manual runs).
    [switch]$AlwaysDeploy,
    # Local dry runs point this at a scratch bare repository.
    [string]$Remote = "https://github.com/$env:GITHUB_REPOSITORY.git"
)
$ErrorActionPreference = 'Stop'
$branch = 'update-channels'
if (!$env:GH_TOKEN -or !$env:GITHUB_REPOSITORY -or !$env:PUBLIC_KEY) { throw 'GH_TOKEN, GITHUB_REPOSITORY and PUBLIC_KEY are required.' }

# Authenticate this process's git commands only; the token never reaches argv.
$auth = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes("x-access-token:$env:GH_TOKEN"))
Write-Host "::add-mask::$auth"
$env:GIT_CONFIG_COUNT = '3'
$env:GIT_CONFIG_KEY_0 = 'http.https://github.com/.extraheader'; $env:GIT_CONFIG_VALUE_0 = "AUTHORIZATION: basic $auth"
$env:GIT_CONFIG_KEY_1 = 'user.name'; $env:GIT_CONFIG_VALUE_1 = 'github-actions[bot]'
$env:GIT_CONFIG_KEY_2 = 'user.email'; $env:GIT_CONFIG_VALUE_2 = '41898282+github-actions[bot]@users.noreply.github.com'
function Invoke-Git {
    git -C $Site @args
    if ($LASTEXITCODE) { throw "git $($args -join ' ') failed with exit code $LASTEXITCODE." }
}

New-Item -ItemType Directory -Force -Path $Site | Out-Null
Invoke-Git init --quiet --initial-branch $branch
Invoke-Git remote add origin $Remote
git -C $Site ls-remote --exit-code --heads origin $branch | Out-Null
switch ($LASTEXITCODE) {
    0 { Invoke-Git fetch --quiet --depth 1 origin $branch; Invoke-Git checkout --quiet -B $branch FETCH_HEAD }
    2 { Write-Host "Creating the $branch branch." }
    default { throw "git ls-remote failed with exit code $LASTEXITCODE." }
}

$arguments = @('-B', (Join-Path $PSScriptRoot 'maicord_release.py'), 'channels', '--site', $Site,
    '--public-key', $env:PUBLIC_KEY, '--refresh-within-days', $RefreshWithinDays)
if ($Publish) { $arguments += @('--publish', $Publish) }
python @arguments
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Invoke-Git add --all updates
$status = git -C $Site status --porcelain -- updates
if ($LASTEXITCODE) { throw "git status failed with exit code $LASTEXITCODE." }
$changed = [bool]$status
if ($changed) {
    Invoke-Git commit --quiet -m $Message
    Invoke-Git push --quiet origin "HEAD:refs/heads/$branch"
}
$deploy = $changed -or $AlwaysDeploy
if ($env:GITHUB_OUTPUT) { [IO.File]::AppendAllText($env:GITHUB_OUTPUT, "deploy=$($deploy.ToString().ToLower())`n") }
Write-Host "Channel manifests changed: $changed; deploy: $deploy"
