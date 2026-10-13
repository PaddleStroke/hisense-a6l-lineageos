$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$keyPath = (Join-Path $taskRoot 'logs/laptop-access/a6l_ed25519').Replace('\', '/')
$knownHosts = (Join-Path $taskRoot 'logs/a6l-laptop-known-hosts').Replace('\', '/')
Write-Host 'A6L: restart the read-only logger. Leave the phone in fastboot.' -ForegroundColor Cyan
& 'C:\Windows\System32\OpenSSH\ssh.exe' -tt -i $keyPath -o IdentitiesOnly=yes -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$knownHosts" pierrelouis@192.168.1.22 python3 /home/pierrelouis/A6L-usb-20260915/Rearm-LaptopProbeCapture-v11.py
[pscustomobject]@{exit=$LASTEXITCODE;finishedUtc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskRoot 'logs/laptop-access/interactive-diagnostic-v11-rearm-result.json') -Encoding UTF8
Read-Host 'Press Enter to close'
