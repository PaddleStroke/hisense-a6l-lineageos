#!/usr/bin/env python3
"""Allow an opt-in memory limit through Soong's cleared child environment."""
from pathlib import Path
import sys

source = Path(sys.argv[1]) / 'build/soong/ui/build/soong.go'
original = source.read_text()
anchor = '\tinvocationEnv := make(map[string]string)\n'
addition = '''\t// Local A6L host-build control; no effect unless explicitly configured.
\tif memoryLimit := os.Getenv("A6L_SOONG_GOMEMLIMIT"); memoryLimit != "" {
\t\tinvocationEnv["GOMEMLIMIT"] = memoryLimit
\t}
'''
if addition in original:
    print('Soong memory forwarding already configured')
elif original.count(anchor) == 1:
    source.write_text(original.replace(anchor, anchor + addition))
    print('Enabled opt-in GOMEMLIMIT forwarding to the Soong graph builder')
else:
    raise SystemExit('Unexpected upstream Soong source; inspect before patching')
