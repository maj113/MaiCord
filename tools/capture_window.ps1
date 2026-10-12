param(
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$Output,
    [ValidateRange(0, 5000)]
    [int]$SettleMilliseconds = 250,
    [switch]$PrintWindow
)

$ErrorActionPreference = 'Stop'
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$outputPath = [System.IO.Path]::GetFullPath($Output)
$outputDirectory = Split-Path -Parent $outputPath
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    $null = New-Item -ItemType Directory -Path $outputDirectory
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class MaiCordWindowCapture {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect bounds);

    [DllImport("user32.dll")]
    public static extern uint GetDpiForWindow(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter,
                                           int x, int y, int width, int height,
                                           uint flags);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr deviceContext, uint flags);
}
'@

$process = Start-Process -FilePath $resolvedExecutable -PassThru -WindowStyle Normal
try {
    if (-not $process.WaitForInputIdle(5000)) {
        throw 'MaiCord did not reach input-idle state before capture.'
    }
    $window = [IntPtr]::Zero
    $windowDeadline = [DateTime]::UtcNow.AddSeconds(5)
    while ($window -eq [IntPtr]::Zero -and
           -not $process.HasExited -and
           [DateTime]::UtcNow -lt $windowDeadline) {
        $process.Refresh()
        $window = $process.MainWindowHandle
        if ($window -eq [IntPtr]::Zero) {
            Start-Sleep -Milliseconds 50
        }
    }
    if ($window -eq [IntPtr]::Zero) {
        throw 'MaiCord did not expose a top-level window.'
    }
    $topmost = [IntPtr](-1)
    $positionFlags = [uint32](0x0001 -bor 0x0002 -bor 0x0040)
    if (-not [MaiCordWindowCapture]::SetWindowPos($window, $topmost, 0, 0, 0, 0,
                                                  $positionFlags)) {
        throw 'SetWindowPos failed to expose the capture window.'
    }
    Start-Sleep -Milliseconds $SettleMilliseconds
    $bounds = New-Object MaiCordWindowCapture+Rect
    if (-not [MaiCordWindowCapture]::GetWindowRect($window, [ref]$bounds)) {
        throw 'GetWindowRect failed.'
    }
    # This PowerShell host is DPI-unaware, so GetWindowRect is virtualized while
    # CopyFromScreen consumes physical pixels. Convert once at the boundary.
    $dpi = [MaiCordWindowCapture]::GetDpiForWindow($window)
    if ($dpi -eq 0) {
        $dpi = 96
    }
    $scale = $dpi / 96.0
    $left = [int][Math]::Round($bounds.Left * $scale)
    $top = [int][Math]::Round($bounds.Top * $scale)
    $right = [int][Math]::Round($bounds.Right * $scale)
    $bottom = [int][Math]::Round($bounds.Bottom * $scale)
    $width = $right - $left
    $height = $bottom - $top
    if ($width -le 0 -or $height -le 0) {
        throw 'MaiCord returned invalid capture bounds.'
    }
    [pscustomobject]@{
        ProcessId = $process.Id
        ProcessPath = $process.Path
        WindowTitle = $process.MainWindowTitle
        WindowDpi = $dpi
        WindowLeft = $left
        WindowTop = $top
        WindowWidth = $width
        WindowHeight = $height
    }

    Add-Type -AssemblyName System.Drawing
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        if ($PrintWindow) {
            $deviceContext = $graphics.GetHdc()
            try {
                if (-not [MaiCordWindowCapture]::PrintWindow($window, $deviceContext, 2)) {
                    throw 'PrintWindow failed.'
                }
            } finally {
                $graphics.ReleaseHdc($deviceContext)
            }
        } else {
            $graphics.CopyFromScreen($left, $top, 0, 0,
                                     (New-Object System.Drawing.Size $width, $height))
        }
        $bitmap.Save($outputPath, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
} finally {
    if (-not $process.HasExited) {
        $null = $process.CloseMainWindow()
        if (-not $process.WaitForExit(2000)) {
            $process.Kill()
            $process.WaitForExit()
        }
    }
    $process.Dispose()
}

Get-Item -LiteralPath $outputPath | Select-Object FullName, Length, LastWriteTime
