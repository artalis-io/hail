"""Summarize retained native trial measurements, without reading microphone PCM."""
import argparse
import json
import math
from pathlib import Path
import re

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()


def fields(line):
    result = {}
    for key, value in re.findall(r'(\w+)=([^\s]+)', line):
        try:
            result[key] = int(value)
        except ValueError:
            try:
                result[key] = float(value)
            except ValueError:
                result[key] = value
    return result


def measurements(text):
    result = {}
    age_buckets = []
    for line in text.splitlines():
        if line.startswith(('sustained_', 'capture_frames=', 'flush_capture_', 'injected_capture_',
                            'capture_delivery phase=', 'PASS allocations=', 'PASS lifecycle_cycles=')):
            result.update(fields(line))
        elif line.startswith('capture_delivery_bucket='):
            item = fields(line)
            age_buckets.append((item['capture_delivery_bucket'], item['count']))
    if result.get('peak', 0) > 0:
        result['capture_peak_dbfs'] = 20 * math.log10(result['peak'])
    if result.get('sustained_ns', 0) > 0:
        result['process_cpu_percent_one_logical_cpu'] = 100 * result['process_cpu_ns'] / result['sustained_ns']
    total = sum(count for _, count in age_buckets)
    if total:
        for percentile in (50, 95, 99):
            threshold = math.ceil(total * percentile / 100)
            cumulative = 0
            for bucket, count in age_buckets:
                cumulative += count
                if cumulative >= threshold:
                    # The final bucket is unbounded. Keep that fact explicit.
                    result[f'capture_age_nonnegative_p{percentile}_upper_ns'] = (bucket + 1) * 100000 if bucket < 4095 else None
                    break
    return result


campaigns = {}
for path in sorted([*args.directory.rglob('summary.json'), *args.directory.rglob('results.json')]):
    records = json.loads(path.read_text(encoding='utf-8-sig'))
    records = records if isinstance(records, list) else [records]
    trials = []
    plan_path = path.parent / 'plan.json'
    plan = json.loads(plan_path.read_text(encoding='utf-8-sig')) if plan_path.exists() else {}
    for record in records:
        if 'name' not in record:
            continue
        stdout = path.parent / f'{record["name"]}.stdout.log'
        stderr = path.parent / f'{record["name"]}.stderr.log'
        observed = measurements(stdout.read_text(encoding='utf-8-sig'))
        trial = {
            'name': record['name'], 'exit_code': record['exit_code'],
            'meaningful_signal': bool(observed['meaningful_capture_minus90dbfs']) if 'meaningful_capture_minus90dbfs' in observed else None,
            'historical_runner_signal_flag': record.get('meaningful_signal'),
            'executable_sha256': record.get('executable_sha256', plan.get('conditioning_sha256')
                if record['name'].endswith('.condition') else plan.get('executable_sha256')),
            'measurements': observed,
            'stderr': stderr.read_text(encoding='utf-8-sig'),
        }
        samples = record.get('process_samples', [])
        if samples:
            trial['process_samples'] = len(samples)
            for key in ('working_set', 'private_bytes', 'handles'):
                trial[f'{key}_min'] = min(sample[key] for sample in samples)
                trial[f'{key}_max'] = max(sample[key] for sample in samples)
        trials.append(trial)
    if trials:
        campaigns[str(path.parent.relative_to(args.directory))] = {
            'passed': sum(trial['exit_code'] == 0 for trial in trials),
            'failed': sum(trial['exit_code'] != 0 for trial in trials), 'trials': trials,
        }
historical = {}
for name in ('initial-duplex-48k-stereo-f32.log', 'sustained-600s-original-48k-stereo-f32.log'):
    path = args.directory / name
    if path.exists():
        historical[name] = measurements(path.read_text(encoding='utf-8-sig'))
args.output.write_text(json.dumps({'campaigns': campaigns, 'historical_quiet_input': historical}, indent=2) + '\n')
for name, campaign in campaigns.items():
    print(f'{name}: {campaign["passed"]} passed, {campaign["failed"]} failed')
