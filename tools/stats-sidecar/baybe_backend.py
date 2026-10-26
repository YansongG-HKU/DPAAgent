"""Optional BayBE adapter over DPA's bounded, whitelisted candidate pool.

No persistent Campaign, device access or execution authority is created here.
The native caller retains its ten-second timeout and suggestion validation.
"""
from importlib.metadata import version


def recommend(request, names, domains, integral_numeric, seed, budget):
    try:
        import numpy as np
        import pandas as pd
        import torch
        from baybe import Campaign
        from baybe.acquisition import LogExpectedImprovement
        from baybe.objectives import SingleTargetObjective
        from baybe.parameters import CategoricalParameter, NumericalDiscreteParameter
        from baybe.recommenders import BotorchRecommender
        from baybe.searchspace import SearchSpace
        from baybe.settings import Settings
        from baybe.targets import NumericalTarget
    except (ImportError, OSError, MemoryError):
        return {"ok": False, "error": "baybe_runtime_missing"}, 3

    if version("baybe") != "0.15.0":
        return {"ok": False, "error": "baybe_version_not_validated"}, 3
    from stats_sidecar import _candidate_grid, _decode_factor, _utility

    previous_threads = torch.get_num_threads()
    try:
        torch.set_num_threads(1)
        candidates = _candidate_grid(names, domains, budget, np.random.default_rng(seed % 2**32))
        decoded = [{name: _decode_factor(row[name], domains[name], integral_numeric.get(name, False))
                    for name in names} for row in candidates]
        observations = request["observations"]
        measured = [{name: request["design"][entry["run_index"]][name] for name in names}
                    for entry in observations]
        # Include measured points so BayBE can bind evidence to its search space;
        # allow_recommending_already_measured=False excludes them from proposals.
        frame = pd.DataFrame(decoded + measured, columns=names).drop_duplicates().reset_index(drop=True)
        parameters = []
        for name in names:
            domain = domains[name]
            if domain["kind"] == "categorical":
                parameter = CategoricalParameter(name, values=domain["levels"])
            else:
                values = sorted(set(frame[name].tolist()))
                if len(values) == 1:
                    # Parameter metadata can contain extra valid levels without
                    # adding them to the explicitly bounded dataframe candidates.
                    values = sorted(set(values + (domain["ladder"] if domain["kind"] == "ordered_enum"
                                                  else [domain["min"], domain["max"]])))
                parameter = NumericalDiscreteParameter(name, values=values, tolerance=0.0)
            parameters.append(parameter)
        utility_name = "__dpa_objective_utility"
        while utility_name in names:
            utility_name += "_"
        measurements = pd.DataFrame(measured, columns=names)
        measurements[utility_name] = [_utility(entry["value"], request.get("direction", "maximize"),
                                               request.get("target")) for entry in observations]
        with Settings(random_seed=seed % 2**32, cache_directory=None,
                      cache_campaign_recommendations=False, parallelize_simulation_runs=False):
            campaign = Campaign(
                searchspace=SearchSpace.from_dataframe(frame, parameters),
                objective=SingleTargetObjective(NumericalTarget(utility_name)),
                recommender=BotorchRecommender(acquisition_function=LogExpectedImprovement()),
                allow_recommending_already_measured=False)
            campaign.add_measurements(measurements)
            result = campaign.recommend(batch_size=1)
        levels = {name: (str(result.iloc[0][name]) if domains[name]["kind"] == "categorical"
                         else float(result.iloc[0][name])) for name in names}
        return {"ok": True, "schema": "dpa.stats.sidecar.bo_next/v1", "next_levels": levels,
                "surrogate": "baybe_gp", "backend_version": version("baybe"),
                "direction": request.get("direction", "maximize"),
                "predicted_mean": None, "predicted_std": None,
                "notes": f"log_ei_over_bounded_pool;{len(frame)}_pool_rows;"
                         f"{len(measured)}_paired_observations;uncertainty_not_exported"}, 0
    except Exception:
        # No exception text containing input values, source paths or library
        # internals is sent back to the agent's protocol.
        return {"ok": False, "error": "baybe_recommendation_failed"}, 2
    finally:
        torch.set_num_threads(previous_threads)
