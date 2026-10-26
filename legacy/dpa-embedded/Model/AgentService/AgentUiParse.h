#pragma once
#include <string>
#include <vector>
namespace Model {
    // A diagnose finding shaped for the UI card. Pure data.
    struct UiFinding {
        std::string fault, severity, evidence;
        double ruleMatchScore = 0;
        bool assessed = true;
        std::vector<std::string> rootCauses, recommendations;
    };
    // Parse a diagnose tool-result JSON (ToolResult::toJson, findings at data.findings[])
    // into UI findings. Returns empty for non-diagnostic / empty results (caller falls back to a bubble).
    std::vector<UiFinding> parseDiagnoseFindings(const std::string& toolResultJson);

    // Build the exact record_operator_feedback contract used by diagnosis-card buttons.
    // Diagnostic severity and feedback severity are intentionally different taxonomies.
    std::string buildDiagnosticFeedbackJson(const std::string& feedbackType,
                                            const std::string& fault,
                                            const std::string& diagnosticSeverity,
                                            const std::string& ruleId,
                                            const std::vector<std::string>& evidenceRefs = {});

    // One executed campaign run shaped for the status table. Pure data.
    struct UiCampaignRunRow {
        int runIndex = -1, designIndex = -1;
        bool ok = false;
        std::string errorCode, runId, evidenceId, levelsText;
    };
    struct UiCampaignSnapshot {
        std::string status, stopReason;
        int plannedRuns = 0;
        std::vector<UiCampaignRunRow> runs;
    };
    // Parses dpa.experiment.campaign.result/v1 (from a tool_result step or
    // artifact). Returns false when the payload is not a campaign result.
    bool parseCampaignRunRows(const std::string& campaignResultJson,
                              UiCampaignSnapshot& out);

    // One knowledge-ledger entry shaped for the history table. Pure data.
    struct UiKnowledgeRow {
        long long recordedAtMs = 0;
        std::string campaignId;
        std::string dominantFactor;
        double effectRange = 0.0, permP = -1.0, residualStd = 0.0;
        bool testable = false;
        bool residualEstimable = false;
        std::string bestLevels;
    };
    // Parses dpa.experiment.knowledge.list/v1 payloads into newest-first rows.
    bool parseKnowledgeRows(const std::string& listToolResultJson,
                            std::vector<UiKnowledgeRow>& rows);

    // A refinement proposal shaped for the authorization card. Pure data.
    struct UiRefinementProposal {
        std::string sourceCampaignId, proposedCampaignId;
        std::string proposedSha256, refinementArtifactId;
        int plannedRuns = 0, replicates = 0;
        double power = -2.0;
        bool hasPower = false, powerCalibrated = false;
        std::vector<std::string> refinedFactors, fixedFactors;
    };
    // Parses dpa.experiment.campaign.refinement/v1 payloads.
    bool parseRefinementProposal(const std::string& toolResultJson,
                                 UiRefinementProposal& out);

    // Interim-checkpoint hint extracted from a campaign result payload.
    struct UiCheckpointHint {
        bool awaiting = false;
        std::string checkpointArtifactId, stopReason, campaignSha256;
        int completedRuns = 0, plannedRuns = 0;
    };
    bool parseCheckpointHint(const std::string& campaignResultJson,
                             UiCheckpointHint& out);
}
