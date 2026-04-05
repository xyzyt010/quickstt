param(
    [Parameter(Mandatory = $true)][string]$BaseDir,
    [Parameter(Mandatory = $true)][string]$ServerFilesDir
)

$ErrorActionPreference = "Stop"
$BaseDir = [System.IO.Path]::GetFullPath($BaseDir)
$ServerFilesDir = [System.IO.Path]::GetFullPath($ServerFilesDir)

function Ensure-Directory {
    param([Parameter(Mandatory = $true)][string]$Path)
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function New-ZipFromDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$ZipPath,
        [switch]$NoCompression
    )

    if (-not (Test-Path $SourceDir)) {
        throw "Missing source directory: $SourceDir"
    }

    Ensure-Directory -Path ([System.IO.Path]::GetDirectoryName($ZipPath))
    if (Test-Path $ZipPath) {
        Remove-Item $ZipPath -Force
    }

    if ($NoCompression) {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::CreateFromDirectory(
            $SourceDir,
            $ZipPath,
            [System.IO.Compression.CompressionLevel]::NoCompression,
            $false
        )
    } else {
        Compress-Archive -Path (Join-Path $SourceDir '*') -DestinationPath $ZipPath -Force
    }
}

function Write-WarnLine {
    param([string]$Text)
    Write-Host "[WARNING] $Text"
}

function Ensure-DownloadedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Url,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    Ensure-Directory -Path ([System.IO.Path]::GetDirectoryName($DestinationPath))
    if ((Test-Path $DestinationPath) -and ((Get-Item $DestinationPath).Length -gt 0)) {
        return $DestinationPath
    }

    $tmpPath = "$DestinationPath.download"
    if (Test-Path $tmpPath) {
        Remove-Item $tmpPath -Force
    }

    Write-Host "[DOWNLOAD] $Url"
    Invoke-WebRequest -Uri $Url -OutFile $tmpPath -Headers @{ "User-Agent" = "QuickSTT Builder" }
    Move-Item -LiteralPath $tmpPath -Destination $DestinationPath -Force
    return $DestinationPath
}

function Run-NativeCommand {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
    }
}

function Ensure-WhisperCppSource {
    param([Parameter(Mandatory = $true)][string]$SourceRoot)

    if (Test-Path (Join-Path $SourceRoot 'CMakeLists.txt')) {
        return $SourceRoot
    }

    Ensure-Directory -Path ([System.IO.Path]::GetDirectoryName($SourceRoot))
    Write-Host "[DOWNLOAD] whisper.cpp source (official)"
    Run-NativeCommand -FilePath 'git' -Arguments @(
        'clone', '--depth', '1', '--branch', 'v1.8.4',
        'https://github.com/ggml-org/whisper.cpp.git',
        $SourceRoot
    )
    return $SourceRoot
}

function Publish-File {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    if (-not (Test-Path $SourcePath)) {
        throw "Missing source file: $SourcePath"
    }
    Ensure-Directory -Path ([System.IO.Path]::GetDirectoryName($DestinationPath))
    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
}

function Resolve-FasterWhisperModelDir {
    param([Parameter(Mandatory = $true)][string]$SearchRoot)

    if (Test-Path (Join-Path $SearchRoot 'model.bin')) {
        return $SearchRoot
    }

    $modelFile = Get-ChildItem -Path $SearchRoot -Recurse -Filter 'model.bin' -File |
        Select-Object -First 1
    if ($null -eq $modelFile) {
        throw "Unable to locate model.bin under $SearchRoot"
    }
    return $modelFile.Directory.FullName
}

$cacheRoot = Join-Path $BaseDir "third_party\package_cache"
$runtimeCacheRoot = Join-Path $cacheRoot "runtimes"
$whisperCacheRoot = Join-Path $cacheRoot "whisper_cpp"
$fasterCacheRoot = Join-Path $cacheRoot "faster_whisper"
$sourceCacheRoot = Join-Path $cacheRoot "src"

