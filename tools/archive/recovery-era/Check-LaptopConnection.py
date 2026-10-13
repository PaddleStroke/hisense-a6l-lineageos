"""Read-only, bounded stability check for the repaired laptop link."""
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / 'logs/laptop-connection-stability.json'
SSH = ['C:/Windows/System32/OpenSSH/ssh.exe', '-F', str(ROOT / 'tools/a6l-laptop-ssh.conf'), 'a6l-laptop']


def now():
    return datetime.now(timezone.utc).isoformat()


def main():
    assert not REPORT.exists(), 'Do not overwrite prior evidence'
    report = {'started_utc': now(), 'attempts': [], 'passed': False}
    def save():
        tmp = REPORT.with_suffix('.json.tmp')
        tmp.write_text(json.dumps(report, indent=2) + '\n')
        tmp.replace(REPORT)
    save()
    # No phone operations: one quiet SSH session with keepalives, alongside
    # independent fresh sessions over a three-minute observation period.
    persistent = subprocess.Popen(SSH + ["python3 -c 'import time; time.sleep(180); print(\"idle-session-complete\")'"],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        for i in range(7):
            started = time.monotonic()
            try:
                r = subprocess.run(SSH + ['hostname'], capture_output=True, text=True, timeout=15)
                result = {'utc': now(), 'exit': r.returncode, 'seconds': round(time.monotonic()-started, 3),
                          'stdout': r.stdout.strip(), 'stderr': r.stderr.strip()}
            except subprocess.TimeoutExpired:
                result = {'utc': now(), 'exit': None, 'error': 'Connection timed out'}
            report['attempts'].append(result)
            save()
            if i < 6:
                time.sleep(30)
        stdout, stderr = persistent.communicate(timeout=20)
        report['idle_session'] = {'exit': persistent.returncode, 'stdout': stdout.strip(), 'stderr': stderr.strip()}
        report['passed'] = (persistent.returncode == 0 and stdout.strip() == 'idle-session-complete'
                            and all(a.get('exit') == 0 and a.get('stdout') == 'system76-pc' for a in report['attempts']))
    finally:
        if persistent.poll() is None:
            persistent.kill()
            persistent.wait(timeout=5)
        report['finished_utc'] = now()
        save()
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report['passed'] else 1)


if __name__ == '__main__':
    main()
