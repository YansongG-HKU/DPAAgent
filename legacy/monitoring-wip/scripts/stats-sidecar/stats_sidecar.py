#!/usr/bin/env python3
"""Read-only statistics sidecar for governed experiment campaigns.

One-shot external process, mirroring the verified measurement-adapter
precedent: the caller passes a request JSON file path as argv[1], the sidecar
prints exactly one response JSON document on stdout and exits.  Any failure is
a machine-readable ``{"ok": false, "error": code}`` with a non-zero exit code;
the process never crashes and never emits partial output.

Heavy statistics are delegated to mature maintained libraries (scikit-learn's
Gaussian-process regression, SciPy's multivariate normal primitives); this
module only composes published algorithms on top of them and never invents
distribution tables or optimizers.

Methods:
  bo_next       Bayesian-optimization refinement suggestion over whitelisted
                campaign knob domains (GP Matern-5/2 surrogate + expected
                improvement).
  seq_boundary  Lan-DeMets alpha-spending group-sequential boundaries
                (O'Brien-Fleming or Pocock spending) via recursive MVN
                integration.
  vibe_analyze  Time-domain / FFT / envelope vibration analysis via viblearn
                (ISO 10816-3 severity, bearing fault frequencies, optional PSD
                and envelope demodulation).
"""

from __future__ import annotations

import json
import math
import sys
import warnings
from pathlib import Path
from typing import Any

MAX_REQUEST_BYTES = 256 * 1024
MAX_RESPONSE_CHARS = 64 * 1024
MAX_OBSERVATIONS = 4096
MAX_FACTORS = 8
MAX_CANDIDATE_BUDGET = 1024
MAX_LOOKS = 10


def _emit(payload: dict[str, Any]) -> None:
    text = json.dumps(payload, ensure_ascii=False, sort_keys=True)
    if len(text) > MAX_RESPONSE_CHARS:
        text = json.dumps({"schema": "dpa.stats.sidecar.error", "version": 1,
                           "ok": False, "error": "response_too_large",
                           "message": "response exceeded the bounded limit"},
                          ensure_ascii=False, sort_keys=True)
    print(text)


def _error(code: str, message: str, exit_code: int = 2) -> int:
    _emit({"schema": "dpa.stats.sidecar.error", "version": 1,
           "ok": False, "error": code, "message": message})
    return exit_code


def _finite(value: Any) -> bool:
    try:
        return math.isfinite(float(value))
    except (TypeError, ValueError):
        return False


# ---------------------------------------------------------------------------
# Shared request validation
# ---------------------------------------------------------------------------

def _validate_common(request: dict[str, Any]) -> str:
    if not isinstance(request.get("method"), str):
        return "method_missing"
    observations = request.get("observations")
    if observations is not None:
        if not isinstance(observations, list) or len(observations) > MAX_OBSERVATIONS:
            return "observations_invalid"
        for entry in observations:
            if not isinstance(entry, dict):
                return "observations_invalid"
            if not isinstance(entry.get("run_index"), int) or isinstance(
                    entry.get("run_index"), bool):
                return "observations_invalid"
            if not _finite(entry.get("value")):
                return "observations_invalid"
    return ""


# ---------------------------------------------------------------------------
# Method 1: bo_next — GP surrogate + expected improvement
# ---------------------------------------------------------------------------

def _encode_factor(value: Any, domain: dict[str, Any]) -> list[float] | None:
    kind = domain.get("kind")
    try:
        if kind == "linear_numeric":
            number = float(value)
            if not math.isfinite(number):
                return None
            low = float(domain["min"])
            high = float(domain["max"])
            if high <= low:
                return [0.0]
            return [(number - low) / (high - low)]
        if kind == "ordered_enum":
            ladder = [float(x) for x in domain["ladder"]]
            number = float(value)
            best = min(range(len(ladder)),
                       key=lambda i: abs(ladder[i] - number))
            return [best / max(1, len(ladder) - 1)]
        if kind == "categorical":
            levels = list(domain["levels"])
            text = str(value)
            if text not in levels:
                return None
            onehot = [0.0] * len(levels)
            onehot[levels.index(text)] = 1.0
            return onehot
    except (KeyError, TypeError, ValueError):
        return None
    return None


