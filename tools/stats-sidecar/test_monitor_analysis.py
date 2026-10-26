"""Independent numerical and protocol checks for bounded offline monitoring."""
import contextlib
import io
import json
import math
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
import stats_sidecar as sidecar


class MonitorAnalysisTests(unittest.TestCase):
    def analyze(self, values, **kwargs):
        payload, code = sidecar.handle({"method":"monitor_analysis", "values":values, **kwargs})
        self.assertEqual(code, 0, payload)
        json.dumps(payload, allow_nan=False)
        return payload

    def test_impulse_is_preserved(self):
        result = self.analyze([0,100,0], timestamps_ms=[0,1000,2000], tolerance=0.5)
        self.assertEqual([p["value"] for p in result["compression"]["points"]], [0,100,0])
        self.assertEqual(result["compression"]["max_absolute_error"], 0)

    def test_noise_and_strict_threshold(self):
        result = self.analyze([0,0.01,-0.01], cusum_target=0, cusum_drift=0, cusum_threshold=1)
        self.assertEqual(result["cusum"]["threshold_exceedance_indices"], [])
        result = self.analyze([0,1,1], cusum_target=0, cusum_drift=0, cusum_threshold=1)
        self.assertEqual(result["cusum"]["threshold_exceedance_indices"], [2])
        self.assertEqual(self.analyze([100]*20)["cusum"]["status"], "not_evaluated")

    def test_nist_published_example(self):
        # NIST 6.3.2.3: first exceedance is sample 14, zero-based index 13.
        values = [324.925,324.675,324.725,324.350,325.350,325.225,324.125,324.525,
                  325.225,324.600,324.625,325.150,328.325,327.250,327.825,328.500,
                  326.675,327.775,326.875,328.350]
        for data, final in [(values,"s_high_final"), ([650-x for x in values],"s_low_final")]:
            result = self.analyze(data, cusum_target=325, cusum_drift=0.3175, cusum_threshold=4.1959)
            self.assertEqual(result["cusum"]["crossing_indices"], [13])
            self.assertAlmostEqual(result["cusum"][final], 19.035, places=9)

    def test_ewma_and_linear_irregular_trend(self):
        self.assertEqual(self.analyze([1,2,3,4,5], alpha=0.5)["ewma"]["values"],
                         [1,1.5,2.25,3.125,4.0625])
        times = [0,1,4,9,20]
        result = self.analyze([2*t+1 for t in times], timestamps_ms=times, tolerance=0)
        self.assertEqual(result["compression"]["compressed_count"], 2)

    def test_randomized_reconstruction(self):
        rng = random.Random(20260910)
        for _ in range(400):
            length, times, values = rng.randint(1,256), [0], [rng.uniform(-10,10)]
            for i in range(1,length):
                times.append(times[-1]+rng.randint(1,1000))
                values.append(values[-1]+rng.uniform(-2,2))
            tolerance = rng.choice([0,0.01,0.5,3])
            points = self.analyze(values,timestamps_ms=times,tolerance=tolerance)["compression"]["points"]
            self.assertEqual((points[0]["index"],points[-1]["index"]), (0,length-1))
            for left,right in zip(points,points[1:]):
                self.assertLess(left["index"],right["index"])
                for i in range(left["index"]+1,right["index"]):
                    fraction = (times[i]-left["time"])/(right["time"]-left["time"])
                    prediction = left["value"]+fraction*(right["value"]-left["value"])
                    self.assertLessEqual(abs(prediction-values[i]),tolerance)

    def test_bad_inputs(self):
        bad = [{"values":v} for v in [[],[0]*257,[True],["1"],[None],[math.nan],[math.inf],[1e101]]]
        bad += [{"alpha":v} for v in [0,-1,2,True,"0.2",None,math.nan]]
        bad += [{"tolerance":v} for v in [-1,True,"1",None,math.inf]]
        bad += [{"timestamps_ms":v} for v in [[0],[0,0],[1,0],[0,True],[0,1.5],[0,"1"],[0,2**53]]]
        bad += [{"cusum_target":0}, {"cusum_target":0,"cusum_drift":0,"cusum_threshold":0}]
        for fragment in bad:
            with self.subTest(fragment=fragment):
                payload,code = sidecar.handle({"method":"monitor_analysis","values":[0,1],**fragment})
                self.assertNotEqual(code,0)
                self.assertFalse(payload["ok"])

    def test_time_basis(self):
        self.assertEqual(self.analyze([42])["time_basis"],"sample_index")
        self.assertEqual(self.analyze([42],timestamps_ms=[-1])["time_basis"],"timestamp_ms")

    def test_output_protocol(self):
        for payload in [{"ok":True,"value":math.nan},{"ok":True,"text":"测"*30000}]:
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertFalse(sidecar._emit(payload))
            self.assertFalse(json.loads(output.getvalue())["ok"])

    def test_subprocess(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)/"request.json"
            path.write_text(json.dumps({"method":"monitor_analysis","values":[0,100,0]}))
            proc = subprocess.run([sys.executable,str(Path(sidecar.__file__)),str(path)],
                                  capture_output=True,text=True,timeout=10)
        self.assertEqual(proc.returncode,0,proc.stderr)
        self.assertEqual(len(proc.stdout.splitlines()),1)
        self.assertTrue(json.loads(proc.stdout)["ok"])


if __name__ == "__main__":
    unittest.main()
