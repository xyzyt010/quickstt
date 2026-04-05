@echo off
echo === QuickSTT Master Build Script ===
setlocal

set BASE_DIR=%~dp0
set BUILD_DIR=%BASE_DIR%build_tmp
set DIST_APP=%BASE_DIR%QuickSTT_App
set DIST_SERVER=%BASE_DIR%QuickSTT_Server
set DIRECT_DIST=%BASE_DIR%QuickSTT_DirectDownload
set DIRECT_APP=%DIRECT_DIST%\QuickSTT_Full
set SERVER_FILES=%DIST_SERVER%\files
set PS_BASE_DIR=%BASE_DIR%
if "%PS_BASE_DIR:~-1%"=="\" set PS_BASE_DIR=%PS_BASE_DIR:~0,-1%
set APP_VERSION=1.5.1
set APP_NOTES=v1.5.1: Restored all features - animated waveform, flexible resize, model search, multi-server updates, setup wizard, AHK bridge, backend health monitor, text size control, MP3 recording, download progress display.

if not exist "%DIST_APP%" mkdir "%DIST_APP%"
if not exist "%DIST_SERVER%" mkdir "%DIST_SERVER%"
if not exist "%SERVER_FILES%" mkdir "%SERVER_FILES%"
if exist "%DIRECT_DIST%" rmdir /S /Q "%DIRECT_DIST%"
if not exist "%DIRECT_DIST%" mkdir "%DIRECT_DIST%"
if not exist "%DIRECT_APP%" mkdir "%DIRECT_APP%"

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
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd "%BUILD_DIR%"
set PATH=C:\Qt\Tools\mingw1310_64\bin;C:\Qt\6.10.2\mingw_64\bin;%PATH%
set QTFRAMEWORK_BYPASS_LICENSE_CHECK=1
cmake .. -G "MinGW Makefiles"
cmake --build . --target QuickSTT QuickSTT_App QuickSTT_Server_App stt_service_native --parallel 8
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed.
    pause
    exit /b
)

echo [2/6] Native STT service compiled. Skipping Python PyInstaller...
echo [3/6] Collecting Binaries to App Folder...
copy /Y "QuickSTT.exe" "%DIST_APP%\"
copy /Y "QuickSTT.exe" "%DIST_APP%\QuickSTT_Portable.exe" >nul
copy /Y "QuickSTT.exe" "%BASE_DIR%QuickSTT_Portable.exe" >nul
copy /Y "QuickSTT_App.exe" "%DIST_APP%\"
copy /Y "QuickSTT_Server_App.exe" "%DIST_SERVER%\"
copy /Y "%BASE_DIR%Source\*.svg" "%DIST_APP%\"
copy /Y "stt_service.exe" "%DIST_APP%\"
if exist "%DIST_APP%\build_qt2" rmdir /S /Q "%DIST_APP%\build_qt2"
if exist "%DIST_APP%\build_native" rmdir /S /Q "%DIST_APP%\build_native"
if exist "%DIST_APP%\Source" rmdir /S /Q "%DIST_APP%\Source"
if exist "%DIST_APP%\CMakeFiles" rmdir /S /Q "%DIST_APP%\CMakeFiles"
if not exist "%DIST_APP%\tools\ahk" mkdir "%DIST_APP%\tools\ahk"
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

echo %APP_VERSION% > "%DIST_APP%\version.txt"
echo %APP_VERSION% > "%DIST_SERVER%\version.txt"

echo [5/6] Running windeployqt for App and Server...
windeployqt --svg --network --no-translations --no-opengl-sw --no-compiler-runtime --dir "%DIST_APP%" "%DIST_APP%\QuickSTT_App.exe"
windeployqt --svg --network --no-translations --no-opengl-sw --no-compiler-runtime --dir "%DIST_SERVER%" "%DIST_SERVER%\QuickSTT_Server_App.exe"

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
if /I "%QUICKSTT_PACKAGE_OPTIONAL_MODELS%"=="1" (
    if exist "%BASE_DIR%package_optional_local_runtimes.ps1" (
        echo [6/6] Packaging optional local model payloads...
        powershell -NoProfile -ExecutionPolicy Bypass -File "%BASE_DIR%package_optional_local_runtimes.ps1" -BaseDir "%PS_BASE_DIR%" -ServerFilesDir "%SERVER_FILES%"
    )
) else (
    echo [6/6] Skipping optional local model packaging for fast builds.
    echo        Set QUICKSTT_PACKAGE_OPTIONAL_MODELS=1 or run PackageOptionalModels.bat when you want to refresh those payloads.
)

