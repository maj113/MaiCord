param([string]$Executable = 'D:/MaiCord-build/Release/MaiCord.exe')
$ErrorActionPreference = 'Stop'
if (Get-Process MaiCord -ErrorAction SilentlyContinue) {
    throw 'Close MaiCord normally before starting the diagnostic build.'
}
$info = [Diagnostics.ProcessStartInfo]::new((Resolve-Path -LiteralPath $Executable).Path)
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.WorkingDirectory = Split-Path -Parent $info.FileName
$info.Environment['MAICORD_GPU_METRICS'] = '1'
$child = [Diagnostics.Process]::Start($info)
Write-Output "MaiCord diagnostic PID: $($child.Id). Normal window; close normally when finished."
Write-Output 'Renderer interval metrics are written to the normal MaiCord log.'
$child.Dispose()
