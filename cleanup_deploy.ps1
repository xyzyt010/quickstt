$src = "C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\QuickSTT_App"
$dst = "C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\QuickSTT_Server\files"

if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
New-Item -ItemType Directory -Path $dst

# Copy top level files (exclude internal, cache, etc)
Get-ChildItem $src -File | Copy-Item -Destination $dst

# Copy specific directories needed for Qt/App and Python STT Engine
$dirs = @("platforms", "styles", "iconengines", "imageformats", "data")
foreach ($d in $dirs) {
    if (Test-Path "$src\$d") {
        Copy-Item -Path "$src\$d" -Destination $dst -Recurse -Force
    }
}

# Zip the _internal directory to speed up manifest and downloading
if (Test-Path "$src\_internal") {
    Write-Host "Zipping _internal directory for faster deployment..."
    Compress-Archive -Path "$src\_internal" -DestinationPath "$dst\_internal.zip" -Force
}

# Always keep version.txt in files/ in sync with the server version
$serverRoot = "C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\QuickSTT_Server"
if (Test-Path "$serverRoot\version.txt") {
    Copy-Item -Path "$serverRoot\version.txt" -Destination "$dst\version.txt" -Force
    Write-Host "version.txt synced to files/."
}

Write-Host "Cleanup Copy Done."