$runtimeRoot = Join-Path $ServerFilesDir "models\runtimes"
$whisperRoot = Join-Path $ServerFilesDir "models\whisper_cpp"
$openvinoRoot = Join-Path $whisperRoot "openvino"
$fasterRoot = Join-Path $ServerFilesDir "models\faster_whisper"

Ensure-Directory -Path $runtimeCacheRoot
Ensure-Directory -Path $whisperCacheRoot
Ensure-Directory -Path $fasterCacheRoot
Ensure-Directory -Path $sourceCacheRoot
Ensure-Directory -Path $runtimeRoot
Ensure-Directory -Path $whisperRoot
Ensure-Directory -Path $openvinoRoot
Ensure-Directory -Path $fasterRoot

$whisperCpuBin = Join-Path $BaseDir "third_party\whisper.cpp\build_cpu\bin\Release"
$whisperCpuZip = Join-Path $runtimeCacheRoot "whisper_cpp_cpu_runtime.zip"
if (Test-Path (Join-Path $whisperCpuBin "whisper-cli.exe")) {
    New-ZipFromDirectory -SourceDir $whisperCpuBin -ZipPath $whisperCpuZip
    Publish-File -SourcePath $whisperCpuZip -DestinationPath (Join-Path $runtimeRoot "whisper_cpp_cpu_runtime.zip")
} else {
    Write-WarnLine "whisper.cpp CPU runtime build not found; CPU runtime zip not refreshed."
}

$whisperNvidiaRuntimeUrl = "https://github.com/ggml-org/whisper.cpp/releases/download/v1.8.4/whisper-cublas-12.4.0-bin-x64.zip"
$whisperNvidiaZip = Ensure-DownloadedFile -Url $whisperNvidiaRuntimeUrl -DestinationPath (Join-Path $runtimeCacheRoot "whisper_cpp_nvidia_runtime.zip")
Publish-File -SourcePath $whisperNvidiaZip -DestinationPath (Join-Path $runtimeRoot "whisper_cpp_nvidia_runtime.zip")

$whisperVulkanZip = Join-Path $runtimeCacheRoot "whisper_cpp_vulkan_runtime.zip"
$qtMingwBin = "C:\Qt\Tools\mingw1310_64\bin"
$mingwMake = Join-Path $qtMingwBin "mingw32-make.exe"
$gcc = Join-Path $qtMingwBin "gcc.exe"
$gxx = Join-Path $qtMingwBin "g++.exe"
$glslc = Get-Command glslc.exe -ErrorAction SilentlyContinue
$vulkanSdkPresent = (-not [string]::IsNullOrWhiteSpace($env:VULKAN_SDK)) -and ($null -ne $glslc)
if ($vulkanSdkPresent -and (Test-Path $mingwMake) -and (Test-Path $gcc) -and (Test-Path $gxx)) {
    try {
        $whisperSource = Ensure-WhisperCppSource -SourceRoot (Join-Path $sourceCacheRoot "whisper.cpp")
        $whisperVulkanBuild = Join-Path $cacheRoot "build_whisper_vulkan"
        if (Test-Path $whisperVulkanBuild) {
            Remove-Item $whisperVulkanBuild -Recurse -Force
        }

        $previousPath = $env:PATH
        $env:PATH = "$qtMingwBin;$previousPath"
        try {
            Run-NativeCommand -FilePath "C:\Program Files\CMake\bin\cmake.exe" -Arguments @(
                '-S', $whisperSource,
                '-B', $whisperVulkanBuild,
                '-G', 'MinGW Makefiles',
                "-DCMAKE_MAKE_PROGRAM=$mingwMake",
                '-DCMAKE_BUILD_TYPE=Release',
                "-DCMAKE_C_COMPILER=$gcc",
                "-DCMAKE_CXX_COMPILER=$gxx",
                '-DGGML_VULKAN=1'
            )
            Run-NativeCommand -FilePath "C:\Program Files\CMake\bin\cmake.exe" -Arguments @(
                '--build', $whisperVulkanBuild, '--config', 'Release', '-j', '4'
            )
        } finally {
            $env:PATH = $previousPath
        }

        $whisperVulkanBin = Join-Path $whisperVulkanBuild "bin"
        if (Test-Path (Join-Path $whisperVulkanBin "whisper-cli.exe")) {
            $vulkanStage = Join-Path $env:TEMP "quickstt_whisper_vulkan_stage"
            if (Test-Path $vulkanStage) {
                Remove-Item $vulkanStage -Recurse -Force
            }
            Ensure-Directory -Path $vulkanStage
            Copy-Item (Join-Path $whisperVulkanBin '*') $vulkanStage -Recurse -Force
            New-ZipFromDirectory -SourceDir $vulkanStage -ZipPath $whisperVulkanZip
            Publish-File -SourcePath $whisperVulkanZip -DestinationPath (Join-Path $runtimeRoot "whisper_cpp_vulkan_runtime.zip")
        } else {
            Write-WarnLine "whisper.cpp Vulkan runtime build completed without whisper-cli.exe; AMD Vulkan runtime zip not refreshed."
        }
    } catch {
        Write-WarnLine "whisper.cpp Vulkan runtime build failed; AMD Vulkan runtime zip not refreshed. $($_.Exception.Message)"
    }
} else {
    Write-WarnLine "Vulkan SDK (including glslc) or MinGW toolchain not found; AMD whisper.cpp Vulkan runtime zip not refreshed."
}

