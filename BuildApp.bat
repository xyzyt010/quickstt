@echo off
echo === QuickSTT Master Build Script ===
setlocal
set QTFRAMEWORK_BYPASS_LICENSE_CHECK=1

set BASE_DIR=%~dp0
set BUILD_DIR=%BASE_DIR%build_tmp
set DIST_APP=%BASE_DIR%QuickSTT_App
set DIST_SERVER=%BASE_DIR%QuickSTT_Server
set DIRECT_DIST=%BASE_DIR%QuickSTT_DirectDownload
set DIRECT_BASIC=%DIRECT_DIST%\QuickSTT_Basic
set DIRECT_FULL=%DIRECT_DIST%\QuickSTT_Full
set SERVER_FILES=%DIST_SERVER%\files
set PS_BASE_DIR=%BASE_DIR%
if "%PS_BASE_DIR:~-1%"=="\" set PS_BASE_DIR=%PS_BASE_DIR:~0,-1%
set MINGW_BIN=C:\Qt\Tools\mingw1310_64\bin
set APP_VERSION=1.5.1
set APP_NOTES=v1.5.1: Restored all features - animated waveform, flexible resize, model search, multi-server updates, setup wizard, AHK bridge, backend health monitor, text size control, MP3 recording, SmartHome controls, and download progress display.

if exist "%DIST_APP%" rmdir /S /Q "%DIST_APP%"
if exist "%DIST_SERVER%" rmdir /S /Q "%DIST_SERVER%"
if exist "%DIRECT_DIST%" rmdir /S /Q "%DIRECT_DIST%"
mkdir "%DIST_APP%"
mkdir "%DIST_SERVER%"
mkdir "%SERVER_FILES%"
mkdir "%DIRECT_DIST%"
mkdir "%DIRECT_BASIC%"
mkdir "%DIRECT_FULL%"

echo [0/6] Stopping running QuickSTT processes...
taskkill /F /IM QuickSTT_App.exe /T >nul 2>&1
taskkill /F /IM QuickSTT.exe /T >nul 2>&1
taskkill /F /IM stt_service.exe /T >nul 2>&1
taskkill /F /IM QuickSTT_Server_App.exe /T >nul 2>&1
taskkill /F /IM QuickSTT_Server.exe /T >nul 2>&1
taskkill /F /IM QuickSTT_Vosk_Service.exe /T >nul 2>&1
timeout /t 2 /nobreak >nul

echo [1/6] Compiling All C++ Components...
if exist "%BUILD_DIR%\QuickSTT_App_autogen\mocs_compilation.cpp" (
    findstr /C:"No files found that require moc" "%BUILD_DIR%\QuickSTT_App_autogen\mocs_compilation.cpp" >nul 2>&1
    if not errorlevel 1 (
        echo [1/6] Resetting stale Qt autogen cache...
        rmdir /S /Q "%BUILD_DIR%"
    )
)
if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /C:"CMAKE_GENERATOR:INTERNAL=Ninja" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 (
        echo [1/6] Resetting build directory to switch generators...
        rmdir /S /Q "%BUILD_DIR%"
    )
)
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"
set PATH=%MINGW_BIN%;C:\Qt\6.10.2\mingw_64\bin;%PATH%
set QTFRAMEWORK_BYPASS_LICENSE_CHECK=1
cmake .. -G Ninja
cmake --build . --target QuickSTT QuickSTT_App stt_service_native --parallel 8
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed.
    pause
    exit /b
)

