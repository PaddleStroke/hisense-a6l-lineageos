"""Check actual generated retry code and delayed storage-completion parsing."""
import ast
import errno
from pathlib import Path
from types import SimpleNamespace
import unittest
from Patch_StorageHostCollector import patch_collector

SOURCE = patch_collector((Path(__file__).parent / 'Collect-ProbeSerial-v22.py').read_text())
TREE = ast.parse(SOURCE)

class CollectorTests(unittest.TestCase):
    def test_windows_newlines(self):
        raw = (Path(__file__).parent / 'Collect-ProbeSerial-v22.py').read_text()
        self.assertEqual(patch_collector(raw.replace('\n', '\r\n')), SOURCE)

    def test_storage_window_uses_actual_fork(self):
        helper = next(n for n in TREE.body if isinstance(n, ast.FunctionDef) and n.name == 'storage_window_complete')
        context = {'re': __import__('re')}
        exec(compile(ast.Module(body=[helper], type_ignores=[]), '<helper>', 'exec'), context)
        complete = context['storage_window_complete']
        self.assertFalse(complete(b'A6L_RAM_PROBE_ALIVE seconds=158\nA6L_STORAGE_MODULE_FORK_BEGIN seconds=145'))
        self.assertTrue(complete(b'A6L_STORAGE_MODULE_FORK_BEGIN seconds=145\nA6L_RAM_PROBE_ALIVE seconds=165'))
        self.assertFalse(complete(b'A6L_RAM_PROBE_ALIVE seconds=400'))
        self.assertTrue(complete(b'A6L_STORAGE_MODULE_FORK_BEGIN seconds=20\nA6L_RAM_PROBE_ALIVE seconds=40'))

    def run_open(self, failures):
        node = next(n for n in ast.walk(TREE) if isinstance(n, ast.Try)
                    and len(n.body) == 1 and isinstance(n.body[0], ast.Assign)
                    and isinstance(n.body[0].value, ast.Call)
                    and isinstance(n.body[0].value.func, ast.Attribute)
                    and n.body[0].value.func.attr == 'Serial')
        code = 'def attempt():\n    permission_denied_since = None\n    while True:\n'
        code += '\n'.join('        ' + line for line in ast.unparse(node).splitlines())
        code += '\n        return connection\n'
        state = {'seconds': 0, 'calls': 0}
        def open_serial(*args, **kwargs):
            state['calls'] += 1
            if failures:
                raise failures.pop(0)
            return 'opened'
        def sleep(seconds):
            state['seconds'] += seconds
        context = dict(serial=SimpleNamespace(Serial=open_serial, SerialException=OSError),
                       matches=[SimpleNamespace(device='/dev/ttyACM0')], errno=errno,
                       time=SimpleNamespace(monotonic=lambda: state['seconds'], sleep=sleep),
                       report={}, save=lambda: None)
        exec(code, context)
        return context['attempt'], state, context['report']

    def test_transient_permission_retry(self):
        attempt, state, report = self.run_open([PermissionError(errno.EACCES, 'udev pending')]*2)
        self.assertEqual(attempt(), 'opened')
        self.assertEqual((state['calls'], state['seconds'], report['permission_retry_count']), (3, .5, 2))

    def test_permission_retry_is_bounded(self):
        attempt, state, report = self.run_open([PermissionError(errno.EACCES, 'denied')]*30)
        with self.assertRaises(PermissionError):
            attempt()
        self.assertEqual(state['seconds'], 5)
        self.assertEqual(report['permission_retry_count'], 20)

    def test_other_errors_do_not_retry(self):
        attempt, state, report = self.run_open([OSError(errno.EBUSY, 'busy')])
        with self.assertRaises(OSError):
            attempt()
        self.assertEqual(state['calls'], 1)
        self.assertEqual(state['seconds'], 0)

if __name__ == '__main__':
    unittest.main()