$whisperIntelBin = Join-Path $BaseDir "third_party\whisper.cpp\build_openvino\bin\Release"
$openvinoLibs = Join-Path $env:LOCALAPPDATA "Programs\Python\Python311\Lib\site-packages\openvino\libs"
$whisperIntelZip = Join-Path $runtimeCacheRoot "whisper_cpp_intel_openvino_runtime.zip"
if ((Test-Path (Join-Path $whisperIntelBin "whisper-cli.exe")) -and (Test-Path $openvinoLibs)) {
    $intelStage = Join-Path $env:TEMP "quickstt_whisper_openvino_stage"
    if (Test-Path $intelStage) {
        Remove-Item $intelStage -Recurse -Force
    }
    Ensure-Directory -Path $intelStage
    Copy-Item (Join-Path $whisperIntelBin '*') $intelStage -Recurse -Force
    Copy-Item (Join-Path $openvinoLibs '*.dll') $intelStage -Force
    New-ZipFromDirectory -SourceDir $intelStage -ZipPath $whisperIntelZip
    Publish-File -SourcePath $whisperIntelZip -DestinationPath (Join-Path $runtimeRoot "whisper_cpp_intel_openvino_runtime.zip")
} else {
    Write-WarnLine "whisper.cpp Intel OpenVINO runtime build or OpenVINO libs not found; Intel runtime zip not refreshed."
}