echo [2/6] Native STT service compiled. Skipping Python PyInstaller...
echo [3/6] Collecting Binaries to App Folder...
copy /Y "QuickSTT.exe" "%BASE_DIR%QuickSTT_Portable.exe" >nul
copy /Y "QuickSTT_App.exe" "%DIST_APP%\"
if exist "%BASE_DIR%quickstt_popup\target\release\quickstt_popup.exe" (
    copy /Y "%BASE_DIR%quickstt_popup\target\release\quickstt_popup.exe" "%DIST_APP%\quickstt_popup.exe" >nul
) else (
    echo [3/6] WARNING: quickstt_popup.exe was not built.
)
if exist "QuickSTT_Server_App.exe" copy /Y "QuickSTT_Server_App.exe" "%DIST_SERVER%\"
copy /Y "C:\Qt\6.10.2\mingw_64\bin\*.dll" "%DIST_APP%\" >nul
for %%F in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do (
    if exist "%MINGW_BIN%\%%F" (
        copy /Y "%MINGW_BIN%\%%F" "%DIST_APP%\" >nul
        copy /Y "%MINGW_BIN%\%%F" "%DIST_SERVER%\" >nul
        if not exist "%DIST_APP%\tools\nemotron" mkdir "%DIST_APP%\tools\nemotron"
        if not exist "%DIST_APP%\tools\parakeet" mkdir "%DIST_APP%\tools\parakeet"
        if not exist "%DIST_APP%\tools\crispasr" mkdir "%DIST_APP%\tools\crispasr"
        copy /Y "%MINGW_BIN%\%%F" "%DIST_APP%\tools\nemotron\" >nul
        copy /Y "%MINGW_BIN%\%%F" "%DIST_APP%\tools\parakeet\" >nul
        copy /Y "%MINGW_BIN%\%%F" "%DIST_APP%\tools\crispasr\" >nul
    )
)
for %%F in (MSVCP140.dll VCRUNTIME140.dll VCRUNTIME140_1.dll) do (
    if exist "C:\Windows\System32\%%F" (
        copy /Y "C:\Windows\System32\%%F" "%DIST_APP%\" >nul
        copy /Y "C:\Windows\System32\%%F" "%DIST_SERVER%\" >nul
    )
)
if exist "%BASE_DIR%Source\native\libportaudio.dll" copy /Y "%BASE_DIR%Source\native\libportaudio.dll" "%DIST_APP%\" >nul
if exist "%BASE_DIR%Source\libvosk.dll" (
    if not exist "%DIST_APP%\vosk" mkdir "%DIST_APP%\vosk"
    copy /Y "%BASE_DIR%Source\libvosk.dll" "%DIST_APP%\vosk\libvosk.dll" >nul
) else if exist "%BASE_DIR%vosk_api\vosk-win64-0.3.42\libvosk.dll" (
    if not exist "%DIST_APP%\vosk" mkdir "%DIST_APP%\vosk"
    copy /Y "%BASE_DIR%vosk_api\vosk-win64-0.3.42\libvosk.dll" "%DIST_APP%\vosk\libvosk.dll" >nul
)
if exist "%BASE_DIR%Source\native\tensorflowlite_c.dll" (
    copy /Y "%BASE_DIR%Source\native\tensorflowlite_c.dll" "%DIST_APP%\" >nul
) else if exist "%BASE_DIR%QuickSTT_App_Fresh\tensorflowlite_c.dll" (
    copy /Y "%BASE_DIR%QuickSTT_App_Fresh\tensorflowlite_c.dll" "%DIST_APP%\" >nul
)
copy /Y "%BASE_DIR%Source\*.svg" "%DIST_APP%\"
if exist "%BASE_DIR%Source\openvino_stt_infer.py" copy /Y "%BASE_DIR%Source\openvino_stt_infer.py" "%DIST_APP%\" >nul
if exist "%BASE_DIR%icon_app.ico" copy /Y "%BASE_DIR%icon_app.ico" "%DIST_APP%\" >nul
if exist "%BASE_DIR%icon_server.ico" copy /Y "%BASE_DIR%icon_server.ico" "%DIST_SERVER%\" >nul
if exist "%BASE_DIR%icon_app.ico" copy /Y "%BASE_DIR%icon_app.ico" "%DIST_APP%\app.ico" >nul
if exist "%BASE_DIR%icon_server.ico" copy /Y "%BASE_DIR%icon_server.ico" "%DIST_SERVER%\server.ico" >nul
if exist "%BASE_DIR%server_icon.png" copy /Y "%BASE_DIR%server_icon.png" "%DIST_SERVER%\" >nul
copy /Y "stt_service.exe" "%DIST_APP%\"
if exist "audio_preprocess" (
    xcopy /S /E /Y /I "audio_preprocess" "%DIST_APP%\audio_preprocess"
) else if exist "%BASE_DIR%third_party\audio_preprocess" (
    xcopy /S /E /Y /I "%BASE_DIR%third_party\audio_preprocess" "%DIST_APP%\audio_preprocess"
)
if exist "%DIST_APP%\build_qt2" rmdir /S /Q "%DIST_APP%\build_qt2"
if exist "%DIST_APP%\build_native" rmdir /S /Q "%DIST_APP%\build_native"
if exist "%DIST_APP%\Source" rmdir /S /Q "%DIST_APP%\Source"
if exist "%DIST_APP%\CMakeFiles" rmdir /S /Q "%DIST_APP%\CMakeFiles"
if not exist "%DIST_APP%\tools\ahk" mkdir "%DIST_APP%\tools\ahk"
if not exist "%DIST_APP%\tools\crispasr" mkdir "%DIST_APP%\tools\crispasr"
if exist "%BASE_DIR%deps\CrispASR\build\bin\crispasr-server.exe" (
    copy /Y "%BASE_DIR%deps\CrispASR\build\bin\*.exe" "%DIST_APP%\tools\crispasr\" >nul
    copy /Y "%BASE_DIR%deps\CrispASR\build\bin\*.dll" "%DIST_APP%\tools\crispasr\" >nul
)
if not exist "%DIST_APP%\tools\parakeet" mkdir "%DIST_APP%\tools\parakeet"
if exist "%BASE_DIR%parakeet_engine\target\release\parakeet_engine.exe" (
    copy /Y "%BASE_DIR%parakeet_engine\target\release\parakeet_engine.exe" "%DIST_APP%\tools\parakeet\" >nul
    copy /Y "%BASE_DIR%parakeet_engine\target\release\*.dll" "%DIST_APP%\tools\parakeet\" >nul
)
if not exist "%DIST_APP%\tools\nemotron" mkdir "%DIST_APP%\tools\nemotron"
if exist "%BASE_DIR%tools\nemotron" (
    xcopy /E /I /Y "%BASE_DIR%tools\nemotron" "%DIST_APP%\tools\nemotron" >nul
)
if not exist "%BASE_DIR%third_party\AutoHotkey\v2\AutoHotkey64.exe" (
    if exist "%USERPROFILE%\Downloads\AutoHotkey_2.0.21_setup.exe" (
        echo [3/6] Installing embedded AutoHotkey runtime - builder only...
        if not exist "%BASE_DIR%third_party\AutoHotkey" mkdir "%BASE_DIR%third_party\AutoHotkey"
        "%USERPROFILE%\Downloads\AutoHotkey_2.0.21_setup.exe" /silent /InstallTo "%BASE_DIR%third_party\AutoHotkey"
    )
)
if exist "%BASE_DIR%third_party\AutoHotkey\v2\AutoHotkey64.exe" (
    copy /Y "%BASE_DIR%third_party\AutoHotkey\v2\AutoHotkey64.exe" "%DIST_APP%\tools\ahk\AutoHotkey64.exe"
    if exist "%BASE_DIR%third_party\AutoHotkey\license.txt" copy /Y "%BASE_DIR%third_party\AutoHotkey\license.txt" "%DIST_APP%\tools\ahk\"
    if exist "%BASE_DIR%Source\QuickSTT_Commands.ahk" copy /Y "%BASE_DIR%Source\QuickSTT_Commands.ahk" "%DIST_APP%\tools\ahk\QuickSTT_Commands.ahk"
) else (
    echo [WARNING] AutoHotkey64.exe not found; special commands will be disabled.
)
if exist "%BASE_DIR%dist\stt_service\stt_service.exe" (
    echo [3/6] Keeping python bundle around just in case, but native is default.
)

