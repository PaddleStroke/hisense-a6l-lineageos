#!/system/bin/sh
# Run the genuine Android loader chain only inside the diskless VM.
set -eu
/system/bin/a6l-guard.sh || exit 70
echo A6L_BPF_LOADER_BEGIN
mount -t bpf -o nodev,noexec,nosuid bpf /sys/fs/bpf
ulimit -l 1048576
old_kptr=$(cat /proc/sys/kernel/kptr_restrict)
trap 'echo "$old_kptr" > /proc/sys/kernel/kptr_restrict' EXIT
echo 1 > /proc/sys/kernel/kptr_restrict
# Match init's `file /dev/kmsg w`; the Rust platform loader consumes this FD.
exec 3>/dev/kmsg
export ANDROID_FILE__dev_kmsg=3
if timeout --foreground -k 3 35 /apex/com.android.tethering/bin/netbpfload > /logs/bpf-loader.log 2>&1; then
    getprop bpf.progs_loaded > /logs/bpf-ready.txt
    if grep -qx 1 /logs/bpf-ready.txt; then
        echo A6L_BPF_LOADER_PASS
        exit 0
    fi
else
    result=$?
    echo A6L_BPF_LOADER_FAILED result=$result
fi
exit 71
