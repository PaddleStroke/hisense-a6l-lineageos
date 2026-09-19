# Direct Linux laptop USB comparison

The spare is currently back in its captured Android 9 firmware, with the same
fingerprint, completed boot, locked bootloader and green verified-boot state.
The laptop test is prepared but has not run. It will determine whether the
alternating fastboot replies also occur without Windows USB forwarding.

## Setup

Use the spare A6L only (serial `1e529013`) and connect it directly to the laptop
with the data cable. Keep its screen unlocked for the laptop's own USB debugging
authorization prompt. The everyday A6L should stay disconnected.

Pierre identified the laptop as Ubuntu 22.04.1 on 2026-09-15. Its running kernel
and USB behavior have not yet been inspected. It needs Python 3, standard `adb`
and `fastboot`, sudo, and kernel usbmon support. In a laptop terminal, install:

```sh
sudo apt update
sudo apt install adb fastboot python3
```

These are Ubuntu's [ADB](https://packages.ubuntu.com/jammy/adb) and
[fastboot](https://packages.ubuntu.com/jammy/fastboot) packages. The runner records
the actual client versions: Jammy's packaged client differs from WSL Ubuntu
24.04's client, so a different result alone would not isolate forwarding as the
sole cause. An Android source tree or build environment is not needed here.

Copy and extract `tools/a6l-linux-usb-test.zip`. It contains only these two
Python scripts and these instructions. The canonical scripts are
`tools/Inspect-A6LLinux.py` and `tools/Inspect-FastbootNative.py`.

For remote setup from the desktop, put both computers on the same local network
and start Ubuntu's [OpenSSH server](https://ubuntu.com/server/docs/how-to/security/openssh-server/):

```sh
sudo apt install openssh-server
sudo systemctl start ssh
whoami
hostname -I
```

SSH enables remote login to the laptop after authentication. Share the username
and local address printed by the last two commands so connection and
authentication can be checked. Enter passwords locally; none are needed in chat.
No firewall or router changes are part of this setup. Alternatively, copy the
kit manually and run the commands below in a laptop terminal.

## Run

From the extracted folder, as your normal user:

```sh
adb devices
python3 Inspect-A6LLinux.py --preflight-only
```

Accept the USB debugging prompt on the spare. If the first command reports
`unauthorized`, accept it and rerun the preflight command. The preflight verifies
the exact serial, Android fingerprint, completed boot and locked/green state.
It prepares and opens the host's passive USB monitor before any phone reboot.
Sudo may request the laptop password for this host setup.

After preflight passes, run:

```sh
python3 Inspect-A6LLinux.py
```

The runner performs the same preflight again, requests the existing bootloader,
runs three fixed `getvar product` queries, and captures only this spare's USB
traffic. If those queries all succeed, it reads five additional fixed variables.
It then requests a normal reboot and checks for the original Android state for
up to 90 seconds. Allow roughly 2–4 minutes including booting and authorization.

The collector retries normal reboot once only after an explicit remote
`unknown command` response, as observed on the desktop. If automatic return is
not verified and the screen stays black, hold only Power for about 20 seconds,
as successfully tested earlier, and stop after Android starts.

There is no unlock, download, flash, erase, arbitrary bootloader-command option,
or image payload in this runner. Root access is used on the laptop for USB
capture, not obtained on the phone. The unused `--padded` collector option belongs
to the earlier WSL experiment and is not used by the laptop runner.

## Results

Each run creates a new timestamped `a6l-linux-*` directory with `session.json`.
A completed bootloader test also records `fastboot/report.json`, `usbmon.txt`,
`usbmon-decoded.json`, and `collector-output.txt`. Keep these reports private;
they include device identifiers and paths. An exit status of 1 can mean the
queries failed even when Android return was verified; check both result fields.

Do not run more experiments automatically. Review this first capture against
the existing WSL results before choosing the next action. A successful small
query test alone is not sufficient evidence that image downloads are reliable.

## Preparation validation

Python syntax checks passed. Four offline fault-injection checks passed: an
unauthorized ADB connection, mismatched firmware, or unavailable USB monitor
stops before any reboot; a valid preflight also leaves Android running.
The Windows-host guard was exercised and rejected execution before any phone
command. These checks validate preparation logic, not physical laptop USB.
The archive builder verifies all three entries against their source files.
