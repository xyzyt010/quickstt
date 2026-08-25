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

function New-ZipFromDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$ZipPath
    )

    Ensure-Directory -Path ([System.IO.Path]::GetDirectoryName($ZipPath))
    if (Test-Path $ZipPath) {
        Remove-Item $ZipPath -Force
    }
    Compress-Archive -Path (Join-Path $SourceDir '*') -DestinationPath $ZipPath -Force
}

$cacheRoot = Join-Path $BaseDir "third_party\package_cache\optional_services"
$serverAddonsRoot = Join-Path $ServerFilesDir "addons\services"
Ensure-Directory -Path $cacheRoot
Ensure-Directory -Path $serverAddonsRoot

$smartLifeStage = Join-Path $env:TEMP "quickstt_smart_life_marker"
if (Test-Path $smartLifeStage) {
    Remove-Item $smartLifeStage -Recurse -Force
}
Ensure-Directory -Path $smartLifeStage
@{
    service = "smart_life"
    installedAtUtc = [DateTime]::UtcNow.ToString('o')
    source = "QuickSTT optional service bundle"
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $smartLifeStage "installed.json") -Encoding UTF8
New-ZipFromDirectory -SourceDir $smartLifeStage -ZipPath (Join-Path $serverAddonsRoot "smart_life_enable.zip")

$androidTvHelperSource = Join-Path $BaseDir "Source\android_tv_remote_helper.py"
if (-not (Test-Path $androidTvHelperSource)) {
    throw "Missing Android TV helper script: $androidTvHelperSource"
}

$androidTvPythonVersion = "3.11.7"
$androidTvPythonTag = "311"
$androidTvLibraryVersion = "0.3.1"
$androidTvZeroconfVersion = "0.88.0"
$androidTvPythonUrl = "https://www.python.org/ftp/python/$androidTvPythonVersion/python-$androidTvPythonVersion-embed-amd64.zip"
$androidTvPythonZip = Ensure-DownloadedFile -Url $androidTvPythonUrl -DestinationPath (Join-Path $cacheRoot "python-$androidTvPythonVersion-embed-amd64.zip")
$androidTvRuntimeZip = Join-Path $cacheRoot "android_tv_remote_runtime.zip"
$androidTvRuntimeMeta = Join-Path $cacheRoot "android_tv_remote_runtime.meta.json"
$androidTvHelperHash = (Get-FileHash -LiteralPath $androidTvHelperSource -Algorithm SHA256).Hash.ToLower()
$androidTvMetaPayload = @{
    pythonVersion = $androidTvPythonVersion
    libraryVersion = $androidTvLibraryVersion
    zeroconfVersion = $androidTvZeroconfVersion
    helperSha256 = $androidTvHelperHash
} | ConvertTo-Json -Compress

$needsAndroidTvRebuild = $true
if ((Test-Path $androidTvRuntimeZip) -and (Test-Path $androidTvRuntimeMeta)) {
    $existingMeta = Get-Content -LiteralPath $androidTvRuntimeMeta -Raw
    if ($existingMeta -eq $androidTvMetaPayload) {
        $needsAndroidTvRebuild = $false
    }
}

if ($needsAndroidTvRebuild) {
    $androidTvStage = Join-Path $env:TEMP "quickstt_android_tv_remote_runtime"
    if (Test-Path $androidTvStage) {
        Remove-Item $androidTvStage -Recurse -Force
    }

    $runtimeRoot = Join-Path $androidTvStage "runtime"
    $pythonRoot = Join-Path $runtimeRoot "python"
    $sitePackagesRoot = Join-Path $pythonRoot "Lib\site-packages"

    Ensure-Directory -Path $runtimeRoot
    Ensure-Directory -Path $pythonRoot
    Ensure-Directory -Path $sitePackagesRoot

    $pythonZipArchive = Join-Path $cacheRoot "android_tv_embed_python.zip"
    Copy-Item -LiteralPath $androidTvPythonZip -Destination $pythonZipArchive -Force
    Expand-Archive -LiteralPath $pythonZipArchive -DestinationPath $pythonRoot -Force
    Remove-Item $pythonZipArchive -Force

    $pythonPthPath = Join-Path $pythonRoot "python$androidTvPythonTag._pth"
    @(
        "python$androidTvPythonTag.zip"
        "."
        "Lib"
        "Lib\site-packages"
        "import site"
    ) | Set-Content -LiteralPath $pythonPthPath -Encoding ASCII

    $builderVenv = Join-Path $cacheRoot "android_tv_runtime_builder_venv"
    if (-not (Test-Path (Join-Path $builderVenv "Scripts\python.exe"))) {
        Write-Host "[PIP] Creating isolated Android TV runtime builder venv..."
        if (Test-Path $builderVenv) {
            Remove-Item $builderVenv -Recurse -Force
        }
        & python -m venv $builderVenv
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to create isolated builder venv for Android TV runtime."
        }
    }

    $builderPython = Join-Path $builderVenv "Scripts\python.exe"
    $builderSitePackages = Join-Path $builderVenv "Lib\site-packages"

    Write-Host "[PIP] Installing androidtvremote2 runtime in isolated builder venv..."
    & $builderPython -m pip install --disable-pip-version-check --upgrade pip
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to upgrade pip in isolated Android TV builder venv."
    }
    & $builderPython -m pip install --disable-pip-version-check --upgrade "androidtvremote2==$androidTvLibraryVersion" "zeroconf==$androidTvZeroconfVersion"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install androidtvremote2 runtime dependencies."
    }

    Copy-Item -Path (Join-Path $builderSitePackages '*') -Destination $sitePackagesRoot -Recurse -Force

    Copy-Item -LiteralPath $androidTvHelperSource -Destination (Join-Path $runtimeRoot "quickstt_android_tv_helper.py") -Force
    @{
        service = "android_tv_remote"
        protocol = "androidtvremote2"
        installedAtUtc = [DateTime]::UtcNow.ToString('o')
        source = "QuickSTT optional service bundle"
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $androidTvStage "installed.json") -Encoding UTF8

    New-ZipFromDirectory -SourceDir $androidTvStage -ZipPath $androidTvRuntimeZip
    Set-Content -LiteralPath $androidTvRuntimeMeta -Value $androidTvMetaPayload -Encoding UTF8
}

$serverAndroidTvZip = Join-Path $serverAddonsRoot "android_tv_remote_runtime.zip"
Copy-Item -LiteralPath $androidTvRuntimeZip -Destination $serverAndroidTvZip -Force
if (Test-Path (Join-Path $serverAddonsRoot "android_tv_platform_tools.zip")) {
    Remove-Item (Join-Path $serverAddonsRoot "android_tv_platform_tools.zip") -Force
}
