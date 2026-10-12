# Shared by bounded correctness and performance probes. TimeoutSeconds is the
# caller's validated 1..15 second deadline; only this exact child is killed.
function Invoke-BoundedProcess {
    param([string]$Path, [string[]]$Arguments, [hashtable]$EnvironmentOverrides,
          [switch]$IncludeStandardError)
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $Path
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    foreach ($name in $EnvironmentOverrides.Keys) { $info.Environment[$name] = $EnvironmentOverrides[$name] }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw "Cannot start $Path" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Media probe exceeded $TimeoutSeconds seconds: $Path"
        }
        $output = $stdout.GetAwaiter().GetResult()
        $errors = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "Media probe failed ($($process.ExitCode)): $errors`n$output" }
        if ($IncludeStandardError) { return $output + $errors }
        return $output
    } finally { $process.Dispose() }
}
