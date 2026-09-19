param([switch]$PauseFwupd, [switch]$GoogleClient, [switch]$NoLpm, [switch]$RamTransfer, [switch]$VendorQuery, [switch]$VendorUnlock, [switch]$RecoveryAccess, [switch]$ButtonEntry, [switch]$RecoveryRestore, [switch]$RecoveryPoweroff, [switch]$DiagnosticInstall, [switch]$DiagnosticRestore, [switch]$ProbeCapture)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$accessDir = Join-Path $taskRoot 'logs/laptop-access'
$resultName = if ($DiagnosticInstall) { 'interactive-diagnostic-install-v2-result.json' } elseif ($DiagnosticRestore) { 'interactive-diagnostic-restore-v2-result.json' } elseif ($ProbeCapture) { 'interactive-probe-capture-v2-result.json' } elseif ($RecoveryPoweroff) { 'interactive-recovery-poweroff-v1-result.json' } elseif ($RecoveryRestore) { 'interactive-recovery-restore-v1-result.json' } elseif ($ButtonEntry) { 'interactive-button-entry-v2-result.json' } elseif ($RecoveryAccess) { 'interactive-recovery-access-v4-result.json' } elseif ($VendorUnlock) { 'interactive-vendor-unlock-result.json' } elseif ($VendorQuery) { 'interactive-vendor-query-result.json' } elseif ($RamTransfer) { 'interactive-ram-transfer-result.json' } elseif ($NoLpm) { 'interactive-no-lpm-test-result.json' } elseif ($GoogleClient) { 'interactive-google-test-result.json' } elseif ($PauseFwupd) { 'interactive-quiet-test-result.json' } else { 'interactive-test-result.json' }
$resultFile = Join-Path $accessDir $resultName
$sshPath = 'C:\Windows\System32\OpenSSH\ssh.exe'
$knownHosts = (Join-Path $taskRoot 'logs/a6l-laptop-known-hosts').Replace('\', '/')
$keyPath = (Join-Path $accessDir 'a6l_ed25519').Replace('\', '/')
$Host.UI.RawUI.WindowTitle = 'A6L - Direct Linux USB test'
Write-Host 'Direct USB test on the Ubuntu laptop' -ForegroundColor Cyan
Write-Host 'Enter the LAPTOP password if sudo asks. Nothing appears while you type.'
if ($DiagnosticInstall) {
    $Host.UI.RawUI.WindowTitle = 'A6L - Diagnostic recovery installation'
    Write-Host 'Installing the fixed diagnostic recovery after exact spare, GPT, stock and empty boot-message checks.'
    Write-Host 'The complete recovery is read back before poweroff. Keep USB connected until completion.'
} elseif ($DiagnosticRestore) {
    $Host.UI.RawUI.WindowTitle = 'A6L - Restore stock recovery'
    Write-Host 'Restoring the verified stock recovery and checking all bytes before normal reboot.'
    Write-Host 'Keep USB connected throughout the operation.'
} elseif ($ProbeCapture) {
    $Host.UI.RawUI.WindowTitle = 'A6L - Diagnostic USB log capture'
    Write-Host 'Only host-side serial log capture. Wait for READY before the physical boot procedure.'
} elseif ($RecoveryPoweroff) {
    $Host.UI.RawUI.WindowTitle = 'A6L - Read checks and power off'
    Write-Host 'This reads the fixed stock regions, requires an empty recovery request, and powers off.'
    Write-Host 'No partition or image is written. Keep USB connected until the session finishes.'
} elseif ($RecoveryRestore) {
    $Host.UI.RawUI.WindowTitle = 'A6L - Stock recovery write and readback'
    Write-Host 'This writes the SAME verified stock recovery to its existing partition, then reads it back.'
    Write-Host 'It first verifies the exact spare, GPT and existing stock recovery. No diagnostic image is installed.'
    Write-Host 'Keep USB connected throughout the operation. Idle probing services are paused temporarily.'
} elseif ($ButtonEntry) {
    $Host.UI.RawUI.WindowTitle = 'A6L - Volume Up and USB test'
    Write-Host 'This checks entry with Volume Up and USB while the phone is powered off.'
    Write-Host 'Wait for READY before unplugging the phone. No image is written; the test queries state and reboots.'
    Write-Host 'fwupd and the bootloader USB quirk are adjusted temporarily and restored afterward.'
} elseif ($RecoveryAccess) {
    Write-Host 'This reads the recovery partition and fixed verification regions in emergency mode.'
    Write-Host 'No image or partition will be written. Keep the spare within reach for a restart if needed.'
    Write-Host 'Idle firmware/modem services will be paused temporarily and restored after the phone returns.'
} elseif ($VendorUnlock) {
    Write-Host 'APPROVED VENDOR UNLOCK: both unlock flags and the custom AVB key will change.' -ForegroundColor Yellow
    Write-Host 'Treat data on the spare as erased. No kernel or recovery image will be flashed in this phase.'
} elseif ($RamTransfer) {
    Write-Host 'Keep the spare beside you. This stages a 1 MiB sample and 64 MiB diagnostic image in RAM only.'
    Write-Host 'No flash, erase, unlock or image-execution command is issued.'
} else { Write-Host 'Keep the spare beside you. No image is downloaded or flashed.' }
if ($PauseFwupd -or $GoogleClient -or $NoLpm -or $RamTransfer -or $VendorQuery -or $VendorUnlock) { Write-Host 'The idle firmware-management service will be paused for this test and restored afterward.' }
if ($NoLpm -or $RamTransfer -or $VendorQuery -or $VendorUnlock) { Write-Host 'This test temporarily disables USB link power management for the fastboot USB identity, then restores the setting.' }
if ($VendorQuery) { Write-Host 'Read-only preflight of the source-built client; no unlock command will be sent.' }
if ($GoogleClient) { Write-Host 'This comparison uses Google fastboot 37.0.1 and captures the initial USB setup.' }
Write-Host ''
try {
    $remoteArgs = if ($DiagnosticInstall) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopDiagnosticInstall-v2.py')
    } elseif ($DiagnosticRestore) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopDiagnosticRestore-v2.py')
    } elseif ($ProbeCapture) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopProbeCapture-v2.py')
    } elseif ($RecoveryPoweroff) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopRecoveryPoweroff-v1.py')
    } elseif ($RecoveryRestore) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopRecoveryRestore-v1.py')
    } elseif ($ButtonEntry) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopButtonEntry-v2.py')
    } elseif ($RecoveryAccess) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopRecoveryAccess-v4.py')
    } elseif ($VendorUnlock) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopQuietUsb-v5.py', '--vendor-unlock')
    } elseif ($VendorQuery) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopQuietUsb-v5.py', '--vendor-query')
    } elseif ($RamTransfer) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopQuietUsb-v4.py', '--ram-transfer')
    } elseif ($NoLpm) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopQuietUsb-v3.py', '--no-lpm')
    } elseif ($GoogleClient) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopQuietUsb-v2.py', '--google-client')
    } elseif ($PauseFwupd) {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Run-LaptopQuietUsb.py')
    } else {
        @('python3', '/home/pierrelouis/A6L-usb-20260915/Inspect-A6LLinux.py', '--output', '/home/pierrelouis/A6L-usb-20260915/capture-native')
    }
    & $sshPath -tt -i $keyPath -o IdentitiesOnly=yes -o BatchMode=yes `
        -o ConnectTimeout=10 -o ServerAliveInterval=10 -o ServerAliveCountMax=3 `
        -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$knownHosts" `
        pierrelouis@192.168.1.22 @remoteArgs
    $sshResult = $LASTEXITCODE
    [pscustomobject]@{ exit=$sshResult; finishedUtc=[DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $resultFile -Encoding UTF8
    Write-Host 'The test session has ended. I will inspect the saved reports.' -ForegroundColor Cyan
} catch {
    [pscustomobject]@{ exit=$null; error=$_.Exception.Message; finishedUtc=[DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $resultFile -Encoding UTF8
    Write-Host $_.Exception.Message -ForegroundColor Red
}
Read-Host 'Press Enter to close'
