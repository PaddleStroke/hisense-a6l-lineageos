# Laptop SSH reliability

## Decision

Use the known working Linux USB transport for further A6L recovery writes.
Native Windows EDL reads passed, but its programming attempt transferred only
the first 1 MiB into recovery before failing. Linux subsequently restored the
full stock image; all twelve copied readbacks passed independent verification.
Stock Android is running and the boot-message area is zero. The Windows
writer remains disabled.

## Observed network failure

The Windows desktop could reach its router, and the laptop could ping the
desktop, but Windows repeatedly showed an unresolved/unreachable neighbor for
the laptop. SSH timed out from both Windows and the desktop's WSL environment.
Supplying the previously observed laptop MAC in a temporary neighbor entry
immediately restored SSH and allowed all restore evidence to be copied.

The laptop independently confirmed its IP, route to the desktop, MAC and
normal ARP sysctl settings. Its interface address assignment type is zero.
Why automatic address resolution fails is still unproven; this is a targeted
workaround rather than a claim that the Wi-Fi/router defect is fixed.

## Installed workaround

On the desktop's Wi-Fi adapter, one permanent neighbor entry maps
`192.168.1.22` to `10-6F-D9-D1-97-33`. The installer verifies the desktop,
adapter MAC and home network before changing anything. It records both the
previous state and successful ActiveStore/PersistentStore readbacks in
`logs/laptop-neighbor-install.json`. No previous permanent entry was replaced.

Microsoft documents that omitting PolicyStore when creating the neighbor
creates active and persistent entries; the persistent entry is loaded across
restarts. Both stores were checked after installation, but a desktop reboot
has not been tested. [Microsoft New-NetNeighbor documentation](https://learn.microsoft.com/en-us/powershell/module/nettcpip/new-netneighbor?view=windowsserver2022-ps).

This mapping depends on the laptop retaining that IP and MAC. The laptop still
uses DHCP; no router reservation or static laptop address was configured.
If its address changes, inspect and update/remove the mapping rather than
turning off SSH host-key checks. This desktop adapter should keep using the
home network while the mapping is installed.

The project-local SSH configuration at `tools/a6l-laptop-ssh.conf` defines
alias `a6l-laptop`, the existing key, strict known-host verification, bounded
connection attempts and keepalives. It does not alter global SSH settings,
firewall rules, credentials or sudo policy. Use:

```powershell
ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf a6l-laptop hostname
```

## Removal

Run `tools/Remove-LaptopNeighbor.ps1` in an elevated PowerShell. It checks the
adapter and exact mapping, then removes only this entry from both stores.
It refuses to delete a mapping whose MAC has changed. Dynamic address
resolution resumes afterward. The earlier temporary helper has already
completed and removed its entry; it will not remove this new installation.

## Verification

`tools/Check-LaptopConnection.py` records seven fresh SSH connections across
three minutes and one quiet, simultaneous three-minute SSH session with
keepalives. Results are stored in `logs/laptop-connection-stability.json`.
Only hostname/read-only network queries are sent; no phone reboot or flash
is part of this check. Consult the final report's `passed` value and finish
timestamp before describing the stability test as complete.

Final result: PASS at 09:09:55 UTC. All seven fresh connections succeeded
over 186 seconds, and the simultaneous three-minute idle session completed
without reconnecting. Fresh Android properties were also verified. This is
a bounded stability check, not proof against future router or DHCP changes.
