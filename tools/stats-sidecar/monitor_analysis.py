"""Bounded offline feature analysis; never a second live alarm authority."""
import math
import statistics

MAX_VALUES = 256


def number(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and abs(value) <= 1e100 and math.isfinite(value))


def cusum(values, target, drift, threshold):
    """NIST tabular CUSUM in original units, with independent mu0, k and h.

    https://www.itl.nist.gov/div898/handbook/pmc/section3/pmc323.htm
    Threshold exceedance is a statistical signal, not a fault diagnosis.
    """
    high = low = 0.0
    result = []
    for index, value in enumerate(values):
        high = max(0.0, high + value - target - drift)
        low = max(0.0, low + target - value - drift)
        result.append({"index": index, "s_high": high, "s_low": low,
                       "detected": high > threshold or low > threshold})
    return result


def compress_trend(values, times, tolerance):
    """Endpoint-constrained slope envelope with independent error verification.

    Each retained endpoint fits ALL preceding samples in its segment. This
    conservative swinging-door variant gives a vertical error bound at original
    timestamps, retains original endpoints and avoids an additional dependency.
    The caller bounds this to 256 feature points, never raw waveform evidence.
    """
    indices, anchor = [0], 0
    low, high = -math.inf, math.inf
    for index in range(1, len(values)):
        dt, dv = times[index] - times[anchor], values[index] - values[anchor]
        if not low <= dv / dt <= high:
            indices.append(index - 1)
            anchor = index - 1
            low, high = -math.inf, math.inf
            dt, dv = times[index] - times[anchor], values[index] - values[anchor]
        low = max(low, (dv - tolerance) / dt)
        high = min(high, (dv + tolerance) / dt)
    if indices[-1] != len(values) - 1:
        indices.append(len(values) - 1)
    error = 0.0
    for left, right in zip(indices, indices[1:]):
        for index in range(left + 1, right):
            fraction = (times[index] - times[left]) / (times[right] - times[left])
            reconstructed = values[left] + fraction * (values[right] - values[left])
            error = max(error, abs(reconstructed - values[index]))
    fallback = None
    if error > tolerance:
        # Floating-point boundary effects cannot weaken the requested bound.
        indices, error, fallback = list(range(len(values))), 0.0, "roundoff_preserve_all"
    return ([{"index": i, "time": times[i], "value": values[i]} for i in indices],
            error, fallback)


def run_monitor_analysis(request):
    values = request.get("values")
    if (not isinstance(values, list) or not 1 <= len(values) <= MAX_VALUES
            or not all(number(value) for value in values)):
        return {"ok": False, "error": "values_invalid"}, 2
    values = [float(value) for value in values]
    times = request.get("timestamps_ms")
    time_basis = "timestamp_ms" if times is not None else "sample_index"
    if times is None:
        times = list(range(len(values)))
    if (not isinstance(times, list) or len(times) != len(values)
            or any(not isinstance(t, int) or isinstance(t, bool)
                   or abs(t) > 2**53 - 1 for t in times)
            or any(right <= left for left, right in zip(times, times[1:]))):
        return {"ok": False, "error": "timestamps_ms_invalid"}, 2
    alpha, tolerance = request.get("alpha", 0.2), request.get("tolerance", 0.5)
    if not number(alpha) or not 0 < alpha <= 1:
        return {"ok": False, "error": "alpha_invalid"}, 2
    if not number(tolerance) or tolerance < 0:
        return {"ok": False, "error": "tolerance_invalid"}, 2
    baseline_keys = ("cusum_target", "cusum_drift", "cusum_threshold")
    analysis = {"status": "not_evaluated", "reason": "baseline_missing"}
    if any(key in request for key in baseline_keys):
        if (not all(number(request.get(key)) for key in baseline_keys)
                or request["cusum_drift"] < 0 or request["cusum_threshold"] <= 0):
            return {"ok": False, "error": "cusum_baseline_invalid"}, 2
        series = cusum(values, request["cusum_target"], request["cusum_drift"],
                       request["cusum_threshold"])
        exceeded = [entry["index"] for entry in series if entry["detected"]]
        crossings = [entry["index"] for i, entry in enumerate(series)
                     if entry["detected"] and (i == 0 or not series[i - 1]["detected"])]
        analysis = {"status": "evaluated", "target": request["cusum_target"],
                    "drift": request["cusum_drift"], "threshold": request["cusum_threshold"],
                    "threshold_exceedance_indices": exceeded, "crossing_indices": crossings,
                    "s_high_final": series[-1]["s_high"], "s_low_final": series[-1]["s_low"],
                    "interpretation": "statistical_signal_not_fault_diagnosis"}
    ewma = [values[0]]
    for value in values[1:]:
        ewma.append(alpha * value + (1.0 - alpha) * ewma[-1])
    points, error, fallback = compress_trend(values, times, tolerance)
    return {"ok": True, "schema": "dpa.stats.sidecar.monitor_analysis/v1",
            "scope": "offline_feature_trend", "time_basis": time_basis,
            "stats": {"count": len(values), "mean": statistics.fmean(values),
                      "std": statistics.pstdev(values), "min": min(values), "max": max(values)},
            "ewma": {"alpha": alpha, "values": ewma, "final": ewma[-1]},
            "cusum": analysis,
            "compression": {"method": "endpoint_slope_envelope", "tolerance": tolerance,
                            "max_absolute_error": error, "fallback": fallback,
                            "original_count": len(values), "compressed_count": len(points),
                            "points": points}}, 0
