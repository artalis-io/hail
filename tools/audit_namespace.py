#!/usr/bin/env python3
"""Read-only sibling source/symbol audit; writes JSON to stdout. No ABI freeze."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def run(*args):
    return subprocess.check_output(args, text=True)


def audit(root):
    tracked = run('git', '-C', str(root), 'ls-files').splitlines()
    matches = []
    scanned = 0
    for name in tracked:
        path = root / name
        if path.suffix not in ('.c', '.h', '.cpp', '.hpp') or not path.is_file():
            continue
        scanned += 1
        for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
            tokens = sorted(set(re.findall(r'\b(?:ha_|HA_)\w*', line)))
            if tokens:
                matches.append({'file': name, 'line': number, 'tokens': tokens})
    artifacts = []
    # Deliberately only known library/application artifacts, not test executables.
    for name in ('libkeel.a', 'libhull.a', 'hull', 'bitnet', 'libsail.a'):
        path = root / name
        if not path.is_file():
            continue
        raw = subprocess.run(['nm', '-g', '--defined-only', str(path)],
                             capture_output=True, text=True, check=True).stdout
        symbols = sorted(set(line.split()[-1] for line in raw.splitlines()
                             if len(line.split()) >= 3))
        artifacts.append({'path': str(path),
                          'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                          'global_defined_count': len(symbols),
                          'ha_matches': [s for s in symbols if re.match(r'_?(ha_|HA_)', s)],
                          'prefix_counts': {p: sum(s.startswith(p) for s in symbols)
                                            for p in ('kl_', 'hl_', 'bn_', 'ha_', 'llhttp_')},
                          'symbols': symbols})
    status = run('git', '-C', str(root), 'status', '--short')
    return {'root': str(root), 'head': run('git', '-C', str(root), 'rev-parse', 'HEAD').strip(),
            'worktree_changed_entries': len(status.splitlines()),
            'worktree_status_sha256': hashlib.sha256(status.encode()).hexdigest(),
            'tracked_source_files_scanned': scanned, 'source_ha_matches': matches,
            'artifacts': artifacts,
            'limits': 'Artifacts may be stale; static globals are not a shared export table. '
                      'Missing artifacts and uncompiled configurations remain unaudited.'}


if __name__ == '__main__':
    roots = [Path(p).resolve() for p in sys.argv[1:]]
    if not roots:
        raise SystemExit('usage: audit_namespace.py REPOSITORY ...')
    print(json.dumps([audit(p) for p in roots], indent=2))
