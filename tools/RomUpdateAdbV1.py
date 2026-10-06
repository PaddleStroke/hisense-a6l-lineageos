"""A6L update transport over adb in the V74 DIAGNOSTIC RECOVERY (agent update-keepdata, 29 Sep 2026). ATTENDED ONLY.

Why: EDL entry from LineageOS is unproven (our kernel has no `reboot edl`, flash-20260924 §5), while `adb reboot recovery`
/ fastboot menu -> Recovery boots the V74 diagnostic recovery, which has adbd (serial HLTE730T-PROBE), toybox and
/sdhci-msm.ko (eMMC = mmcblk1 on that kernel, no ueventd: block nodes must be created by hand).

Same device interface as RomFlashEngineV1.FileDevice / the Firehose device (read / program / power), so the update engine
and all its checks are identical. Extra guards here:
  * the eMMC disk is the one whose partitions carry every layout PARTNAME; each partition's sysfs start/size must equal
    the layout (= the verified 14 Sep GPT) before any node is created; nodes are made in /dev/block/a6lupd from the
    verified major:minor only;
  * reads map to one partition node (or the whole disk only for the GPT regions); programs must START at a partition start,
    pass RomUpdateLayoutV1.check_update_plan (boot/dtbo/vendor/system only) and are written through that partition's
    node - a whole-disk node is never opened for writing;
  * programs go in chunks of <= 256 MiB: `adb push` of the chunk to /tmp (adb's sync protocol, checksummed per packet), the
    chunk's sha256 recomputed ON THE PHONE and compared, then `toybox dd if=<chunk> of=<partition node> seek=<MiB offset>
    conv=notrunc` and the written range read back and hashed on the phone (r6c: `adb exec-in 'toybox cat > node'` silently
    dropped the last ~125 KB of vendor and system on the attended 30 Sep run; the chunked push + dd is the method that
    restored them, .relay/tmp/pushwrite.sh);
  * after each program: sync + blockdev --flushbufs + drop_caches, so the readback comes from the eMMC, not the page cache.
"""
import hashlib
import os
import re
import subprocess
import tempfile
from pathlib import Path

import RomFlashLayoutV1 as L
import RomUpdateLayoutV1 as U

T = '/system/bin/toybox'
NODES = '/dev/block/a6lupd'
MIB = 1 << 20
CHUNK = 256 * MIB                 # r6c: push chunk size (the V75 recovery /tmp is RAM: one chunk at a time)
REMOTE_CHUNK = '/tmp/a6l-upd-chunk'
_SHA = re.compile(r'\b([0-9a-f]{64})\b')


class AdbStop(Exception):
    pass


