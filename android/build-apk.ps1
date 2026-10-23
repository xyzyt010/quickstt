# Build the release APK for the phone.
#
#   1. cargo apk build        -> the Rust library, the manifest and the .so
#   2. javac + d8             -> classes.dex with the floating-bubble helper
#   3. the dex is added to the APK, the APK is aligned and re-signed
#
# The bubble needs a Java class (a View.OnClickListener cannot be created
# from JNI), and cargo-apk cannot put a dex into the APK — so the APK is
# finished by hand here.
#
#   ./build-apk.ps1            release build (signed with the release key)
#   ./build-apk.ps1 -Debug     debug build
param(
    [switch]$Debug,
    # The ABI to build for. The phone is arm64-v8a; the emulator on this
    # machine is x86_64, so the same script can build a test APK for it.
    [string]$Abi = "aarch64-linux-android",
    # Where the finished APK is copied to. Empty means cargo-apk's output.
    [string]$OutName = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $root

$sdk = $null
foreach ($candidate in @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, "$env:LOCALAPPDATA\Android\Sdk")) {
    # ANDROID_HOME sometimes points at platform-tools; only a real SDK root
    # has build-tools and platforms.
    if ($candidate -and (Test-Path (Join-Path $candidate "build-tools")) -and (Test-Path (Join-Path $candidate "platforms"))) {
        $sdk = $candidate
        break
    }
}
if (-not $sdk) { throw "no Android SDK found (set ANDROID_HOME to the SDK root)" }
$buildTools = (Get-ChildItem "$sdk\build-tools" -Directory |
    Where-Object { $_.Name -notmatch "rc" } |
    Sort-Object { [int](($_.Name -split "\.")[0]) } -Descending | Select-Object -First 1).FullName
$platform = (Get-ChildItem "$sdk\platforms" -Directory |
    Sort-Object { [int](($_.Name -replace "android-", "")) } -Descending | Select-Object -First 1).FullName
$androidJar = Join-Path $platform "android.jar"
if (-not (Test-Path $androidJar)) { throw "no android.jar in $platform" }

$work = Join-Path $root "target\dex"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$work\classes" | Out-Null
New-Item -ItemType Directory -Force "$work\dex" | Out-Null

Write-Host "==> compiling the Java helpers"
& javac --release 8 -nowarn -classpath $androidJar -d "$work\classes" `
    "java\ai\quickstt\app\Bubble.java" "java\ai\quickstt\app\Overlay.java" `
    "java\ai\quickstt\app\Engine.java" "java\ai\quickstt\app\SpeechService.java" `
    "java\ai\quickstt\app\ModelDownloader.java"
if ($LASTEXITCODE -ne 0) { throw "javac failed" }

Write-Host "==> dexing"
$d8 = Get-ChildItem "$buildTools" -Filter "d8*" | Select-Object -First 1
& $d8.FullName --min-api 26 --output "$work\dex" `
    "$work\classes\ai\quickstt\app\Bubble.class" "$work\classes\ai\quickstt\app\Overlay.class" `
    "$work\classes\ai\quickstt\app\Engine.class" "$work\classes\ai\quickstt\app\SpeechService.class" `
    "$work\classes\ai\quickstt\app\ModelDownloader.class"
if ($LASTEXITCODE -ne 0) { throw "d8 failed" }
if (-not (Test-Path "$work\dex\classes.dex")) { throw "d8 produced no classes.dex" }

