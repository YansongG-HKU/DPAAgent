#pragma once
#include <string>
#include <vector>
#include "ToolResult.h"
#include "StandardsEvidence.h"
namespace Model {
    // A baseline is keyed by explicit asset/measurement identity plus physical quantity,
    // normalized unit, standard profile and a derived analysis-config fingerprint. The
    // transient data id is observation provenance, not part of the long-lived identity.

    inline constexpr const char* kBaselineFeatureDefinitionId = "dpa-spectrum-peak-trend/2.0.0";

    struct BaselineProvenance {
        std::string sourceDataId;
        int sourceChannel = -1;
        std::string sourceUnit;
        std::string sourcePhysicalType;
        double samplingRateHz = 0;
        std::string processType;
        std::string resultFunctionType;
        int resultDataForm = 0;
        int axisTypeX = 0;
        int frameSize = 0;
        int averageNum = 0;
        int overlapIndex = -1;
        double overlapValue = 0;
        int windowType = -1;
        std::string resultUnit;
        std::string configFingerprint;
    };

    struct BaselineObservation {
        long long capturedMs = 0;
        std::string isoZone;
        std::vector<double> peakFreqsHz;
        BaselineProvenance provenance;
        std::string standardProfile;
        int legacyMachineClass = 0;
        std::string standardApplicabilityBasis;
        std::string severitySource;
        std::string severityDataRef;
        std::string standardEvidenceJson;
    };

    struct MachineBaseline {
        std::string assetId;
        std::string measurementPoint;
        std::string direction;
        std::string operatingCondition;
        std::string physicalQuantity;
        std::string unit;
        std::string featureDefinitionId = kBaselineFeatureDefinitionId;
        std::string configFingerprint;
        std::string standardProfile = "none";
        int legacyMachineClass = 0;
        std::vector<double> peakFreqsHz;    // dominant peak frequencies when healthy
        std::string isoZone;                // "", "A".."D"
        std::string standardEvidenceJson;   // historical receipt, never a live permission
        long long capturedMs = 0;
        bool valid = false;                 // false = no baseline stored for this machine
        std::string error;
        BaselineProvenance provenance;
        std::vector<BaselineObservation> history; // confirmed baseline captures, oldest first
    };

    struct DeviationReport {
        bool hasBaseline = false;
        std::vector<double> newPeaksHz;     // peaks present now but absent in the baseline
        bool isoWorsened = false;
        std::string severity;               // info | warn | alarm
        std::string summary;
    };

    // Pure: how many ISO zones worse `cur` is vs `base` (A<B<C<D); <=0 if not worse / unknown.
    int isoZoneDelta(const std::string& base, const std::string& cur);

    // Pure: compare a current measurement to a stored baseline -> grounded deviation report.
    // Escalation: new peaks / ISO worse-by-1 -> warn; zone D / worse-by>=2 -> alarm.
    // Absolute magnitude comparison is intentionally excluded until a canonical,
    // calibrated time-domain RMS extractor is available.
    DeviationReport compareToBaseline(const MachineBaseline& base,
                                      const std::vector<double>& currentPeaksHz,
                                      const std::string& currentIsoZone,
                                      double peakTolRatio);

    // --- Tool layer -------------------------------------------------------------------
    struct BaselineArgs {
        std::string assetId;
        std::string measurementPoint;
        std::string direction;
        std::string operatingCondition;
        std::string standardProfile = "none";
        std::string dataId;
        int channel = -1;
        std::string physicalQuantity;
        std::string unit;
        double samplingRateHz = 0;
        bool standardApplicabilityConfirmed = false;
        std::string standardApplicabilityBasis;
        double severityVelocityRmsMmS = 0;
        std::string severitySource;
        std::string severityDataRef;
        int legacyIso10816MachineClass = 0;
        int topN = 8;
        StandardExecutionEvidence standardExecutionEvidence;
    };
    std::string baselineStandardScopeJson(const BaselineArgs& args);
    // Empty unless the complete measurement identity and analysis fingerprint are present.
    std::string baselineKey(const BaselineArgs& args, const std::string& configFingerprint);
    std::string baselineConfigFingerprint(const BaselineProvenance& provenance);
    std::string serializeBaseline(const MachineBaseline& baseline);
    MachineBaseline parseBaseline(const std::string& json);
    // Pure: parse save_baseline / compare_baseline args.
    BaselineArgs parseBaselineArgs(const std::string& argsJson);
    // Empty unless the explicit legacy profile, applicability, units and inputs are valid.
    std::string baselineSeverityZone(const BaselineArgs& args);
    // Pure: a deviation report -> ToolResult (JSON + human summary; ok even with no baseline).
    ToolResult buildDeviationResult(const DeviationReport& d);
    ToolResult buildBaselineTrendResult(const MachineBaseline& baseline);
    // Adapter (framework-coupled, plugin-only): current result's dominant peak frequencies
    // plus exact analysis/source provenance for storing/comparing a baseline.
    MachineBaseline currentBaselineFeatures(class M* dpaTest, int topN);
}
