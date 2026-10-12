param(
    [Parameter(Mandatory)][string[]]$InputFiles,
    [Parameter(Mandatory)][string]$BeforeExecutable,
    [Parameter(Mandatory)][string]$AfterExecutable,
    [ValidateRange(1, 5)][int]$Repeats = 5,
    [ValidateRange(1, 15)][int]$TimeoutSeconds = 15,
    [ValidateSet(1, 2, 4, 8)][int]$QueueDepth = 1,
    [switch]$MfBaseline,
    [switch]$FfmpegBaseline,
    [switch]$NativeIdrFirst
)
$ErrorActionPreference = 'Stop'
if ($MfBaseline -and $FfmpegBaseline) { throw 'Select one comparison backend.' }
if ($NativeIdrFirst -and ($MfBaseline -or $FfmpegBaseline -or $QueueDepth -ne 1)) {
    throw 'NativeIdrFirst is a first-picture comparison, not a full-playback/deeper-queue backend.'
}
$executables = @((Resolve-Path -LiteralPath $BeforeExecutable).Path,
                 (Resolve-Path -LiteralPath $AfterExecutable).Path)
foreach ($inputFile in $InputFiles) {
    foreach ($warm in @($false, $true)) {
        for ($repeat = 0; $repeat -lt $Repeats; ++$repeat) {
            $order = if (($repeat % 2) -eq 0) { @(0, 1) } else { @(1, 0) }
            foreach ($variant in $order) {
                $samples = if ($NativeIdrFirst) {
                    & (Join-Path $PSScriptRoot 'probe_h264_first.ps1') -InputFiles $inputFile `
                        -Executable $executables[$variant] -Backends NativeIdr -Repeats 1 `
                        -TimeoutSeconds $TimeoutSeconds -WarmRenderer:$warm
                } else {
                    & (Join-Path $PSScriptRoot 'measure_mp4_gpu.ps1') -InputFiles $inputFile `
                    -Executable $executables[$variant] -Repeats 1 -TimeoutSeconds $TimeoutSeconds `
                    -QueueDepth $QueueDepth -WarmRenderer:$warm -MfBaseline:$MfBaseline `
                    -FfmpegBaseline:$FfmpegBaseline
                }
                $samples | ForEach-Object {
                        $_.Repeat = $repeat
                        $_ | Add-Member -NotePropertyName Variant -NotePropertyValue @('Before', 'After')[$variant] -PassThru
                    }
            }
        }
    }
}