echo [4/6] Syncing default Vosk Model (Small En)...
set MODEL_SRC=%APPDATA%\QuickSTT\models\vosk-model-small-en-us-0.15
set MODEL_DST=%DIST_APP%\data\models\vosk-model-small-en-us-0.15
if exist "%MODEL_SRC%" (
    if not exist "%MODEL_DST%" mkdir "%MODEL_DST%"
    xcopy /S /E /Y /I "%MODEL_SRC%" "%MODEL_DST%"
)
set OWW_SRC=%BASE_DIR%dist\stt_service\_internal\openwakeword\resources\models
set OWW_DST=%DIST_APP%\data\oww_models
if exist "%OWW_SRC%" (
    if not exist "%OWW_DST%" mkdir "%OWW_DST%"
    for %%F in (melspectrogram.tflite embedding_model.tflite hey_jarvis_v0.1.tflite hey_rhasspy_v0.1.tflite hey_mycroft_v0.1.tflite alexa_v0.1.tflite timer_v0.1.tflite weather_v0.1.tflite) do (
        if exist "%OWW_SRC%\%%F" copy /Y "%OWW_SRC%\%%F" "%OWW_DST%\" >nul
    )
)
if exist "%BASE_DIR%new_wakeword" (
    if not exist "%OWW_DST%" mkdir "%OWW_DST%"
    copy /Y "%BASE_DIR%new_wakeword\*.onnx" "%OWW_DST%\" >nul
    copy /Y "%BASE_DIR%new_wakeword\*.pt" "%OWW_DST%\" >nul
    copy /Y "%BASE_DIR%new_wakeword\*.json" "%OWW_DST%\" >nul
)

