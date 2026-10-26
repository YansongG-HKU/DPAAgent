#pragma once

#include <string>

namespace Model {

// Deterministic statistical analysis of a completed experiment campaign.
// Inference uses the Fisher randomization test paired with the campaign's own
// randomized design: exact under complete randomization, no distributional
// assumptions, fully reproducible from an explicit seed. Statistics come from
// here, never free-form from a model.
struct CampaignAnalysisOutput {
    bool ok = false;
    std::string error;
    std::string resultJson;
};

// Request grammar (exact keys):
// {"contract": <dpa.experiment.campaign>,
//  "observations": [{"run_index": int, "value": finite number}, ...],
//  "seed": int 0..2^53-1,
//  "permutations": optional int 199..4999, default 999}
CampaignAnalysisOutput analyzeExperimentCampaign(const std::string& requestJson);

// Governance gate: binds caller-supplied observations to the verified run
// records of a campaign result artifact. Rejects when the artifact belongs to
// a different campaign, when a referenced run did not complete with evidence,
// or when coverage falls below the threshold. Values remain transcripts until
// value-extraction from evidence artifacts lands (tracked in delivery list).
struct EvidenceBoundObservations {
    bool ok = false;
    std::string error;
    std::string observationsJson;
    int verifiedRuns = 0;
};
EvidenceBoundObservations bindObservationsToEvidence(
    const std::string& campaignResultJson,
    const std::string& expectedCampaignSha256,
    const std::string& observationsJson,
    double minCoverage);

EvidenceBoundObservations extractObservationsFromCampaignResult(
    const std::string& campaignResultJson,
    const std::string& expectedCampaignSha256,
    const std::string& expectedResponseMetric,
    double minCoverage,
    const std::string& expectedResponseUnit = std::string());

} // namespace Model
