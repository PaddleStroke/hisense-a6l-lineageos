#!/system/bin/sh
# A6L rom-v1 debug (androidboot.a6l_logcat=1): stream logcat W/E (main, system, crash) into the kernel log.
# One kmsg record per line, cut to 900 chars: /dev/kmsg rejects writes > 992 bytes with EINVAL, which killed a plain
# `logcat` service on every crash dump ("Output error: Invalid argument") and hid the crash text (agent gnss, QEMU r5/r6).
/system/bin/logcat -b main,system,crash -v brief -T 1 '*:W' | while IFS= read -r l; do
    print -r -- "${l:0:900}"
done