echo %APP_VERSION% > "%DIST_APP%\version.txt"
echo %APP_VERSION% > "%DIST_SERVER%\version.txt"

echo [5/6] Running windeployqt for App and Server...
windeployqt --svg --network --no-translations --no-opengl-sw --no-compiler-runtime --dir "%DIST_APP%" "%DIST_APP%\QuickSTT_App.exe"
windeployqt --svg --network --no-translations --no-opengl-sw --no-compiler-runtime --dir "%DIST_SERVER%" "%DIST_SERVER%\QuickSTT_Server_App.exe"
for %%F in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll MSVCP140.dll VCRUNTIME140.dll VCRUNTIME140_1.dll) do (
    if exist "%DIST_APP%\%%F" copy /Y "%DIST_APP%\%%F" "%DIST_SERVER%\%%F" >nul
)

echo [6/6] Creating Master Server Distribution Package...
echo Creating update payload in %SERVER_FILES%...
xcopy /S /E /Y /I "%DIST_APP%\*" "%SERVER_FILES%\"
copy /Y "%DIST_APP%\Untitled-1.svg" "%DIST_SERVER%\"
echo %APP_VERSION% > "%SERVER_FILES%\version.txt"
if exist "%SERVER_FILES%\startup_log.txt" del /F /Q "%SERVER_FILES%\startup_log.txt"
if exist "%SERVER_FILES%\service_error.log" del /F /Q "%SERVER_FILES%\service_error.log"
if exist "%SERVER_FILES%\stt_startup_log.txt" del /F /Q "%SERVER_FILES%\stt_startup_log.txt"
if exist "%SERVER_FILES%\error.log" del /F /Q "%SERVER_FILES%\error.log"
if exist "%SERVER_FILES%\data\recordings" rmdir /S /Q "%SERVER_FILES%\data\recordings"
if exist "%SERVER_FILES%\_internal" rmdir /S /Q "%SERVER_FILES%\_internal"
if exist "%SERVER_FILES%\_internal.zip" del /F /Q "%SERVER_FILES%\_internal.zip"
if exist "%SERVER_FILES%\.qt" rmdir /S /Q "%SERVER_FILES%\.qt"
if exist "%SERVER_FILES%\CMakeFiles" rmdir /S /Q "%SERVER_FILES%\CMakeFiles"
if exist "%SERVER_FILES%\QuickSTT_App_autogen" rmdir /S /Q "%SERVER_FILES%\QuickSTT_App_autogen"
if exist "%SERVER_FILES%\QuickSTT_autogen" rmdir /S /Q "%SERVER_FILES%\QuickSTT_autogen"
if exist "%SERVER_FILES%\Source" rmdir /S /Q "%SERVER_FILES%\Source"
if exist "%SERVER_FILES%\build_qt2" rmdir /S /Q "%SERVER_FILES%\build_qt2"
if exist "%SERVER_FILES%\build_native" rmdir /S /Q "%SERVER_FILES%\build_native"
if exist "%SERVER_FILES%\__pycache__" rmdir /S /Q "%SERVER_FILES%\__pycache__"
if exist "%SERVER_FILES%\opengl32sw.dll" del /F /Q "%SERVER_FILES%\opengl32sw.dll"
if exist "%SERVER_FILES%\D3Dcompiler_47.dll" del /F /Q "%SERVER_FILES%\D3Dcompiler_47.dll"
if exist "%SERVER_FILES%\CMakeLists.txt" del /F /Q "%SERVER_FILES%\CMakeLists.txt"
if exist "%DIST_APP%\_internal" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%DIST_APP%\_internal\*' -DestinationPath '%SERVER_FILES%\_internal.zip' -Force"
)
if /I not "%QUICKSTT_PACKAGE_OPTIONAL_MODELS%"=="0" (
    if exist "%BASE_DIR%package_optional_local_runtimes.ps1" (
        echo [6/6] Packaging optional local model payloads...
        powershell -NoProfile -ExecutionPolicy Bypass -File "%BASE_DIR%package_optional_local_runtimes.ps1" -BaseDir "%PS_BASE_DIR%" -ServerFilesDir "%SERVER_FILES%"
    )
) else (
    echo [6/6] Skipping optional local model packaging for fast builds.
    echo        Set QUICKSTT_PACKAGE_OPTIONAL_MODELS=0 to skip payload publishing entirely.
    echo        Set QUICKSTT_REFRESH_OPTIONAL_MODELS=1 when you explicitly want slow remote payload refreshes.
)
if exist "%BASE_DIR%package_optional_services.ps1" (
    echo [6/6] Packaging optional service payloads...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%BASE_DIR%package_optional_services.ps1" -BaseDir "%PS_BASE_DIR%" -ServerFilesDir "%SERVER_FILES%"
)

