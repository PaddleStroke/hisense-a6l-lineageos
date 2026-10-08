#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""r5 review round4 F36 host test: the real a6l_eink_mirror binary against a fake a6l_epdd on a Unix socket, constant
source page. The first frame fails (ERR reply / connection closed before the reply / no reply at all); every later
command is acknowledged OK. The mirror must resend the (unchanged) page and then stop once it was acknowledged.
usage: mirror_ack_test.py <mirror binary> <workdir>  -> prints MIRROR_ACK_TEST PASS|FAIL"""
import os, socket, struct, subprocess, sys, threading

mirror, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
raw = os.path.join(work, 'constant.raw')
with open(raw, 'wb') as f:
    f.write(struct.pack('<4I', 8, 16, 1, 0) + bytes([128, 128, 128, 255]) * 128)
fails = 0

def scenario(name):
    global fails
    path = os.path.join(work, name + '.sock')
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX); srv.bind(path); srv.listen(); srv.settimeout(0.1)
    cmds, replies, stop, errors, state = [], [], threading.Event(), [], {'failed': False}

    def serve():
        try:
            while not stop.is_set():
                try:
                    conn, _ = srv.accept()
                except socket.timeout:
                    continue
                with conn:
                    conn.settimeout(0.2); buf = b''
                    while not stop.is_set():
                        try:
                            while b'\n' not in buf:
                                d = conn.recv(65536)
                                if not d:
                                    break
                                buf += d
                            if b'\n' not in buf:
                                break
                            line, buf = buf.split(b'\n', 1); cmds.append(line.decode())
                            if line.startswith(b'frame '):
                                _, w, h, *_ = line.split(); need = int(w) * int(h)
                                while len(buf) < need:
                                    d = conn.recv(65536)
                                    if not d:
                                        raise RuntimeError('short frame')
                                    buf += d
                                buf = buf[need:]
                                if not state['failed']:
                                    state['failed'] = True
                                    if name == 'disconnect':
                                        break
                                    if name == 'noreply':
                                        continue
                                    conn.sendall(b'ERR update failed (injected)\n'); replies.append('ERR'); continue
                            conn.sendall(b'OK shown\n'); replies.append('OK')
                        except socket.timeout:
                            continue
        except Exception as e:
            errors.append(repr(e))

    th = threading.Thread(target=serve); th.start()
    try:
        r = subprocess.run([mirror, '--source', 'file:' + raw, '--epd-socket', path, '--no-props', '--mode', 'mirror',
                            '--key-dev', 'none', '--touch-dev', 'none', '--interval', '50', '--frames', '140', '--idle-max-ms', '0',  # eink-round9: a fixed capture count
                            '--reply-timeout', '1000'], capture_output=True, text=True, timeout=60)
    finally:
        stop.set(); th.join(2); srv.close()
    open(os.path.join(work, 'mirror-' + name + '.log'), 'w').write(r.stdout + r.stderr)
    frames = [c for c in cmds if c.startswith('frame ')]
    ok = not errors and 2 <= len(frames) <= 3 and 'exit after 140 frames' in r.stdout and replies and replies[-1] == 'OK'
    print(('ok  ' if ok else 'FAIL') + ' F36 %s: first frame failed, page resent: %d frame cmds %s replies %s %s'
          % (name, len(frames), cmds, replies, errors))
    fails += 0 if ok else 1

for s in ('error', 'disconnect', 'noreply'):
    scenario(s)
print('MIRROR_ACK_TEST ' + ('PASS' if fails == 0 else 'FAIL'))
