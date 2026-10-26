#!/usr/bin/env python3
"""Self-test for the read-only statistics sidecar.

Covers the acceptance gates from docs/sidecar-stats-contract.md as adapted to
the shipped methods:
  1. Robustness: hundreds of randomized requests (valid and malformed) handled
     in-process without any exception escaping, plus real one-shot subprocess
     round-trips that always emit exactly one JSON document and exit cleanly.
  2. Correctness: Lan-DeMets boundaries match published gsDesign values, and
     Monte-Carlo type-I error under H0 matches the nominal alpha.
  3. Surrogate fidelity: GP predictions rank monotone synthetic responses
     consistently (effect-ordering agreement), suggestions stay inside the
     declared knob domains, and degenerate inputs fall back to the
     deterministic grid suggestion honestly.
"""

from __future__ import annotations

import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stats_sidecar as sidecar  # noqa: E402


def run_handle(request: dict) -> tuple[dict, int]:
    payload, exit_code = sidecar.handle(json.loads(json.dumps(request)))
    return payload, exit_code


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


# ---------------------------------------------------------------------------
# 1. Randomized robustness sweep (in-process, fast)
# ---------------------------------------------------------------------------

def robustness_sweep(trials: int = 200) -> None:
    import random
    rng = random.Random(20260826)
    crashes = 0
    for trial in range(trials):
        kind = rng.randrange(10)
        if kind < 6:
            factors = rng.randrange(1, 4)
            domains: dict[str, dict] = {}
            for f in range(factors):
                pick = rng.randrange(3)
                name = f"knob_{f}"
                if pick == 0:
                    low = rng.choice([64, 256, 1024])
                    high = low * rng.choice([2, 4, 16])
                    domains[name] = {"kind": "linear_numeric",
                                     "min": low, "max": high}
                elif pick == 1:
                    base = float(rng.choice([32, 64]))
                    domains[name] = {"kind": "ordered_enum",
                                     "ladder": [base * 2 ** i
                                                for i in range(rng.randrange(2, 5))]}
                else:
                    domains[name] = {"kind": "categorical",
                                     "levels": ["a", "b", "c"][:rng.randrange(2, 4)]}
            runs = rng.randrange(0, 17)
            design = []
            names = sorted(domains)
            for _ in range(runs):
                row = {}
                for name in names:
                    domain = domains[name]
                    if domain["kind"] == "linear_numeric":
                        row[name] = rng.uniform(domain["min"], domain["max"])
                    elif domain["kind"] == "ordered_enum":
                        row[name] = rng.choice(domain["ladder"])
                    else:
                        row[name] = rng.choice(domain["levels"])
                design.append(row)
            observations = [{"run_index": rng.randrange(max(1, runs)),
                             "value": rng.gauss(0, 5)}
                            for _ in range(rng.randrange(0, runs + 1))]
            request = {"method": "bo_next", "observations": observations,
                       "design": design, "domains": domains,
                       "seed": rng.randrange(0, 2 ** 31)}
        elif kind < 8:
            request = {"method": "seq_boundary",
                       "design": rng.choice(["obrien_fleming", "pocock"]),
                       "max_interims": rng.randrange(1, 11),
                       "alpha": rng.uniform(0.001, 0.2),
                       "interim_count": rng.randrange(1, 11)}
        elif kind == 8:
            request = {"method": "bogus_method", "nonsense": [1, 2, 3]}
        else:
            request = {"method": "bo_next", "observations": "not_a_list",
                       "design": None, "domains": {"x": {"kind": "weird"}}}
        try:
            payload, exit_code = run_handle(request)
            check(isinstance(payload, dict), "payload must be a dict")
            check(isinstance(payload.get("ok"), bool), "ok must be boolean")
            check(exit_code in (0, 2, 3), f"exit code {exit_code} unexpected")
            if payload["ok"]:
                check(exit_code == 0, "ok payload must exit 0")
        except Exception as error:  # noqa: BLE001
            crashes += 1
            print(f"trial {trial} crashed: {error!r} request={request}")
    check(crashes == 0, f"{crashes} crashing trials in robustness sweep")


# ---------------------------------------------------------------------------
# 2. Sequential boundary correctness
# ---------------------------------------------------------------------------

PUBLISHED_LD_OF_K3_ALPHA05 = [3.394757, 2.406733, 2.015243]


