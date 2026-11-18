"""Preserve a completed native campaign as exact zipped logs plus readable summaries."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--raw', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--builds', type=Path, default=Path('build-windows-validation'))
args = parser.parse_args()
raw = args.raw.resolve(strict=True)
output = args.output.resolve()
if raw == output or raw in output.parents or output in raw.parents:
    raise SystemExit('Keep raw campaign and publication directories separate')
output.mkdir(parents=True, exist_ok=True)
archive = output / 'raw-trials.zip'
if archive.exists():
    raise SystemExit('Refusing to overwrite published campaign evidence')


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


files = sorted(path for path in raw.rglob('*') if path.is_file())
with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
    for path in files:
        if path.is_symlink():
            raise SystemExit(f'Unexpected raw evidence symlink: {path}')
        bundle.write(path, path.relative_to(raw).as_posix())
    for config in ('OFF-Debug', 'OFF-Release', 'ON-Debug', 'ON-Release'):
        build = args.builds / config
        for name in ('CMakeCache.txt', 'build.log', 'ctest.log', 'contracts.log', 'consumer-ctest.log'):
            bundle.write(build / name, f'builds/{config}/{name}')
with zipfile.ZipFile(archive) as bundle:
    if bundle.testzip() is not None:
        raise SystemExit('Evidence archive failed CRC validation')
subprocess.run([sys.executable, str(Path(__file__).with_name('summarize_wasapi_physical.py')),
                str(raw), '--output', str(output / 'measurements.json')], check=True)
for relative in ('driver-and-asan-runtime.json', 'shure-pnp-chain.json',
                 'sustained-fixed-600s/provenance.json',
                 'sustained-fixed-600s/duplex-48000-2-f32-primed.stdout.log',
                 'sustained-fixed-600s/duplex-48000-2-f32-primed.stderr.log',
                 'sustained-fixed-600s/input-settings-original.json',
                 'sustained-fixed-600s/input-settings-restored.json'):
    destination = output / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(raw / relative, destination)
sources = ('CMakeLists.txt', 'src/backend_wasapi.c', 'src/native.c', 'src/core.c', 'src/queue.c',
           'tests/wasapi_loopback.c', 'tests/wasapi_buffers.c', 'tests/wasapi_retirement.c',
           'tests/wasapi_endpoint_level.c', 'tools/run_wasapi_physical.ps1',
           'tools/run_wasapi_physical_hotplug.ps1', 'tools/probes/windows_asan_runtime.c',
           'tools/summarize_wasapi_physical.py', 'tools/package_wasapi_physical.py')
manifest = {
    'head_at_collection': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
    'working_tree_at_collection': subprocess.check_output(['git', 'status', '--short'], text=True).splitlines(),
    'source_sha256': {name: digest(Path(name)) for name in sources},
    'archive_sha256': digest(archive), 'raw_files': len(files),
    'native_microphone_pcm_saved': False,
    'physical_hotplug': 'Not executed; owner coordination required',
    'paired_fixture': 'Not executed; approved signed fixture manifest required',
}
(output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(f'Preserved {len(files)} raw files in {archive}; SHA-256 {manifest["archive_sha256"]}')
