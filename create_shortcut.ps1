$WshShell = New-Object -ComObject WScript.Shell
$ShortcutPath = Join-Path ([Environment]::GetFolderPath("Desktop")) "QuickSTT.lnk"
$TargetApp = "C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\QuickSTT_App\QuickSTT.exe"
$Shortcut = $WshShell.CreateShortcut($ShortcutPath)
$Shortcut.TargetPath = $TargetApp
$Shortcut.Arguments = "--background"
$Shortcut.WorkingDirectory = "C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\QuickSTT_App"
$Shortcut.IconLocation = "C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\icon_app.ico"
$Shortcut.Description = "Launch QuickSTT in Background"
$Shortcut.Save()
Write-Host "Shortcut created at $ShortcutPath"