class AdbRecoveryDevice:
    def __init__(self, serial, adb='/usr/bin/adb', expected_image='v74', timeout=3600):
        self.adb = [adb, '-s', serial]
        self.expected_image = expected_image
        self.timeout = timeout
        self.log = []
        self.parts = None      # name -> (node, start, sectors)
        self.disk = None

    # -- helpers --
    def shell(self, cmd, timeout=60):
        r = subprocess.run(self.adb + ['shell', cmd], capture_output=True, text=True, timeout=timeout)
        return r.returncode, r.stdout.replace('\r\n', '\n'), r.stderr

    def must(self, cmd, timeout=60):
        rc, out, err = self.shell(cmd, timeout)
        if rc != 0:
            raise AdbStop('remote command failed (%s): %s %s' % (rc, cmd[:120], err[-300:]))
        return out

    def preflight(self):
        """Identify the recovery, load the eMMC driver if needed, verify the partition geometry, create the nodes."""
        img = self.must(f"{T} cat /proc/device-tree/chosen/hisense,a6l-image 2>/dev/null | {T} tr -d '\\000'; echo").strip()
        if self.expected_image and img != self.expected_image:
            raise AdbStop('not the %s diagnostic recovery (image=%r)' % (self.expected_image, img))
        self.must(f'{T} grep -q "^sdhci_msm " /proc/modules || {{ {T} insmod /sdhci-msm.ko && {T} sleep 6; }}', timeout=60)
        out = self.must(f'for b in /sys/class/block/*; do n=${{b##*/}}; '
                        f'echo "B|$n|$({T} cat $b/dev)|$({T} cat $b/size)|$({T} cat $b/start 2>/dev/null)|'
                        f'$({T} grep ^PARTNAME= $b/uevent 2>/dev/null)|$({T} grep ^DEVTYPE= $b/uevent)"; done')
        rows = []
        for line in out.splitlines():
            if not line.startswith('B|'):
                continue
            _, name, dev, size, start, pn, dt = (line.split('|') + [''] * 7)[:7]
            rows.append({'name': name, 'dev': dev.strip(), 'size': int(size or 0), 'start': int(start) if start.strip() else None,
                         'partname': pn.split('=', 1)[1].strip() if '=' in pn else '', 'devtype': dt.split('=', 1)[-1].strip()})
        disks = {}
        for r in rows:
            if r['devtype'] == 'partition' and r['partname'] in L.PARTITIONS:
                disk = r['name'].rsplit('p', 1)[0]
                disks.setdefault(disk, {})[r['partname']] = r
        full = [d for d, p in disks.items() if set(p) == set(L.PARTITIONS)]
        if len(full) != 1:
            raise AdbStop('expected exactly one disk carrying every layout partition, got %s' % sorted(disks))
        d = full[0]
        disk_row = [r for r in rows if r['name'] == d and r['devtype'] == 'disk']
        if len(disk_row) != 1 or disk_row[0]['size'] * L.SECTOR != L.DISK_BYTES:
            raise AdbStop('eMMC disk size differs from the layout')
        parts = {}
        for name, (start, sectors) in L.PARTITIONS.items():
            r = disks[d][name]
            if (r['start'], r['size']) != (start, sectors):
                raise AdbStop('sysfs geometry differs for %s: %s != %s' % (name, (r['start'], r['size']), (start, sectors)))
            parts[name] = (r['dev'], start, sectors)
        mk = [f'{T} mkdir -p {NODES}']
        for name, (dev, _, _) in parts.items():
            ma, mi = dev.split(':')
            mk.append(f'{T} rm -f {NODES}/{name}; {T} mknod {NODES}/{name} b {int(ma)} {int(mi)}')
        ma, mi = disk_row[0]['dev'].split(':')
        mk.append(f'{T} rm -f {NODES}/disk; {T} mknod {NODES}/disk b {int(ma)} {int(mi)}')
        self.must(' && '.join(mk))
        self.parts = {n: (f'{NODES}/{n}', s, c) for n, (_, s, c) in parts.items()}
        self.disk = f'{NODES}/disk'
        self.log.append(('preflight', d))
        return d

    def _map_read(self, start, sectors):
        for node, ps, pn in self.parts.values():
            if ps <= start and start + sectors <= ps + pn:
                return node, (start - ps) * L.SECTOR
        for gs, gn in (L.GPT_PRIMARY, L.GPT_TAIL):
            if gs <= start and start + sectors <= gs + gn:
                return self.disk, start * L.SECTOR
        raise AdbStop('read outside every partition and the GPT: %d+%d' % (start, sectors))

    # -- device interface --
    def hash_region(self, start, sectors):
        """On-phone readback hash; same verified read ranges, no host snapshot."""
        if self.parts is None:
            raise AdbStop('preflight not done')
        node, off = self._map_read(start, sectors)
        n = sectors * L.SECTOR
        self.log.append(('hash', start, sectors))
        return self._remote_sha(f'{T} dd if={node} bs={MIB} iflag=skip_bytes,count_bytes '
                                f'skip={off} count={n} 2>/dev/null | {T} sha256sum', 'read range')

    def read(self, start, sectors, out_path):
        if self.parts is None:
            raise AdbStop('preflight not done')
        node, off = self._map_read(start, sectors)
        n = sectors * L.SECTOR
        self.log.append(('read', start, sectors))
        # attended fix 30 Sep 2026: `adb exec-out` is a raw stream that also carries the recovery shell's own stderr. The V74/V75
        # recovery has no /linkerconfig/ld.config.txt, so /system/bin/sh prints two linker warning lines (205 bytes) BEFORE
        # the data. toybox prints a marker after the shell has started; everything up to and including it is dropped.
        mark = b'A6L_READ_BEGIN_5f2c9e'
        p = subprocess.Popen(self.adb + ['exec-out', f'{T} printf {mark.decode()}; {T} dd if={node} bs={MIB} '
                                         f'iflag=skip_bytes,count_bytes skip={off} count={n} 2>/dev/null'],
                             stdout=subprocess.PIPE)
        got = 0
        try:
            with open(out_path, 'wb') as f:
                head = b''
                while mark not in head:
                    b = p.stdout.read(4096)
                    if not b or len(head) > 65536:
                        raise AdbStop('read: stream marker not found: %d+%d %r' % (start, sectors, head[:200]))
                    head += b
                data = head[head.index(mark) + len(mark):]
                while True:
                    if data:
                        f.write(data)
                        got += len(data)
                    data = p.stdout.read(MIB)
                    if not data:
                        break
            rc = p.wait(timeout=self.timeout)
        except BaseException:
            p.kill()
            raise
        if rc != 0 or got != n or Path(out_path).stat().st_size != n:
            raise AdbStop('read failed or short: %d+%d (got %d)' % (start, sectors, got))

    def program(self, start, sectors, stream):
        if self.parts is None:
            raise AdbStop('preflight not done')
        hit = [n for n, (_, ps, _) in self.parts.items() if ps == start]
        if len(hit) != 1:
            raise AdbStop('program does not start at a partition start: %d' % start)
        name = hit[0]
        try:
            U.check_update_plan([(name, start, sectors)])
        except ValueError as e:
            raise AdbStop(str(e))
        node = self.parts[name][0]
        n = sectors * L.SECTOR
        self.log.append(('program', start, sectors))
        # r6c: chunked push + on-phone sha + dd seek (see the module docstring). Chunks start at MiB multiples of the
        # partition, so `seek` counts 1 MiB blocks; the last chunk may be shorter (dd writes the partial record).
        sent = 0
        buf = bytearray()
        with tempfile.TemporaryDirectory(prefix='a6l-upd-') as td:
            local = os.path.join(td, 'chunk')
            it = iter(stream)
            done = False
            while not done:
                while len(buf) < CHUNK:
                    try:
                        b = next(it)
                    except StopIteration:
                        done = True
                        break
                    if sent + len(buf) + len(b) > n:
                        raise AdbStop('payload longer than the programmed range')
                    buf += b
                if not buf:
                    break
                chunk, buf = bytes(buf[:CHUNK]), bytearray(buf[CHUNK:])
                self._write_chunk(node, sent, chunk, local)
                sent += len(chunk)
            while buf:          # the iterator ended with more than one chunk buffered
                chunk, buf = bytes(buf[:CHUNK]), bytearray(buf[CHUNK:])
                self._write_chunk(node, sent, chunk, local)
                sent += len(chunk)
        if sent != n:
            raise AdbStop('payload shorter than the programmed range (%d of %d bytes)' % (sent, n))
        self.must(f'{T} rm -f {REMOTE_CHUNK}; {T} sync; {T} blockdev --flushbufs {node}; echo 3 > /proc/sys/vm/drop_caches')

    def _remote_sha(self, cmd, what):
        rc, out, err = self.shell(cmd, timeout=600)
        m = _SHA.search(out)
        if rc != 0 or not m:
            raise AdbStop('%s: no sha256 from the phone (rc %s): %r %r' % (what, rc, out[-200:], err[-200:]))
        return m.group(1)

    def _write_chunk(self, node, off, chunk, local):
        if off % MIB:
            raise AdbStop('chunk offset not MiB aligned: %d' % off)
        want = hashlib.sha256(chunk).hexdigest()
        with open(local, 'wb') as f:
            f.write(chunk)
        self.log.append(('chunk', node, off, len(chunk)))
        r = subprocess.run(self.adb + ['push', local, REMOTE_CHUNK], capture_output=True, text=True, timeout=self.timeout)
        if r.returncode != 0:
            raise AdbStop('adb push of the chunk at %d failed (%s): %s' % (off, r.returncode, (r.stderr or r.stdout)[-300:]))
        got = self._remote_sha(f'{T} sha256sum {REMOTE_CHUNK}', 'pushed chunk')
        size = self.must(f'{T} stat -c %s {REMOTE_CHUNK}').strip().splitlines()[-1:]
        if got != want or size != [str(len(chunk))]:
            raise AdbStop('pushed chunk at %d differs on the phone (sha %s != %s, size %s != %d); nothing written for it'
                          % (off, got[:16], want[:16], size, len(chunk)))
        self.must(f'{T} dd if={REMOTE_CHUNK} of={node} bs={MIB} seek={off // MIB} conv=notrunc 2>/dev/null', timeout=600)
        back = self._remote_sha(f'{T} dd if={node} bs={MIB} iflag=skip_bytes,count_bytes skip={off} count={len(chunk)} '
                                f'2>/dev/null | {T} sha256sum', 'written range')
        if back != want:
            raise AdbStop('written range at %d reads back %s, expected %s' % (off, back[:16], want[:16]))
        self.must(f'{T} rm -f {REMOTE_CHUNK}')

    def power(self, value):
        self.log.append(('power', value))
        cmd = {'off': f'{T} sync; {T} poweroff', 'reset': f'{T} sync; {T} reboot'}.get(value)
        if cmd is None:
            raise AdbStop('power value not permitted: %r' % value)
        subprocess.run(self.adb + ['shell', cmd], capture_output=True, timeout=30)