def _factor_axis(domain: dict[str, Any]) -> list[list[float]]:
    """Encoded coordinate candidates for one knob."""
    kind = domain.get("kind")
    if kind == "linear_numeric":
        low, high = float(domain["min"]), float(domain["max"])
        span = max(1e-12, high - low)
        steps = 33
        return [[(low + (high - low) * i / (steps - 1) - low) / span]
                for i in range(steps)]
    if kind == "ordered_enum":
        ladder_count = len(list(domain["ladder"]))
        denominator = max(1, ladder_count - 1)
        return [[i / denominator] for i in range(ladder_count)]
    if kind == "categorical":
        count = len(list(domain["levels"]))
        return [[1.0 if j == i else 0.0 for j in range(count)]
                for i in range(count)]
    raise ValueError("domain_kind_invalid")


def _decode_factor(coordinates: list[float], domain: dict[str, Any],
                   integral_numeric: bool) -> Any:
    kind = domain.get("kind")
    if kind == "linear_numeric":
        low = float(domain["min"])
        high = float(domain["max"])
        value = low + max(0.0, min(1.0, coordinates[0])) * (high - low)
        return float(round(value)) if integral_numeric else value
    if kind == "ordered_enum":
        ladder = [float(x) for x in domain["ladder"]]
        index = max(0, min(len(ladder) - 1,
                           int(round(coordinates[0] * max(1, len(ladder) - 1)))))
        value = ladder[index]
        return float(int(value)) if float(value).is_integer() else value
    if kind == "categorical":
        levels = list(domain["levels"])
        index = max(0, min(len(levels) - 1,
                           int(round(max(range(len(coordinates)),
                                         key=lambda j: coordinates[j])))))
        return levels[index]
    return None


