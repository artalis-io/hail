#!/usr/bin/env python3
"""Bisect native RT-module initialization with the standalone ASan C probe."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--probe', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
report = {'scope': 'Standalone PipeWire context; no Hail linked; no sanitizer suppressions', 'cases': []}
with tempfile.TemporaryDirectory(prefix='hail-pw-leak-') as directory:
    base = (ROOT / 'tests/fixtures/pipewire/client.conf').read_text().replace(
        'context.properties = { support.dbus = false }', 'context.properties = { }')
    for name, module in [
        ('no_rt_module', ''),
        ('rt_default', '{ name = libpipewire-module-rt flags = [ ifexists nofail ] }'),
        ('rt_without_dbus_fallback', '{ name = libpipewire-module-rt flags = [ ifexists nofail ] args = { rtkit.enabled = false rtportal.enabled = false } }'),
    ]:
        config = Path(directory) / 'client.conf'
        config.write_text(base.replace('context.modules = [', 'context.modules = [\n' + module))
        env = dict(os.environ, PIPEWIRE_CONFIG_DIR=directory, PIPEWIRE_CONFIG_NAME='client.conf',
                   PIPEWIRE_CONFIG_PREFIX='', XDG_CONFIG_HOME=directory)
        env.pop('PIPEWIRE_PROPS', None)
        result = subprocess.run([str(args.probe.resolve())], env=env, text=True, capture_output=True, timeout=15)
        match = re.search(r'SUMMARY: AddressSanitizer: (\d+) byte\(s\) leaked in (\d+) allocation\(s\)', result.stderr)
        case = {'name': name, 'returncode': result.returncode,
                'leaked_bytes': int(match[1]) if match else 0, 'allocations': int(match[2]) if match else 0,
                'stderr': result.stderr}
        if result.returncode and not match:
            raise RuntimeError(f'Probe failed without a leak report: {case}')
        report['cases'].append(case)
report['source_sha256'] = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
    for path in [ROOT / 'tools/probes/pipewire_context.c', ROOT / 'tools/probes/pipewire_leaks.py', ROOT / 'tests/fixtures/pipewire/client.conf']}
report['probe_sha256'] = hashlib.sha256(args.probe.resolve().read_bytes()).hexdigest()
report['pipewire'] = subprocess.check_output(['pipewire', '--version'], text=True).strip()
args.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps([{k: v for k, v in case.items() if k != 'stderr'} for case in report['cases']], indent=2))
