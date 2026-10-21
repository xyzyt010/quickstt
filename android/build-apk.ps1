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
param([switch]$Debug)

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

Write-Host "==> compiling the bubble helper"
& javac --release 8 -nowarn -classpath $androidJar -d "$work\classes" `
    "java\ai\quickstt\app\Bubble.java" "java\ai\quickstt\app\Engine.java"
if ($LASTEXITCODE -ne 0) { throw "javac failed" }

Write-Host "==> dexing"
$d8 = Get-ChildItem "$buildTools" -Filter "d8*" | Select-Object -First 1
& $d8.FullName --min-api 26 --output "$work\dex" `
    "$work\classes\ai\quickstt\app\Bubble.class" "$work\classes\ai\quickstt\app\Engine.class"
if ($LASTEXITCODE -ne 0) { throw "d8 failed" }
if (-not (Test-Path "$work\dex\classes.dex")) { throw "d8 produced no classes.dex" }

Write-Host "==> building the Rust library"
$env:ANDROID_HOME = $sdk
if (-not $env:ANDROID_NDK_ROOT) {
    $ndk = (Get-ChildItem "$sdk\ndk" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
    $env:ANDROID_NDK_ROOT = $ndk
}
if ($Debug) {
    cargo apk build --target aarch64-linux-android --lib
} else {
    cargo apk build --release --target aarch64-linux-android --lib
}
if ($LASTEXITCODE -ne 0) { throw "cargo apk build failed" }

$apk = Join-Path $root "target\release\apk\quickstt-android.apk"
if (-not (Test-Path $apk)) { $apk = Join-Path $root "target\debug\apk\quickstt-android.apk" }
if (-not (Test-Path $apk)) { throw "cargo apk did not produce an APK" }

Write-Host "==> adding the dex and the engines"
$merged = Join-Path $work "with-dex.apk"
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    @'
import sys, zipfile
src, dst, dex = sys.argv[1], sys.argv[2], sys.argv[3]
# The engines: the file name in the APK and the binary to put there.
engines = [(a.split("=")[0], a.split("=", 1)[1]) for a in sys.argv[4:]]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w") as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        info = zipfile.ZipInfo(item.filename, date_time=item.date_time)
        info.compress_type = item.compress_type
        info.external_attr = item.external_attr
        info.create_system = item.create_system
        zout.writestr(info, data)
    zout.write(dex, "classes.dex")
    # The engines go into the APK's lib directory: the installer extracts
    # them into the app's native library directory, which is the only place
    # where Android lets an app run a program.
    for name, path in engines:
        with open(path, "rb") as handle:
            data = handle.read()
        info = zipfile.ZipInfo("lib/arm64-v8a/" + name, date_time=item.date_time)
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o755 << 16
        zout.writestr(info, data)
'@ | & $python.Source - $apk $merged "$work\dex\classes.dex" `
        "libwhisper-cli.so=$root\prebuilt\whisper-cli-arm64-v8a" `
        "libneedle.so=$root\prebuilt\needle-arm64-v8a" `
        "libtranscribe-cli.so=$root\prebuilt\transcribe-cli-arm64-v8a"
} else {
    # Fall back to the JDK's jar tool.
    $jar = (Get-Command jar -ErrorAction Stop).Source
    Copy-Item $apk $merged -Force
    Push-Location "$work\dex"
    & $jar uf $merged classes.dex
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

Pop-Location
