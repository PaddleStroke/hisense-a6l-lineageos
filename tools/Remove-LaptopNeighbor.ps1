$ErrorActionPreference = 'Stop'
$root = 'C:\Users\Pierre\Desktop\A6L'
if ($env:COMPUTERNAME -ne 'DESKTOP-3HCGN2H') { throw 'Wrong desktop' }
$adapter = Get-NetAdapter -InterfaceIndex 4
if ($adapter.MacAddress -ne '60-FF-9E-51-92-24') { throw 'Adapter identity differs' }
$install = Get-Content -LiteralPath (Join-Path $root 'logs\laptop-neighbor-install.json') -Raw | ConvertFrom-Json
if (-not $install.installed) { throw 'No successful installation recorded' }
foreach ($store in @('PersistentStore','ActiveStore')) {
    $entry = Get-NetNeighbor -InterfaceIndex 4 -IPAddress '192.168.1.22' -PolicyStore $store -ErrorAction SilentlyContinue
    if ($entry) {
        if ($entry.State -ne 'Permanent' -or $entry.LinkLayerAddress -ne '10-6F-D9-D1-97-33') { throw 'Mapping changed; refusing to remove unrelated entry' }
        Remove-NetNeighbor -InterfaceIndex 4 -IPAddress '192.168.1.22' -PolicyStore $store -Confirm:$false
    }
}
@{ removed = $true; utc = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'logs\laptop-neighbor-removed.json') -Encoding utf8
