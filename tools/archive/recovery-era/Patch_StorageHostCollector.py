"""Generate the V23 collector's bounded permission retry and storage window."""

HELPER = '''def storage_window_complete(data):
    forks = [int(v) for v in re.findall(rb'A6L_STORAGE_MODULE_FORK_BEGIN seconds=([0-9]+)', data)]
    alive = [int(v) for v in re.findall(rb'A6L_RAM_PROBE_ALIVE seconds=([0-9]+)', data)]
    return bool(forks and alive and max(alive) >= forks[0] + 20)

'''

def patch_collector(source):
    source = source.replace('\r\n', '\n')
    assert source.count('import argparse\n') == 1
    source = source.replace('import argparse\n', 'import argparse\nimport errno\n')
    source = source.replace('boot_deadline = None\n', 'boot_deadline = None\npermission_denied_since = None\n')
    source = source.replace('def save():\n', HELPER + 'def save():\n')
    old = '                connection = serial.Serial(matches[0].device, baudrate=115200, timeout=0.5, exclusive=True)'
    assert source.count(old) == 1
    source = source.replace(old, '''                try:
                    connection = serial.Serial(matches[0].device, baudrate=115200, timeout=0.5, exclusive=True)
                except (serial.SerialException, OSError) as error:
                    if getattr(error, 'errno', None) != errno.EACCES:
                        raise
                    now = time.monotonic()
                    if permission_denied_since is None:
                        permission_denied_since = now
                    if now - permission_denied_since >= 5:
                        raise
                    report['permission_retry_count'] = report.get('permission_retry_count', 0) + 1
                    save()
                    time.sleep(0.25)
                    continue
                permission_denied_since = None''')
    old_window = "any(int(v) >= 40 for v in re.findall(rb'A6L_RAM_PROBE_ALIVE seconds=([0-9]+)', payload))"
    assert source.count(old_window) == 2
    source = source.replace(old_window, 'storage_window_complete(payload)')
    return source