echo {"version": "%APP_VERSION%", "notes": "%APP_NOTES%"} > "%DIST_SERVER%\version.json"
copy /Y "%DIST_SERVER%\version.json" "%SERVER_FILES%\version.json" >nul

echo [6/6] Creating direct-download fallback package...
xcopy /S /E /Y /I "%DIST_APP%\*" "%DIRECT_BASIC%\"
xcopy /S /E /Y /I "%DIST_APP%\*" "%DIRECT_FULL%\"
for %%D in ("%DIRECT_BASIC%" "%DIRECT_FULL%") do (
    if exist "%%~D\startup_log.txt" del /F /Q "%%~D\startup_log.txt"
    if exist "%%~D\service_error.log" del /F /Q "%%~D\service_error.log"
    if exist "%%~D\stt_startup_log.txt" del /F /Q "%%~D\stt_startup_log.txt"
    if exist "%%~D\error.log" del /F /Q "%%~D\error.log"
)
copy /Y "%BASE_DIR%QuickSTT_Portable.exe" "%DIRECT_DIST%\QuickSTT_Portable.exe" >nul
if exist "%BASE_DIR%DIRECT_DOWNLOAD_README.txt" copy /Y "%BASE_DIR%DIRECT_DOWNLOAD_README.txt" "%DIRECT_DIST%\README.txt" >nul
if exist "%BASE_DIR%EnableQuickSTT_Server_Firewall.bat" copy /Y "%BASE_DIR%EnableQuickSTT_Server_Firewall.bat" "%DIRECT_DIST%\EnableQuickSTT_Server_Firewall.bat" >nul
if exist "%BASE_DIR%EnableQuickSTT_Server_Firewall.bat" copy /Y "%BASE_DIR%EnableQuickSTT_Server_Firewall.bat" "%DIST_SERVER%\EnableQuickSTT_Server_Firewall.bat" >nul
if exist "%BASE_DIR%published_server_urls.txt" (
    copy /Y "%BASE_DIR%published_server_urls.txt" "%DIRECT_DIST%\server_urls.txt" >nul
    copy /Y "%BASE_DIR%published_server_urls.txt" "%DIRECT_BASIC%\server_urls.txt" >nul
    copy /Y "%BASE_DIR%published_server_urls.txt" "%DIRECT_FULL%\server_urls.txt" >nul
    copy /Y "%BASE_DIR%published_server_urls.txt" "%DIST_SERVER%\published_server_urls.txt" >nul
    copy /Y "%BASE_DIR%published_server_urls.txt" "%BASE_DIR%server_urls.txt" >nul
)
if not exist "%BASE_DIR%published_server_urls.txt" (
    if exist "%BASE_DIR%published_server_urls.example.txt" copy /Y "%BASE_DIR%published_server_urls.example.txt" "%DIRECT_DIST%\published_server_urls.example.txt" >nul
    if exist "%BASE_DIR%published_server_urls.example.txt" copy /Y "%BASE_DIR%published_server_urls.example.txt" "%DIST_SERVER%\published_server_urls.example.txt" >nul
)
for %%D in ("%DIRECT_BASIC%" "%DIRECT_FULL%") do (
    if exist "%%~D\build_qt2" rmdir /S /Q "%%~D\build_qt2"
    if exist "%%~D\build_native" rmdir /S /Q "%%~D\build_native"
    if exist "%%~D\Source" rmdir /S /Q "%%~D\Source"
    if exist "%%~D\CMakeFiles" rmdir /S /Q "%%~D\CMakeFiles"
    if exist "%%~D\QuickSTT_App_autogen" rmdir /S /Q "%%~D\QuickSTT_App_autogen"
    if exist "%%~D\QuickSTT_autogen" rmdir /S /Q "%%~D\QuickSTT_autogen"
    if exist "%%~D\__pycache__" rmdir /S /Q "%%~D\__pycache__"
    if exist "%%~D\_internal" rmdir /S /Q "%%~D\_internal"
    if exist "%%~D\_internal.zip" del /F /Q "%%~D\_internal.zip"
    if exist "%%~D\opengl32sw.dll" del /F /Q "%%~D\opengl32sw.dll"
    if exist "%%~D\D3Dcompiler_47.dll" del /F /Q "%%~D\D3Dcompiler_47.dll"
    if exist "%%~D\CMakeLists.txt" del /F /Q "%%~D\CMakeLists.txt"
)
if exist "%SERVER_FILES%\addons\services\smart_life_enable.zip" (
    if not exist "%DIRECT_FULL%\data\services\smart_life" mkdir "%DIRECT_FULL%\data\services\smart_life"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath '%SERVER_FILES%\addons\services\smart_life_enable.zip' -DestinationPath '%DIRECT_FULL%\data\services\smart_life' -Force"
)
if exist "%SERVER_FILES%\addons\services\android_tv_remote_runtime.zip" (
    if not exist "%DIRECT_FULL%\data\services\android_tv_remote" mkdir "%DIRECT_FULL%\data\services\android_tv_remote"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath '%SERVER_FILES%\addons\services\android_tv_remote_runtime.zip' -DestinationPath '%DIRECT_FULL%\data\services\android_tv_remote' -Force"
)
if exist "%DIST_SERVER%\QuickSTT_LAN_Package.tar" del /F /Q "%DIST_SERVER%\QuickSTT_LAN_Package.tar"
tar -cf "%DIST_SERVER%\QuickSTT_LAN_Package.tar" -C "%DIRECT_BASIC%" .
if /I "%QUICKSTT_BUILD_DIRECT_ZIP%"=="1" (
    if exist "%DIRECT_DIST%\QuickSTT_Basic.zip" del /F /Q "%DIRECT_DIST%\QuickSTT_Basic.zip"
    if exist "%DIRECT_DIST%\QuickSTT_Full.zip" del /F /Q "%DIRECT_DIST%\QuickSTT_Full.zip"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%DIRECT_BASIC%\*' -DestinationPath '%DIRECT_DIST%\QuickSTT_Basic.zip' -Force"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%DIRECT_FULL%\*' -DestinationPath '%DIRECT_DIST%\QuickSTT_Full.zip' -Force"
) else (
    if exist "%DIRECT_DIST%\QuickSTT_Basic.zip" del /F /Q "%DIRECT_DIST%\QuickSTT_Basic.zip"
    if exist "%DIRECT_DIST%\QuickSTT_Full.zip" del /F /Q "%DIRECT_DIST%\QuickSTT_Full.zip"
    echo [6/6] Skipping direct ZIP creation for fast builds.
    echo        Set QUICKSTT_BUILD_DIRECT_ZIP=1 when you want the website ZIP package.
)

