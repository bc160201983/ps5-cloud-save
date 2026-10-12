#!/usr/bin/env python3
"""Print failing unittest tracebacks from a log as GitHub Actions error annotations."""
import re
import sys

log = open(sys.argv[1], encoding='utf-8', errors='replace').read()
blocks = re.findall(r'^={20,}\n((?:FAIL|ERROR): .*?)(?=^={20,}\n|^-{20,}\nRan )', log, re.M | re.S)
if not blocks:
    blocks = [log[-3000:]]
for block in blocks[:20]:
    text = block.strip()[-3000:].replace('%', '%25').replace('\r', '').replace('\n', '%0A')
    print('::error title=Host test failure::' + text)
