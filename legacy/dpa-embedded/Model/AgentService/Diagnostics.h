#pragma once
#include <string>
#include <vector>
#include "ToolResult.h"
#include "StandardsEvidence.h"
namespace Model {
    // --- Pure-core rotating-machinery diagnostic engine ---------------------------------
    // All functions here are deterministic and framework-free (unit-tested). The LLM calls
    // this engine so its conclusions are GROUNDED in computed spectral features, not guessed.

    inline constexpr const char* kDiagnosticAlgorithmVersion = "dpa-spectral-diagnostic/1.1.0";
    inline constexpr const char* kDiagnosticRuleVersion = "dpa-vibration-rules/1.2.0";
    inline constexpr const char* kDiagnosticResultContractVersion = "dpa-diagnostic-result/2.1.0";
    inline constexpr const char* kLegacyIso10816ClassProfile = "legacy-iso10816-class-i-iv/1.0.0";
    inline constexpr const char* kNoStandardProfile = "none";

    struct SpectralPeak { double freqHz = 0; double value = 0; double prominence = 0; };

    struct BearingGeometry {
        int rollingElements = 0;      // Nb
        double ballDiaMm = 0;         // Bd
        double pitchDiaMm = 0;        // Pd
        double contactAngleDeg = 0;   // contact angle
        bool contactAngleKnown = false; // zero degrees is valid, so presence must be explicit
    };
    struct BearingFreqs { double bpfo = 0, bpfi = 0, bsf = 0, ftf = 0; };

    // User/domain-owned context. Values are never inferred from spectra or names.
    struct AssetProfile {
        std::string assetId;             // stable plant identity, not a display name
        std::string machineType;
        double ratedPowerKw = 0;
        std::string applicabilityInfo;    // accepted instead of rated power when domain-owned
        double rpm = 0;
        std::string rpmSource;            // tachometer / control system / user-confirmed, etc.
        BearingGeometry bearing;
        std::string operatingCondition;
    };
    struct MeasurementProfile {
        std::string dataId;
        int channel = -1;
        std::string measurementPoint;
        std::string direction;
        std::string physicalQuantity;    // vibration_acceleration | vibration_velocity | vibration_displacement
        std::string unit;
        double samplingRateHz = 0;
        std::string standardProfile;      // exact versioned id, or explicit "none"
    };
    struct MeasurementBinding {
        std::string dataId;
        int channel = -1;
        std::string unit;
        double samplingRateHz = 0;
    };
    struct ProfileValidation {
        std::vector<std::string> errors;
        bool valid() const { return errors.empty(); }
    };

    struct DiagnosticFinding {
        std::string fault;        // imbalance | misalignment | looseness | bearing_outer|inner|ball|cage | resonance | severity
        std::string severity;     // info | warn | alarm | unassessed
        double ruleMatchScore = 0; // deterministic heuristic strength, NOT probability/confidence
        std::string evidence;     // grounded: the peak/frequency/ratio that supports this
        bool assessed = true;
        std::string ruleId;
        std::string ruleVersion = kDiagnosticRuleVersion;
        std::string algorithmVersion = kDiagnosticAlgorithmVersion;
        std::string standardProfile;
    };

    struct DiagnoseConfig {
        AssetProfile asset;
        MeasurementProfile measurement;
        double freqTolRatio = 0.05;     // match harmonics/bearing freqs within +-5%
        double severityVelocityRmsMmS = 0;
        int legacyIso10816MachineClass = 0;
        bool standardApplicabilityConfirmed = false;
        std::string standardApplicabilityBasis;
        std::string velocityRmsSource;
        std::string velocityRmsDataRef;
        StandardExecutionEvidence standardExecutionEvidence;
    };

    // Stable applicability scope; transient data IDs/RMS values are observation
    // provenance, not permission to change the approved asset or machine class.
    std::string diagnosticStandardScopeJson(const DiagnoseConfig& config);

    // Structural and physical validation for the user-supplied domain context.
    ProfileValidation validateMeasurementProfile(const MeasurementProfile& measurement);
    ProfileValidation validateDiagnosticProfile(const AssetProfile& asset,
                                                 const MeasurementProfile& measurement);
    // Pure: verify that the user/domain measurement profile identifies the actual
    // source channel. Empty means verified; otherwise a stable mismatch code.
    std::string validateMeasurementBinding(const MeasurementProfile& requested,
                                           const MeasurementBinding& actual);
    // Pure Nyquist gate shared by adapters and manifest-driven evals.
    std::string validateFrequencyRange(double samplingRateHz, double maxFrequencyHz);

