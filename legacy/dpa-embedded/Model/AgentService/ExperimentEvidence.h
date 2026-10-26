#pragma once

#include "SessionRecord.h"

#include <string>

namespace Model {

struct ExperimentEvidenceSnapshot {
    long long generatedAtMs = 0;
    std::string toolCatalogSha256;
    std::string experimentContextSha256;
    std::string preflightJson;
    std::string companionStatusJson;
    std::string diagnosticRunJson;
    // {"present":false}, or a context-bound dpa.standards.test-plan envelope
    // wrapped as {"present":true,"plan":{...}}. Only metadata and digests are
    // persisted; standard source text is never embedded.
    std::string standardsPlanJson;
    // {"present":false}, or a strictly bounded applied simulator recipe and progress.
    std::string simulatorJson;
};

struct ExperimentEvidenceVerification {
    bool ok = false;
    std::string error;
    std::string payloadSha256;
    bool catalogMatches = true;
    int timelineEntries = 0;
    int toolEvents = 0;
    int companionEvents = 0;
    int omittedEvents = 0;
    bool standardsPlanPresent = false;
    bool standardsPlanContextMatches = true;
    std::string standardsPlanSha256;
    bool simulatorRecipePresent = false;
    // A reconstructed, verification-only timeline. It cannot execute tools, call a
    // model, or write to a device because this module has no such dependencies.
    std::string replayJson;
};

// Builds a privacy-minimized, integrity-protected experiment evidence document.
// Raw conversation turns, attachments, waveforms, paths, and event text are excluded.
std::string buildExperimentEvidenceJson(const SessionRecord& session,
                                        const ExperimentEvidenceSnapshot& snapshot);

// Verifies schema, bounds, the payload digest, timeline ordering, and simulator
// recipe integrity. Catalog drift is reported separately and is not executed around.
ExperimentEvidenceVerification verifyExperimentEvidenceJson(
    const std::string& json, const std::string& expectedToolCatalogSha256 = {});

} // namespace Model
