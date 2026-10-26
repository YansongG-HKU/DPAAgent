#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Model {

struct CampaignFactor {
    std::string name;
    std::vector<std::string> levelsJson;
};

// A governed experiment campaign: a factorial design over whitelisted plan
// knobs, expanded into concrete single-run plans before anything touches a
// device. The campaign layer owns design and bookkeeping; the existing
// autonomous experiment executor stays the only path to hardware.
struct ExperimentCampaign {
    std::string objectiveSha256;
    std::string campaignId;
    std::string hypothesis;
    std::vector<CampaignFactor> factors;
    int replicates = 1;
    bool randomize = false;
    unsigned long long seed = 0;
    std::string onFailure = "abort";
    int maxRuns = 0;
    int maxFailedRuns = 0;
    // Planned operational checkpoint after this many completed runs. It is a
    // human-oversight gate (AlabOS acknowledge pattern), NOT a corrected
    // statistical interim test.
    int interimAfter = 0;
    // Declared response variable for value extraction from evidence artifacts.
    // Grammar-first: capture wiring lands with evidence value extraction.
    std::string responseMetric;
    std::string responseUnit;
    // Empty preserves legacy analysis-only contracts. Automatic refinement
    // requires maximize|minimize|target so "best" is never guessed.
    std::string responseDirection;
    bool responseTargetPresent = false;
    double responseTarget = 0;
    double responseTolerance = 0;
    std::string templateJson;
    std::vector<std::string> designLevelsJson;
    std::vector<std::string> runPlanJsons;
    std::vector<int> designIndices;
    std::string canonicalJson;
    std::string campaignSha256;
};

struct CampaignValidation {
    bool ok = false;
    std::string error;
    ExperimentCampaign campaign;
};

struct CampaignRunOutcome {
    bool ok = false;
    std::string errorCode;
    std::string runId;
    std::string evidenceArtifactId;
    bool responsePresent = false;
    double responseValue = 0;
    int responseChannelId = -1;
    int responseSampleCount = 0;
    std::string responseUnit;
    std::string responseSource;
    std::string responseEvidenceSha256;
};

// A model-proposed experiment goal is data, not authorization. The objective
// fixes the current context, permitted design space, budgets and evidence
// requirements before a campaign may be executed.
struct ExperimentObjective {
    std::string objectiveId;
    std::string contextFingerprint;
    std::string toolCatalogSha256;
    std::string hypothesis;
    ExperimentCampaign minimumCampaign;
    std::string fixedPlanJson;
    int maxRuns = 0;
    int maxRunDurationMs = 0;
    long long maxTotalDurationMs = 0;
    bool randomize = false;
    int minReplicates = 1;
    std::string canonicalJson;
    std::string objectiveSha256;
};

struct ExperimentObjectiveValidation {
    bool ok = false;
    std::string error;
    ExperimentObjective objective;
};

struct ObjectiveCampaignValidation {
    bool ok = false;
    std::string error;
    long long totalDurationMs = 0;
};

struct CampaignRunRecord {
    int runIndex = -1;
    int designIndex = -1;
    bool ok = false;
    std::string errorCode;
    std::string runId;
    std::string evidenceArtifactId;
    bool responsePresent = false;
    double responseValue = 0;
    int responseChannelId = -1;
    int responseSampleCount = 0;
    std::string responseUnit;
    std::string responseSource;
    std::string responseEvidenceSha256;
};

struct CampaignState {
    std::string campaignSha256;
    std::string status = "running";
    std::string stopReason;
    int nextRunIndex = 0;
    bool interimTaken = false;
    std::vector<CampaignRunRecord> records;
};

struct CampaignStateValidation {
    bool ok = false;
    std::string error;
    CampaignState state;
};

struct CampaignExecution {
    bool ok = false;
    bool awaitingInterim = false;
    std::string error;
    CampaignState state;
};

CampaignValidation parseAndValidateExperimentCampaign(const std::string& json);

ExperimentObjectiveValidation parseAndValidateExperimentObjective(
    const std::string& json,
    const std::string& expectedContextFingerprint = std::string(),
    const std::string& expectedToolCatalogSha256 = std::string());

ObjectiveCampaignValidation validateCampaignAgainstObjective(
    const ExperimentObjective& objective,
    const ExperimentCampaign& campaign);

// Domain metadata for the whitelisted plan knobs. Single source of truth so
// downstream layers (refinement proposals) cannot drift from what the
// validator accepts.
enum class CampaignKnobKind {
    LinearNumeric,
    OrderedEnum,
    Categorical
};
CampaignKnobKind campaignKnobKind(const std::string& planKey);
double campaignKnobMinimum(const std::string& planKey);
double campaignKnobMaximum(const std::string& planKey);
std::vector<double> campaignKnobLadder(const std::string& planKey);
// Whitelisted string levels for categorical knobs; empty for other kinds.
std::vector<std::string> campaignKnobCategoricalLevels(const std::string& planKey);

// Larger utility is always better. Target objectives use negative absolute
// distance; legacy contracts retain historical maximize semantics for analysis.
double campaignResponseUtility(const ExperimentCampaign& campaign, double value);

std::string serializeCampaignState(const CampaignState& state);
CampaignStateValidation parseCampaignState(const std::string& json);

// Drives the expanded run sequence through the caller-supplied executor.
// A successful run without a run id or evidence artifact id is a structural
// violation and aborts the campaign regardless of the failure policy.
CampaignExecution executeExperimentCampaign(
    const ExperimentCampaign& campaign,
    const std::string& resumeStateJson,
    std::function<CampaignRunOutcome(int runIndex, int designIndex,
                                     const std::string& planJson)> executor,
    std::function<bool()> cancelProbe);

} // namespace Model
