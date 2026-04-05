@echo off
setlocal

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Requesting administrator permission to add QuickSTT firewall rules...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

echo Adding QuickSTT server firewall rules...
netsh advfirewall firewall add rule name="QuickSTT Server TCP 5000" dir=in action=allow protocol=TCP localport=5000 >nul
netsh advfirewall firewall add rule name="QuickSTT Server UDP 5001" dir=in action=allow protocol=UDP localport=5001 >nul
echo Done. Inbound TCP 5000 and UDP 5001 are allowed for QuickSTT Server.
pause
