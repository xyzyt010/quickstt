# Build-Installer.ps1 — build the QuickSTT Windows setup.exe (Inno Setup 6).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File quickstt-rust\installer\Build-Installer.ps1
#
# What it does:
#   1. Reads the version from quickstt-rust\Cargo.toml (single source of truth)
#      and passes it to the .iss so the installer version never drifts.
#   2. Verifies the release exe and the bundled payloads exist.
#   3. Runs ISCC.exe if Inno Setup is installed; otherwise it prints the exact
#      command to run once ISCC is available (the .iss itself is complete and
#      needs no further editing).
param(
    # Skip the release rebuild (use the exe that is already there).
    [switch]$NoBuild,
    # Build the app first in release mode (slow: LTO across Slint ~40 min).
    [switch]$BuildRelease
)

$ErrorActionPreference = 'Stop'

$installerDir = $PSScriptRoot
$rustDir = Split-Path $installerDir -Parent
$repoRoot = Split-Path $rustDir -Parent
$iss = Join-Path $installerDir 'quickstt.iss'
$exe = Join-Path $rustDir 'target\release\quickstt-slint.exe'

# ── version straight from Cargo.toml ──
$cargoToml = Get-Content -LiteralPath (Join-Path $rustDir 'Cargo.toml') -Raw
if ($cargoToml -notmatch '(?m)^version\s*=\s*"([^"]+)"') {
    throw 'Could not read the version from Cargo.toml'
}
$version = $Matches[1]
"QuickSTT version: $version"

# The Windows file-version resource needs four numeric fields, but the app's
# version carries a pre-release label ("2.0.0-alpha.18"). Keep the digits:
#   2.0.0-alpha.18 -> 2.0.0.18    1.5.1 -> 1.5.1.0
$core = ($version -split '-')[0]
$build = if ($version -match '-[^0-9]*([0-9]+)$') { $Matches[1] } else { '0' }
$parts = @($core.Split('.') | ForEach-Object { $_ }) + @('0', '0', '0')
while ($parts.Count -lt 3) { $parts += '0' }
$versionNumeric = "$($parts[0]).$($parts[1]).$($parts[2]).$build"
"File version:  $versionNumeric"

if ($BuildRelease) {
    "==> cargo build --release -p quickstt-slint (this takes a while: LTO)"
    Push-Location $rustDir
    try {
        cargo build --release -p quickstt-slint
        if ($LASTEXITCODE -ne 0) { throw "release build failed ($LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
}

# ── sanity: the payloads the .iss references ──
# wakeword_models must come from the REPO ROOT (the flat <phrase>_v0.1.onnx
# heads the loader actually discovers); the quickstt-rust copy only holds
# per-phrase subfolders, which the loader never reads.
$missing = @()
foreach ($p in @(
    $exe,
    (Join-Path $repoRoot 'wakeword_models'),
    (Join-Path $repoRoot 'tools'),
    (Join-Path $repoRoot 'icon_app.ico')
)) {
    if (-not (Test-Path $p)) { $missing += $p }
}
# The two heads wakeword_loader::discover_oww_heads requires.
foreach ($head in @('hey_jarvis_v0.1.onnx', 'alexa_v0.1.onnx')) {
    $hp = Join-Path (Join-Path $repoRoot 'wakeword_models') $head
    if (-not (Test-Path $hp)) { $missing += $hp }
}
if ($missing.Count -gt 0) {
    "WARNING: these expected files are missing (the installer will skip or fail):"
    $missing | ForEach-Object { "  - $_" }
}

# ── find Inno Setup's compiler ──
$iscc = $null
foreach ($candidate in @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)) {
    if (Test-Path $candidate) { $iscc = $candidate; break }
}
if (-not $iscc) {
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { $iscc = $cmd.Source }
}

if (-not $iscc) {
    ""
    "Inno Setup 6 is not installed, so setup.exe cannot be built here."
    "The .iss script itself is complete and needs no further editing — install"
    "Inno Setup 6 (https://jrsoftware.org/isdl.php), then run:"
    ""
    "    ISCC.exe `"$iss`""
    ""
    "or just re-run this script; it locates ISCC.exe automatically."
    ""
    "Command-line install of Inno Setup (winget):"
    "    winget install --id JRSoftware.InnoSetup -e"
    exit 2
}

"==> $iscc"
& $iscc "/DMyAppVersion=$version" "/DMyAppVersionNumeric=$versionNumeric" "`"$iss`""
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "ISCC failed with exit code $code"
}

$out = Join-Path $rustDir "installer-output\QuickSTT-Setup-$($version).exe"
if (Test-Path $out) {
    $item = Get-Item -LiteralPath $out
    ""
    "SUCCESS: $($item.FullName)"
    "         $([int]($item.Length/1MB)) MB"
} else {
    throw 'ISCC reported success but the setup exe is missing'
}
