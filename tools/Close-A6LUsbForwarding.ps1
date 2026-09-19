$ErrorActionPreference = 'Stop'
$workspace = 'C:\Users\Pierre\Desktop\A6L'
$result = [ordered]@{ scope = 'Remove only the spare bootloader USB sharing'; success = $false }
try {
    $usbipd = 'C:\Program Files\usbipd-win\usbipd.exe'
    $state = (& $usbipd state | Out-String) | ConvertFrom-Json
    $devices = @($state.Devices | Where-Object { $_.InstanceId -eq 'USB\VID_18D1&PID_D00D\1E529013' })
    if ($devices.Count -ne 1 -or -not $devices[0].PersistedGuid) {
        throw 'Expected exactly one persisted spare bootloader binding'
    }
    $result.bindingGuid = $devices[0].PersistedGuid
    $unbind = Start-Process -FilePath $usbipd -ArgumentList @('unbind', '--guid', $result.bindingGuid) -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput (Join-Path $workspace 'logs\usbipd-unbind.log') -RedirectStandardError (Join-Path $workspace 'logs\usbipd-unbind.stderr.log')
    $result.unbindExit = $unbind.ExitCode
    if ($unbind.ExitCode -ne 0) { throw "USB unbind returned $($unbind.ExitCode)" }
    $updated = (& $usbipd state | Out-String) | ConvertFrom-Json
    $remaining = @($updated.Devices | Where-Object { $_.InstanceId -eq 'USB\VID_18D1&PID_D00D\1E529013' -and ($_.PersistedGuid -or $_.ClientIPAddress -or $_.IsForced) })
    if ($remaining.Count) { throw 'Spare bootloader binding still present' }
    $rules = @(Get-NetFirewallRule | Where-Object { $_.DisplayName -eq 'usbipd' })
    $result.firewallRemoteAddresses = @($rules | Get-NetFirewallAddressFilter | ForEach-Object RemoteAddress)
    $result.success = $true
} catch {
    $result.error = $_.Exception.Message
} finally {
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $workspace 'logs\usbipd-cleanup-result.json')
}
if (-not $result.success) { exit 1 }
