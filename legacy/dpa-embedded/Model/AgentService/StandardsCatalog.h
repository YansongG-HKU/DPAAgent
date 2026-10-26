#pragma once

#include <string>
#include <vector>

namespace Model {

struct StandardCatalogEntry {
    std::string id;
    std::string authority;
    std::string title;
    std::string status;
    std::string applicability;
    std::string executionStatus;
    std::string access;
    std::string sourceUrl;
    bool requiresOwnerApproval = true;
};

// Context supplied by the domain owner when a standard is considered for a
// concrete test. These fields are intentionally explicit: the catalog never
// infers the machine class, platform, or test objective from a name.
struct StandardApplicabilityRequest {
    std::string standardProfile;
    std::string machineType;
    double ratedPowerKw = 0;
    double rpm = 0;
    std::string platform;          // industrial | shipboard | airborne | railway | road_vehicle | space
    std::string testPurpose;       // operational_vibration | sine_vibration | random_vibration | shock | balancing | monitoring | governance
    std::string physicalQuantity;  // vibration_acceleration | vibration_velocity | vibration_displacement
    std::string applicabilityBasis;
    bool authorizedCopy = false;
    bool ownerApproved = false;
    // Set only by the service after the clause-level manifest validator has
    // accepted a manifest covering the selected profile.
    bool authorizedEvidenceValidated = false;
    std::string authorizedEvidenceManifestSha256;
};

struct StandardApplicabilityFinding {
    std::string code;
    std::string severity; // blocker | warning | evidence
    std::string message;
};

struct StandardApplicabilityResult {
    std::string standardProfile;
    std::string disposition; // unknown | context_missing | not_applicable | review_required | execution_ready
    bool known = false;
    bool applicable = false;
    bool executionReady = false;
    std::vector<StandardApplicabilityFinding> findings;
    std::vector<std::string> requiredEvidence;
};

struct StandardTestPlanRequest {
    std::string machineType;
    double ratedPowerKw = 0;
    double rpm = 0;
    std::string platform;
    std::string testPurpose;
    std::string physicalQuantity;
    std::string applicabilityBasis;
    bool authorizedCopy = false;
    bool ownerApproved = false;
    bool authorizedEvidenceValidated = false;
    std::string authorizedEvidenceManifestSha256;
    // When supplied by the service adapter this binds the plan to the exact
    // project/experiment context that will later produce evidence. A blank
    // value is allowed for an offline planning worksheet, but such a plan is
    // not accepted as current-experiment evidence.
    std::string experimentContextSha256;
    std::vector<std::string> requestedStandardProfiles;
};

struct StandardTestPlanResult {
    bool ready = false;
    std::vector<StandardApplicabilityResult> assessments;
    std::vector<std::string> blockers;
    std::vector<std::string> warnings;
    std::vector<std::string> requiredEvidence;
};

struct StandardTestPlanValidation {
    bool ok = false;
    std::string error;
    std::string planSha256;
    bool ready = false;
    int profileCount = 0;
};

// Project-owned metadata only. This deliberately contains no copyrighted
// standard text; it is the governed index used by the read-only agent tool.
const std::vector<StandardCatalogEntry>& standardCatalog();
bool isKnownStandardProfile(const std::string& id);
bool isExecutionApprovedStandardProfile(const std::string& id);
std::string standardsCatalogJson();
std::vector<StandardCatalogEntry> searchStandardCatalog(const std::string& query,
                                                        const std::string& authority = {},
                                                        const std::string& executionStatus = {});
StandardApplicabilityResult assessStandardApplicability(const StandardApplicabilityRequest& request);
StandardTestPlanResult assessStandardTestPlan(const StandardTestPlanRequest& request);
std::string standardApplicabilityJson(const StandardApplicabilityResult& result);
std::string standardTestPlanJson(const StandardTestPlanResult& result);
// Builds a context-bearing, integrity-digested plan envelope. The envelope
// contains metadata and derived findings only; it never embeds standard text.
std::string buildStandardTestPlanJson(const StandardTestPlanRequest& request);
// Recomputes the deterministic plan from its explicit context and rejects
// tampering, unknown profiles, duplicate profiles, or a stale experiment hash.
StandardTestPlanValidation validateStandardTestPlanJson(
    const std::string& json, const std::string& expectedExperimentContextSha256 = {});

} // namespace Model
