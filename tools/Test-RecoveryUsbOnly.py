#!/usr/bin/env python3
"""Apply the same captured-ABL regression suite to the isolated USB candidate."""
import importlib.util
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('abl_suite', ROOT / 'tools/Test-RecoveryRegulatorConstraints.py')
suite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suite)
suite.OUT = ROOT / 'firmware/extracted/recovery-probe-usb-only-20260916'
if __name__ == '__main__':
    suite.main()