# The manifest. cargo-apk generates one that can only describe an activity;
# QuickSTT needs a foreground service (the bubble has to keep working while
# the user works in another app), so the manifest is compiled here with
# aapt2 and put into the APK afterwards.
Write-Host "==> compiling the manifest"
$cargoToml = Get-Content "$root\Cargo.toml" -Raw
$version = "0.1.0"
$versionCode = 16777216
if ($cargoToml -match 'version\s*=\s*"(\d+)\.(\d+)\.(\d+)"') {
    $major = [int]$Matches[1]
    $minor = [int]$Matches[2]
    $patch = [int]$Matches[3]
    $version = "$major.$minor.$patch"
    # The same scheme cargo-apk uses, so the version code keeps growing.
    $versionCode = 0x1000000 + ($minor * 256) + $patch
}
$manifestWork = Join-Path $root "target\manifest"
Remove-Item $manifestWork -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $manifestWork | Out-Null
& "$buildTools\aapt2.exe" link -I $androidJar --manifest "$root\AndroidManifest.xml" `
    --auto-add-overlay --min-sdk-version 26 --target-sdk-version 34 `
    --version-code $versionCode --version-name $version `
    -o "$manifestWork\manifest.apk"
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed" }
if (-not (Test-Path "$manifestWork\manifest.apk")) { throw "aapt2 produced no manifest" }

Write-Host "==> building the Rust library"
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
if (-not $env:ANDROID_NDK_ROOT) {
    $ndk = (Get-ChildItem "$sdk\ndk" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
    $env:ANDROID_NDK_ROOT = $ndk
}
# The `cc` crate compiles the AAudio shim and only finds the compiler on PATH
# ("aarch64-linux-android21-clang"); the NDK is not there by default.
$ndkHost = if ($IsWindows) { "windows-x86_64" } else { "linux-x86_64" }
$ndkBin = Join-Path $env:ANDROID_NDK_ROOT "toolchains\llvm\prebuilt\$ndkHost\bin"
if (Test-Path $ndkBin) {
    if (-not ($env:PATH -split ';' | Where-Object { $_ -eq $ndkBin })) {
        $env:PATH = "$ndkBin;$env:PATH"
    }
    Write-Host "    ndk toolchain: $ndkBin"
} else {
    Write-Host "    WARNING: no NDK toolchain at $ndkBin"
}
if ($Debug) {
    cargo apk build --target $Abi --lib
} else {
    cargo apk build --release --target $Abi --lib
}
if ($LASTEXITCODE -ne 0) { throw "cargo apk build failed" }

$profile = if ($Debug) { "debug" } else { "release" }
$apk = @(
    (Join-Path $root "target\$Abi\$profile\apk\quickstt-android.apk"),
    (Join-Path $root "target\$profile\apk\quickstt-android.apk")
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $apk) { throw "cargo apk did not produce an APK" }

Write-Host "==> adding the dex, the manifest and the engines"
$merged = Join-Path $work "with-dex.apk"
# The speech engines are arm64 binaries; an emulator APK has none.
$engineArgs = @()
if ($Abi -eq "aarch64-linux-android") {
    $engineArgs = @(
        "libwhisper-cli.so=$root\prebuilt\whisper-cli-arm64-v8a",
        "libneedle.so=$root\prebuilt\needle-arm64-v8a",
        "libtranscribe-cli.so=$root\prebuilt\transcribe-cli-arm64-v8a"
    )
}
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    @'
import sys, zipfile
src, dst, dex, manifest_apk = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
# The engines: the file name in the APK and the binary to put there.
engines = [(a.split("=")[0], a.split("=", 1)[1]) for a in sys.argv[5:]]

# Our own manifest (the one aapt2 compiled). It declares the foreground
# service that keeps QuickSTT alive in the background.
with zipfile.ZipFile(manifest_apk) as zm:
    manifest = zm.read("AndroidManifest.xml")

with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w") as zout:
    stamp = None
    for item in zin.infolist():
        stamp = item.date_time
        if item.filename == "AndroidManifest.xml":
            continue
        data = zin.read(item.filename)
        info = zipfile.ZipInfo(item.filename, date_time=item.date_time)
        info.compress_type = item.compress_type
        info.external_attr = item.external_attr
        info.create_system = item.create_system
        zout.writestr(info, data)
    info = zipfile.ZipInfo("AndroidManifest.xml", date_time=stamp)
    info.compress_type = zipfile.ZIP_DEFLATED
    zout.writestr(info, manifest)
    zout.write(dex, "classes.dex")
    # The engines go into the APK's lib directory: the installer extracts
    # them into the app's native library directory, which is the only place
    # where Android lets an app run a program.
    for name, path in engines:
        with open(path, "rb") as handle:
            data = handle.read()
        info = zipfile.ZipInfo("lib/arm64-v8a/" + name, date_time=stamp)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o755 << 16
        zout.writestr(info, data)
'@ | & $python.Source - $apk $merged "$work\dex\classes.dex" "$manifestWork\manifest.apk" @engineArgs
} else {
    # Fall back to the JDK's jar tool.
    $jar = (Get-Command jar -ErrorAction Stop).Source
    Copy-Item $apk $merged -Force
    Push-Location "$work\dex"
    & $jar uf $merged classes.dex
    Pop-Location
    # The manifest out of the aapt2 output.
    $manifestDir = Join-Path $work "manifest"
    New-Item -ItemType Directory -Force $manifestDir | Out-Null
    Push-Location $manifestDir
    & $jar xf "$manifestWork\manifest.apk" AndroidManifest.xml
    Pop-Location
    Push-Location $manifestDir
    & $jar uf $merged AndroidManifest.xml
    Pop-Location
}
if (-not (Test-Path $merged)) { throw "the dex was not added" }

Write-Host "==> aligning"
$aligned = Join-Path $work "aligned.apk"
& "$buildTools\zipalign.exe" -f -p 4 $merged $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }

if ($Debug) {
    Copy-Item $aligned $apk -Force
    Write-Host "==> debug APK (not signed): $apk"
} else {
    Write-Host "==> signing"
    & "$buildTools\apksigner.bat" sign --ks "quickstt-release.jks" --ks-key-alias quickstt `
        --ks-pass pass:quickstt --key-pass pass:quickstt --out $apk $aligned
    if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }
    Write-Host "==> signed release APK: $apk"
}

if ($OutName) {
    $dest = Join-Path $root $OutName
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Copy-Item $apk $dest -Force
    Write-Host "==> copied to $dest"
}

Pop-Location