echo [6/6] Generating update manifest...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$serverFiles='%SERVER_FILES%'; $manifestPath=Join-Path '%DIST_SERVER%' 'manifest_txt';" ^
  "$lines = Get-ChildItem -LiteralPath $serverFiles -Recurse -File | Sort-Object FullName | ForEach-Object { $rel=$_.FullName.Substring($serverFiles.Length).TrimStart('\').Replace('\','/'); $hash=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower(); '{0}|{1}|{2}' -f $rel,$hash,$_.Length };" ^
  "Set-Content -LiteralPath $manifestPath -Value $lines -Encoding ASCII"

echo [6/6] Writing release metadata...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$directDist='%DIRECT_DIST%'; $serverDir='%DIST_SERVER%'; $version='%APP_VERSION%';" ^
  "$artifacts=@();" ^
  "$artifactPaths=@('%BASE_DIR%QuickSTT_Portable.exe', (Join-Path $directDist 'QuickSTT_Portable.exe'), (Join-Path $serverDir 'QuickSTT_LAN_Package.tar'), (Join-Path $directDist 'QuickSTT_Basic\\QuickSTT_App.exe'), (Join-Path $directDist 'QuickSTT_Full\\QuickSTT_App.exe'), (Join-Path $directDist 'QuickSTT_Basic.zip'), (Join-Path $directDist 'QuickSTT_Full.zip'));" ^
  "foreach($path in $artifactPaths){ if(Test-Path $path){ $item=Get-Item $path; $artifacts += [pscustomobject]@{ name=$item.Name; relativePath=($item.FullName.Substring(('%PS_BASE_DIR%').Length).TrimStart('\')); size=$item.Length } } }" ^
  "$manifest=[pscustomobject]@{ version=$version; generatedAt=[DateTime]::UtcNow.ToString('o'); artifacts=$artifacts } | ConvertTo-Json -Depth 4;" ^
  "Set-Content -LiteralPath (Join-Path $directDist 'release_manifest.json') -Value $manifest -Encoding UTF8;" ^
  "$hashTargets=@('%BASE_DIR%QuickSTT_Portable.exe', (Join-Path $directDist 'QuickSTT_Portable.exe'), (Join-Path $serverDir 'QuickSTT_LAN_Package.tar'), (Join-Path $directDist 'QuickSTT_Basic\\QuickSTT_App.exe'), (Join-Path $directDist 'QuickSTT_Full\\QuickSTT_App.exe'), (Join-Path $directDist 'QuickSTT_Basic.zip'), (Join-Path $directDist 'QuickSTT_Full.zip')) | Where-Object { Test-Path $_ };" ^
  "$lines = foreach($target in $hashTargets){ $hash=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLower(); $name=$target.Substring(('%PS_BASE_DIR%').Length).TrimStart('\'); '{0} *{1}' -f $hash,$name };" ^
  "Set-Content -LiteralPath (Join-Path $directDist 'SHA256SUMS.txt') -Value $lines -Encoding ASCII"

echo.
echo SUCCESS: Master build completed!
echo App: %DIST_APP%
echo Server: %DIST_SERVER%
