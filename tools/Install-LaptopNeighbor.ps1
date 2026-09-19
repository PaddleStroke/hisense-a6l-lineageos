$ErrorActionPreference = 'Stop'
$root = 'C:\Users\Pierre\Desktop\A6L'
$logPath = Join-Path $root 'logs\laptop-neighbor-install.json'
$laptopIP = '192.168.1.22'
$laptopMac = '10-6F-D9-D1-97-33'
$taskState = @{ started = [DateTime]::UtcNow.ToString('o'); installed = $false; interfaceIndex = 4; ip = $laptopIP; mac = $laptopMac }
function Save-TaskState { $taskState | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $logPath -Encoding utf8 }
if ($env:COMPUTERNAME -ne 'DESKTOP-3HCGN2H') { throw 'Wrong desktop' }
if (Test-Path -LiteralPath $logPath) { throw 'Inspect the existing installation report instead of rerunning' }
Save-TaskState
$attempted = $false
try {
    $adapter = Get-NetAdapter -InterfaceIndex 4
    if ($adapter.Name -ne 'Wi-Fi' -or $adapter.MacAddress -ne '60-FF-9E-51-92-24') { throw 'Adapter identity differs' }
    if ((Get-NetConnectionProfile -InterfaceIndex 4).Name -ne 'Livebox-65F0') { throw 'Not on the expected home network' }
    if ((Get-NetIPAddress -InterfaceIndex 4 -AddressFamily IPv4).IPAddress -ne '192.168.1.11') { throw 'Desktop address differs' }
    $taskState.prior = @{}
    foreach ($store in @('ActiveStore','PersistentStore')) {
        $old = @(Get-NetNeighbor -InterfaceIndex 4 -IPAddress $laptopIP -PolicyStore $store -ErrorAction SilentlyContinue)
        $taskState.prior[$store] = @($old | Select-Object IPAddress,LinkLayerAddress,State)
        foreach ($entry in $old) {
            if ($entry.State -eq 'Permanent' -or $entry.LinkLayerAddress -notin @('00-00-00-00-00-00',$laptopMac)) { throw 'Unexpected existing mapping; refusing to replace it' }
        }
    }
    Save-TaskState
    if ($taskState.prior.ActiveStore.Count) {
        Remove-NetNeighbor -InterfaceIndex 4 -IPAddress $laptopIP -PolicyStore ActiveStore -Confirm:$false
    }
    $attempted = $true
    # Omitting PolicyStore creates both active and persistent entries.
    New-NetNeighbor -InterfaceIndex 4 -IPAddress $laptopIP -LinkLayerAddress $laptopMac -State Permanent | Out-Null
    $taskState.verified = @{}
    foreach ($store in @('ActiveStore','PersistentStore')) {
        $entries = @(Get-NetNeighbor -InterfaceIndex 4 -IPAddress $laptopIP -PolicyStore $store)
        if ($entries.Count -ne 1 -or $entries[0].State -ne 'Permanent' -or $entries[0].LinkLayerAddress -ne $laptopMac) { throw "Mapping verification failed: $store" }
        $taskState.verified[$store] = $entries | Select-Object IPAddress,LinkLayerAddress,State
    }
    $taskState.installed = $true
} catch {
    $taskState.error = $_.Exception.Message
    if ($attempted) {
        foreach ($store in @('PersistentStore','ActiveStore')) {
            $entry = Get-NetNeighbor -InterfaceIndex 4 -IPAddress $laptopIP -PolicyStore $store -ErrorAction SilentlyContinue
            if ($entry.State -eq 'Permanent' -and $entry.LinkLayerAddress -eq $laptopMac) {
                Remove-NetNeighbor -InterfaceIndex 4 -IPAddress $laptopIP -PolicyStore $store -Confirm:$false
            }
        }
        $taskState.failed_install_removed = $true
    }
} finally {
    $taskState.finished = [DateTime]::UtcNow.ToString('o')
    Save-TaskState
}
