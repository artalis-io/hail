#!/usr/bin/env python3
"""Run explicit physical CoreAudio trials; save text/measurements, never PCM.

No device configuration, default routing or volume changes. Each trial runs once;
unsupported formats and failed trials remain in the report. macOS authorization
is handled by the signed test executable using its embedded purpose string.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import os
import sys
from datetime import datetime, timezone

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--test', required=True, type=Path)
p.add_argument('--capture', required=True, help='Exact enumerated name, or - to disable')
p.add_argument('--playback', required=True, help='Exact enumerated name, or - to disable')
p.add_argument('--seconds', type=int, default=3)
p.add_argument('--matrix', action='store_true', help='16/24/44.1/48 kHz, mono/stereo, S16/F32')
p.add_argument('--silent', action='store_true', help='Submit digital silence while exercising render callbacks')
p.add_argument('--load-workers', type=int, choices=range(5), default=0, help='0..4 independent SHA256 CPU load processes')
p.add_argument('--rate', type=int, default=48000)
p.add_argument('--channels', type=int, default=1)
p.add_argument('--pcm', type=int, choices=(1, 2), default=2)
p.add_argument('--output', required=True, type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parent.parent

def command(argv, env=None):
    r = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    return {'command': list(map(str, argv)), 'exit_code': r.returncode, 'output': r.stdout}

report = {'started_utc': datetime.now(timezone.utc).isoformat(),
          'source_revision': command(['git', 'rev-parse', 'HEAD'])['output'].strip(),
          'source_sha256': {str(f.relative_to(root)): hashlib.sha256(f.read_bytes()).hexdigest()
                            for f in [*sorted((root/'src').glob('*.[ch]')), *sorted((root/'src').glob('*.m')), root/'include/hail.h',
                                      root/'tests/coreaudio_physical.c', root/'tests/coreaudio_authorize.m']},
          'binary_sha256': hashlib.sha256(a.test.read_bytes()).hexdigest(),
          'configuration': vars(a) | {'test': str(a.test), 'output': str(a.output)},
          'platform': [command(c) for c in [['sw_vers'], ['uname', '-m'], ['xcrun', '--show-sdk-version'],
                      ['sysctl', '-n', 'machdep.cpu.brand_string', 'hw.memsize'], ['pmset', '-g', 'custom']]],
          'trials': []}
formats = [(r,c,s) for r in (16000,24000,44100,48000) for c in (1,2) for s in (1,2)] if a.matrix else [(a.rate,a.channels,a.pcm)]
a.output.parent.mkdir(parents=True, exist_ok=True)

def save():
    a.output.write_text(json.dumps(report, indent=2)+'\n')

save()
workers=[]
try:
    for _ in range(a.load_workers):
        workers.append(subprocess.Popen([sys.executable, '-c', 'import hashlib\nb=b"x"*1048576\nwhile True: hashlib.sha256(b).digest()']))
    report['load']={'method':'independent SHA256 loops over 1 MiB buffer', 'workers':len(workers)}
    for rate, channels, pcm in formats:
        env=os.environ.copy()
        if a.silent:
            env['HA_TEST_SILENT']='1'
        trial = command([str(a.test.resolve()), a.capture, a.playback, str(rate), str(channels), str(pcm), str(a.seconds)], env)
        trial.update(rate=rate, channels=channels, pcm=pcm)
        trial['measurements'] = [json.loads(line) for line in trial['output'].splitlines() if line.startswith('{')]
        trial['unsupported'] = 'RESULT open=unsupported format' in trial['output']
        report['trials'].append(trial)
        save()
        print(f'{rate} Hz {channels} ch PCM {pcm}: exit={trial["exit_code"]} unsupported={trial["unsupported"]}', flush=True)
finally:
    for worker in workers:
        worker.terminate()
    for worker in workers:
        worker.wait()
report['finished_utc'] = datetime.now(timezone.utc).isoformat()
save()
# Unsupported is a documented outcome, never converted to a passing trial.
raise SystemExit(1 if any(t['exit_code'] and not t['unsupported'] for t in report['trials']) else 0)