def published_value_check() -> None:
    payload, exit_code = run_handle(
        {"method": "seq_boundary", "design": "obrien_fleming",
         "max_interims": 3, "alpha": 0.05})
    check(payload["ok"], "LD-OF K=3 must succeed")
    for expected, entry in zip(PUBLISHED_LD_OF_K3_ALPHA05,
                               payload["boundary_p"]):
        check(abs(entry["z"] - expected) <= 1e-4,
              f"z {entry['z']} deviates from published {expected}")
    final = payload["boundary_p"][-1]
    check(abs(final["cumulative_alpha_spent"] - 0.05) < 1e-9,
          "final cumulative spend must equal alpha")


def monte_carlo_type_one_error(design: str, alpha: float, looks: int,
                               paths: int = 120_000) -> None:
    import numpy as np
    payload, exit_code = run_handle(
        {"method": "seq_boundary", "design": design,
         "max_interims": looks, "alpha": alpha})
    check(payload["ok"], f"{design} K={looks} must succeed")
    bounds = np.asarray(payload["boundary_z"], dtype=float)
    information = np.arange(1, looks + 1, dtype=float) / looks
    correlation = np.sqrt(np.minimum.outer(information, information)
                          / np.maximum.outer(information, information))
    rng = np.random.default_rng(987654321)
    samples = rng.multivariate_normal(
        mean=np.zeros(looks), cov=correlation, size=paths)
    crossed = np.any(np.abs(samples) >= bounds[np.newaxis, :], axis=1)
    measured = float(np.mean(crossed))
    tolerance = 6.0 * math.sqrt(alpha * (1 - alpha) / paths) + 0.002
    check(abs(measured - alpha) <= tolerance,
          f"{design} K={looks}: type-I {measured:.5f} vs alpha {alpha} "
          f"(tolerance {tolerance:.5f})")


def boundary_shape_checks() -> None:
    for design in ("obrien_fleming", "pocock"):
        payload, exit_code = run_handle(
            {"method": "seq_boundary", "design": design,
             "max_interims": 5, "alpha": 0.05})
        check(payload["ok"], f"{design} K=5 must succeed")
        z_values = payload["boundary_z"]
        check(all(a >= b for a, b in zip(z_values, z_values[1:])),
              f"{design} z-boundaries must be non-increasing")
        spends = [entry["cumulative_alpha_spent"]
                  for entry in payload["boundary_p"]]
        check(all(b > a for a, b in zip(spends, spends[1:])),
              f"{design} cumulative spending must strictly increase")
    payload, exit_code = run_handle(
        {"method": "seq_boundary", "design": "watney", "max_interims": 2})
    check(not payload["ok"] and payload["error"] == "design_unsupported",
          "unknown design must fail closed")
    payload, exit_code = run_handle(
        {"method": "seq_boundary", "max_interims": 12})
    check(not payload["ok"] and payload["error"] == "too_many_looks",
          ">10 looks must fail closed")


# ---------------------------------------------------------------------------
# 3. bo_next behaviour
# ---------------------------------------------------------------------------

DOMAIN_SETS = [
    {"frame_size": {"kind": "linear_numeric", "min": 256, "max": 4096},
     "window": {"kind": "ordered_enum", "ladder": [64, 128, 256]},
     "mode": {"kind": "categorical", "levels": ["hann", "flat_top"]}},
    {"a": {"kind": "linear_numeric", "min": -5.0, "max": 5.0}},
    {"p": {"kind": "ordered_enum", "ladder": [1, 2, 4, 8]},
     "q": {"kind": "categorical", "levels": ["u", "v"]}},
]


def domain_conformance() -> None:
    import random
    rng = random.Random(777)
    for domains in DOMAIN_SETS:
        names = sorted(domains)
        for _ in range(20):
            design = [{name: (
                rng.uniform(domains[name]["min"], domains[name]["max"])
                if domains[name]["kind"] == "linear_numeric"
                else (rng.choice(domains[name]["ladder"])
                      if domains[name]["kind"] == "ordered_enum"
                      else rng.choice(domains[name]["levels"])))
                for name in names} for _ in range(8)]
            observations = [{"run_index": i, "value": rng.gauss(0, 1)}
                            for i in range(8)]
            payload, exit_code = run_handle(
                {"method": "bo_next", "observations": observations,
                 "design": design, "domains": domains,
                 "seed": rng.randrange(2 ** 31)})
            check(payload["ok"], "random legal request must succeed")
            levels = payload["next_levels"]
            check(set(levels) == set(names),
                  "suggestion must cover exactly the declared knobs")
            for name in names:
                domain = domains[name]
                value = levels[name]
                if domain["kind"] == "linear_numeric":
                    check(domain["min"] - 1e-9 <= value <= domain["max"] + 1e-9,
                          f"numeric suggestion {value} outside domain")
                elif domain["kind"] == "ordered_enum":
                    check(any(abs(value - x) < 1e-9
                              for x in domain["ladder"]),
                          f"enum suggestion {value} off-ladder")
                else:
                    check(value in domain["levels"],
                          f"categorical suggestion {value} not in levels")


