param(
    [Parameter(Mandatory = $true)][string]$BaseDir,
    [Parameter(Mandatory = $true)][string]$ServerFilesDir
)

$ErrorActionPreference = "Stop"

$BaseDir = [System.IO.Path]::GetFullPath($BaseDir)
$ServerFilesDir = [System.IO.Path]::GetFullPath($ServerFilesDir)
$cacheRoot = Join-Path $BaseDir "third_party\package_cache\sherpa_onnx"
$modelsRoot = Join-Path $ServerFilesDir "models"
$runtimeRoot = Join-Path $modelsRoot "runtimes"
$refresh = $env:QUICKSTT_REFRESH_OPTIONAL_MODELS -eq "1"

function Ensure-ParentDir {
    param([Parameter(Mandatory = $true)][string]$Path)
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
}

function Remove-PathIfPresent {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (Test-Path $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
        Write-Host "[CLEANUP] Removed $Path"
    }
}

function Remove-FileIfPresent {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (Test-Path $Path) {
        Remove-Item -LiteralPath $Path -Force
        Write-Host "[CLEANUP] Removed $Path"
    }
}

function Ensure-CachedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$CachePath
    )

    Ensure-ParentDir -Path $CachePath
    if ($refresh -and (Test-Path $CachePath)) {
        Remove-Item -LiteralPath $CachePath -Force
    }
    if (Test-Path $CachePath) {
        $lower = $CachePath.ToLowerInvariant()
        if ($lower.EndsWith(".tar.bz2") -or $lower.EndsWith(".tar.gz") -or $lower.EndsWith(".tgz") -or $lower.EndsWith(".tar")) {
            cmd /c "tar -tf `"$CachePath`" >nul 2>nul"
            if ($LASTEXITCODE -ne 0) {
                Write-Host "[CACHE] Corrupt archive detected, re-downloading $CachePath"
                Remove-Item -LiteralPath $CachePath -Force
            }
        }
    }

    if (Test-Path $CachePath) {
        Write-Host "[CACHE] Reusing $CachePath"
        return $CachePath
    }

    Write-Host "[DOWNLOAD] $Url"
    Invoke-WebRequest -Uri $Url -OutFile $CachePath
    return $CachePath
}

function Publish-Package {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$CacheName,
        [Parameter(Mandatory = $true)][string]$RelativeServerPath
    )

    $cachePath = Join-Path $cacheRoot $CacheName
    $targetPath = Join-Path $ServerFilesDir $RelativeServerPath
    Ensure-ParentDir -Path $targetPath

    $sourcePath = Ensure-CachedFile -Url $Url -CachePath $cachePath
    Copy-Item -LiteralPath $sourcePath -Destination $targetPath -Force
    Write-Host "[PUBLISH] $RelativeServerPath"
}

New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
New-Item -ItemType Directory -Force -Path $modelsRoot | Out-Null
New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null

$legacyModelDirs = @("whisper_cpp", "faster_whisper")
foreach ($dirName in $legacyModelDirs) {
    Remove-PathIfPresent -Path (Join-Path $modelsRoot $dirName)
}

foreach ($fileName in @(
    "whisper_cpp_cpu_runtime.zip",
    "whisper_cpp_nvidia_runtime.zip",
    "whisper_cpp_vulkan_runtime.zip",
    "whisper_cpp_intel_openvino_runtime.zip",
    "faster_whisper_runtime.zip"
)) {
    Remove-FileIfPresent -Path (Join-Path $runtimeRoot $fileName)
}

$packages = @(
    @{
        Url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.12.34/sherpa-onnx-v1.12.34-win-x64-shared-MT-Release-no-tts.tar.bz2"
        CacheName = "sherpa-onnx-v1.12.34-win-x64-shared-MT-Release-no-tts.tar.bz2"
        RelativeServerPath = "models/runtimes/sherpa_onnx_cpu_runtime.tar.bz2"
    },
    @{
        Url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.12.34/sherpa-onnx-v1.12.34-win-x64-cuda.tar.bz2"
        CacheName = "sherpa-onnx-v1.12.34-win-x64-cuda.tar.bz2"
        RelativeServerPath = "models/runtimes/sherpa_onnx_cuda_runtime.tar.bz2"
    },
    @{
        Url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-moonshine-tiny-en-quantized-2026-02-27.tar.bz2"
        CacheName = "sherpa-onnx-moonshine-tiny-en-quantized-2026-02-27.tar.bz2"
        RelativeServerPath = "models/sherpa_onnx/moonshine_v2_tiny_en.tar.bz2"
    },
    @{
        Url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-moonshine-base-en-quantized-2026-02-27.tar.bz2"
        CacheName = "sherpa-onnx-moonshine-base-en-quantized-2026-02-27.tar.bz2"
        RelativeServerPath = "models/sherpa_onnx/moonshine_v2_base_en.tar.bz2"
    },
    @{
        Url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2"
        CacheName = "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2"
        RelativeServerPath = "models/sherpa_onnx/sensevoice_small.tar.bz2"
    }
)

foreach ($package in $packages) {
    Publish-Package -Url $package.Url `
        -CacheName $package.CacheName `
        -RelativeServerPath $package.RelativeServerPath
}

Write-Host "[SUCCESS] sherpa-onnx CPU/CUDA runtime and model payloads are published."
