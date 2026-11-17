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
for source in ('src/core.c', 'src/queue.c', 'src/native.c', 'src/virtual.c',
               'src/hail_internal.h', 'include/hail.h', 'include/hail/virtual.h'):
    if args.no_virtual and source in ('src/virtual.c', 'include/hail/virtual.h'):
        continue
    text = (root / source).read_text()
    assert not re.search(r'#\s*include\s*[<"](?:pthread|windows|winsock|mmdevice|audioclient|'
                         r'unistd|sys/|mach/|CoreAudio/|AudioUnit/|AudioToolbox/|CoreFoundation/|'
                         r'AVFoundation/|Foundation/|dispatch/|alsa|pipewire|spa/|hull|keel)', text), source
    assert not re.search(r'\b(?:AudioUnit\w*|AudioObject\w*|AudioOutputUnit\w*|'
                         r'IAudioClient_\w*|IMMDevice\w*|snd_pcm_\w*|pw_stream_\w*|'
                         r'WaitFor\w*|pthread_\w*)\s*\(', text), source
    assert not re.search(r'\b(?:malloc|calloc|realloc|free|printf|fprintf|sleep|dlopen)\s*\(', text), source
for header in (root / 'include').rglob('*.h'):
    for macro in re.findall(r'^#define\s+(\w+)', header.read_text(), re.MULTILINE):
        assert macro.startswith('HA_'), (str(header), macro)
for source in ('src/core.c', 'src/queue.c'):
    assert not re.search(r'\bHA_BACKEND_(?:VIRTUAL|ALSA|PIPEWIRE|COREAUDIO|WASAPI)\b',
                         (root / source).read_text()), source
stream = (root / 'src/hail_internal.h').read_text().split('struct ha_stream {', 1)[1].split('};', 1)[0]
assert not re.search(r'\b(?:faults|start_delay|stop_delay|flush_delay|capture_drift|playback_drift|'
                     r'host_ns|clock_ns|clock_remainder|control_ticks|scratch)\b', stream), 'adapter state in ha_stream'
print(f'{len(public)} public symbols match; shared visibility and portable-core boundaries checked')
