$ErrorActionPreference = 'Stop'
$taskRoot = 'C:\Users\Pierre\Desktop\A6L'
$statusPath = Join-Path $taskRoot 'logs\temporary-laptop-neighbor.json'
$stopPath = Join-Path $taskRoot 'logs\temporary-laptop-neighbor.stop'
$taskState = @{ started = [DateTime]::UtcNow.ToString('o'); installed = $false; removed = $false }
function Save-TaskState { $taskState | ConvertTo-Json | Set-Content -LiteralPath $statusPath -Encoding utf8 }
if ($env:COMPUTERNAME -ne 'DESKTOP-3HCGN2H') { throw 'Wrong desktop' }
if (Test-Path -LiteralPath $statusPath) { throw 'One-shot helper already used' }
if (Test-Path -LiteralPath $stopPath) { throw 'Stop marker already exists' }
Save-TaskState
$changed = $false
try {
    $adapter = Get-NetAdapter -InterfaceIndex 4
    if ($adapter.Name -ne 'Wi-Fi' -or $adapter.MacAddress -ne '60-FF-9E-51-92-24') { throw 'Adapter identity differs' }
    $address = Get-NetIPAddress -InterfaceIndex 4 -AddressFamily IPv4
    if ($address.IPAddress -ne '192.168.1.11') { throw 'Desktop address differs' }
    $old = Get-NetNeighbor -InterfaceIndex 4 -IPAddress '192.168.1.22' -ErrorAction SilentlyContinue
    if ($old -and ($old.State -eq 'Permanent' -or $old.LinkLayerAddress -notin @('00-00-00-00-00-00','10-6F-D9-D1-97-33'))) { throw 'Unexpected prior neighbor' }
    $taskState.prior = if ($old) { @{ state = [string]$old.State; mac = $old.LinkLayerAddress } } else { $null }
    if ($old) {
        Set-NetNeighbor -InterfaceIndex 4 -IPAddress '192.168.1.22' -LinkLayerAddress '10-6F-D9-D1-97-33' -State Permanent -PolicyStore ActiveStore -Confirm:$false
    } else {
        New-NetNeighbor -InterfaceIndex 4 -IPAddress '192.168.1.22' -LinkLayerAddress '10-6F-D9-D1-97-33' -State Permanent -PolicyStore ActiveStore | Out-Null
    }
    $changed = $true
    $taskState.installed = $true
    Save-TaskState
    $deadline = [DateTime]::UtcNow.AddMinutes(5)
    while ([DateTime]::UtcNow -lt $deadline -and -not (Test-Path -LiteralPath $stopPath)) { Start-Sleep -Seconds 1 }
} catch {
    $taskState.error = $_.Exception.Message
} finally {
    if ($changed) {
        $current = Get-NetNeighbor -InterfaceIndex 4 -IPAddress '192.168.1.22' -ErrorAction SilentlyContinue
        if ($current.State -eq 'Permanent' -and $current.LinkLayerAddress -eq '10-6F-D9-D1-97-33') {
            Remove-NetNeighbor -InterfaceIndex 4 -IPAddress '192.168.1.22' -Confirm:$false
            $taskState.removed = $true
        } else { $taskState.cleanup_error = 'Neighbor changed; not removing someone else''s entry' }
    }
    $taskState.finished = [DateTime]::UtcNow.ToString('o')
    Save-TaskState
}
