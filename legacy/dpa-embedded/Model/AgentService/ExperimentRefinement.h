#pragma once

#include <string>

namespace Model {

// Turns one completed campaign's findings into the next campaign's design:
// factors whose effect clears the signal-to-noise threshold are refined around
// their best observed level, the rest are fixed at their best level and baked
// into the template. The proposal is self-validated against the campaign
// contract grammar and, whenever a noise estimate exists, power-checked with
// the same machinery used for pre-trial planning. Composed entirely from the
// validated campaign/analysis/power components; execution of the proposal is a
// separate, freshly authorized decision.
struct CampaignRefinementOutput {
    bool ok = false;
    std::string error;
    std::string resultJson;
};

// Request grammar mirrors the analysis layer plus one knob:
// {"contract": <dpa.experiment.campaign>,
//  "observations": [{"run_index": int, "value": number}, ...],
//  "seed": int 0..2^53-1,
//  "threshold": optional number (0,100], default 2.0}
CampaignRefinementOutput refineExperimentCampaign(const std::string& requestJson);

} // namespace Model
