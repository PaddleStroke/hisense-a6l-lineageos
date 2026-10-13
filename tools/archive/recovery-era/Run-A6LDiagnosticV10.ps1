param([Parameter(Mandatory=$true)][ValidateSet('Install','Capture','Restore')][string]$Action)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$accessDir = Join-Path $taskRoot 'logs/laptop-access'
$resultFile = Join-Path $accessDir ('interactive-diagnostic-v10-' + $Action.ToLowerInvariant() + '-result.json')
$knownHosts = (Join-Path $taskRoot 'logs/a6l-laptop-known-hosts').Replace('\', '/')
$keyPath = (Join-Path $accessDir 'a6l_ed25519').Replace('\', '/')
$remoteName = @{Install='Run-LaptopDiagnosticInstall-v10.py';Capture='Run-LaptopProbeCapture-v10.py';Restore='Run-LaptopDiagnosticRestore-v10.py'}[$Action]
$Host.UI.RawUI.WindowTitle = 'A6L - Early LCD console recovery - ' + $Action
Write-Host ('A6L early LCD console recovery: ' + $Action) -ForegroundColor Cyan
Write-Host 'Enter the LAPTOP password if sudo asks. Nothing appears while you type.'
Write-Host 'Keep USB connected. Wait for instructions before using the phone buttons.'
try {
    & 'C:\Windows\System32\OpenSSH\ssh.exe' -tt -i $keyPath -o IdentitiesOnly=yes -o BatchMode=yes `
        -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 `
        -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$knownHosts" `
        pierrelouis@192.168.1.22 python3 ('/home/pierrelouis/A6L-usb-20260915/' + $remoteName)
    $sshResult = $LASTEXITCODE
    [pscustomobject]@{exit=$sshResult;finishedUtc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath $resultFile -Encoding UTF8
    Write-Host 'Session ended. Codex will inspect the saved reports.' -ForegroundColor Cyan
} catch {
    [pscustomobject]@{exit=$null;error=$_.Exception.Message;finishedUtc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath $resultFile -Encoding UTF8
    Write-Host $_.Exception.Message -ForegroundColor Red
}
Read-Host 'Press Enter to close'
