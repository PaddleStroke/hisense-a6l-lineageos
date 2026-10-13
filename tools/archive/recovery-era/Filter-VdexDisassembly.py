#!/usr/bin/env python3
"""Filter vdexExtractor --dis output by class name without altering firmware."""
import argparse
import re

p = argparse.ArgumentParser()
p.add_argument('input')
p.add_argument('output')
p.add_argument('--class-pattern', default='EpdManager|IEpdManager|SurfaceControl')
a = p.parse_args()
include = False
with open(a.input, encoding='utf-8', errors='replace') as src, open(a.output, 'w', encoding='utf-8') as dst:
    for line in src:
        if line.startswith('  class #'):
            include = bool(re.search(a.class_pattern, line))
        if include:
            dst.write(line)
