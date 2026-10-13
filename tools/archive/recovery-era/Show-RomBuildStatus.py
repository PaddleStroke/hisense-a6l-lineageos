#!/usr/bin/env python3
"""Read-only ROM build status. Run in WSL; --watch refreshes every five seconds."""
import argparse
import json
from datetime import datetime
from pathlib import Path
import re
import time


def tail(path, limit=32768):
    if not path.is_file():
        return ''
    with path.open('rb') as stream:
        stream.seek(max(0, path.stat().st_size - limit))
        return stream.read().decode(errors='replace')


def show(tag):
    repo = Path(__file__).resolve().parent.parent
    work = Path('/home/a6l/rom-v2')
    if tag is None:
        recent = sorted([*(repo / 'firmware/extracted').glob('rom-r*-*/pipeline*.log'),
                         *(repo / 'firmware/extracted').glob('rom-r*-*/status.json')],
                        key=lambda path: path.stat().st_mtime)
        match = re.match(r'rom-(r[0-9]+[a-z]*)-', recent[-1].parent.name) if recent else None
        tag = match.group(1) if match else 'r6g'
    builds = sorted((repo / 'firmware/extracted').glob('rom-' + tag + '-*/pipeline*.log'),
                    key=lambda path: path.stat().st_mtime)
    active_logs = sorted([*builds, *work.glob('pipeline-' + tag + '-*.log')],
                         key=lambda path: path.stat().st_mtime)
    pipeline = tail(active_logs[-1]) if active_logs else ''
    log = work / ('build-' + tag + '.log')
    build = tail(log)
    phases = [line for line in pipeline.splitlines() if line.startswith('== ')]
    progress = re.findall(r'\[\s*(\d+)%\s+(\d+)/(\d+)(?:\s+[^\]\r\n]*)?\]\s*([^\r\n]*)', build)
    print('A6L ' + tag + ' - ' + datetime.now().astimezone().strftime('%H:%M:%S %Z'))
    print('Phase: ' + (phases[-1].split(' 2026 ', 1)[-1] if phases else 'not started'))
    status = {}
    statuses = sorted((repo / 'firmware/extracted').glob('rom-' + tag + '-*/status.json'),
                      key=lambda path: path.stat().st_mtime)
    if statuses:
        status_file = statuses[-1]
        try:
            status = json.loads(status_file.read_text())
            print('Install preparation: ' + status.get('phase', status.get('status', 'not recorded')))
            if status.get('installed'):
                print('Phone installation recorded complete.')
        except (OSError, ValueError):
            pass
    if progress:
        percent, done, total, task = progress[-1]
        graph_step = any(phrase in task for phrase in (
            'bootstrap blueprint', 'generating ninja file', 'Make module parser',
            'including ', 'finishing Make packaging rules'))
        if graph_step and 'PIPELINE_DONE ' + tag not in pipeline:
            print('Build graph preparation; image compilation has not reported task counts yet.')
        else:
            print('Compilation: %s%% of tasks (%s / %s); not a time estimate.' % (percent, done, total))
    if log.exists():
        age = max(0, int(time.time() - log.stat().st_mtime))
        print('Build log last updated: %s seconds ago.' % age)
    errors = [line for line in (pipeline + '\n' + build).splitlines()
              if re.search(r'^FAILED:|ninja: build stopped|PIPELINE_FAIL|^ERROR:', line)]
    if errors:
        print('Failure recorded: ' + errors[-1][:250])
    elif 'PIPELINE_DONE ' + tag in pipeline and status.get('pipeline_complete'):
        print('Build pipeline finished. Image validation / kit preparation / laptop staging / flashing are separate steps.')
    elif 'PIPELINE_DONE ' + tag in pipeline:
        print('One image phase finished; the complete bundle has not reported completion.')
    else:
        print('Build pipeline has not reported completion.')
    print('Build log: ' + str(log))
    print('Ctrl+C closes this viewer; it does not stop the build.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag', nargs='?', help='omit to follow the latest build')
    parser.add_argument('--watch', action='store_true')
    args = parser.parse_args()
    if args.tag is not None and not re.fullmatch(r'[a-zA-Z0-9_-]+', args.tag):
        parser.error('invalid build tag')
    try:
        while True:
            if args.watch:
                print('\033[2J\033[H', end='', flush=True)
            show(args.tag)
            if not args.watch:
                break
            time.sleep(5)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