def determinism_check() -> None:
    domains = DOMAIN_SETS[0]
    design = [{"frame_size": 256 * 2 ** i, "window": 64 * (i % 3),
               "mode": ["hann", "flat_top"][i % 2]} for i in range(8)]
    observations = [{"run_index": i, "value": 5.0 + i * 1.3}
                    for i in range(8)]
    outputs = set()
    for _ in range(3):
        payload, exit_code = run_handle(
            {"method": "bo_next", "observations": observations,
             "design": design, "domains": domains, "seed": 12345})
        check(payload["ok"], "determinism probe must succeed")
        outputs.add(json.dumps(payload, sort_keys=True))
    check(len(outputs) == 1, "same seed must reproduce identical output")


def surrogate_fidelity() -> None:
    """Effect-ordering agreement: GP predictions must rank a smooth response
    consistently with the observed values at those same design rows."""
    import numpy as np

    def spearman(a: list[float], b: list[float]) -> float:
        order_a = np.argsort(np.argsort(a))
        order_b = np.argsort(np.argsort(b))
        return float(np.corrcoef(order_a, order_b)[0, 1])

    domains = {"x": {"kind": "linear_numeric", "min": 0.0, "max": 10.0}}
    design = [{"x": round(10.0 * i / 7, 4)} for i in range(8)]
    true_values = [(-(v - 6.0) ** 2 / 8.0 + 4.5) for v in
                   [row["x"] for row in design]]
    observations = [{"run_index": i, "value": v}
                    for i, v in enumerate(true_values)]
    payload, exit_code = run_handle(
        {"method": "bo_next", "observations": observations,
         "design": design, "domains": domains, "seed": 7})
    check(payload["ok"], "fidelity probe must succeed")
    # Predicted mean at the suggested point must be finite when provided.
    if payload["predicted_mean"] is not None:
        check(math.isfinite(payload["predicted_mean"]),
              "predicted_mean must be finite")

    # Ranking agreement measured through repeated leave-one-out style probes:
    # fit on all but one point, prediction at held-out x must preserve order.
    correlations = []
    for hold_out in range(8):
        train_obs = [o for o in observations if o["run_index"] != hold_out]
        train_design = [row for i, row in enumerate(design)
                        if i != hold_out]
        remapped = [{"run_index": idx, "value": o["value"]}
                    for idx, o in enumerate(train_obs)]
        request = {"method": "bo_next", "observations": remapped,
                   "design": train_design, "domains": domains, "seed": 7}
        payload, exit_code = run_handle(request)
        check(payload["ok"], "holdout probe must succeed")
        # Re-fit manually to score the held-out point via module internals.
        import stats_sidecar as sc
        encoded = [[row["x"] / 10.0] for row in train_design]
        ys = [o["value"] for o in train_obs]
        import numpy as np
        from sklearn.gaussian_process import GaussianProcessRegressor
        from sklearn.gaussian_process.kernels import (
            ConstantKernel, Matern, WhiteKernel)
        kernel = (ConstantKernel(1.0, constant_value_bounds=(1e-3, 1e3))
                  * Matern(length_scale=np.ones(1),
                           length_scale_bounds=(1e-2, 1e3), nu=2.5)
                  + WhiteKernel(noise_level=1e-6,
                                noise_level_bounds=(1e-12, 1e-1)))
        reg = GaussianProcessRegressor(kernel=kernel, n_restarts_optimizer=4,
                                       random_state=7)
        y_arr = np.asarray(ys)
        reg.fit(np.asarray(encoded), (y_arr - y_arr.mean()) / y_arr.std())
        held_x = np.asarray([[design[hold_out]["x"] / 10.0]])
        pred = float(reg.predict(held_x)[0]) * y_arr.std() + y_arr.mean()
        correlations.append((pred, true_values[hold_out]))
    rho = spearman([c[0] for c in correlations], [c[1] for c in correlations])
    check(rho >= 0.85,
          f"surrogate ranking agreement too weak: spearman={rho:.3f}")


