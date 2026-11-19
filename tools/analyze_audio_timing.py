#!/usr/bin/env python3
"""Reduce retained qualification traces; never infer missing physical events.

CSV: trial,event,time_ns,uncertainty_ns,clock,frame,rate,domain,epoch
See docs/gate-e.md for event semantics and the required campaign manifest.
This offline tool is not used by Hail or its device callbacks.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path

PAIRS = {
    'input_delivery_latency_ns': ('electrical_input', 'capture_delivery'),
    'capture_delivery_age_ns': ('capture_anchor', 'capture_delivery'),
    'playback_latency_ns': ('submission', 'electrical_output'),
    'physical_round_trip_ns': ('submission', 'electrical_capture'),
    'reference_alignment_ns': ('reference_anchor', 'electrical_capture'),
    'logical_invalidation_ns': ('cancel_request', 'queue_invalidated'),
    'backend_ack_ns': ('cancel_request', 'flush_ack'),
    'presentation_tail_ns': ('cancel_request', 'last_presented'),
    'electrical_tail_ns': ('cancel_request', 'last_electrical'),
    'audible_tail_ns': ('cancel_request', 'last_acoustic'),
    'callback_duration_ns': ('callback_begin', 'callback_end'),
}
PHYSICAL = {'input_delivery_latency_ns', 'playback_latency_ns', 'physical_round_trip_ns', 'reference_alignment_ns',
            'electrical_tail_ns', 'audible_tail_ns'}


def distribution(values):
    if not values:
        return None
    v = sorted(values)
    return dict(count=len(v), median=(v[(len(v)-1)//2]+v[len(v)//2])/2,
                p95=v[math.ceil(len(v)*.95)-1], p99=v[math.ceil(len(v)*.99)-1],
                maximum=v[-1], minimum=v[0])


def integer(value, name):
    if not isinstance(value, str) or not value.isdecimal():
        raise ValueError(f'{name} must be an unsigned decimal integer')
    return int(value)


def analyze(manifest, rows):
    for key in ('backend', 'os', 'hardware', 'driver', 'sample_rate', 'channels',
                'buffer_frames', 'duration_seconds', 'methodology', 'limitations'):
        if not manifest.get(key):
            raise ValueError(f'missing manifest field: {key}')
    if manifest.get('kind') not in ('simulated', 'software_loopback', 'physical_electrical', 'physical_acoustic'):
        raise ValueError('explicit measurement kind required')
    for key in ('sample_rate', 'channels', 'buffer_frames'):
        if type(manifest[key]) is not int or manifest[key] <= 0:
            raise ValueError(f'{key} must be a positive integer')
    duration = manifest['duration_seconds']
    if type(duration) not in (int, float) or not math.isfinite(duration) or duration <= 0:
        raise ValueError('finite positive measurement duration required')
    trials, anchors, callback_starts = {}, {}, []
    for row in rows:
        event = row['event']
        if event not in {event for pair in PAIRS.values() for event in pair}:
            raise ValueError(f'unknown event: {event}')
        time = integer(row['time_ns'], 'time_ns')
        uncertainty = None if row['uncertainty_ns'] == '' else integer(row['uncertainty_ns'], 'uncertainty_ns')
        if not row['clock'] or not row['trial']:
            raise ValueError('clock and trial are required')
        key = (row['trial'], event)
        if key in trials:
            raise ValueError(f'duplicate event {key}')
        trials[key] = (time, uncertainty, row['clock'])
        if event == 'callback_begin':
            callback_starts.append((row['clock'], time))
        if event in ('capture_anchor', 'reference_anchor') and row.get('frame', ''):
            domain = integer(row['domain'], 'domain'); epoch = integer(row['epoch'], 'epoch')
            rate = integer(row['rate'], 'rate')
            if not domain or not epoch or not rate:
                raise ValueError('anchors require nonzero domain, epoch and rate')
            anchors.setdefault((event, domain, epoch, row['clock'], rate), []).append(
                (time, integer(row['frame'], 'frame'), uncertainty))
    metrics = {}
    for metric, (begin, end) in PAIRS.items():
        samples, uncertainties = [], []
        unknown = 0
        for trial, event in trials:
            if event != begin or (trial, end) not in trials:
                continue
            a, b = trials[(trial, begin)], trials[(trial, end)]
            if a[2] != b[2]:
                raise ValueError(f'{metric}: timestamps require a common calibrated clock')
            if metric in PHYSICAL:
                if manifest['kind'] not in ('physical_electrical', 'physical_acoustic') or not manifest.get('calibration'):
                    raise ValueError(f'{metric}: physical measurement and calibration required')
                if a[1] is None or b[1] is None:
                    raise ValueError(f'{metric}: calibrated uncertainty required')
            delta = b[0] - a[0]
            if delta < 0 and metric not in ('reference_alignment_ns', 'presentation_tail_ns', 'electrical_tail_ns', 'audible_tail_ns'):
                raise ValueError(f'{metric}: reversed events')
            samples.append(delta)
            if a[1] is None or b[1] is None:
                unknown += 1
            else:
                uncertainties.append(a[1] + b[1])
        metrics[metric] = dict(distribution=distribution(samples),
                               uncertainty_bound_ns=distribution(uncertainties), unknown_uncertainty_count=unknown)
    drifts = []
    for key, points in anchors.items():
        points.sort()
        for a, b in zip(points, points[1:]):
            if b[0] <= a[0] or b[1] <= a[1]:
                raise ValueError('non-monotonic anchors within domain/epoch')
        if len(points) < 2:
            continue
        first, last = points[0], points[-1]
        elapsed = last[0] - first[0]
        bound = None if first[2] is None or last[2] is None else first[2] + last[2]
        drifts.append(dict(event=key[0], domain=key[1], epoch=key[2], clock=key[3], rate=key[4],
                           anchors=len(points), span_ns=elapsed, span_uncertainty_ns=bound,
                           observed_ppm=((last[1]-first[1])*1e9/elapsed/key[4]-1)*1e6,
                           interpretation='endpoint slope; includes timestamp estimation error'))
    result = dict(manifest=manifest, metrics=metrics, clock_segments=drifts,
                  qualification='measurements only; no automatic Gate E verdict')
    deadline = manifest.get('callback_deadline_ns')
    if deadline is not None:
        if type(deadline) is not int or deadline <= 0:
            raise ValueError('positive integer callback deadline required')
        durations = [v[0] - trials[(t, 'callback_begin')][0] for (t, e), v in trials.items()
                     if e == 'callback_end' and (t, 'callback_begin') in trials]
        result['callback_deadline_misses'] = sum(v > deadline for v in durations) if durations else None
    expected = manifest.get('callback_period_ns')
    if expected is not None:
        if type(expected) is not int or expected <= 0:
            raise ValueError('positive integer callback period required')
        clocks = {c for c, _ in callback_starts}
        if len(clocks) > 1:
            raise ValueError('callback jitter requires one clock per trace')
        starts = sorted(t for _, t in callback_starts)
        result['callback_absolute_jitter_ns'] = distribution([abs(b-a-expected) for a,b in zip(starts, starts[1:])])
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('manifest', type=Path); p.add_argument('trace', type=Path)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    manifest = json.loads(a.manifest.read_text(encoding='utf-8-sig'))
    # Calibration is an actual retained artifact, not a boolean declaration.
    if manifest.get('calibration'):
        calibration = (a.manifest.parent / manifest['calibration']).resolve()
        manifest['calibration_sha256'] = hashlib.sha256(calibration.read_bytes()).hexdigest()
    with a.trace.open(encoding='utf-8-sig', newline='') as f:
        result = analyze(manifest, list(csv.DictReader(f)))
    result['trace_sha256'] = hashlib.sha256(a.trace.read_bytes()).hexdigest()
    result['manifest_sha256'] = hashlib.sha256(a.manifest.read_bytes()).hexdigest()
    with a.output.open('x', encoding='utf-8') as f:
        json.dump(result, f, indent=2, allow_nan=False); f.write('\n')


if __name__ == '__main__':
    main()
