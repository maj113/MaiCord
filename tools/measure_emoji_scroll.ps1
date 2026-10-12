param(
    [string]$Candidate = 'D:/MaiCord-build/Release/MaiCordUiProbe.exe',
    [string]$Baseline,
    [int]$Runs = 5,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

# Measures the real catalog sweep in the native UI probe. Each process starts
# with an empty glyph atlas; OS font/file and driver shader caches are untouched.
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$saved = @{}
foreach ($name in @('MAICORD_UI_PROBE_ONCE', 'MAICORD_GPU_METRICS', 'MAICORD_EMOJI_SCROLL_METRICS')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name)
}
$pages = @()
$processes = @()
try {
    $env:MAICORD_UI_PROBE_ONCE = '1'
    $env:MAICORD_GPU_METRICS = '1'
    for ($run = 1; $run -le $Runs; ++$run) {
        $kinds = if ($Baseline) { @('baseline', 'candidate') } else { @('candidate') }
        if ($run % 2 -eq 0) { [array]::Reverse($kinds) }
        foreach ($kind in $kinds) {
            $exe = (Resolve-Path -LiteralPath $(if ($kind -eq 'baseline') { $Baseline } else { $Candidate })).Path
            $path = Join-Path $OutputDirectory "$kind-$run.bin"
            $env:MAICORD_EMOJI_SCROLL_METRICS = $path
            $process = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -WindowStyle Hidden -PassThru
            [long]$privatePeak = 0
            [long]$workingPeak = 0
            while (-not $process.WaitForExit(10)) {
                $process.Refresh()
                if (-not $process.HasExited) {
                    $privatePeak = [Math]::Max($privatePeak, $process.PrivateMemorySize64)
                    $workingPeak = [Math]::Max($workingPeak, $process.WorkingSet64)
                }
            }
            if ($process.ExitCode -ne 0) { throw "$kind run $run failed: $($process.ExitCode)" }
            $process.Dispose()
            $processes += [pscustomobject]@{ Kind=$kind; Run=$run; PrivatePeak=$privatePeak; WorkingPeak=$workingPeak }
            $bytes = [IO.File]::ReadAllBytes($path)
            $count = [BitConverter]::ToUInt32($bytes, 0)
            if ($bytes.Length -ne 4 + 24 * $count) { throw "Invalid metrics: $path" }
            for ($i = 0; $i -lt $count; ++$i) {
                $at = 4 + 24 * $i
                $pages += [pscustomobject]@{
                    Kind=$kind; Run=$run
                    LayoutTextUs=[BitConverter]::ToUInt64($bytes, $at)
                    FrameUs=[BitConverter]::ToUInt64($bytes, $at + 8)
                    Row=[BitConverter]::ToUInt32($bytes, $at + 16)
                    NewGlyphs=[BitConverter]::ToUInt32($bytes, $at + 20)
                }
            }
            Write-Output "$kind run $run passed ($count catalog pages)"
        }
    }
    $pages | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $OutputDirectory 'pages.csv')
    $processes | Export-Csv -NoTypeInformation -LiteralPath (Join-Path $OutputDirectory 'processes.csv')
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}
