"""Test measurement semantics, including paths that must refuse qualification."""
import importlib.util
from pathlib import Path
import unittest
import json
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location('timing', Path(__file__).resolve().parents[1]/'tools/analyze_audio_timing.py')
timing = importlib.util.module_from_spec(spec); spec.loader.exec_module(timing)


def campaign(kind='simulated'):
    return dict(kind=kind, backend='virtual', os='test', hardware='synthetic', driver='none',
                sample_rate=48000, channels=2, buffer_frames=32, duration_seconds=1,
                methodology='synthetic test data only', limitations='not physical evidence')


def event(name, time, trial='1', uncertainty='1', clock='monotonic', **kw):
    return dict(event=name, time_ns=str(time), trial=trial, uncertainty_ns=uncertainty, clock=clock, **kw)


class Timing(unittest.TestCase):
    def test_cancellation_events_are_distinct(self):
        m = campaign('physical_electrical'); m['calibration']='test calibration'
        rows = [event('cancel_request',100),event('queue_invalidated',101),event('flush_ack',120),
                event('last_presented',130),event('last_electrical',150)]
        r = timing.analyze(m, rows)['metrics']
        for metric, expected in [('logical_invalidation_ns',1),('backend_ack_ns',20),
                                 ('presentation_tail_ns',30),('electrical_tail_ns',50)]:
            self.assertEqual(r[metric]['distribution']['maximum'],expected)
        self.assertIsNone(r['audible_tail_ns']['distribution'])
        self.assertIsNone(r['physical_round_trip_ns']['distribution'])

    def test_unknown_uncertainty_stays_unknown(self):
        r = timing.analyze(campaign(),[event('capture_anchor',1,uncertainty=''),event('capture_delivery',3)])
        v = r['metrics']['capture_delivery_age_ns']
        self.assertEqual(v['unknown_uncertainty_count'],1)
        self.assertIsNone(v['uncertainty_bound_ns'])

    def test_refuse_unqualified_physical_and_mixed_clocks(self):
        rows = [event('submission',1),event('electrical_capture',5)]
        for m in [campaign(),campaign('physical_electrical')]:
            with self.assertRaises(ValueError): timing.analyze(m,rows)
        m = campaign('physical_electrical'); m['calibration']='test'
        rows[1]['clock']='different'
        with self.assertRaises(ValueError): timing.analyze(m,rows)
        rows[1]['clock']='monotonic'; rows[1]['uncertainty_ns']=''
        with self.assertRaises(ValueError): timing.analyze(m,rows)

    def test_drift_segmented_by_leg_domain_epoch_and_rate(self):
        rows=[]
        for i,(name,domain,epoch,rate,frames) in enumerate([
            ('capture_anchor',1,1,16000,16016),('reference_anchor',2,1,48000,47952),
            ('capture_anchor',1,2,16000,16000)]):
            rows += [event(name,1000000000,trial=f'{i}a',domain=str(domain),epoch=str(epoch),rate=str(rate),frame='0'),
                     event(name,2000000000,trial=f'{i}b',domain=str(domain),epoch=str(epoch),rate=str(rate),frame=str(frames))]
        r=timing.analyze(campaign(),rows)['clock_segments']
        self.assertEqual(len(r),3)
        for item, ppm in zip(r,[1000,-1000,0]): self.assertAlmostEqual(item['observed_ppm'],ppm,places=6)

    def test_deadlines_jitter_and_percentiles(self):
        m=campaign(); m.update(callback_deadline_ns=10,callback_period_ns=100)
        rows=[]
        for i,(start,duration) in enumerate([(0,5),(100,10),(205,12)]):
            rows += [event('callback_begin',start,str(i)),event('callback_end',start+duration,str(i))]
        r=timing.analyze(m,rows)
        self.assertEqual(r['callback_deadline_misses'],1)
        self.assertEqual(r['callback_absolute_jitter_ns']['maximum'],5)
        self.assertEqual(r['metrics']['callback_duration_ns']['distribution'],
                         dict(count=3,median=10,p95=12,p99=12,maximum=12,minimum=5))

    def test_invalid_trace_rejected(self):
        for rows in [[event('flush_ack',1),event('flush_ack',1)],
                     [event('submission',10),event('electrical_output',1)],
                     [event('capture_delivery','nan')]]:
            with self.assertRaises(ValueError): timing.analyze(campaign(),rows)

    def test_cli_retains_hashes_missing_metrics_and_prior_output(self):
        with tempfile.TemporaryDirectory(prefix='hail-timing-test-') as directory:
            root=Path(directory); m=campaign('physical_electrical'); m['calibration']='calibration.json'
            (root/'calibration.json').write_text('{"synthetic_test_only":true}')
            (root/'manifest.json').write_text(json.dumps(m))
            (root/'trace.csv').write_text('trial,event,time_ns,uncertainty_ns,clock,frame,rate,domain,epoch\n'
                '1,submission,100,1,test,,,,\n1,electrical_capture,150,2,test,,,,\n')
            command=[sys.executable,str(Path(timing.__file__)),str(root/'manifest.json'),str(root/'trace.csv'),
                     '--output',str(root/'result.json')]
            r=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(r.returncode,0,r.stderr)
            raw=(root/'result.json').read_bytes(); report=json.loads(raw)
            self.assertEqual(report['metrics']['physical_round_trip_ns']['distribution']['median'],50)
            self.assertEqual(report['metrics']['physical_round_trip_ns']['uncertainty_bound_ns']['maximum'],3)
            self.assertIsNone(report['metrics']['audible_tail_ns']['distribution'])
            self.assertEqual(len(report['manifest']['calibration_sha256']),64)
            self.assertNotEqual(subprocess.run(command,capture_output=True).returncode,0)
            self.assertEqual((root/'result.json').read_bytes(),raw)


if __name__=='__main__': unittest.main()
