param(
    [string[]]$InputFiles = @(),
    [Parameter(Mandatory)][string]$Ffmpeg,
    [string]$Executable = 'D:\MaiCord-build\Release\MaiCordMp4Benchmark.exe',
    [ValidateRange(1, 15)][int]$TimeoutSeconds = 15,
    [switch]$Synthetic,
    [switch]$ExtendedSynthetic,
    [switch]$AspectSynthetic,
    [switch]$Gpu,
    [switch]$MfGpu,
    [switch]$FfmpegGpu,
    [switch]$NativeIdrFirst,
    [switch]$NativeGpu
)

$ErrorActionPreference = 'Stop'
if ($MfGpu -and -not $Gpu) { throw 'MfGpu requires Gpu.' }
if ($FfmpegGpu -and (-not $Gpu -or $MfGpu)) { throw 'FfmpegGpu requires Gpu and excludes MfGpu.' }
if ($NativeIdrFirst -and (-not $Gpu -or $MfGpu -or $FfmpegGpu)) {
    throw 'NativeIdrFirst requires Gpu and excludes other GPU backends.'
}
if ($NativeGpu -and (-not $Gpu -or $MfGpu -or $FfmpegGpu -or $NativeIdrFirst)) {
    throw 'NativeGpu requires Gpu and excludes other GPU backends.'
}
if ($AspectSynthetic -and (-not $Gpu -or -not $NativeGpu)) {
    throw 'AspectSynthetic requires Gpu and NativeGpu to verify shipping display geometry.'
}
if ($InputFiles.Count -eq 0 -and -not $Synthetic -and -not $ExtendedSynthetic -and -not $AspectSynthetic) {
    throw 'Specify InputFiles, Synthetic, ExtendedSynthetic or AspectSynthetic.'
}

. (Join-Path $PSScriptRoot 'mp4_process.ps1')

