#pragma once

#include <QJsonValue>
#include <string>
#include <vector>

namespace Model {

// Result of one one-shot statistics sidecar request. Transport failures and
// sidecar-reported failures both land here fail-closed; `error` always carries
// a stable machine-readable code prefixed for attribution.
struct StatsSidecarOutput {
    bool ok = false;
    std::string error;
    std::string resultJson;
};

// Runs one request against the read-only external statistics sidecar
// (`scripts/stats-sidecar/stats_sidecar.py`, one process per call, single
// JSON document on stdout). Mirrors the verified MDF adapter precedent:
// bounded request, 10 second fail-closed timeout, bounded output, exit-code
// plus in-band `ok` double check. No shell, no stdin, no raw waveforms.
StatsSidecarOutput runStatsSidecarRequest(const std::string& requestJson);

// Whitelisted knob-domain descriptor for one campaign plan knob, expressed in
// the sidecar's grammar (kind/min/max or ladder or levels). Returns an empty
// string for unknown knobs so callers can reject them upstream.
std::string campaignKnobDomainJson(const std::string& planKey);

// True when the suggested level stays inside the declared domain of the knob
// (numeric within [min,max], ordered enum exactly on the ladder, categorical
// exactly on the whitelisted strings).
bool campaignSuggestionWithinDomain(const std::string& planKey,
                                    const QJsonValue& value);

struct StatsSuggestionOutput {
    bool ok = false;
    std::string error;
    std::string resultJson;
};

// Deterministic core of the read-only refinement consultation: validates the
// campaign contract, pairs observations with expanded design rows through the
// randomized run order, consults the sidecar GP (`bo_next`), revalidates every
// suggested level against the whitelisted knob domains, and degrades to the
// built-in deterministic grid suggestion with explicit warning codes when the
// sidecar is unavailable or answers out of domain. Never touches hardware,
// sessions, or artifacts; the result payload carries `source` and `warnings`
// so callers can relay degradation honestly.
StatsSuggestionOutput buildExperimentRefinementSuggestion(
    const std::string& requestJson);

} // namespace Model
