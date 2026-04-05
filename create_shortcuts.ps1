# Create Start Menu + Desktop shortcuts with proper ICO icons
$WshShell = New-Object -ComObject WScript.Shell
$startMenu = [System.Environment]::GetFolderPath("StartMenu") + "\Programs"
$desktop = [System.Environment]::GetFolderPath("Desktop")

$appExe = (Resolve-Path "QuickSTT_App\QuickSTT_App.exe").Path
$appDir = Split-Path $appExe
$appIco = (Resolve-Path "QuickSTT_App\app.ico").Path

$srvExe = (Resolve-Path "QuickSTT_Server\QuickSTT_Server_App.exe").Path
$srvDir = Split-Path $srvExe
$srvIco = (Resolve-Path "QuickSTT_Server\server.ico").Path

foreach ($loc in @($startMenu, $desktop)) {
    $s = $WshShell.CreateShortcut("$loc\QuickSTT.lnk")
    $s.TargetPath = $appExe
    $s.WorkingDirectory = $appDir
    $s.Description = "QuickSTT Speech to Text"
    $s.IconLocation = "$appIco,0"
    $s.Save()
    Write-Host "OK: $loc\QuickSTT.lnk"

    $s2 = $WshShell.CreateShortcut("$loc\QuickSTT Server.lnk")
    $s2.TargetPath = $srvExe
    $s2.WorkingDirectory = $srvDir
    $s2.Description = "QuickSTT Update Server"
    $s2.IconLocation = "$srvIco,0"
    $s2.Save()
    Write-Host "OK: $loc\QuickSTT Server.lnk"
}
Write-Host "All shortcuts with ICO icons created"