$Executable = (Resolve-Path -LiteralPath $Executable).Path
$Ffmpeg = (Resolve-Path -LiteralPath $Ffmpeg).Path
$scratchParent = [IO.Path]::GetDirectoryName($Executable)
$scratchPath = Join-Path $scratchParent ('mp4-validation-' + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $scratchPath)
$referencePath = Join-Path $scratchPath 'reference.i420'
$generatedPaths = @($referencePath)
try {
    $fixtures = [Collections.Generic.List[object]]::new()
    $displayExpectations = @{}
    if ($AspectSynthetic) {
        foreach ($spec in @(
            @('wide', '66x50', '4/3', 88, 50, ''),
            @('narrow', '66x50', '1/2', 66, 100, ''),
            @('extended', '638x358', '297/296', 640, 358, ''),
            @('vui-only', '66x50', '4/3', 88, 50, 'remove'),
            @('container-override', '66x50', '1/2', 88, 50, 'override')
        )) {
            $fixtures.Add(@{ Name="aspect-$($spec[0])"; Size=$spec[1]; Sar=$spec[2];
                DisplayWidth=$spec[3]; DisplayHeight=$spec[4]; Pasp=$spec[5]; Frames=16;
                Profile='high'; Matrix='bt709'; Range='tv';
                Parameters='bframes=3:ref=4:b-adapt=0:keyint=12:scenecut=0' })
        }
    }
    if ($Synthetic) {
        foreach ($matrix in @('smpte170m', 'bt709')) {
            foreach ($range in @('tv', 'pc')) {
                $fixtures.Add(@{ Name="$matrix-$range"; Size='66x50'; Frames=12;
                    Profile=''; Matrix=$matrix; Range=$range;
                    Parameters='bframes=2:keyint=6:scenecut=0' })
            }
        }
    }
    if ($ExtendedSynthetic) {
        # Force B-picture placement instead of relying on x264's adaptive
        # decisions. Exercise large cropped surfaces, CABAC/CAVLC, deep
        # reference/reorder buffers and interlaced source pictures.
        foreach ($spec in @(
            @('baseline','318x178',0,1,''), @('main','640x358',3,3,''),
            @('high','1280x718',4,8,''), @('high','1920x1080',4,8,''),
            @('high','2048x1150',4,8,''), @('high','320x192',2,4,':tff=1:weightp=0')
        )) {
            $fixtures.Add(@{ Name="$($spec[0])-$($spec[1])"; Size=$spec[1]; Frames=16;
                Profile=$spec[0]; Matrix='bt709'; Range='tv';
                Parameters="bframes=$($spec[2]):ref=$($spec[3]):b-adapt=0:keyint=12:scenecut=0$($spec[4])" })
        }
        $fixtures.Add(@{ Name='high-multislice-jvt'; Size='642x358'; Frames=16;
            Profile='high'; Matrix='bt709'; Range='tv';
            Parameters='bframes=3:ref=4:b-adapt=0:keyint=12:scenecut=0:slices=4:cqm=jvt' })
    }
    foreach ($fixture in $fixtures) {
        $fixturePath = Join-Path $scratchPath ($fixture.Name + '.mp4')
        $generatedPaths += $fixturePath
        $filter = if ($fixture.Range -eq 'pc') {
            'scale=in_range=limited:out_range=full,format=yuvj420p'
        } else { 'format=yuv420p' }
        if ($fixture.Sar) { $filter += ",setsar=$($fixture.Sar):max=65535" }
        $codecOptions = if ($fixture.Profile) { @('-profile:v', $fixture.Profile) } else { @() }
        [void](Invoke-BoundedProcess -Path $Ffmpeg -Arguments (@(
            '-nostdin', '-hide_banner', '-loglevel', 'error',
            '-f', 'lavfi', '-i', "testsrc2=size=$($fixture.Size):rate=30",
            '-frames:v', [string]$fixture.Frames, '-vf', $filter, '-c:v', 'libx264',
            '-threads', '1', '-x264-params', $fixture.Parameters,
            '-colorspace', $fixture.Matrix, '-color_primaries', $fixture.Matrix,
            '-color_trc', $fixture.Matrix, '-color_range', $fixture.Range
        ) + $codecOptions + @('-y', $fixturePath)) -EnvironmentOverrides @{})
        if ($fixture.Pasp) {
            # Only mutate our generated fixture. Replacing pasp with a same-size
            # free box preserves every sample/table offset and leaves VUI intact.
            $bytes = [IO.File]::ReadAllBytes($fixturePath)
            $matches = [Collections.Generic.List[int]]::new()
            for ($i = 4; $i + 12 -le $bytes.Length; ++$i) {
                if ($bytes[$i] -eq 112 -and $bytes[$i+1] -eq 97 -and $bytes[$i+2] -eq 115 -and
                    $bytes[$i+3] -eq 112 -and $bytes[$i-4] -eq 0 -and $bytes[$i-3] -eq 0 -and
                    $bytes[$i-2] -eq 0 -and $bytes[$i-1] -eq 16) { $matches.Add($i) }
            }
            if ($matches.Count -ne 1) { throw 'Expected one generated 16-byte pasp box.' }
            $offset = $matches[0]
            if ($fixture.Pasp -eq 'remove') {
                [Text.Encoding]::ASCII.GetBytes('free').CopyTo($bytes, $offset)
            } else {
                ([byte[]]@(0, 0, 0, 4, 0, 0, 0, 3)).CopyTo($bytes, $offset + 4)
            }
            [IO.File]::WriteAllBytes($fixturePath, $bytes)
        }
        if ($fixture.Sar) {
            $displayExpectations[$fixturePath] = @($fixture.DisplayWidth, $fixture.DisplayHeight)
        }
        $InputFiles += $fixturePath
    }
    foreach ($inputFile in $InputFiles) {
        $inputPath = (Resolve-Path -LiteralPath $inputFile).Path
        [void](Invoke-BoundedProcess -Path $Ffmpeg -Arguments @(
            '-nostdin', '-hide_banner', '-loglevel', 'error', '-threads', '1',
            '-i', $inputPath, '-map', '0:v:0', '-fps_mode', 'passthrough',
            '-c:v', 'rawvideo', '-f', 'rawvideo', '-y', $referencePath
        ) -EnvironmentOverrides @{})
        $output = Invoke-BoundedProcess -Path $Executable -Arguments @() -EnvironmentOverrides @{
            'MAICORD_MP4_BENCHMARK_INPUT' = $inputPath
            'MAICORD_MP4_REFERENCE_I420' = $referencePath
            'MAICORD_MP4_REFERENCE_RGBA' = ''
            'MAICORD_MP4_DIRECT_COLD' = '0'
            'MAICORD_MP4_SKIP_DEMUX_REPEAT' = '1'
            'MAICORD_MP4_GPU' = '0'
        }
        $metrics = @{}
        foreach ($line in $output -split '[\r\n]+') {
            if ($line -match '^([^=]+)=(\d+)$') { $metrics[$Matches[1]] = $Matches[2] }
        }
        if ($metrics.native_reference_present -ne '1' -or
            $metrics.native_reference_complete -ne '1' -or
            $metrics.native_reference_mismatched_bytes -ne '0' -or
            $metrics.decode_structure_complete -ne '1') {
            throw "Incomplete native-plane validation: $output"
        }
        if ($Gpu) {
            $gpuOutput = Invoke-BoundedProcess -Path $Executable -Arguments @() -EnvironmentOverrides @{
                'MAICORD_MP4_BENCHMARK_INPUT' = $inputPath
                'MAICORD_MP4_SKIP_DEMUX_REPEAT' = '1'
                'MAICORD_MP4_GPU' = '1'
                'MAICORD_MP4_GPU_FIRST_ONLY' = [string][int][bool]$NativeIdrFirst
                'MAICORD_MP4_GPU_NATIVE_IDR' = [string][int][bool]$NativeIdrFirst
                'MAICORD_MP4_GPU_NATIVE_STREAM' = [string][int][bool]$NativeGpu
                'MAICORD_MP4_GPU_VERIFY' = '1'
                'MAICORD_MP4_GPU_DEBUG' = '1'
                'MAICORD_MP4_GPU_CPU_BASELINE' = '0'
                'MAICORD_MP4_GPU_STRESS' = if ($MfGpu -or $FfmpegGpu -or $NativeIdrFirst) { '0' } else { '1' }
                'MAICORD_MP4_GPU_MF_BASELINE' = [string][int][bool]$MfGpu
                'MAICORD_MP4_GPU_FFMPEG_BASELINE' = [string][int][bool]$FfmpegGpu
                'MAICORD_MP4_GPU_WARM' = '0'
                'MAICORD_MP4_GPU_TRACE' = '0'
                'MAICORD_MP4_INSPECT' = '0'
                'MAICORD_MP4_DECODE_TRACE' = '0'
                'MAICORD_MP4_LOW_LATENCY' = '0'
                'MAICORD_MP4_SHADER_READABLE' = '0'
                'MAICORD_MP4_CONVERTER_ONLY' = '0'
                'MAICORD_MP4_WORKER_THREADS' = '0'
            }
            $gpuMetrics = @{}
            foreach ($line in $gpuOutput -split '[\r\n]+') {
                if ($line -match '^([^=]+)=(\d+)$') { $gpuMetrics[$Matches[1]] = $Matches[2] }
            }
            if ($displayExpectations.ContainsKey($inputPath)) {
                $expected = $displayExpectations[$inputPath]
                if ([long]$gpuMetrics.gpu_display_width -ne $expected[0] -or
                    [long]$gpuMetrics.gpu_display_height -ne $expected[1]) {
                    throw "Incorrect native display geometry: $gpuOutput"
                }
            }
            $expectedGpuFrames = if ($NativeIdrFirst) { '1' } else { $metrics.native_reference_frames }
            if ($gpuMetrics.gpu_frames -ne $expectedGpuFrames -or
                (($NativeIdrFirst -or $NativeGpu) -and [long]$gpuMetrics.h264_state_pictures -ne ([long]$metrics.native_reference_frames / 2)) -or
                ($NativeGpu -and ($gpuMetrics.gpu_native_stream -ne '1' -or
                    $gpuMetrics.gpu_native_playback_wrapper -ne '1' -or $gpuMetrics.gpu_concurrent_native -ne '1')) -or
                (-not $NativeGpu -and $gpuMetrics.gpu_native_stream -eq '1') -or
                ($NativeIdrFirst -and ($gpuMetrics.gpu_native_idr -ne '1' -or $gpuMetrics.gpu_first_only -ne '1')) -or
                (-not $NativeIdrFirst -and ($gpuMetrics.gpu_native_idr -eq '1' -or $gpuMetrics.gpu_first_only -eq '1')) -or
                $gpuMetrics.gpu_mf_baseline -ne [string][int][bool]$MfGpu -or
                $gpuMetrics.gpu_ffmpeg_baseline -ne [string][int][bool]$FfmpegGpu -or
                $gpuMetrics.gpu_rgba_mismatch_bytes -ne '0' -or
                $gpuMetrics.gpu_border_mismatch_bytes -ne '0' -or
                $gpuMetrics.gpu_debug_errors -ne '0' -or
                $gpuMetrics.gpu_decoder_rgba_allocation_bytes -ne '0' -or
                (-not $MfGpu -and -not $FfmpegGpu -and -not $NativeIdrFirst -and (
                    [long]$gpuMetrics.gpu_concurrent_frames -ne ([Math]::Min(192, [long]$metrics.native_reference_frames / 2) * 4) -or
                    $gpuMetrics.gpu_concurrent_mismatch_bytes -ne '0' -or
                    $gpuMetrics.gpu_concurrent_border_mismatch_bytes -ne '0'))) {
                throw "Incomplete GPU validation: $gpuOutput"
            }
        }
        [pscustomobject]@{
            Input = [IO.Path]::GetFileName($inputPath)
            InputSha256 = (Get-FileHash -LiteralPath $inputPath -Algorithm SHA256).Hash
            FramesIncludingTwoLoops = [int]$metrics.native_reference_frames
            MismatchedYuvBytes = [long]$metrics.native_reference_mismatched_bytes
            Storage = "$($metrics.direct_storage_width)x$($metrics.direct_storage_height)"
            Aperture = "$($metrics.direct_aperture_width)x$($metrics.direct_aperture_height)"
            ColorMatrix = [int]$metrics.direct_color_matrix
            NominalRange = [int]$metrics.direct_nominal_range
            GpuVerified = [bool]$Gpu
            MfGpuVerified = [bool]$MfGpu
            FfmpegGpuVerified = [bool]$FfmpegGpu
            NativeIdrFirstVerified = [bool]$NativeIdrFirst
            NativeGpuVerified = [bool]$NativeGpu
            Display = if ($NativeGpu) { "$($gpuMetrics.gpu_display_width)x$($gpuMetrics.gpu_display_height)" } else { '' }
            H264StatePictures = if ($NativeIdrFirst -or $NativeGpu) { [long]$gpuMetrics.h264_state_pictures } else { 0 }
            H264StateSlices = if ($NativeIdrFirst -or $NativeGpu) { [long]$gpuMetrics.h264_state_slices } else { 0 }
            H264StateBPictures = if ($NativeIdrFirst -or $NativeGpu) { [long]$gpuMetrics.h264_state_b_pictures } else { 0 }
            H264StateMarkingMask = if ($NativeIdrFirst -or $NativeGpu) { [long]$gpuMetrics.h264_state_marking_mask } else { 0 }
            GpuFramesVerified = if ($Gpu) { [long]$gpuMetrics.gpu_frames } else { 0 }
            GpuConcurrentFrames = if ($Gpu -and -not $MfGpu -and -not $FfmpegGpu -and -not $NativeIdrFirst) { [long]$gpuMetrics.gpu_concurrent_frames } else { 0 }
        }
    }
} finally {
    # Exact, locally generated targets only; never recurse over the build root.
    $resolvedScratch = [IO.Path]::GetFullPath($scratchPath)
    if ([IO.Path]::GetDirectoryName($resolvedScratch) -ne $scratchParent -or
        [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($referencePath)) -ne $resolvedScratch) {
        throw 'Unexpected validation scratch path.'
    }
    foreach ($generatedPath in $generatedPaths) {
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($generatedPath)) -ne $resolvedScratch) {
            throw 'Unexpected generated validation file path.'
        }
        if ([IO.File]::Exists($generatedPath)) { [IO.File]::Delete($generatedPath) }
    }
    [IO.Directory]::Delete($resolvedScratch)
}
