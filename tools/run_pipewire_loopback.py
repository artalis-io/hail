#!/usr/bin/env python3
"""Run real PipeWire I/O on a private null sink/monitor, with no hardware nodes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import select
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def run_test(executable):
    with tempfile.TemporaryDirectory(prefix='hail-loopback-') as directory:
        temporary = Path(directory)
        for name in ('client.conf', 'server.conf'):
            shutil.copyfile(ROOT / 'tests/fixtures/pipewire' / name, temporary / name)
        env = os.environ.copy()
        # No ambient remote, drop-in, session manager or device discovery.
        env.update(PIPEWIRE_RUNTIME_DIR=directory, XDG_RUNTIME_DIR=directory,
                   PIPEWIRE_REMOTE='hail-test', PIPEWIRE_CONFIG_DIR=directory,
                   PIPEWIRE_CONFIG_NAME='client.conf', PIPEWIRE_CONFIG_PREFIX='',
                   XDG_CONFIG_HOME=directory, HA_ISOLATED_PIPEWIRE='1')
        env.pop('PIPEWIRE_PROPS', None)
        child = None
        transcript = []
        diagnostics = []
        with (temporary / 'server.log').open('w+') as server_log, (temporary / 'test.log').open('w+') as test_log:
            server_env = dict(env, PIPEWIRE_CONFIG_NAME='server.conf')
            server = subprocess.Popen(['pipewire', '-c', str(temporary / 'server.conf')],
                                      env=server_env, stdout=server_log, stderr=server_log)
            try:
                deadline = time.monotonic() + 5
                while not (temporary / 'hail-test').exists():
                    if server.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('isolated PipeWire server failed to start')
                    time.sleep(.01)
                child = subprocess.Popen([str(executable)], env=env, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=test_log, bufsize=0)
                pending = bytearray()

                def line():
                    deadline = time.monotonic() + 15
                    while b'\n' not in pending:
                        left = deadline - time.monotonic()
                        if left <= 0 or not select.select([child.stdout], [], [], left)[0]:
                            raise RuntimeError('loopback test timed out')
                        block = os.read(child.stdout.fileno(), 65536)
                        if not block:
                            raise RuntimeError('loopback test exited unexpectedly')
                        pending.extend(block)
                    end = pending.index(b'\n')
                    value = bytes(pending[:end]).decode()
                    del pending[:end + 1]
                    transcript.append(value)
                    return value

                def command(*args):
                    result = subprocess.run(args, env=env, text=True, capture_output=True, timeout=5)
                    if result.stderr:
                        diagnostics.append(result.stderr)
                    if result.returncode:
                        raise RuntimeError(f'{args[0]} failed: {result.stderr}')
                    return result.stdout

                def send(value):
                    child.stdin.write((value + '\n').encode())
                    child.stdin.flush()

                def graph():
                    return json.loads(command('pw-dump'))

                def node(objects, name=None, media=None):
                    return next(o['id'] for o in objects if o['type'].endswith(':Node') and
                                (o['info']['props'].get('node.name') == name if name else
                                 o['info']['props'].get('media.name') == media))

                def port(objects, node_id, direction, channel):
                    return next(o['id'] for o in objects if o['type'].endswith(':Port') and
                                int(o['info']['props']['node.id']) == node_id and
                                o['info']['props']['port.direction'] == direction and
                                o['info']['props'].get('audio.channel') == channel)

                assert line() == 'READY'
                def link_graph():
                    objects = graph()
                    sink = node(objects, name='hail-loopback')
                    capture = node(objects, media='Hail capture')
                    playback = node(objects, media='Hail playback')
                    for channel in ('FL', 'FR'):
                        command('pw-link', str(port(objects, playback, 'out', channel)),
                                str(port(objects, sink, 'in', channel)))
                        command('pw-link', str(port(objects, sink, 'out', channel)),
                                str(port(objects, capture, 'in', channel)))
                    return sink

                sink = link_graph()
                send('GO')
                result = json.loads(line())
                assert line() == 'REMOVE'
                command('pw-cli', 'destroy', str(sink))
                send('REMOVED')
                assert line() == 'REPLACE'
                command('pw-cli', 'create-node', 'adapter',
                        '{ factory.name=support.null-audio-sink node.name=hail-loopback '
                        'node.description="Hail isolated loopback" media.class=Audio/Sink '
                        'audio.rate=48000 audio.channels=2 audio.position=[FL FR] '
                        'object.linger=true adapter.auto-port-config={mode=dsp monitor=true position=preserve} }')
                send('REPLACED')
                assert line() == 'REOPEN'
                link_graph()
                send('GO')
                starvation = line()
                assert starvation.startswith('XRUN ')
                result['injected_starvation'] = json.loads(starvation[5:])
                assert line().startswith('PASS ')
                assert child.wait(timeout=5) == 0
                result['lifecycle'] = 'device loss, replacement incarnation, explicit reopen and teardown passed'
                result['transcript'] = transcript
                return result
            except Exception as error:
                test_log.flush()
                test_log.seek(0)
                server_log.flush()
                server_log.seek(0)
                raise RuntimeError(f'{error}\nTest: {test_log.read()}\nServer: {server_log.read()}\n'
                                   f'Transcript: {transcript}\nDiagnostics: {diagnostics}') from error
            finally:
                for process in (child, server):
                    if process and process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test', required=True, type=Path)
    parser.add_argument('--iterations', type=int, default=3)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 1 <= args.iterations <= 100:
        parser.error('iterations must be 1..100')
    results = [run_test(args.test.resolve()) for _ in range(args.iterations)]
    cpu = next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                if line.startswith('model name')), 'unknown')
    report = {'date_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'scope': 'Private native PipeWire software loopback, no hardware; not physical latency or a hard RT guarantee',
              'platform': {'os': platform.system(), 'kernel': platform.release(), 'machine': platform.machine(), 'cpu': cpu,
                           'pipewire': subprocess.check_output(['pipewire', '--version'], text=True).strip()},
              'configuration': {'rate_hz': 48000, 'channels': 2, 'format': 'F32', 'quantum_frames': 256,
                                'capture_slots': 16, 'playback_slots': 4, 'phase_seconds': 1,
                                'scheduling': 'ordinary userspace; no RT module or session manager'},
              'runs': results}
    cache_path = args.test.resolve().parent / 'CMakeCache.txt'
    if cache_path.exists():
        cache = {}
        for entry in cache_path.read_text().splitlines():
            if not entry.startswith(('#', '//')) and '=' in entry and ':' in entry.split('=', 1)[0]:
                key, value = entry.split('=', 1)
                cache[key.split(':', 1)[0]] = value
        compiler = cache.get('CMAKE_C_COMPILER')
        report['build'] = {'type': cache.get('CMAKE_BUILD_TYPE'),
                           'shared': cache.get('HA_BUILD_SHARED'),
                           'c_flags': cache.get('CMAKE_C_FLAGS'),
                           'configuration_flags': cache.get('CMAKE_C_FLAGS_' + cache.get('CMAKE_BUILD_TYPE', '').upper()),
                           'compiler': subprocess.check_output([compiler, '--version'], text=True).splitlines()[0] if compiler else None}
    paths = ['CMakeLists.txt', 'include/hail.h', 'src/hail_internal.h', 'src/core.c', 'src/queue.c',
             'src/native.c', 'src/backend_pipewire.c', 'tests/pipewire_loopback.c',
             'tools/run_pipewire_loopback.py', 'tests/fixtures/pipewire/server.conf',
             'tests/fixtures/pipewire/client.conf']
    report['source_sha256'] = {path: hashlib.sha256((ROOT / path).read_bytes()).hexdigest() for path in paths}
    report['executable_sha256'] = hashlib.sha256(args.test.resolve().read_bytes()).hexdigest()
    text = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()
