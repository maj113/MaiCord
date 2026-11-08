#requires -Version 5.1
# Starts tools/fake_discord_server.py and a MaiCord client built with tools/build.ps1 -TestHooks, pointed at it with a private
# profile (MAICORD_TEST_SERVER, MAICORD_TEST_CA_FILE; include/maicord/platform/net/test_server.hpp), and returns success only
# when the fake server has logged the client's first request. Nothing is installed and no certificate store is touched.
#
#   tools\run_fake_server_client.ps1 [-Executable PATH] [-World DIR] [-Directory DIR] [-Port N] [-TimeoutSeconds N]
#   tools\run_fake_server_client.ps1 -Stop [-Directory DIR]
#
# The executable (default: the build directory's MaiCord.exe) must hold test hooks; the script refuses a shipping client, which
# would sign in to the real Discord. It is copied with its fonts and licenses to <Directory>\client and run from there, so the
# build directory stays free for builds. -World names a MAICORD_FAKE_WORLD directory (world.json and cdn\). The certificate
# (a throwaway, valid from 2000 to 2099) is made in <Directory> with Git's openssl on every run. Output, in <Directory> (default
# %TEMP%\mc-fake-client): server.log, requests.log, profile\ (the client's LOCALAPPDATA), state.json (process ids), cert.pem.
# Sign in with any e-mail and password. -Stop ends both processes.
[CmdletBinding()]
param(
    [string]$Executable = '',
    [string]$World = '',
    [string]$Directory = (Join-Path $env:TEMP 'mc-fake-client'),
    [ValidateRange(1, 65535)][int]$Port = 18443,
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 30,
    [switch]$Stop
)

$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
$state = Join-Path $Directory 'state.json'

function Stop-Recorded {
    if (!(Test-Path -LiteralPath $state)) { return }
    $recorded = Get-Content -LiteralPath $state -Raw | ConvertFrom-Json
    foreach ($id in @($recorded.client, $recorded.server)) {
        if ($id) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item -LiteralPath $state -Force
}

if ($Stop) { Stop-Recorded; return }

if (!$Executable) { $Executable = Join-Path $repository '..\MaiCord-build\Release\MaiCord.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
# The marker string every image with test hooks carries (include/maicord/core/test_hook.hpp; tools/audit_image.py).
$image = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($Executable))
if (!$image.Contains('maicord-test-hooks-compiled-in')) {
    throw "$Executable holds no test hooks (a shipping build): it would reach the real Discord. Build with tools\build.ps1 -TestHooks."
}
$image = $null

Stop-Recorded
New-Item -ItemType Directory -Force -Path $Directory | Out-Null
$Directory = (Resolve-Path -LiteralPath $Directory).Path
$client = Join-Path $Directory 'client'
$userData = Join-Path $Directory 'profile'
$serverLog = Join-Path $Directory 'server.log'
$requestLog = Join-Path $Directory 'requests.log'
$certificate = Join-Path $Directory 'cert.pem'
$key = Join-Path $Directory 'key.pem'

$listening = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
if ($listening) { throw "Port $Port is in use (process $($listening[0].OwningProcess)): choose another with -Port." }

# The client and the runtime assets it reads beside it.
if (Test-Path -LiteralPath $client) { Remove-Item -LiteralPath $client -Recurse -Force }
New-Item -ItemType Directory -Path $client | Out-Null
Copy-Item -LiteralPath $Executable -Destination (Join-Path $client 'MaiCord.exe')
foreach ($assets in 'fonts', 'licenses') {
    $source = Join-Path (Split-Path -Parent $Executable) $assets
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $client $assets) -Recurse }
}
if (Test-Path -LiteralPath $userData) { Remove-Item -LiteralPath $userData -Recurse -Force }
New-Item -ItemType Directory -Path "$userData\Temp" | Out-Null

# The certificate, made on every run, valid from 2000 to 2099 rather than from now, as tests/platform/net/tls_certificate.sh
# makes it (the reasons are there): `openssl req` can only date a certificate from now, so `openssl ca -selfsign` signs the
# request with these dates, from a throwaway CA database, with the extensions `openssl req -x509` gives.
$openssl = (Get-Command openssl.exe -ErrorAction SilentlyContinue).Source
if (!$openssl) { $openssl = Join-Path $env:ProgramFiles 'Git\usr\bin\openssl.exe' }
if (!(Test-Path -LiteralPath $openssl)) { throw 'openssl.exe was not found (it ships with Git for Windows).' }
$names = 'discord.com', 'gateway.discord.gg', 'cdn.discordapp.com', 'media.discordapp.net', 'remote-auth-gateway.discord.gg',
    'us-qa.discord.media', 'maj113.github.io'
