<#
.SYNOPSIS
  QuickSTT installer for Windows (v1.0.0).

.DESCRIPTION
  One command (interactive Inno Setup wizard - proper setup screens):
    irm https://raw.githubusercontent.com/xyzyt010/quickstt/main/install.ps1 | iex

  Or download this file and run it:
    .\install.ps1              # download QuickSTT-Setup-1.0.0.exe and launch the wizard
    .\install.ps1 -Silent      # unattended install (/VERYSILENT)
    .\install.ps1 -Portable    # no installer: portable zip -> %LOCALAPPDATA%\Programs\QuickSTT-Portable
    .\install.ps1 -Version 1.0.0

  NOTES ON ARCHITECTURE:
  - The setup exe (QuickSTT-Setup-1.0.0.exe) and the portable zip
    (quickstt-desktop-v1.0.0-win64-portable.zip) are x64 builds. On Windows
    ARM64 they run through Windows' built-in x64 emulation until a native
    arm64 setup ships.
  - If a future release publishes native arm64 assets
    (QuickSTT-Setup-<ver>-arm64.exe / quickstt-desktop-v<ver>-arm64-portable.zip),
    this script prefers them automatically when the OS architecture is ARM64.

  Compatible with Windows PowerShell 5.1.
#>
[CmdletBinding()]
param(
    [switch]$Silent,
    [switch]$Portable,
    [string]$Version = "1.0.0"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072 } catch { }

$Repo = "xyzyt010/quickstt"

# Normalise: accept 1.0.0, v1.0.0 or desktop-v1.0.0
$Ver = $Version.Trim()
if ($Ver.StartsWith("desktop-")) { $Ver = $Ver.Substring(8) }
if ($Ver.StartsWith("v")) { $Ver = $Ver.Substring(1) }
if ([string]::IsNullOrWhiteSpace($Ver)) { throw "Empty version." }
$DesktopTag = "desktop-v$Ver"
$BaseUrl = "https://github.com/$Repo/releases/download/$DesktopTag"

# Detect OS architecture (RuntimeInformation reports the OS, not the
# emulated process, so this is correct on Windows ARM64 too).
$Arch = "x64"
try {
    $osArch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    if ($osArch -eq "Arm64") { $Arch = "arm64" }
    elseif ($osArch -eq "X64") { $Arch = "x64" }
} catch {
    if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64" -or $env:PROCESSOR_ARCHITEW6432 -eq "ARM64") { $Arch = "arm64" }
}
Write-Host "[quickstt] QuickSTT $DesktopTag | windows arch=$Arch"

$WorkDir = Join-Path ([System.IO.Path]::GetTempPath()) ("quickstt-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

function Get-ShaMap {
    $sumsPath = Join-Path $WorkDir "SHA256SUMS"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/SHA256SUMS" -OutFile $sumsPath
    } catch {
        Write-Host "[quickstt] no SHA256SUMS published for $DesktopTag - skipping checksum verification"
        return $null
    }
    $map = @{}
    foreach ($line in Get-Content $sumsPath) {
        $parts = $line.Trim() -split '\s+', 2
        if ($parts.Count -eq 2) { $map[$parts[1].TrimStart('*')] = $parts[0].ToLower() }
    }
    return $map
}

function Get-VerifiedAsset {
    param([string[]]$Candidates, $ShaMap)
    foreach ($asset in $Candidates) {
        $dest = Join-Path $WorkDir $asset
        try {
            Write-Host "[quickstt] downloading $asset"
            Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/$asset" -OutFile $dest
        } catch {
            Write-Host "[quickstt] not available: $asset ($($_.Exception.Message))"
            continue
        }
        if ($null -ne $ShaMap -and $ShaMap.ContainsKey($asset)) {
            $actual = (Get-FileHash -Algorithm SHA256 $dest).Hash.ToLower()
            if ($actual -ne $ShaMap[$asset]) { throw "Checksum FAILED for $asset" }
            Write-Host "[quickstt] checksum OK ($asset)"
        } elseif ($null -ne $ShaMap) {
            Write-Host "[quickstt] SHA256SUMS has no entry for $asset - skipping checksum verification"
        }
        return $dest
    }
    return $null
}

