@echo off
setlocal enabledelayedexpansion
echo === QuickSTT Global Update Push ===

set BASE_DIR=%~dp0
set SRC_APP=%BASE_DIR%QuickSTT_App
set DST_SERVER=%BASE_DIR%QuickSTT_Server\files
set SERVER_ROOT=%BASE_DIR%QuickSTT_Server

if not exist "%SRC_APP%" (
    echo [ERROR] QuickSTT_App folder not found.
    pause
    exit /b
)

if not exist "%DST_SERVER%" mkdir "%DST_SERVER%"

echo [1/2] Syncing Application Files...
REM Only sync the files the end user actually needs (exclude build artifacts)
REM Also exclude opengl32sw.dll (20MB software OpenGL - not needed)
robocopy "%SRC_APP%" "%DST_SERVER%" /MIR /NJH /NJS /MT:16 /Z ^
    /XD ".qt" "CMakeFiles" "QuickSTT_App_autogen" "QuickSTT_autogen" "__pycache__" "_internal" "Source" "recordings" ^
    /XF "*.log" "startup_log.txt" "service_error.log" "stt_startup_log.txt" "error.log" "opengl32sw.dll" "CMakeLists.txt" "D3Dcompiler_47.dll"

if exist "%DST_SERVER%\_internal" rmdir /S /Q "%DST_SERVER%\_internal"
if exist "%DST_SERVER%\_internal.zip" del /F /Q "%DST_SERVER%\_internal.zip"
if exist "%DST_SERVER%\data\recordings" rmdir /S /Q "%DST_SERVER%\data\recordings"
if exist "%DST_SERVER%\.qt" rmdir /S /Q "%DST_SERVER%\.qt"
if exist "%DST_SERVER%\CMakeFiles" rmdir /S /Q "%DST_SERVER%\CMakeFiles"
if exist "%DST_SERVER%\QuickSTT_App_autogen" rmdir /S /Q "%DST_SERVER%\QuickSTT_App_autogen"
if exist "%DST_SERVER%\QuickSTT_autogen" rmdir /S /Q "%DST_SERVER%\QuickSTT_autogen"
if exist "%DST_SERVER%\Source" rmdir /S /Q "%DST_SERVER%\Source"
if exist "%DST_SERVER%\__pycache__" rmdir /S /Q "%DST_SERVER%\__pycache__"
if exist "%DST_SERVER%\startup_log.txt" del /F /Q "%DST_SERVER%\startup_log.txt"
if exist "%DST_SERVER%\service_error.log" del /F /Q "%DST_SERVER%\service_error.log"
if exist "%DST_SERVER%\stt_startup_log.txt" del /F /Q "%DST_SERVER%\stt_startup_log.txt"
if exist "%DST_SERVER%\error.log" del /F /Q "%DST_SERVER%\error.log"
if exist "%DST_SERVER%\opengl32sw.dll" del /F /Q "%DST_SERVER%\opengl32sw.dll"
if exist "%DST_SERVER%\D3Dcompiler_47.dll" del /F /Q "%DST_SERVER%\D3Dcompiler_47.dll"
if exist "%DST_SERVER%\CMakeLists.txt" del /F /Q "%DST_SERVER%\CMakeLists.txt"
if exist "%SRC_APP%\_internal" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%SRC_APP%\_internal\*' -DestinationPath '%DST_SERVER%\_internal.zip' -Force"
)

if exist "%SERVER_ROOT%\version.txt" copy /Y "%SERVER_ROOT%\version.txt" "%DST_SERVER%\version.txt" >nul
if exist "%SERVER_ROOT%\version.json" copy /Y "%SERVER_ROOT%\version.json" "%DST_SERVER%\version.json" >nul

echo [2/2] Regenerating Manifest...
if exist "%BASE_DIR%QuickSTT_Server\gen_manifest.py" (
    python "%BASE_DIR%QuickSTT_Server\gen_manifest.py"
) else (
    echo [WARNING] gen_manifest.py not found in QuickSTT_Server.
)

echo.
echo SUCCESS: Update pushed to server.
pause
