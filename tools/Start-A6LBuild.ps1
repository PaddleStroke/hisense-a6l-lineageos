# Start the compile probe independently of an interactive tool/terminal session.
# No phone access. Build status remains in WSL ~/logs/build-probe.log.
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$logDirectory = Join-Path $workspace 'logs'
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$pidFile = Join-Path $logDirectory 'build-detached.pid'
if (Test-Path -LiteralPath $pidFile) {
    $previousPid = [int](Get-Content -LiteralPath $pidFile -Raw).Trim()
    $previous = Get-Process -Id $previousPid -ErrorAction SilentlyContinue
    if ($previous -and $previous.ProcessName -eq 'wsl') {
        throw "Recorded WSL build process $previousPid is still running; inspect it before starting another."
    }
}
if ($workspace -ne 'C:\Users\Pierre\Desktop\A6L') {
    throw 'Update the mounted Linux workspace path before moving this launcher.'
}
$process = Start-Process -FilePath wsl.exe -ArgumentList @(
    '-d', 'Ubuntu-24.04', '-u', 'a6l', '--exec', 'bash',
    '/mnt/c/Users/Pierre/Desktop/A6L/tools/build-linux-probe.sh'
) -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $logDirectory 'build-detached.stdout.log') `
    -RedirectStandardError (Join-Path $logDirectory 'build-detached.stderr.log')
$process.Id | Set-Content -LiteralPath $pidFile
Write-Output "Started WSL build process $($process.Id). Logs: $logDirectory"
