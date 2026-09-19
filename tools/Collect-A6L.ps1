#Requires -Version 7.0
<#
Stock A6L diagnostic collection. Reads only; no root request, reboot, flashing,
APK install, settings modification, or sysfs writes. This is NOT a ROM backup.
Captures may contain identifiers; keep the output private.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Serial,
    [string]$AdbPath = (Join-Path $PSScriptRoot 'platform-tools/adb.exe'),
    [string]$OutputRoot = (Join-Path $PSScriptRoot '../captures'),
    [ValidateRange(1,120)][int]$TimeoutSeconds = 25
)
$ErrorActionPreference = 'Stop'
$AdbPath = (Resolve-Path -LiteralPath $AdbPath).Path

function Invoke-AdbRead {
    param([string[]]$Arguments)
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $AdbPath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            $process.WaitForExit()
            return [pscustomobject]@{ ExitCode = -1; Text = 'TIMED OUT'; Error = 'ADB read timed out' }
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Text = $stdout.GetAwaiter().GetResult()
            Error = $stderr.GetAwaiter().GetResult()
        }
    } finally { $process.Dispose() }
}

$devices = Invoke-AdbRead -Arguments @('devices', '-l')
if ($devices.ExitCode -ne 0) { throw "ADB enumeration failed: $($devices.Error)" }
$rows = @($devices.Text -split '\r?\n' | Where-Object { $_ -match '^\S+\s+(device|offline|unauthorized)\b' })
if ($rows.Count -ne 1 -or $rows[0] -notmatch ('^' + [regex]::Escape($Serial) + '\s+device\b')) {
    throw 'Connect only the spare phone, authorize USB debugging, and supply its exact ADB serial.'
}
$model = Invoke-AdbRead -Arguments @('-s', $Serial, 'shell', 'getprop', 'ro.product.model')
$device = Invoke-AdbRead -Arguments @('-s', $Serial, 'shell', 'getprop', 'ro.product.device')
if ($model.ExitCode -ne 0 -or $device.ExitCode -ne 0 -or
    ($model.Text + ' ' + $device.Text) -notmatch '(?i)\b(A6L|HLTE730T(?:\.[A-Z0-9]+)?)\b') {
    throw 'Device does not identify as A6L/HLTE730T. Inspect identity manually before continuing.'
}

$output = Join-Path $OutputRoot ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
[void](New-Item -ItemType Directory -Path $output)
$properties = @(
    'ro.product.model','ro.product.device','ro.product.board','ro.product.name',
    'ro.product.manufacturer','ro.hardware','ro.board.platform','ro.build.fingerprint',
    'ro.build.display.id','ro.build.version.release','ro.build.version.sdk',
    'ro.build.version.security_patch','ro.vendor.build.fingerprint',
    'ro.vendor.build.security_patch','ro.vndk.version','ro.treble.enabled',
    'ro.build.ab_update','ro.boot.slot_suffix','ro.build.system_root_image',
    'ro.boot.verifiedbootstate','ro.boot.flash.locked','ro.boot.vbmeta.device_state',
    'ro.crypto.state','ro.crypto.type','ro.product.cpu.abilist'
)
$propertyLines = [System.Collections.Generic.List[string]]::new()
foreach ($property in $properties) {
    $result = Invoke-AdbRead -Arguments @('-s', $Serial, 'shell', 'getprop', $property)
    $propertyLines.Add("[$property] [$($result.Text.Trim())] exit=$($result.ExitCode) $($result.Error.Trim())")
}
$propertyLines | Set-Content -LiteralPath (Join-Path $output 'properties.txt') -Encoding utf8

# Fixed, reviewed read-only shell commands. No user input is interpolated here.
$reads = [ordered]@{
    'kernel' = 'uname -a'
    'kernel-command-line' = 'cat /proc/cmdline'
    'partitions' = 'cat /proc/partitions'
    'mounts' = 'cat /proc/mounts'
    'partition-links' = 'ls -l /dev/block/by-name /dev/block/bootdevice/by-name'
    'input-devices' = 'cat /proc/bus/input/devices'
    'display' = 'dumpsys display'
    'surfaceflinger' = 'dumpsys SurfaceFlinger'
    'input-routing' = 'dumpsys input'
    'battery' = 'dumpsys battery'
    'services' = 'service list'
    'hal-services' = 'lshal'
    'vintf' = 'cat /vendor/manifest.xml /vendor/etc/vintf/manifest.xml /system/etc/vintf/manifest.xml'
    'fstab' = 'cat /vendor/etc/fstab* /fstab.*'
    'display-node-names' = 'ls -l /sys/class/graphics /sys/class/graphics/fb0/ /sys/class/graphics/fb1/ /sys/ctp1/ctp_func/ /sys/class/leds/'
    'selinux' = 'getenforce'
    'logcat-recent' = 'logcat -b all -d -t 2000'
    'kernel-log' = 'dmesg'
    'pstore' = 'ls -l /sys/fs/pstore; cat /sys/fs/pstore/*'
    'wifi-service' = 'dumpsys wifi'
    'connectivity-service' = 'dumpsys connectivity'
    'power-service' = 'dumpsys power'
    'eink-service' = 'dumpsys epd'
}
$summary = @()
foreach ($entry in $reads.GetEnumerator()) {
    Write-Host "Reading $($entry.Key)..."
    $result = Invoke-AdbRead -Arguments @('-s', $Serial, 'shell', $entry.Value)
    $result.Text | Set-Content -LiteralPath (Join-Path $output "$($entry.Key).txt") -Encoding utf8
    $result.Error | Set-Content -LiteralPath (Join-Path $output "$($entry.Key).stderr.txt") -Encoding utf8
    $summary += [pscustomobject]@{ Name = $entry.Key; ExitCode = $result.ExitCode; Command = $entry.Value }
}
$summary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'commands.json') -Encoding utf8
$captureFiles = @(Get-ChildItem -LiteralPath $output -File | Where-Object Name -ne 'SHA256.csv')
$captureFiles | Get-FileHash -Algorithm SHA256 |
    Select-Object @{Name='File';Expression={Split-Path $_.Path -Leaf}},Hash |
    Export-Csv -LiteralPath (Join-Path $output 'SHA256.csv') -NoTypeInformation -Encoding utf8
Write-Host "Saved diagnostics to $((Resolve-Path -LiteralPath $output).Path)"
Write-Host 'This is not a firmware backup. No phone modifications were requested.'
