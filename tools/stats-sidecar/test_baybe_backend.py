"""Exercise the real optional library; absence/failure is not a passing skip."""
import copy
import socket
import unittest
from unittest.mock import patch
from stats_sidecar import run_bo_next


def request():
    return {"method": "bo_next", "backend": "baybe", "seed": 42, "candidate_budget": 32,
            "domains": {"x": {"kind": "linear_numeric", "min": 0, "max": 16},
                        "mode": {"kind": "categorical", "levels": ["a", "b"]}},
            "design": [{"x": 0, "mode": "a"}, {"x": 16, "mode": "b"}, {"x": 4, "mode": "a"}],
            "observations": [{"run_index": 0, "value": -49}, {"run_index": 1, "value": -81},
                             {"run_index": 2, "value": -9}]}


class BaybeTests(unittest.TestCase):
    def test_real_library_offline_domain_and_reproducibility(self):
        req = request()
        before = copy.deepcopy(req)
        # Any outgoing network call during import/recommendation fails the test.
        with patch.object(socket.socket, "connect", side_effect=AssertionError("network_forbidden")):
            result, code = run_bo_next(req)
            repeated, repeated_code = run_bo_next(req)
        self.assertEqual(code, 0, result)
        self.assertEqual(repeated_code, 0, repeated)
        self.assertEqual(result["surrogate"], "baybe_gp")
        self.assertEqual(result["next_levels"], repeated["next_levels"])
        self.assertEqual(set(result["next_levels"]), {"x", "mode"})
        self.assertTrue(0 <= result["next_levels"]["x"] <= 16)
        self.assertIn(result["next_levels"]["mode"], ["a", "b"])
        self.assertNotIn(result["next_levels"], req["design"])
        self.assertEqual(req, before)

    def test_minimize_and_target_semantics(self):
        maximize = request()
        minimized = copy.deepcopy(maximize)
        minimized["direction"] = "minimize"
        for observation in minimized["observations"]:
            observation["value"] *= -1
        target = copy.deepcopy(minimized)
        target.update(direction="target", target=0.)
        results = [run_bo_next(req) for req in [maximize, minimized, target]]
        for result, code in results:
            self.assertEqual(code, 0, result)
        self.assertEqual(results[0][0]["next_levels"], results[1][0]["next_levels"])
        self.assertEqual(results[1][0]["next_levels"], results[2][0]["next_levels"])


if __name__ == "__main__":
    unittest.main()