$alternatives = ($names | ForEach-Object { "DNS:$_" }) -join ','
$authority = Join-Path ([IO.Path]::GetTempPath()) ('mc-certificate-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $authority | Out-Null
$database = $authority.Replace('\', '/') # forward slashes: a backslash escapes in an OpenSSL configuration
$configuration = @('[ca]', 'default_ca = selfsigned', '[selfsigned]', "database = $database/index.txt",
    "new_certs_dir = $database", "serial = $database/serial", 'default_md = sha256', 'policy = any_name',
    'copy_extensions = copy', 'x509_extensions = authority', 'unique_subject = no', '[any_name]', 'commonName = supplied',
    '[authority]', 'subjectKeyIdentifier = hash', 'authorityKeyIdentifier = keyid:always,issuer',
    'basicConstraints = critical,CA:true')
[IO.File]::WriteAllText("$authority\ca.cnf", ($configuration -join "`n") + "`n")
[IO.File]::WriteAllText("$authority\index.txt", '')
[IO.File]::WriteAllText("$authority\serial", "01`n")
$env:MSYS_NO_PATHCONV = '1'
$ErrorActionPreference = 'Continue' # openssl writes its progress to stderr, which Windows PowerShell 5.1 treats as an error
& $openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -subj /CN=discord.com `
    -addext "subjectAltName=$alternatives" -keyout $key -out "$database/request.csr" 2>&1 | Out-Null
$made = $LASTEXITCODE -eq 0
if ($made) {
    & $openssl ca -batch -config "$database/ca.cnf" -selfsign -keyfile $key -in "$database/request.csr" -out $certificate `
        -notext -startdate 20000101000000Z -enddate 20991231235959Z 2>&1 | Out-Null
    $made = $LASTEXITCODE -eq 0
}
$ErrorActionPreference = 'Stop'
Remove-Item -LiteralPath $authority -Recurse -Force
if (!$made) { throw 'openssl could not make the certificate.' }

foreach ($log in $serverLog, $requestLog) { Set-Content -LiteralPath $log -Value '' -NoNewline }
$python = (Get-Command python.exe -ErrorAction Stop).Source
$serverOut = Join-Path $Directory 'server.out.log'
if ($World) { $env:MAICORD_FAKE_WORLD = (Resolve-Path -LiteralPath $World).Path } else { Remove-Item Env:\MAICORD_FAKE_WORLD -ErrorAction SilentlyContinue }
$server = Start-Process -FilePath $python -PassThru -WindowStyle Hidden -RedirectStandardOutput $serverOut -RedirectStandardError $serverLog `
    -WorkingDirectory $repository -ArgumentList @('-B', (Join-Path $PSScriptRoot 'fake_discord_server.py'),
        '--cert', "`"$certificate`"", '--key', "`"$key`"", '--port', $Port, '--log', "`"$requestLog`"")
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$ready = $false
while ($stopwatch.Elapsed.TotalSeconds -lt $TimeoutSeconds -and !$server.HasExited) {
    if ((Test-Path -LiteralPath $serverOut) -and (Get-Content -LiteralPath $serverOut -Raw) -match 'ready') { $ready = $true; break }
    Start-Sleep -Milliseconds 200
}
if (!$ready) {
    $detail = Get-Content -LiteralPath $serverLog -Raw
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
    throw "The fake server did not start: $detail"
}

$env:LOCALAPPDATA = $userData
$env:TEMP = "$userData\Temp"
$env:TMP = "$userData\Temp"
$env:MAICORD_TEST_SERVER = "127.0.0.1:$Port"
$env:MAICORD_TEST_CA_FILE = $certificate
$app = Start-Process -FilePath (Join-Path $client 'MaiCord.exe') -WorkingDirectory $client -PassThru
@{ client = $app.Id; server = $server.Id; port = $Port; profile = $userData; requests = $requestLog } | ConvertTo-Json | Set-Content -LiteralPath $state

# Success is the first request the server logged: the client reached it, over TLS it trusts through the CA file.
$requested = $false
$stopwatch.Restart()
while ($stopwatch.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
    if ($app.HasExited) { break }
    if ((Test-Path -LiteralPath $requestLog) -and ((Get-Item -LiteralPath $requestLog).Length -gt 0)) { $requested = $true; break }
    Start-Sleep -Milliseconds 250
}
if (!$requested) {
    $why = if ($app.HasExited) { "the client exited ($($app.ExitCode))" } else { "no request reached the server in $TimeoutSeconds seconds" }
    $log = Join-Path $userData 'MaiCord\logs\maicord.log'
    $lines = if (Test-Path -LiteralPath $log) { (Get-Content -LiteralPath $log | Where-Object { $_ -match 'test_hooks|\[E\]' } | Select-Object -First 5) -join "`n" } else { '' }
    Stop-Recorded
    throw "The client did not reach the fake server: $why.`n$lines"
}
Write-Host "Client $($app.Id) reached the fake server on port $Port (server $($server.Id)); profile $userData; requests $requestLog."
Write-Host 'Sign in with any e-mail and password. Stop both with: tools\run_fake_server_client.ps1 -Stop'