try {
    $shaMap = Get-ShaMap

    if ($Portable) {
        if ($Arch -eq "arm64") {
            # Prefer a native arm64 portable if one is ever published; the
            # win64 portable runs under x64 emulation otherwise.
            $candidates = @("quickstt-$DesktopTag-arm64-portable.zip", "quickstt-$DesktopTag-win64-portable.zip")
        } else {
            $candidates = @("quickstt-$DesktopTag-win64-portable.zip")
        }
        $zip = Get-VerifiedAsset -Candidates $candidates -ShaMap $shaMap
        if ($null -eq $zip) { throw "Portable zip download failed from $BaseUrl" }

        $destDir = Join-Path $env:LOCALAPPDATA "Programs\QuickSTT-Portable"
        $extractDir = Join-Path $WorkDir "extract"
        Expand-Archive -Path $zip -DestinationPath $extractDir -Force
        # The zip wraps everything in a QuickSTT-Portable\ folder; flatten it.
        $inner = Join-Path $extractDir "QuickSTT-Portable"
        $sourceDir = if (Test-Path (Join-Path $inner "QuickSTT.exe")) { $inner } else { $extractDir }
        if (-not (Test-Path (Join-Path $sourceDir "QuickSTT.exe"))) { throw "Portable zip did not contain QuickSTT.exe" }
        if (Test-Path $destDir) { Remove-Item $destDir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
        Copy-Item (Join-Path $sourceDir "*") $destDir -Recurse -Force

        # Desktop shortcut (best effort).
        try {
            $shell = New-Object -ComObject WScript.Shell
            $shortcut = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath("Desktop")) "QuickSTT.lnk"))
            $shortcut.TargetPath = Join-Path $destDir "QuickSTT.exe"
            $shortcut.WorkingDirectory = $destDir
            $icon = Join-Path $destDir "icon_app.ico"
            if (Test-Path $icon) { $shortcut.IconLocation = $icon }
            $shortcut.Save()
            Write-Host "[quickstt] desktop shortcut created: QuickSTT.lnk"
        } catch {
            Write-Host "[quickstt] shortcut note: create a shortcut to $destDir\QuickSTT.exe manually ($($_.Exception.Message))"
        }
        Write-Host "[quickstt] portable installed. Run: $destDir\QuickSTT.exe"
        return
    }

    # Default: interactive Inno Setup wizard (proper setup screens).
    if ($Arch -eq "arm64") {
        Write-Host "[quickstt] note: the setup exe is x64 and runs on Windows ARM64 via built-in x64 emulation until a native arm64 setup ships."
        $setupCandidates = @("QuickSTT-Setup-$Ver-arm64.exe", "QuickSTT-Setup-$Ver.exe")
    } else {
        $setupCandidates = @("QuickSTT-Setup-$Ver.exe")
    }
    $setup = Get-VerifiedAsset -Candidates $setupCandidates -ShaMap $shaMap
    if ($null -eq $setup) {
        throw "Setup exe download failed from $BaseUrl. Try the portable install instead: .\install.ps1 -Portable"
    }

    if ($Silent) {
        Write-Host "[quickstt] installing silently..."
        $proc = Start-Process -FilePath $setup -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART" -Wait -PassThru
        if ($proc.ExitCode -ne 0) { throw "Installer exited with code $($proc.ExitCode)" }
        Write-Host "[quickstt] installed. Find QuickSTT in the Start Menu."
    } else {
        # Launch from a stable path: the finally block removes $WorkDir,
        # which must not yank the exe from under the running wizard.
        $stableSetup = Join-Path ([System.IO.Path]::GetTempPath()) (Split-Path $setup -Leaf)
        Copy-Item $setup $stableSetup -Force
        Write-Host "[quickstt] launching the QuickSTT setup wizard..."
        Start-Process -FilePath $stableSetup | Out-Null
        Write-Host "[quickstt] setup launched - follow the wizard screens to finish installing."
    }
} finally {
    Remove-Item $WorkDir -Recurse -Force -ErrorAction SilentlyContinue
}