def _grid_suggestion(domains: dict[str, dict[str, Any]],
                     observations: list[dict[str, Any]]) -> tuple[Any, str]:
    """Deterministic fallback used for empty/single/flat observation sets."""
    midpoint: dict[str, Any] = {}
    for name in sorted(domains):
        domain = domains[name]
        kind = domain.get("kind")
        if kind == "linear_numeric":
            low, high = float(domain["min"]), float(domain["max"])
            value = (low + high) / 2.0
            midpoint[name] = (float(round(value))
                              if float(value).is_integer() else value)
        elif kind == "ordered_enum":
            ladder = [float(x) for x in domain["ladder"]]
            value = ladder[len(ladder) // 2]
            midpoint[name] = (float(int(value))
                              if float(value).is_integer() else value)
        else:
            levels = list(domain["levels"])
            midpoint[name] = levels[len(levels) // 2]
    if not observations:
        return midpoint, "empty_observations_deterministic_grid"
    best = max(observations, key=lambda entry: float(entry["value"]))
    note = f"flat_or_single_observation_kept_best_run_{int(best['run_index'])}"
    return midpoint, note


def _candidate_grid(names: list[str], domains: dict[str, dict[str, Any]],
                    budget: int, rng) -> list[dict[str, list[float]]]:
    from math import prod
    axes = {name: _factor_axis(domains[name]) for name in names}
    total = prod(len(axes[name]) for name in names)

    candidates: list[dict[str, list[float]]] = []

    def walk(index: int, current: dict[str, list[float]]) -> None:
        if len(candidates) >= budget:
            return
        if index == len(names):
            candidates.append(dict(current))
            return
        name = names[index]
        order = list(range(len(axes[name])))
        if total > budget:
            order = sorted(rng.permutation(len(axes[name])).tolist())
        for position in order:
            current[name] = axes[name][position]
            walk(index + 1, current)
            if len(candidates) >= budget:
                return

    walk(0, {})
    return candidates


def run_bo_next(request: dict[str, Any]) -> tuple[dict[str, Any], int]:
    observations = request.get("observations") or []
    design_rows = request.get("design")
    domains = request.get("domains")
    if not isinstance(domains, dict) or not domains or len(domains) > MAX_FACTORS:
        return {"ok": False, "error": "domains_invalid"}, 2
    if not isinstance(design_rows, list) or len(design_rows) > MAX_OBSERVATIONS:
        return {"ok": False, "error": "design_invalid"}, 2

    integral_numeric: dict[str, bool] = {}
    for name, domain in domains.items():
        if not isinstance(name, str) or not isinstance(domain, dict):
            return {"ok": False, "error": "domains_invalid"}, 2
        kind = domain.get("kind")
        if kind == "linear_numeric":
            if not (_finite(domain.get("min")) and _finite(domain.get("max"))
                    and float(domain["max"]) > float(domain["min"])):
                return {"ok": False, "error": "domains_invalid"}, 2
            integral_numeric[name] = (float(domain["min"]).is_integer()
                                      and float(domain["max"]).is_integer())
        elif kind == "ordered_enum":
            ladder = domain.get("ladder")
            if (not isinstance(ladder, list) or len(ladder) < 2
                    or not all(_finite(x) for x in ladder)):
                return {"ok": False, "error": "domains_invalid"}, 2
        elif kind == "categorical":
            levels = domain.get("levels")
            if (not isinstance(levels, list) or len(levels) < 2
                    or not all(isinstance(x, str) for x in levels)):
                return {"ok": False, "error": "domains_invalid"}, 2
        else:
            return {"ok": False, "error": "domains_invalid"}, 2

    names = sorted(domains)
    encoded_by_run: dict[int, list[float]] = {}
    for index, row in enumerate(design_rows):
        if not isinstance(row, dict):
            continue
        vector: list[float] = []
        usable_row = True
        for name in names:
            coordinates = _encode_factor(row.get(name), domains[name])
            if coordinates is None:
                usable_row = False
                break
            vector.extend(coordinates)
        if usable_row:
            encoded_by_run[index] = vector

    paired_x: list[list[float]] = []
    paired_y: list[float] = []
    for entry in observations:
        run_index = int(entry["run_index"])
        if run_index in encoded_by_run:
            paired_x.append(encoded_by_run[run_index])
            paired_y.append(float(entry["value"]))

    variance = 0.0
    if len(paired_y) >= 2:
        mean_y = sum(paired_y) / len(paired_y)
        variance = sum((y - mean_y) ** 2
                       for y in paired_y) / (len(paired_y) - 1)
    if len(paired_y) < 2 or variance <= 1e-15:
        suggestion, note = _grid_suggestion(domains, observations)
        return {"ok": True, "schema": "dpa.stats.sidecar.bo_next/v1",
                "next_levels": suggestion, "predicted_mean": None,
                "predicted_std": None, "surrogate": "builtin_grid",
                "notes": note}, 0

    try:
        import numpy as np
        from scipy.stats import norm
        from sklearn.gaussian_process import GaussianProcessRegressor
        from sklearn.gaussian_process.kernels import (
            ConstantKernel, Matern, WhiteKernel)
    except ImportError:
        return {"ok": False, "error": "runtime_missing",
                "message": "numpy/scipy/scikit-learn are required"}, 3

    seed = request.get("seed")
    if not isinstance(seed, int) or isinstance(seed, bool) or seed < 0:
        seed = 0
    budget = request.get("candidate_budget", 256)
    if not isinstance(budget, int) or isinstance(budget, bool) or budget <= 0:
        budget = 256
    budget = min(budget, MAX_CANDIDATE_BUDGET)

    try:
        x_train = np.asarray(paired_x, dtype=float)
        y_train = np.asarray(paired_y, dtype=float)
        y_center = float(np.mean(y_train))
        y_scale = float(np.std(y_train)) or 1.0
        kernel = (ConstantKernel(1.0, constant_value_bounds=(1e-3, 1e3))
                  * Matern(length_scale=np.ones(x_train.shape[1]),
                           length_scale_bounds=(1e-2, 1e3), nu=2.5)
                  + WhiteKernel(noise_level=1e-6,
                                noise_level_bounds=(1e-12, 1e-1)))
        regressor = GaussianProcessRegressor(
            kernel=kernel, normalize_y=False, n_restarts_optimizer=4,
            random_state=int(seed % (2 ** 31)))
        regressor.fit(x_train, (y_train - y_center) / y_scale)
    except Exception:  # noqa: BLE001 - fail closed, never crash
        return {"ok": False, "error": "gp_failed",
                "message": "surrogate fit failed"}, 2

    try:
        rng = np.random.default_rng(seed % (2 ** 32))
        candidates = _candidate_grid(names, domains, budget, rng)
        rows = [[c[name] for name in names] for c in candidates]
        matrix = np.asarray([np.concatenate(row) for row in rows], dtype=float)
        mean, std = regressor.predict(matrix, return_std=True)
        mean = np.asarray(mean, dtype=float).ravel() * y_scale + y_center
        std = np.maximum(np.asarray(std, dtype=float).ravel() * y_scale, 1e-12)
        best_observed = float(np.max(y_train))
        improvement = mean - best_observed
        z = improvement / std
        expected_improvement = improvement * norm.cdf(z) + std * norm.pdf(z)
        choice = int(np.argmax(expected_improvement))
        chosen = candidates[choice]
        next_levels: dict[str, Any] = {}
        for name in names:
            next_levels[name] = _decode_factor(
                chosen[name], domains[name], integral_numeric.get(name, False))
        return {"ok": True, "schema": "dpa.stats.sidecar.bo_next/v1",
                "next_levels": next_levels,
                "predicted_mean": round(float(mean[choice]), 9),
                "predicted_std": round(float(std[choice]), 9),
                "surrogate": "gp_matern52",
                "notes": f"ei_over_{len(candidates)}_candidates;"
                         f"{len(paired_y)}_paired_observations"}, 0
    except Exception:  # noqa: BLE001 - fail closed, never crash
        return {"ok": False, "error": "bo_failed",
                "message": "acquisition optimization failed"}, 2


# ---------------------------------------------------------------------------
# Method 2: seq_boundary — Lan-DeMets alpha spending boundaries
# ---------------------------------------------------------------------------

def _spending(design: str, t: float, alpha: float) -> float:
    from scipy.stats import norm
    if design == "obrien_fleming":
        # Lan-DeMets O'Brien-Fleming spending function:
        # alpha*(t) = 2*(1 - Phi(Phi^{-1}(1 - alpha/2)/sqrt(t))),
        # expressed for a total two-sided budget alpha.
        center_quantile = norm.ppf(1.0 - alpha / 2.0)
        return 2.0 * (1.0 - norm.cdf(center_quantile / math.sqrt(t)))
    # Pocock-type Lan-DeMets spending: alpha*(t) = alpha*ln(1+(e-1)t).
    return alpha * math.log(1.0 + (math.e - 1.0) * t)


def _composite_grid(low: float, high: float, target_width: float):
    """Composite Gauss-Legendre nodes covering [low, high].

    Panel width tracks the local process scale so the sharply peaked stage
    densities are integrated without missing mass (a single fixed panel over
    wide continuation regions underestimates the integral).
    """
    import numpy as np
    nodes_per_panel = 12
    length = max(high - low, 1e-12)
    panels = max(1, min(int(math.ceil(length / max(target_width, 1e-12))),
                        128))
    base_nodes, base_weights = np.polynomial.legendre.leggauss(nodes_per_panel)
    panel_length = length / panels
    centers = low + (np.arange(panels) + 0.5) * panel_length
    nodes = (centers[:, None]
             + panel_length * base_nodes[None, :] / 2.0).ravel()
    weights = np.tile(panel_length * base_weights / 2.0, panels)
    return nodes, weights


def _no_crossing_probability(bounds: list[float], information: list[float]) -> float:
    """P(no boundary crossing through every look) under H0.

    Canonical joint-law recursion over the Brownian B-value process
    (Armitage-McPherson-Rowe / Jennison-Turnbull section 7): stage densities
    are propagated with composite Gauss-Legendre quadrature across each
    continuation region. Symmetric two-sided boundaries throughout.
    """
    import numpy as np
    from scipy.stats import norm
    previous_t = information[0]
    half_width = bounds[0] * math.sqrt(previous_t)
    nodes, weights = _composite_grid(
        -half_width, half_width, 0.5 * math.sqrt(previous_t))
    density = norm.pdf(nodes / math.sqrt(previous_t)) / math.sqrt(previous_t)
    for k in range(1, len(bounds)):
        current_t = information[k]
        delta = current_t - previous_t
        half_width = bounds[k] * math.sqrt(current_t)
        nodes_next, weights_next = _composite_grid(
            -half_width, half_width,
            0.5 * min(math.sqrt(delta), math.sqrt(current_t)))
        kernel = np.exp(
            -((nodes_next[:, None] - nodes[None, :]) ** 2) / (2.0 * delta)
        ) / math.sqrt(2.0 * math.pi * delta)
        density = kernel @ (density * weights)
        nodes, weights = nodes_next, weights_next
        previous_t = current_t
    mass = float(np.sum(weights * density))
    return max(0.0, min(1.0, mass))


def run_seq_boundary(request: dict[str, Any]) -> tuple[dict[str, Any], int]:
    from scipy.optimize import brentq
    from scipy.stats import norm

    design = request.get("design", "obrien_fleming")
    if design not in ("obrien_fleming", "pocock"):
        return {"ok": False, "error": "design_unsupported",
                "message": "supported designs: obrien_fleming, pocock"}, 2
    alpha = request.get("alpha", 0.05)
    if not _finite(alpha) or not 0.0 < float(alpha) < 1.0:
        return {"ok": False, "error": "alpha_invalid"}, 2
    alpha = float(alpha)
    max_interims = request.get("max_interims", 3)
    interim_count = request.get("interim_count", 1)
    for name, value in (("max_interims", max_interims),
                        ("interim_count", interim_count)):
        if not isinstance(value, int) or isinstance(value, bool) or value < 1:
            return {"ok": False, "error": f"{name}_invalid"}, 2
    if interim_count > max_interims:
        return {"ok": False, "error": "interim_count_exceeds_max"}, 2
    if max_interims > MAX_LOOKS:
        return {"ok": False, "error": "too_many_looks"}, 2

    information = [(k + 1) / max_interims for k in range(max_interims)]
    bounds: list[float] = []
    boundary_p: list[dict[str, Any]] = []
    try:
        for k in range(max_interims):
            spent = _spending(design, information[k], alpha)
            if k > 0 and spent <= boundary_p[-1]["cumulative_alpha_spent"]:
                spent = boundary_p[-1]["cumulative_alpha_spent"] + 1e-12
            target_survival = 1.0 - spent

            def objective(candidate: float) -> float:
                return (_no_crossing_probability(
                    bounds + [candidate], information[: len(bounds) + 1])
                    - target_survival)

            low, high = 0.5, 40.0
            if objective(low) > 0.0 or objective(high) < 0.0:
                return {"ok": False, "error": "boundary_not_bracketed",
                        "message": "spending function root not bracketed"}, 2
            bound = brentq(objective, low, high, xtol=1e-9, rtol=1e-12,
                           maxiter=200)
            bounds.append(bound)
            boundary_p.append({
                "look": k + 1,
                "t": round(information[k], 6),
                "z": round(bound, 6),
                "p_max": round(2.0 * (1.0 - norm.cdf(bound)), 8),
                "cumulative_alpha_spent": round(spent, 10),
            })
    except Exception:  # noqa: BLE001 - fail closed, never crash
        return {"ok": False, "error": "boundary_failed",
                "message": "recursive integration failed"}, 2

    return {"ok": True, "schema": "dpa.stats.sidecar.seq_boundary/v1",
            "design": design, "alpha": alpha,
            "max_interims": max_interims, "interim_count": interim_count,
            "boundary_p": boundary_p,
            "boundary_z": [entry["z"] for entry in boundary_p],
            "source": "lan_demets_spending+gauss_legendre_recursion"}, 0


# ---------------------------------------------------------------------------
# Method 3: vibe_analyze — viblearn vibration analysis
# ---------------------------------------------------------------------------

def run_vibe_analyze(request: dict[str, Any]) -> tuple[dict[str, Any], int]:
    signal = request.get("signal")
    if signal is None:
        return {"ok": False, "error": "signal_missing",
                "message": "signal array is required"}, 2
    if not isinstance(signal, list) or len(signal) == 0:
        return {"ok": False, "error": "signal_invalid",
                "message": "signal must be a non-empty array"}, 2
    if not all(_finite(v) for v in signal):
        return {"ok": False, "error": "signal_invalid",
                "message": "signal must contain only finite numbers"}, 2

    sampling_rate = request.get("sampling_rate", 10000.0)
    if not _finite(sampling_rate) or float(sampling_rate) <= 0:
        return {"ok": False, "error": "sampling_rate_invalid"}, 2

    try:
        import numpy as np
        from viblearn import (
            TimeDomainAnalyzer,
            FFTAnalyzer,
            PSDAnalyzer,
            EnvelopeAnalyzer,
            FaultFrequencyCalculator,
            iso_10816_3_severity,
            api_670_limit,
        )
    except ImportError:
        return {"ok": False, "error": "runtime_missing",
                "message": "numpy/viblearn are required"}, 3

    try:
        sig = np.asarray(signal, dtype=float)
        if sig.ndim != 1:
            return {"ok": False, "error": "signal_invalid",
                    "message": "signal must be 1-D"}, 2
        sr = float(sampling_rate)

        features: dict[str, Any] = {}

        td = TimeDomainAnalyzer()
        td_features = td.analyze(sig, sampling_rate=sr)
        features["time_domain"] = td_features

        fft = FFTAnalyzer()
        fft_result = fft.analyze(sig, sampling_rate=sr)
        features["fft"] = fft_result

        features["severity"] = iso_10816_3_severity(td_features)

        if request.get("psd"):
            psd = PSDAnalyzer()
            features["psd"] = psd.analyze(sig, sampling_rate=sr)

        if request.get("envelope"):
            env = EnvelopeAnalyzer()
            features["envelope"] = env.analyze(sig, sampling_rate=sr)

        bearing = request.get("bearing")
        if isinstance(bearing, dict):
            fc = FaultFrequencyCalculator()
            features["fault_frequencies"] = fc.calculate(
                bearing.get("num_balls", 9),
                bearing.get("ball_diameter", 7.94),
                bearing.get("pitch_diameter", 39.04),
                bearing.get("contact_angle", 0.0),
                sr,
            )

        shaft_disp = request.get("shaft_displacement")
        if shaft_disp is not None and _finite(shaft_disp):
            features["api_670_limit"] = api_670_limit(float(shaft_disp))

        return {"ok": True, "schema": "dpa.stats.sidecar.vibe_analyze/v1",
                "features": features,
                "sampling_rate": sr,
                "signal_length": len(sig),
                "source": "viblearn"}, 0
    except Exception:  # noqa: BLE001 - fail closed, never crash
        return {"ok": False, "error": "vibe_failed",
                "message": "vibration analysis failed"}, 2


# ---------------------------------------------------------------------------
# Method 4: monitor_analysis — EWMA, CUSUM, swing-and-door compression
# ---------------------------------------------------------------------------

def _ewma(values: list[float], alpha: float = 0.2) -> list[float]:
    """Exponentially weighted moving average.  alpha=0.2 gives span~9."""
    if not values:
        return []
    result = [values[0]]
    for v in values[1:]:
        result.append(alpha * v + (1.0 - alpha) * result[-1])
    return result


def _cusum(values: list[float], target: float | None = None,
           drift: float = 0.0) -> list[dict[str, Any]]:
    """Two-sided CUSUM change-point detection.

    Returns list of dicts with index, value, s_high, s_low, detected.
    drift is the允许偏移量 (slack); smaller = more sensitive.
    """
    if target is None:
        target = sum(values) / len(values) if values else 0.0
    results: list[dict[str, Any]] = []
    s_high = 0.0
    s_low = 0.0
    for i, v in enumerate(values):
        delta = v - target
        s_high = max(0.0, s_high + delta - drift)
        s_low = max(0.0, s_low - delta - drift)
        detected = s_high > 0.0 or s_low > 0.0
        results.append({
            "index": i, "value": v,
            "s_high": round(s_high, 6),
            "s_low": round(s_low, 6),
            "detected": detected,
        })
    return results


def _swing_and_door(values: list[float],
                    timestamps_ms: list[int],
                    tolerance: float = 0.5) -> list[dict[str, Any]]:
    """Swing-and-door (SDT) trend compression.

    Reduces a time series to inflection points within the given tolerance.
    Each output point has: index, timestamp_ms, value.
    """
    if not values:
        return []
    n = len(values)
    if n <= 2:
        return [{"index": i, "timestamp_ms": timestamps_ms[i] if i < len(timestamps_ms) else 0,
                 "value": values[i]} for i in range(n)]

    door_points: list[dict[str, Any]] = [
        {"index": 0, "timestamp_ms": timestamps_ms[0] if timestamps_ms else 0,
         "value": values[0]}
    ]
    anchor_idx = 0
    anchor_val = values[0]

    for i in range(2, n):
        v = values[i]
        if abs(v - anchor_val) > tolerance:
            door_points.append({
                "index": i - 1,
                "timestamp_ms": timestamps_ms[i - 1] if i - 1 < len(timestamps_ms) else 0,
                "value": values[i - 1],
            })
            anchor_idx = i - 1
            anchor_val = values[i - 1]

    last_idx = n - 1
    door_points.append({
        "index": last_idx,
        "timestamp_ms": timestamps_ms[last_idx] if last_idx < len(timestamps_ms) else 0,
        "value": values[last_idx],
    })
    return door_points


def run_monitor_analysis(request: dict[str, Any]) -> tuple[dict[str, Any], int]:
    """Run EWMA, CUSUM, and SDT compression on a time series.

    Request fields:
      values: list[float]          -- required, the time series
      timestamps_ms: list[int]     -- optional, per-sample timestamps
      alpha: float                 -- optional, EWMA alpha (default 0.2)
      cusum_drift: float           -- optional, CUSUM drift/slack (default: stdev/2)
      sdt_tolerance: float         -- optional, SDT tolerance (default 0.5)
    """
    values = request.get("values")
    if not isinstance(values, list) or len(values) == 0:
        return {"ok": False, "error": "values_invalid",
                "message": "values must be a non-empty array"}, 2
    if not all(_finite(v) for v in values):
        return {"ok": False, "error": "values_invalid",
                "message": "values must contain only finite numbers"}, 2

    ts = request.get("timestamps_ms")
    if ts is not None:
        if not isinstance(ts, list) or len(ts) != len(values):
            return {"ok": False, "error": "timestamps_ms_invalid"}, 2
        ts = [int(x) for x in ts]
    else:
        ts = list(range(len(values)))

    alpha = float(request.get("alpha", 0.2))
    if not (0.0 < alpha <= 1.0):
        return {"ok": False, "error": "alpha_invalid"}, 2

    try:
        import numpy as np
        arr = np.asarray(values, dtype=float)
        mean_val = float(arr.mean())
        std_val = float(arr.std()) if len(arr) > 1 else 0.0

        cusum_drift = float(request.get("cusum_drift", max(std_val / 2.0, 1e-6)))
        sdt_tol = float(request.get("sdt_tolerance", 0.5))

        ewma_result = _ewma(values, alpha)
        cusum_result = _cusum(values, target=mean_val, drift=cusum_drift)
        sdt_result = _swing_and_door(values, ts, tolerance=sdt_tol)

        detected_indices = [c["index"] for c in cusum_result if c["detected"]]

        return {
            "ok": True,
            "schema": "dpa.stats.sidecar.monitor_analysis/v1",
            "stats": {
                "count": len(values),
                "mean": round(mean_val, 6),
                "std": round(std_val, 6),
                "min": round(float(arr.min()), 6),
                "max": round(float(arr.max()), 6),
            },
            "ewma": {
                "alpha": alpha,
                "values": [round(v, 6) for v in ewma_result],
                "final": round(ewma_result[-1], 6) if ewma_result else None,
            },
            "cusum": {
                "drift": cusum_drift,
                "target": round(mean_val, 6),
                "change_points": detected_indices,
                "change_point_count": len(detected_indices),
                "s_high_final": cusum_result[-1]["s_high"] if cusum_result else 0.0,
                "s_low_final": cusum_result[-1]["s_low"] if cusum_result else 0.0,
            },
            "sdt": {
                "tolerance": sdt_tol,
                "compressed_count": len(sdt_result),
                "compression_ratio": round(len(sdt_result) / max(1, len(values)), 4),
                "points": sdt_result,
            },
            "source": "ewma+cusum+sdt",
        }, 0
    except Exception:  # noqa: BLE001
        return {"ok": False, "error": "monitor_analysis_failed",
                "message": "monitor analysis failed"}, 2


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def handle(request: dict[str, Any]) -> tuple[dict[str, Any], int]:
    # Kernel hyperparameter convergence notes are informational: the bounded
    # surrogate still produces valid predictions, and stderr noise must never
    # leak into protocol decisions.
    warnings.filterwarnings("ignore", category=Warning)
    invalid = _validate_common(request)
    if invalid:
        return {"ok": False, "error": invalid}, 2
    method = request["method"]
    if method == "bo_next":
        return run_bo_next(request)
    if method == "seq_boundary":
        return run_seq_boundary(request)
    if method == "vibe_analyze":
        return run_vibe_analyze(request)
    if method == "monitor_analysis":
        return run_monitor_analysis(request)
    return {"ok": False, "error": "unknown_method",
            "message": f"unsupported method: {method}"}, 2


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        return _error("bad_request", "usage: stats_sidecar.py <request.json>", 2)
    request_path = Path(argv[1])
    try:
        if not request_path.is_file():
            return _error("request_missing", "request file does not exist", 2)
        if request_path.stat().st_size > MAX_REQUEST_BYTES:
            return _error("request_too_large",
                          "request exceeded the bounded limit", 2)
        request = json.loads(request_path.read_text(encoding="utf-8-sig"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return _error("request_json_invalid", "request is not valid JSON", 2)
    except OSError:
        return _error("request_unreadable", "request file could not be read", 2)
    if not isinstance(request, dict):
        return _error("request_json_invalid", "request must be a JSON object", 2)
    try:
        payload, exit_code = handle(request)
    except Exception:  # noqa: BLE001 - absolute crash guard
        return _error("internal_error", "unexpected internal failure", 4)
    _emit(payload)
    return exit_code


if __name__ == "__main__":
    sys.exit(main(sys.argv))