$whisperModelMap = @(
    @{ Name = "ggml-tiny.bin"; Url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin" },
    @{ Name = "ggml-small.bin"; Url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin" },
    @{ Name = "ggml-large-v3-turbo.bin"; Url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin" }
)
foreach ($entry in $whisperModelMap) {
    $cachedModel = Ensure-DownloadedFile -Url $entry.Url -DestinationPath (Join-Path $whisperCacheRoot $entry.Name)
    Publish-File -SourcePath $cachedModel -DestinationPath (Join-Path $whisperRoot $entry.Name)
}

$openvinoEncoderMap = @(
    @{ Name = "ggml-tiny-encoder-openvino.zip"; Url = "https://huggingface.co/Whisper-Pascal/whisper-openvino/resolve/main/ggml-tiny-encoder-openvino.zip" },
    @{ Name = "ggml-small-encoder-openvino.zip"; Url = "https://huggingface.co/Whisper-Pascal/whisper-openvino/resolve/main/ggml-small-encoder-openvino.zip" },
    @{ Name = "ggml-large-v3-turbo-encoder-openvino.zip"; Url = "https://huggingface.co/Whisper-Pascal/whisper-openvino/resolve/main/ggml-large-v3-turbo-encoder-openvino.zip" }
)
foreach ($entry in $openvinoEncoderMap) {
    $cachedEncoder = Ensure-DownloadedFile -Url $entry.Url -DestinationPath (Join-Path $whisperCacheRoot $entry.Name)
    Publish-File -SourcePath $cachedEncoder -DestinationPath (Join-Path $openvinoRoot $entry.Name)
}

$runnerScript = Join-Path $BaseDir "Source\faster_whisper_runner.py"
$fasterRuntimeZip = Join-Path $runtimeCacheRoot "faster_whisper_runtime.zip"
if (Test-Path $runnerScript) {
    $runnerWork = Join-Path $env:TEMP "quickstt_faster_whisper_runner"
    $runnerDist = Join-Path $runnerWork "dist"
    $runnerBuild = Join-Path $runnerWork "build"
    $runnerSpec = Join-Path $runnerWork "spec"
    if (Test-Path $runnerWork) {
        Remove-Item $runnerWork -Recurse -Force
    }
    Ensure-Directory -Path $runnerDist
    Ensure-Directory -Path $runnerBuild
    Ensure-Directory -Path $runnerSpec

    $pyInstallerArgs = @(
        "-m", "PyInstaller",
        "--noconfirm",
        "--onedir",
        "--clean",
        "--name", "faster_whisper_runner",
        "--distpath", $runnerDist,
        "--workpath", $runnerBuild,
        "--specpath", $runnerSpec,
        "--collect-all", "faster_whisper",
        "--collect-all", "ctranslate2",
        "--collect-all", "tokenizers",
        "--collect-all", "huggingface_hub",
        "--collect-all", "av",
        "--collect-all", "numpy",
        $runnerScript
    )

    & python @pyInstallerArgs
    if ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $runnerDist "faster_whisper_runner\faster_whisper_runner.exe"))) {
        New-ZipFromDirectory -SourceDir (Join-Path $runnerDist "faster_whisper_runner") -ZipPath $fasterRuntimeZip -NoCompression
        Publish-File -SourcePath $fasterRuntimeZip -DestinationPath (Join-Path $runtimeRoot "faster_whisper_runtime.zip")
    } else {
        Write-WarnLine "faster-whisper runtime build failed; runtime zip not refreshed."
    }
} else {
    Write-WarnLine "faster_whisper_runner.py not found; faster-whisper runtime zip not refreshed."
}

$fasterModelVariants = @("tiny", "small", "turbo")
foreach ($variant in $fasterModelVariants) {
    if (-not (Test-Path $runnerScript)) {
        break
    }

    $variantStage = Join-Path $env:TEMP ("quickstt_faster_model_" + $variant)
    if (Test-Path $variantStage) {
        Remove-Item $variantStage -Recurse -Force
    }
    Ensure-Directory -Path $variantStage

    $downloadOutput = & python $runnerScript download --model $variant --output-dir $variantStage
    if ($LASTEXITCODE -ne 0) {
        Write-WarnLine "faster-whisper $variant model download failed; model zip not refreshed."
        continue
    }

    $downloadedPath = ($downloadOutput | Select-Object -Last 1).Trim()
    if ([string]::IsNullOrWhiteSpace($downloadedPath) -or -not (Test-Path $downloadedPath)) {
        $downloadedPath = Resolve-FasterWhisperModelDir -SearchRoot $variantStage
    } elseif (-not (Test-Path (Join-Path $downloadedPath 'model.bin'))) {
        $downloadedPath = Resolve-FasterWhisperModelDir -SearchRoot $variantStage
    }

    $variantZip = Join-Path $fasterCacheRoot ($variant + ".zip")
    New-ZipFromDirectory -SourceDir $downloadedPath -ZipPath $variantZip
    Publish-File -SourcePath $variantZip -DestinationPath (Join-Path $fasterRoot ($variant + ".zip"))
}
