#pragma once

#include <string>
#include <vector>

namespace Model {

// A clause reference is a citation pointer, not a copy of the standard text.
// Keeping the locator and its intended use makes a plan auditable while
// respecting the source owner's license and controlled-distribution boundary.
struct StandardClauseReference {
    std::string clauseId;
    std::string pageOrSection;
    std::string title;
    std::string purpose;       // scope | method | acceptance | evidence | uncertainty
    std::string evidenceRef;  // project evidence/artifact identifier, if known
};

struct StandardEvidenceValidation {
    bool ok = false;
    std::string error;
    int recordCount = 0;
    int clauseReferenceCount = 0;
    int completeRecordCount = 0;
    std::vector<std::string> warnings;
};

// Validates the project-owned authorized-evidence manifest. Source bytes are
// never loaded by this pure function; it validates provenance, safe local
// references, hashes, and clause-level citations only.
StandardEvidenceValidation validateAuthorizedEvidenceManifestJson(
    const std::string& json);
std::string standardEvidenceValidationJson(const StandardEvidenceValidation& result);

// Runtime receipt, never populated from tool JSON. The operator owns the local
// DPA_STANDARDS_ROOT directory; hashing proves integrity, not reviewer identity.
struct StandardExecutionEvidence {
    bool ok = false;
    std::string error = "standard_execution_evidence_required";
    std::string standardProfile, ruleVersion, scopeSha256;
    std::string manifestSha256, recordSha256, sourceSha256, approvalSha256;
    std::string approvalRef, reviewer;
};
std::string standardEvidenceScopeSha256(const std::string& scopeJson);
bool standardExecutionEvidenceMatches(const StandardExecutionEvidence& evidence,
    const std::string& profile, const std::string& scopeJson, const std::string& ruleVersion);
// Reads only the configured local manifest, authorized source bytes, and the
// context-addressed approval. Missing, changed, expired or revoked evidence fails closed.
StandardExecutionEvidence validateLocalStandardExecutionEvidence(
    const std::string& profile, const std::string& scopeJson, const std::string& ruleVersion);
std::string standardExecutionEvidenceJson(const StandardExecutionEvidence& evidence);

} // namespace Model