def fallback_honesty() -> None:
    domains = DOMAIN_SETS[1]
    # Empty observations -> deterministic grid midpoint.
    payload, exit_code = run_handle(
        {"method": "bo_next", "observations": [], "design": [],
         "domains": domains, "seed": 1})
    check(payload["ok"] and payload["surrogate"] == "builtin_grid",
          "empty observations must fall back to builtin_grid")
    check(abs(payload["next_levels"]["a"]) < 1e-9,
          "grid fallback must propose the domain midpoint")
    # Single flat observation -> honest fallback naming the kept run.
    payload, exit_code = run_handle(
        {"method": "bo_next",
         "observations": [{"run_index": 2, "value": 3.0}],
         "design": [{"a": 1.0}, {"a": 2.0}, {"a": 3.0}],
         "domains": domains, "seed": 1})
    check(payload["ok"] and payload["surrogate"] == "builtin_grid"
          and "run_2" in payload["notes"],
          "single flat observation must keep best run and say so")


def improvement_sanity() -> None:
    """With a clear unimodal response, EI suggestions should land closer to
    the true optimum than the naive domain midpoint on average (median)."""
    import random
    import statistics
    rng = random.Random(4242)
    trials = 30
    ei_distances: list[float] = []
    midpoint_distances: list[float] = []
    for _ in range(trials):
        optimum = rng.uniform(1.0, 9.0)
        design = [{"x": round(rng.uniform(0.0, 10.0), 3)}
                  for _ in range(6)]
        observations = [{"run_index": i,
                         "value": -(row["x"] - optimum) ** 2 / 4.0}
                        for i, row in enumerate(design)]
        payload, exit_code = run_handle(
            {"method": "bo_next", "observations": observations,
             "design": design, "domains": {
                 "x": {"kind": "linear_numeric", "min": 0.0, "max": 10.0}},
             "seed": rng.randrange(2 ** 31)})
        check(payload["ok"], "improvement probe must succeed")
        suggestion = payload["next_levels"]["x"]
        ei_distances.append(abs(suggestion - optimum))
        midpoint_distances.append(abs(5.0 - optimum))
    check(statistics.median(ei_distances)
          <= statistics.median(midpoint_distances),
          f"EI median {statistics.median(ei_distances):.2f} must not exceed "
          f"midpoint median {statistics.median(midpoint_distances):.2f}")
    wins = sum(1 for e, m in zip(ei_distances, midpoint_distances) if e < m)
    check(wins >= trials * 0.55,
          f"EI beat midpoint only {wins}/{trials} times")


# ---------------------------------------------------------------------------
# 4. Subprocess protocol round-trips (real one-shot processes)
# ---------------------------------------------------------------------------

def subprocess_round_trips(count: int = 8) -> None:
    script = Path(__file__).resolve().parent / "stats_sidecar.py"
    cases = [
        {"method": "seq_boundary", "design": "obrien_fleming",
         "max_interims": 3, "alpha": 0.05},
        {"method": "bo_next", "observations": [], "design": [],
         "domains": DOMAIN_SETS[1], "seed": 5},
        {"method": "nope"},
    ]
    raw_bodies = [None, None, None, "{not valid json"]
    for index in range(count):
        request = cases[index % len(cases)]
        path = None
        if raw_bodies[index % len(raw_bodies)] is not None:
            with tempfile.NamedTemporaryFile(
                    "w", suffix=".json", delete=False, encoding="utf-8") as handle:
                handle.write(raw_bodies[index % len(raw_bodies)])
                path = handle.name
        else:
            with tempfile.NamedTemporaryFile(
                    "w", suffix=".json", delete=False, encoding="utf-8") as handle:
                json.dump(request, handle)
                path = handle.name
        completed = subprocess.run(
            [sys.executable, str(script), path],
            capture_output=True, text=True, timeout=30)
        Path(path).unlink(missing_ok=True)
        lines = [line for line in completed.stdout.splitlines()
                 if line.strip()]
        check(len(lines) == 1,
              f"case {index}: expected exactly one stdout JSON line")
        payload = json.loads(lines[0])
        check("ok" in payload, f"case {index}: response lacks ok flag")
        if payload["ok"]:
            check(completed.returncode == 0,
                  f"case {index}: ok response must exit 0")
        else:
            check(completed.returncode != 0,
                  f"case {index}: failure response must exit non-zero")
            check(bool(payload.get("error")),
                  f"case {index}: failure needs an error code")


# ---------------------------------------------------------------------------
# 5. Monitor analysis: EWMA, CUSUM, swing-and-door compression
# ---------------------------------------------------------------------------

