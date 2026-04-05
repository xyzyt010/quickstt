@echo off
setlocal

set BASE_DIR=%~dp0
set PS_BASE_DIR=%BASE_DIR%
if "%PS_BASE_DIR:~-1%"=="\" set PS_BASE_DIR=%PS_BASE_DIR:~0,-1%
set SERVER_FILES=%BASE_DIR%QuickSTT_Server\files

echo === QuickSTT Optional Model Packaging ===
if not exist "%SERVER_FILES%" (
    echo [ERROR] Server files folder not found: %SERVER_FILES%
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%BASE_DIR%package_optional_local_runtimes.ps1" -BaseDir "%PS_BASE_DIR%" -ServerFilesDir "%SERVER_FILES%"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Optional model packaging failed.
    exit /b %ERRORLEVEL%
)

echo [SUCCESS] Optional model payloads refreshed.
