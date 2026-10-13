$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$accessDir = Join-Path $taskRoot 'logs/laptop-access'
$resultFile = Join-Path $accessDir 'interactive-login-result.json'
$sshPath = 'C:\Windows\System32\OpenSSH\ssh.exe'
$knownHosts = (Join-Path $taskRoot 'logs/a6l-laptop-known-hosts').Replace('\', '/')
$Host.UI.RawUI.WindowTitle = 'A6L - Ubuntu laptop sign-in'
Write-Host 'Sign in to your Ubuntu laptop: pierrelouis@192.168.1.22' -ForegroundColor Cyan
Write-Host 'Enter the LAPTOP password below. Nothing appears while you type.'
Write-Host 'This adds the dedicated A6L access key; it does not operate the phone.'
Write-Host ''
try {
    $remoteCommand = Get-Content -LiteralPath (Join-Path $accessDir 'remote-command.txt') -Raw
    & $sshPath -o ConnectTimeout=10 -o StrictHostKeyChecking=yes `
        -o "UserKnownHostsFile=$knownHosts" -o PreferredAuthentications=keyboard-interactive,password `
        -o PubkeyAuthentication=no -o NumberOfPasswordPrompts=2 `
        pierrelouis@192.168.1.22 $remoteCommand
    $sshResult = $LASTEXITCODE
    [pscustomobject]@{ exit=$sshResult; finishedUtc=[DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $resultFile -Encoding UTF8
    if ($sshResult -eq 0) {
        Write-Host 'Laptop access is ready. You can close this window.' -ForegroundColor Green
    } else {
        Write-Host 'Sign-in did not complete. Tell me the error shown above.' -ForegroundColor Yellow
    }
} catch {
    [pscustomobject]@{ exit=$null; error=$_.Exception.Message; finishedUtc=[DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $resultFile -Encoding UTF8
    Write-Host $_.Exception.Message -ForegroundColor Red
}
Read-Host 'Press Enter to close'
