# Updates the update-channels branch, which holds the GitHub Pages site that
# serves the signed channel manifests, changelogs and preview packages (see
# SLOTS in maicord_release.py). Checks the branch out into -Site, runs
# `maicord_release.py channels`, and when anything changed replaces the branch
# with a single parentless commit of the new site, so retired preview packages
# leave no history. Writers in other concurrency groups can race: the push is
# leased on the commit the site was built from and the whole update is retried
# from the new tip. Writes deploy=true|false to GITHUB_OUTPUT.
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
    [string]$Remote = "https://github.com/$env:GITHUB_REPOSITORY.git",
    [int]$Attempts = 3
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

for ($attempt = 1; ; $attempt++) {
    if (Test-Path -LiteralPath $Site) { Remove-Item -LiteralPath $Site -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Site | Out-Null
    Invoke-Git init --quiet --initial-branch $branch
    Invoke-Git remote add origin $Remote
    $tip = git -C $Site ls-remote --heads origin "refs/heads/$branch"
    if ($LASTEXITCODE) { throw "git ls-remote failed with exit code $LASTEXITCODE." }
    $base = ''
    if ($tip) {
        Invoke-Git fetch --quiet --depth 1 origin $branch
        Invoke-Git checkout --quiet -B $branch FETCH_HEAD
        $base = git -C $Site rev-parse HEAD
    } else { Write-Host "Creating the $branch branch." }

    $arguments = @('-B', (Join-Path $PSScriptRoot 'maicord_release.py'), 'channels', '--site', $Site,
        '--public-key', $env:PUBLIC_KEY, '--refresh-within-days', $RefreshWithinDays)
    if ($Publish) { $arguments += @('--publish', $Publish) }
    python @arguments
    if ($LASTEXITCODE) { exit $LASTEXITCODE }

    Invoke-Git add --all updates
    $status = git -C $Site status --porcelain -- updates
    if ($LASTEXITCODE) { throw "git status failed with exit code $LASTEXITCODE." }
    $changed = [bool]$status
    if (!$changed) { break }
    $tree = git -C $Site write-tree
    if ($LASTEXITCODE) { throw "git write-tree failed with exit code $LASTEXITCODE." }
    $commit = git -C $Site commit-tree $tree -m $Message
    if ($LASTEXITCODE) { throw "git commit-tree failed with exit code $LASTEXITCODE." }
    git -C $Site push --quiet "--force-with-lease=refs/heads/${branch}:$base" origin "${commit}:refs/heads/$branch"
    if (!$LASTEXITCODE) { break }
    if ($attempt -ge $Attempts) { throw "Pushing $branch failed $Attempts times." }
    Write-Host "The $branch branch moved; updating again from its new tip."
}
$deploy = $changed -or $AlwaysDeploy
if ($env:GITHUB_OUTPUT) { [IO.File]::AppendAllText($env:GITHUB_OUTPUT, "deploy=$($deploy.ToString().ToLower())`n") }
Write-Host "Update channels changed: $changed; deploy: $deploy"
