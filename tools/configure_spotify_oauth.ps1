#requires -Version 5.1
# Spotify desktop PKCE uses only a public client ID, never a client secret.
[CmdletBinding()]
param([Parameter(Mandatory = $true)][ValidatePattern('\A[0-9a-fA-F]{32}\z')][string]$ClientId)
$ErrorActionPreference = 'Stop'
$directory = Join-Path $env:LOCALAPPDATA 'MaiCord'
[IO.Directory]::CreateDirectory($directory) | Out-Null
$destination = Join-Path $directory 'spotify-client.txt'
$temporary = Join-Path $directory ('spotify-client-' + [guid]::NewGuid().ToString('N') + '.tmp')
try {
    [IO.File]::WriteAllText($temporary, $ClientId, [Text.Encoding]::ASCII)
    if ([IO.File]::Exists($destination)) { [IO.File]::Replace($temporary, $destination, $null) }
    else { [IO.File]::Move($temporary, $destination) }
    Write-Output 'Spotify registration configured. Register http://127.0.0.1/oauth2callback as the redirect URI, then sign in under Settings > Voice & Video > Media accounts.'
} finally {
    if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
}