echo {"version": "%APP_VERSION%", "notes": "%APP_NOTES%"} > "%DIST_SERVER%\version.json"
copy /Y "%DIST_SERVER%\version.json" "%SERVER_FILES%\version.json" >nul

echo [6/6] Creating direct-download fallback package...
xcopy /S /E /Y /I "%DIST_APP%\*" "%DIRECT_APP%\"
if exist "%DIRECT_APP%\startup_log.txt" del /F /Q "%DIRECT_APP%\startup_log.txt"
if exist "%DIRECT_APP%\service_error.log" del /F /Q "%DIRECT_APP%\service_error.log"
if exist "%DIRECT_APP%\stt_startup_log.txt" del /F /Q "%DIRECT_APP%\stt_startup_log.txt"
if exist "%DIRECT_APP%\error.log" del /F /Q "%DIRECT_APP%\error.log"
copy /Y "%BASE_DIR%QuickSTT_Portable.exe" "%DIRECT_DIST%\QuickSTT_Portable.exe" >nul
if exist "%BASE_DIR%DIRECT_DOWNLOAD_README.txt" copy /Y "%BASE_DIR%DIRECT_DOWNLOAD_README.txt" "%DIRECT_DIST%\README.txt" >nul
if exist "%BASE_DIR%EnableQuickSTT_Server_Firewall.bat" copy /Y "%BASE_DIR%EnableQuickSTT_Server_Firewall.bat" "%DIRECT_DIST%\EnableQuickSTT_Server_Firewall.bat" >nul
if exist "%BASE_DIR%EnableQuickSTT_Server_Firewall.bat" copy /Y "%BASE_DIR%EnableQuickSTT_Server_Firewall.bat" "%DIST_SERVER%\EnableQuickSTT_Server_Firewall.bat" >nul
if exist "%BASE_DIR%published_server_urls.txt" (
    copy /Y "%BASE_DIR%published_server_urls.txt" "%DIRECT_DIST%\server_urls.txt" >nul
    copy /Y "%BASE_DIR%published_server_urls.txt" "%DIRECT_APP%\server_urls.txt" >nul
    copy /Y "%BASE_DIR%published_server_urls.txt" "%DIST_SERVER%\published_server_urls.txt" >nul
    copy /Y "%BASE_DIR%published_server_urls.txt" "%BASE_DIR%server_urls.txt" >nul
)
if exist "%DIST_SERVER%\QuickSTT_LAN_Package.tar" del /F /Q "%DIST_SERVER%\QuickSTT_LAN_Package.tar"
tar -cf "%DIST_SERVER%\QuickSTT_LAN_Package.tar" -C "%DIRECT_APP%" .
if /I "%QUICKSTT_BUILD_DIRECT_ZIP%"=="1" (
    if exist "%DIRECT_DIST%\QuickSTT_Full.zip" del /F /Q "%DIRECT_DIST%\QuickSTT_Full.zip"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%DIRECT_APP%\*' -DestinationPath '%DIRECT_DIST%\QuickSTT_Full.zip' -Force"
) else (
    if exist "%DIRECT_DIST%\QuickSTT_Full.zip" del /F /Q "%DIRECT_DIST%\QuickSTT_Full.zip"
    echo [6/6] Skipping direct ZIP creation for fast builds.
    echo        Set QUICKSTT_BUILD_DIRECT_ZIP=1 when you want the website ZIP package.
)

if exist "%DIST_SERVER%\gen_manifest.py" (
    python "%DIST_SERVER%\gen_manifest.py"
)

echo.
echo SUCCESS: Master build completed!
echo App: %DIST_APP%
echo Server: %DIST_SERVER%
pause
