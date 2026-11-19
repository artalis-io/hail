"""Package the bounded native-only run and subsequent audit evidence (no audio)."""
import csv
import hashlib
import json
import re
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUN = ROOT / 'build-wasapi-native-only'
OUT = ROOT / 'docs/evidence/wasapi-native-only-2026-11-19'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    plan = read_json(RUN / 'matrix/plan.json')
    results = read_json(RUN / 'matrix/results.json')
    expected = [f'{mode}-{delay}-{ack}' for mode in ('primed', 'unprimed')
                for delay in (0, 3, 7, 11) for ack in (0, 16)]
    if [r['name'] for r in results] != expected:
        raise RuntimeError('Incomplete or reordered fixed matrix')
    rows = []
    for result in results:
        name = result['name']
        for stream in ('stdout', 'stderr'):
            path = RUN / f'matrix/{name}.{stream}.log'
            if digest(path).lower() != result[f'{stream}_sha256'].lower():
                raise RuntimeError(f'Changed output: {path}')
        log = (RUN / f'matrix/{name}.stdout.log').read_text()
        stats = re.search(r'^stage=capture_final (.+)$', log, re.MULTILINE)
        if not stats:
            raise RuntimeError(f'No final observation: {name}')
        values = dict(re.findall(r'(\w+)=(\d+)', stats[1]))
        passed = (result['exit_code'] == 0 and not result['watchdog_expired']
                  and 'PASS native_only_exact_marker_check resources_released=1' in log)
        if not passed or any(int(values[k]) for k in ('fractional', 'asymmetric', 'nonfinite', 'bad')):
            raise RuntimeError(f'Unexpected result; retain and review: {name}')
        if min(int(values[k]) for k in ('verified_A', 'verified_B')) <= 16000:
            raise RuntimeError(f'Insufficient markers: {name}')
        rows.append(dict(case=name, exit_code=result['exit_code'], **values))
    with (OUT / 'matrix.csv').open('w', newline='', encoding='utf-8') as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    for kind in ('defaults', 'shure'):
        before = (RUN / f'{kind}-before.{"jsonl" if kind == "defaults" else "json"}')
        after = (RUN / f'{kind}-after.{"jsonl" if kind == "defaults" else "json"}')
        if before.read_text(encoding='utf-8-sig') != after.read_text(encoding='utf-8-sig'):
            raise RuntimeError(f'{kind} readbacks changed')
    source = ROOT / 'tools/probes/wasapi_native_repro.c'
    executable = ROOT / 'build-windows-validation/OFF-Release/wasapi_native_repro.exe'
    if digest(executable).lower() != plan['executable_sha256'].lower():
        raise RuntimeError('Matrix executable changed')
    compiler_record = next((ROOT / 'build-windows-validation/OFF-Release/CMakeFiles').glob('*/CMakeCCompiler.cmake'))
    compiler = re.search(r'set\(CMAKE_C_COMPILER "([^"]+)"\)', compiler_record.read_text())
    if not compiler:
        raise RuntimeError('Missing configured compiler')
    provenance = {
        'matrix_source_commit': plan['source'],
        'packaging_base_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'source_sha256': digest(source), 'matrix_executable_sha256': digest(executable),
        'compiler': subprocess.check_output([compiler[1], '--version'], cwd=ROOT, text=True).strip(),
        'pipewire_review_source': 'https://github.com/PipeWire/pipewire/blob/1.0.5/src/pipewire/stream.c',
    }
    # Compiler identity is in CMakeCache; keep the actual configured path/version records.
    (RUN / 'audit/provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    diff = subprocess.check_output(['git', 'diff', '--', 'CMakeLists.txt'], cwd=ROOT)
    (RUN / 'audit/sanitizer-fix.diff').write_bytes(diff)
    with zipfile.ZipFile(OUT / 'raw.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(RUN.rglob('*')):
            if path.is_file():
                archive.write(path, 'run/' + path.relative_to(RUN).as_posix())
        for config in ('OFF-Debug', 'OFF-Release', 'ON-Debug', 'ON-Release'):
            folder = ROOT / 'build-windows-validation' / config
            for path in sorted(folder.glob('*.log')):
                archive.write(path, f'builds/{config}/{path.name}')
            archive.write(folder / 'CMakeCache.txt', f'builds/{config}/CMakeCache.txt')
        archive.write(ROOT / 'build-wasapi-sdk-asan/CMakeCache.txt', 'builds/asan/CMakeCache.txt')
        for path in (source, ROOT / 'tools/run_wasapi_native_only.ps1', ROOT / 'CMakeLists.txt',
                     ROOT / 'tools/package_wasapi_native_only.py',
                     ROOT / 'docs/c-audit-2026-11-19.md', ROOT / 'docs/axis-audit.md'):
            archive.write(path, path.relative_to(ROOT).as_posix())
    readme = '''Standalone Windows WASAPI/VB-CABLE marker reproduction

Observed result: 16/16 exact cases passed; fractional ramp NOT reproduced.
This reduced scheduling/priming model does not qualify Hail paired transitions.
See wasapi-native-only-results.md and the predeclared plan for limits.

Build from an x64 Visual Studio developer shell with clang-cl and Windows SDK:
clang-cl /nologo /W4 /WX /std:c11 wasapi_native_repro.c /link ole32.lib uuid.lib
Device-free oracle:
wasapi_native_repro.exe --self-test
Authorized reserved synthetic 48 kHz stereo VB-CABLE pair only:
wasapi_native_repro.exe CAPTURE_ENDPOINT_ID RENDER_ENDPOINT_ID UNPRIMED DELAY_MS ACK_DELAY_MS
UNPRIMED=0/1; fixed matrix DELAY_MS=0/3/7/11 and ACK_DELAY_MS=0/16.
Endpoints must be explicit; no defaults are selected or settings changed.
Each process self-conditions silence and releases resources before reporting.
Exit 0=exact pass; 1=numerical rejection; 2=setup/SDK/check failure.
Place a 40-second independent process watchdog around each invocation.
No Hail library, driver binary or bulk PCM is included.
'''
    with zipfile.ZipFile(OUT / 'standalone.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('README.txt', readme)
        archive.write(source, source.name)
        for name in ('wasapi-native-only-results.md', 'wasapi-native-only-plan.md'):
            archive.write(ROOT / 'docs' / name, name)
    summary = read_json(RUN / 'matrix/summary.json')
    summary.update(outcome='fractional ramp not reproduced', passed_cases=len(rows),
                   defaults_and_shure_settings_unchanged=True,
                   asan_native_profile='separate primed/7/16 pass; not part of fixed matrix',
                   provenance=provenance,
                   artifacts={p.name: digest(p) for p in sorted(OUT.iterdir())
                              if p.name in ('raw.zip', 'standalone.zip', 'matrix.csv')})
    (OUT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    for name in ('raw.zip', 'standalone.zip'):
        with zipfile.ZipFile(OUT / name) as archive:
            if archive.testzip() is not None:
                raise RuntimeError(f'Corrupt archive: {name}')
    print(f'PASS: {len(rows)} cases, hashes/readbacks/archives verified')


if __name__ == '__main__':
    main()