    // Top-N local-maxima peaks, sorted by value desc.
    std::vector<SpectralPeak> findTopNPeaks(const std::vector<double>& x, const std::vector<double>& y, int n);
    // Amplitude at k*fundamental for k=1..maxOrder (0 if no peak within tol). Size == maxOrder.
    std::vector<double> harmonicAmplitudes(const std::vector<SpectralPeak>& peaks,
                                           double fundamentalHz, int maxOrder, double tolRatio);
    // Bearing characteristic frequencies from shaft rpm + geometry (Hz).
    BearingFreqs bearingFrequencies(double rpm, const BearingGeometry& g);
    // Project compatibility profile using the legacy ISO 10816 Class I-IV table.
    // This is not an ISO 20816 applicability decision; callers must confirm applicability separately.
    std::string legacyIso10816ClassZone(double velocityRmsMmS, int machineClass);
    // Kept for existing non-diagnostic baseline callers; identical legacy-table semantics.
    std::string isoSeverityZone(double velocityRmsMmS, int machineClass);
    // The rule engine: extracted peaks + config -> grounded findings (empty if nothing conclusive).
    std::vector<DiagnosticFinding> diagnoseVibration(const std::vector<SpectralPeak>& peaks,
                                                     const DiagnoseConfig& cfg);

    // --- Tool layer -------------------------------------------------------------------
    // Pure: the diagnostic knowledge base (fault signatures + bearing formulas + ISO) as JSON.
    std::string diagnosticReferenceJson();
    // Pure: parse diagnose tool args -> config (computes bearing freqs from geometry).
    DiagnoseConfig parseDiagnoseArgs(const std::string& argsJson);
    struct FaultAdvice {
        std::vector<std::string> rootCauses;      // grounded probable causes for a fault label
        std::vector<std::string> recommendations; // grounded recommended actions
    };
    // Pure: grounded root-cause + recommendation knowledge for a fault label (empty if unknown).
    // Keeps the assistant's advice tied to an engineering reference instead of free generation.
    // `severity` gates zone-dependent advice (the "severity"/ISO finding is info|warn|alarm):
    // an info-level (ISO zone A/B, within-limits) machine must NOT be told it exceeds limits.
    FaultAdvice faultAdvice(const std::string& fault, const std::string& severity = "");
    // Pure: findings -> grounded ToolResult (each finding carries its root_causes + recommendations).
    ToolResult buildDiagnoseResult(const std::vector<DiagnosticFinding>& findings);
    // Pure: findings + explicit diagnostic input -> the same result with a compact,
    // machine-readable provenance and assessment-scope contract.
    ToolResult buildDiagnoseResult(const std::vector<DiagnosticFinding>& findings,
                                   const DiagnoseConfig& config, int spectralPeakCount,
                                   double maxFrequencyHz);
    // Pure: peaks + explicit profile -> grounded result. An empty feature set is an
    // input error, not evidence of a healthy machine.
    ToolResult diagnoseSpectrum(const std::vector<SpectralPeak>& peaks, const DiagnoseConfig& cfg);
    struct CurrentDiagnosticSpectrum {
        bool valid = false;
        std::string error;
        std::string analysisType;
        std::vector<SpectralPeak> peaks;
    };
    // Adapter (framework-coupled, plugin-only): extract a supported frequency-domain
    // current result. It never interprets time/statistical/SRS/RF/SPL output as Hz.
    CurrentDiagnosticSpectrum currentDiagnosticSpectrum(class M* dpaTest);
    // Adapter: bind the user/domain measurement profile to the current analysis source.
    // Empty means verified; otherwise returns a stable mismatch/unavailable error code.
    std::string validateCurrentMeasurementBinding(class M* dpaTest,
                                                  const MeasurementProfile& measurement);

    // Pure: format a labelled spectral-peak list as a ToolResult.
    ToolResult buildPeaksResult(const std::vector<SpectralPeak>& peaks, const std::string& label);
    // Adapter (framework-coupled, plugin-only): envelope-demodulation spectrum peaks of a raw
    // time-domain channel waveform (band-pass -> Hilbert envelope -> FFT), for bearing diagnosis.
    std::vector<SpectralPeak> envelopeSpectrumPeaks(class M* dpaTest, const std::string& dataId,
                                                    int channel, double bandLoHz, double bandHiHz, int topN);
}
