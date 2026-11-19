import base64
import hashlib
import json
from pathlib import Path

root = Path('docs/evidence/wasapi-paired-2026-11-18')
p = root / 'provisioning.json'
record = json.loads(p.read_text(encoding='utf-8-sig'))
for leg in record['configuration']:
    for prop in leg['properties']:
        value = prop['value']
        if isinstance(value, list) and value and all(type(x) is int and 0 <= x <= 255 for x in value):
            raw = bytes(value)
            prop['value'] = {'encoding': 'base64', 'byte_count': len(raw),
                             'sha256': hashlib.sha256(raw).hexdigest().upper(),
                             'data': base64.b64encode(raw).decode('ascii')}
p.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
for p in root.rglob('*'):
    if p.is_file() and p.read_bytes().startswith((b'\xff\xfe', b'\xfe\xff')):
        p.write_text(p.read_text(encoding='utf-16'), encoding='utf-8')
p = root / 'execution.json'
record = json.loads(p.read_text(encoding='utf-8-sig'))
record['artifact_encoding'] = 'Registry binary values losslessly encoded as base64 with byte count and SHA-256; UTF-16 text normalized to UTF-8.'
record['artifacts'] = [dict(path=str(p.relative_to(root)), sha256=hashlib.sha256(p.read_bytes()).hexdigest().upper(), bytes=p.stat().st_size)
                       for p in sorted(root.rglob('*')) if p.is_file() and p.name != 'execution.json']
p.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