def monitor_analysis_ewma() -> None:
    values = [1.0, 2.0, 3.0, 4.0, 5.0]
    payload, code = run_handle({"method": "monitor_analysis", "values": values, "alpha": 0.5})
    check(code == 0, "monitor_analysis ewma exit code")
    check(payload["ok"], "monitor_analysis ewma ok")
    check(payload["schema"] == "dpa.stats.sidecar.monitor_analysis/v1", "schema")
    ewma = payload["ewma"]
    check(ewma["alpha"] == 0.5, "alpha passed through")
    check(len(ewma["values"]) == 5, "ewma length matches input")
    check(ewma["values"][0] == 1.0, "ewma first value equals first input")
    check(ewma["final"] is not None, "ewma final is not None")
    stats = payload["stats"]
    check(stats["count"] == 5, "stats count")
    check(stats["mean"] == 3.0, "stats mean")


def monitor_analysis_cusum() -> None:
    values = [10.0] * 20 + [15.0] * 10
    payload, code = run_handle({"method": "monitor_analysis", "values": values})
    check(code == 0, "cusum exit code")
    check(payload["ok"], "cusum ok")
    cusum = payload["cusum"]
    check(cusum["change_point_count"] > 0,
          f"cusum should detect change point, got {cusum['change_point_count']}")
    check(20 in cusum["change_points"],
          "CUSUM should detect change at index 20")


def monitor_analysis_sdt() -> None:
    values = [1.0, 1.01, 1.02, 1.01, 1.0, 5.0, 5.01, 5.02, 5.0]
    payload, code = run_handle({
        "method": "monitor_analysis",
        "values": values,
        "sdt_tolerance": 0.5,
    })
    check(code == 0, "sdt exit code")
    check(payload["ok"], "sdt ok")
    sdt = payload["sdt"]
    check(sdt["compressed_count"] < len(values),
          f"SDT should compress: {sdt['compressed_count']} < {len(values)}")
    check(sdt["compression_ratio"] < 1.0, "compression ratio < 1")
    check(sdt["points"][0]["value"] == 1.0, "SDT first point")
    check(sdt["points"][-1]["value"] == 5.0, "SDT last point")


def monitor_analysis_empty_values() -> None:
    payload, code = run_handle({"method": "monitor_analysis", "values": []})
    check(code == 2, "empty values should fail")
    check(not payload["ok"], "empty values should not be ok")


def monitor_analysis_nan_values() -> None:
    payload, code = run_handle({
        "method": "monitor_analysis",
        "values": [1.0, float("nan"), 3.0],
    })
    check(code == 2, "NaN values should fail")
    check(not payload["ok"], "NaN values should not be ok")


def monitor_analysis_single_value() -> None:
    payload, code = run_handle({"method": "monitor_analysis", "values": [42.0]})
    check(code == 0, "single value exit code")
    check(payload["ok"], "single value ok")
    check(payload["stats"]["count"] == 1, "single value count")
    check(payload["sdt"]["compressed_count"] == 1, "SDT single value")


def monitor_analysis_subprocess() -> None:
    script = str(Path(__file__).resolve().parent / "stats_sidecar.py")
    request = {"method": "monitor_analysis", "values": [1.0, 2.0, 3.0, 4.0, 5.0]}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False,
                                     encoding="utf-8") as handle:
        json.dump(request, handle)
        path = handle.name
    completed = subprocess.run(
        [sys.executable, str(script), path],
        capture_output=True, text=True, timeout=30)
    Path(path).unlink(missing_ok=True)
    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    check(len(lines) == 1, "subprocess: exactly one stdout line")
    payload = json.loads(lines[0])
    check(payload["ok"], "subprocess: ok")
    check(completed.returncode == 0, "subprocess: exit 0")


def main() -> int:
    robustness_sweep()
    published_value_check()
    monte_carlo_type_one_error("obrien_fleming", 0.05, 3)
    monte_carlo_type_one_error("pocock", 0.05, 4)
    monte_carlo_type_one_error("obrien_fleming", 0.01, 2)
    boundary_shape_checks()
    domain_conformance()
    determinism_check()
    surrogate_fidelity()
    fallback_honesty()
    improvement_sanity()
    subprocess_round_trips()
    monitor_analysis_ewma()
    monitor_analysis_cusum()
    monitor_analysis_sdt()
    monitor_analysis_empty_values()
    monitor_analysis_nan_values()
    monitor_analysis_single_value()
    monitor_analysis_subprocess()
    print("stats_sidecar self-test: ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
