$ErrorActionPreference = 'Stop'
$workspace = 'C:\Users\Pierre\Desktop\A6L'
$installer = Join-Path $workspace 'tools\usbipd-win_5.3.0_x64.msi'
$resultPath = Join-Path $workspace 'logs\usbipd-setup-result.json'
$result = [ordered]@{ scope = 'Host USB forwarding for the spare bootloader only'; success = $false }
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator rights are required for the USB forwarding driver'
    }
    $expectedHash = '1c984914aec944de19b64eff232421439629699f8138e3ddc29301175bc6d938'
    if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedHash) {
        throw 'Installer hash differs from the inspected official release'
    }
    if ((Get-AuthenticodeSignature -LiteralPath $installer).Status -ne 'Valid') {
        throw 'Installer signature is not valid'
    }
    $guestAddresses = (& wsl.exe -d Ubuntu-24.04 -u a6l -- hostname -I).Trim().Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
    $guestIpv4 = @($guestAddresses | Where-Object { $_ -match '^\d+\.\d+\.\d+\.\d+$' })
    if ($guestIpv4.Count -lt 1) { throw 'Cannot identify WSL IPv4 address' }
    $usbipd = 'C:\Program Files\usbipd-win\usbipd.exe'
    if (Test-Path -LiteralPath $usbipd) {
        $installedVersion = (& $usbipd --version | Out-String).Trim()
        if ($installedVersion -notmatch '^5\.3\.0(?:-|$)') { throw 'Unexpected installed usbipd version' }
        $result.alreadyInstalled = $installedVersion
    } else {
        $installLog = Join-Path $workspace 'logs\usbipd-install.log'
        $process = Start-Process -FilePath 'msiexec.exe' -ArgumentList @('/i', $installer, '/qn', '/norestart', 'REBOOT=ReallySuppress', '/l*v', $installLog) -WindowStyle Hidden -Wait -PassThru
        $result.installExit = $process.ExitCode
        if ($process.ExitCode -notin @(0, 3010)) { throw "Installer returned $($process.ExitCode)" }
    }
    # The default rule permits the local subnet. Limit it to this WSL guest.
    $rules = @(Get-NetFirewallRule | Where-Object { $_.DisplayName -eq 'usbipd' })
    if ($rules.Count -lt 1) { throw 'Cannot locate USB forwarding firewall rule' }
    $rules | Set-NetFirewallRule -RemoteAddress $guestIpv4
    $result.allowedRemoteAddresses = $guestIpv4
    $state = (& $usbipd state | Out-String) | ConvertFrom-Json
    $devices = @($state.Devices | Where-Object { $_.InstanceId -eq 'USB\VID_18D1&PID_D00D\1E529013' })
    if ($devices.Count -ne 1) { throw 'Expected exactly the spare A6L bootloader interface' }
    $result.busId = $devices[0].BusId
    # This interface has no Windows function driver (problem 28).
    $bindProcess = Start-Process -FilePath $usbipd -ArgumentList @('bind', '--force', '--busid', $result.busId) -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $workspace 'logs\usbipd-bind.log') -RedirectStandardError (Join-Path $workspace 'logs\usbipd-bind.stderr.log')
    $result.bindExit = $bindProcess.ExitCode
    if ($bindProcess.ExitCode -ne 0) { throw "USB bind returned $($bindProcess.ExitCode)" }
    $result.success = $true
} catch {
    $result.error = $_.Exception.Message
} finally {
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath
}
if (-not $result.success) { exit 1 }
