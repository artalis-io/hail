#!/usr/bin/env python3
"""ELF/Mach-O symbol and portable-core boundary checks. No file mutations."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('library', type=Path)
parser.add_argument('--shared', action='store_true')
parser.add_argument('--no-virtual', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
expected = set((root / 'cmake/exports.txt').read_text().split())
if not args.no_virtual:
    expected.update(('ha_virtual_configure', 'ha_virtual_step', 'ha_virtual_device_set',
                     'ha_virtual_device_add', 'ha_virtual_default_set'))
darwin = sys.platform == 'darwin'
cmd = ['nm', '-g', '-U'] if darwin else ['nm', '--defined-only', '-g']
if args.shared and not darwin:
    cmd.append('-D')
raw = subprocess.check_output([*cmd, str(args.library)], text=True)
symbols = {line.split()[-1].split('@')[0] for line in raw.splitlines() if len(line.split()) >= 3}
if darwin:
    symbols = {s[1:] if s.startswith('_') else s for s in symbols}
public = {s for s in symbols if s.startswith('ha_')}
assert public == expected, {'missing': sorted(expected - public), 'extra': sorted(public - expected)}
private = {s for s in symbols if s not in expected and s != 'HAIL_0'}
assert not args.shared or not private, sorted(private)
assert all(s.startswith('hail_') for s in private), sorted(private)
for source in ('src/core.c', 'src/queue.c', 'src/native.c', 'src/hail_internal.h', 'include/hail.h'):
    text = (root / source).read_text()
    assert not re.search(r'#\s*include\s*[<"](?:pthread|windows|unistd|sys/|alsa|pipewire|hull|keel)', text), source
    assert not re.search(r'\b(?:malloc|calloc|realloc|free|printf|fprintf|sleep|dlopen)\s*\(', text), source
for header in (root / 'include').rglob('*.h'):
    for macro in re.findall(r'^#define\s+(\w+)', header.read_text(), re.MULTILINE):
        assert macro.startswith('HA_'), (str(header), macro)
print(f'{len(public)} public symbols match; shared visibility and portable-core boundaries checked')
