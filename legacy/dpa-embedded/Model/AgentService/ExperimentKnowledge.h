#pragma once

#include <string>

namespace Model {

// Cross-campaign experiment memory. Entries are derived exclusively from
// verified deterministic-analysis payloads; free-form model text can never
// enter the ledger. Storage is an append-only JSONL file owned by the service
// layer; this module owns entry construction and strict parsing.
struct ExperimentKnowledgeEntry {
    long long recordedAtMs = 0;
    std::string campaignId;
    std::string campaignSha256;
    double residualStd = 0.0;
    bool residualEstimable = false;
    std::string factorsJson;
    std::string bestCellJson;
    std::string sourceArtifactId;
    std::string canonicalJson;
};

struct KnowledgeBuildOutput {
    bool ok = false;
    std::string error;
    std::string entryJson;
};

// Extracts a ledger entry from a deterministic analysis result document
// (schema dpa.experiment.campaign.analysis/v1).
KnowledgeBuildOutput buildKnowledgeEntry(const std::string& analysisResultJson,
                                         const std::string& sourceArtifactId,
                                         long long recordedAtMs);

struct KnowledgeEntryParse {
    bool ok = false;
    std::string error;
    ExperimentKnowledgeEntry entry;
};

KnowledgeEntryParse parseKnowledgeEntry(const std::string& json);

} // namespace Model
