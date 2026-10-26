#pragma once

#include <string>

namespace Model {

// Simulation-based statistical power for a planned campaign, matched exactly
// to the Fisher randomization test used by the analysis layer. Powering a
// randomization test with t/F approximations would plan one procedure and run
// another; Monte Carlo over the design itself is the methodologically correct
// companion and reuses the verified inference components end to end.
struct CampaignPowerOutput {
    bool ok = false;
    std::string error;
    std::string resultJson;
};

// Request grammar (exact keys):
// {"contract": <dpa.experiment.campaign>, "factor": bounded token,
//  "effect_range": number >= 0, "noise_std": number >= 0,
//  "seed": int 0..2^53-1, "alpha": optional number (0,0.5], default 0.05,
//  "simulations": optional int 50..2000, default 200,
//  "permutations": optional int 99..1999, default 199}
// Both effect_range and noise_std zero is rejected: the question is empty.
CampaignPowerOutput estimateCampaignPower(const std::string& requestJson);

} // namespace Model
