$ErrorActionPreference = "Stop"

$baseDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$freshDir = Join-Path $baseDir "QuickSTT_App_Fresh"
$liveDir = Join-Path $baseDir "QuickSTT_App"
$liveExe = Join-Path $liveDir "QuickSTT_App.exe"
$logPath = Join-Path $baseDir "switch_to_fresh_log.txt"

function Write-Log {
    param([string]$Message)
    $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
    Add-Content -LiteralPath $logPath -Value "[$timestamp] $Message"
}

try {
    Write-Log "Switch helper started."

    if (-not (Test-Path -LiteralPath $freshDir)) {
        throw "Fresh app folder not found: $freshDir"
    }

    Write-Log "Waiting for QuickSTT_App and stt_service to stop..."
    $deadline = (Get-Date).AddMinutes(30)
    while ((Get-Date) -lt $deadline) {
        $running = Get-Process QuickSTT_App, stt_service -ErrorAction SilentlyContinue
        if (-not $running) {
            Write-Log "Processes stopped. Syncing fresh app files."
            break
        }
        Start-Sleep -Seconds 2
    }

    $stillRunning = Get-Process QuickSTT_App, stt_service -ErrorAction SilentlyContinue
    if ($stillRunning) {
        throw "Timed out waiting for the old QuickSTT app to close."
    }

    if (-not (Test-Path -LiteralPath $liveDir)) {
        New-Item -ItemType Directory -Path $liveDir | Out-Null
    }

    & robocopy $freshDir $liveDir /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) {
        throw "robocopy failed with exit code $LASTEXITCODE"
    }

    Write-Log "Sync complete. Launching updated QuickSTT_App."
    Start-Process -FilePath $liveExe -WorkingDirectory $liveDir
    Write-Log "Updated app launched."
} catch {
    Write-Log ("FAILED: " + $_.Exception.Message)
    throw
}
