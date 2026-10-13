$Host.UI.RawUI.WindowTitle = 'A6L - one-time Ubuntu setup'
Write-Host 'One-time A6L USB and scoped sudo setup' -ForegroundColor Cyan
Write-Host 'Enter the Ubuntu laptop password only when sudo asks below.'
Write-Host 'Password characters will not appear. No password is saved or logged.'
Write-Host ''
& C:\Windows\System32\OpenSSH\ssh.exe -tt -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf a6l-laptop 'sudo /usr/bin/python3 -I /home/pierrelouis/A6L-usb-20260915/host-access-v1/Install-A6LHostAccess.py'
$taskExit = $LASTEXITCODE
Write-Host ''
Write-Host "Setup command finished with exit code $taskExit. Codex will verify the result."
Read-Host 'Press Enter to close this window'
