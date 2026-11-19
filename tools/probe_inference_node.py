"""Inspect an existing Spark over SSH; no installs, credentials or service changes.

One optional, bounded synthetic text request goes directly to the backend.
Gateway authentication and end-to-end speech qualification remain separate.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path

REMOTE = r'''
import datetime, hashlib, json, pathlib, platform, subprocess, time
import urllib.error, urllib.request

LIMIT = 1024 * 1024

def command(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=25)
    return dict(exit_code=result.returncode, stdout=result.stdout,
                stderr=result.stderr)

def request(port, path, body=None):
    raw = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request('http://127.0.0.1:%d%s' % (port, path),
                                 data=raw, headers={'Content-Type': 'application/json'})
    try:
        response = urllib.request.urlopen(req, timeout=8)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = response.read(LIMIT + 1)
        if len(data) > LIMIT:
            raise RuntimeError('HTTP body exceeds inspection bound')
        try:
            value = json.loads(data)
        except ValueError:
            value = data.decode('utf-8', 'replace')
        return dict(status=response.status, body=value)

out = dict(observed_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
           node=platform.node(), machine=platform.machine(), kernel=platform.release(),
           managed_services=command(['hs-infer', 'list']),
           service_health=command(['hs-infer', 'health', ALIAS, '--json']),
           gateway_status=command(['hs-gateway', 'status', '--json']))
source = pathlib.Path('/opt/herding-sparks/bin/hs-gateway').read_bytes()
out['gateway_source_sha256'] = hashlib.sha256(source).hexdigest()
definition = json.loads(pathlib.Path('/etc/herding-sparks/infer/' + ALIAS + '.json').read_text())
out['service'] = {key: definition[key] for key in ('service', 'model', 'runtime', 'server')}
recipe = definition.get('vllm', {}).get('recipe', {})
out['service']['vllm'] = {
    key: definition.get('vllm', {}).get(key)
    for key in ('max_model_len', 'gpu_memory_utilization', 'kv_cache_dtype')}
out['service']['recipe'] = {key: recipe.get(key) for key in ('name', 'sha256', 'args')}
config_path = pathlib.Path(definition['model']['file']) / 'config.json'
if config_path.is_file():
    data = config_path.read_bytes()
    config = json.loads(data)
    out['model_config'] = dict(sha256=hashlib.sha256(data).hexdigest(),
                              architectures=config.get('architectures'),
                              model_type=config.get('model_type'),
                              top_level_audio_fields=[k for k in config if 'audio' in k.lower() or 'speech' in k.lower()])
out['backend_models'] = request(BACKEND_PORT, '/v1/models')
schema = request(BACKEND_PORT, '/openapi.json')
out['backend_openapi'] = dict(status=schema['status'], paths=sorted(schema['body'].get('paths', {})))
out['gateway_http'] = {}
for path, body in [('/hs-gateway/health', None), ('/v1/models', None),
                   ('/v1/chat/completions', {}), ('/v1/audio/transcriptions', {}),
                   ('/v1/audio/speech', {}), ('/v1/realtime', {})]:
    out['gateway_http'][('GET ' if body is None else 'POST ') + path] = request(GATEWAY_PORT, path, body)
out['text_stream'] = dict(performed=False)
if TEXT_SMOKE:
    payload = dict(model=ALIAS, messages=[dict(role='user', content='Reply with OK.')],
                   stream=True, max_tokens=16, temperature=0,
                   chat_template_kwargs=dict(enable_thinking=False))
    req = urllib.request.Request('http://127.0.0.1:%d/v1/chat/completions' % BACKEND_PORT,
                                 data=json.dumps(payload).encode(),
                                 headers={'Content-Type': 'application/json', 'Accept': 'text/event-stream'})
    start = time.monotonic()
    frames, content, done, total, first = 0, '', False, 0, None
    with urllib.request.urlopen(req, timeout=30) as response:
        status = response.status
        content_type = response.headers.get('Content-Type')
        while True:
            line = response.readline(LIMIT - total + 1)
            if not line:
                break
            total += len(line)
            if total > LIMIT or time.monotonic() - start > 30:
                raise RuntimeError('Text smoke exceeds inspection bound')
            if not line.startswith(b'data: '):
                continue
            if line.strip() == b'data: [DONE]':
                done = True
                break
            frame = json.loads(line[6:])
            if 'error' in frame:
                raise RuntimeError('Backend returned a streaming error')
            frames += 1
            for choice in frame.get('choices', []):
                part = choice.get('delta', {}).get('content') or ''
                if part and first is None:
                    first = time.monotonic() - start
                content += part
    out['text_stream'] = dict(performed=True, scope='direct loopback backend, not authenticated gateway',
                              status=status, content_type=content_type, data_frames=frames,
                              content=content, done=done, first_content_seconds=first,
                              elapsed_seconds=time.monotonic() - start,
                              max_tokens=16, concurrent_requests=1)
out['gateway_after'] = request(GATEWAY_PORT, '/hs-gateway/health')
print(json.dumps(out, indent=2))
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--alias', required=True)
    parser.add_argument('--backend-port', type=int, required=True)
    parser.add_argument('--gateway-port', type=int, default=8780)
    parser.add_argument('--text-smoke', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', args.host):
        parser.error('host must be an SSH-config alias or hostname')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', args.alias):
        parser.error('alias must be a simple managed service name')
    if not all(1 <= p <= 65535 for p in (args.backend_port, args.gateway_port)):
        parser.error('invalid port')
    if args.output.exists():
        parser.error('use a fresh output path to retain prior evidence')
    preface = (f'ALIAS={args.alias!r}\nBACKEND_PORT={args.backend_port}\n'
               f'GATEWAY_PORT={args.gateway_port}\nTEXT_SMOKE={args.text_smoke!r}\n')
    result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=12',
                             args.host, 'python3 -I -'], input=preface + REMOTE,
                            capture_output=True, text=True, timeout=180, check=True)
    data = json.loads(result.stdout)
    data['probe_scope'] = 'existing node inspection; no credentials, audio or service changes'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(output=str(args.output), observed_utc=data['observed_utc'],
                          text_stream=data['text_stream']), indent=2))


if __name__ == '__main__':
    main()
