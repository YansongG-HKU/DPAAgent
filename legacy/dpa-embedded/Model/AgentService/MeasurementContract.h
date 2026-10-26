#pragma once

#include <string>
#include <vector>

namespace Model {

// A privacy-safe, source-bound description of one measured channel.  The
// contract intentionally contains metadata and hashes, never waveform samples.
struct CanonicalMeasurement {
    int version = 1;
    std::string measurementId;
    std::string sourceDataId;
    std::string assetId;
    std::string testId;
    int channelId = -1;
    std::string measurementPoint;
    std::string direction;
    std::string physicalQuantity;
    std::string unit;
    double samplingRateHz = 0;
    long long startTimeMs = 0;
    long long endTimeMs = 0;
    long long sampleCount = 0;
    std::string operatingCondition;
    std::string contextFingerprint;
    std::string sourceSha256;
    std::string calibrationProfile;
};

struct QualityReport {
    int version = 1;
    std::string status = "unknown"; // pass, warn, fail, unknown
    long long finiteSamples = 0;
    long long missingSamples = 0;
    long long clippedSamples = 0;
    bool monotonicTime = false;
    bool nyquistValid = false;
    bool calibrationValid = false;
    std::vector<std::string> codes;
};

struct EvidenceRef {
    int version = 1;
    std::string evidenceId;
    std::string kind;
    std::string measurementId;
    std::string sourceSha256;
    std::string algorithmVersion;
    std::string ruleVersion;
    long long createdAtMs = 0;
    std::string summarySha256;
    std::vector<std::string> artifactRefs;
};

struct ContractValidation {
    bool ok = false;
    std::string error;
};

std::string canonicalMeasurementJson(const CanonicalMeasurement& measurement);
std::string qualityReportJson(const QualityReport& quality);
std::string evidenceRefJson(const EvidenceRef& evidence);

ContractValidation validateCanonicalMeasurementJson(const std::string& json);
ContractValidation validateQualityReportJson(const std::string& json,
                                             long long sampleCount = -1);
ContractValidation validateEvidenceRefJson(const std::string& json,
                                           const std::string& expectedMeasurementId = {});

// Validates the complete P0 bundle.  Quality and evidence are required so a
// caller cannot accidentally treat a metadata-only measurement as a trusted
// diagnostic input.
ContractValidation validateMeasurementBundleJson(const std::string& measurementJson,
                                                 const std::string& qualityJson,
                                                 const std::string& evidenceJson);

} // namespace Model
