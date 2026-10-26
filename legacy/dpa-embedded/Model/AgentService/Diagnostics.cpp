#include "Diagnostics.h"
#include "StandardsCatalog.h"
#include "AgentJson.h"
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
using namespace Model;

namespace {
bool blank(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

std::string normalizedUnit(const std::string& value) {
    std::string normalized;
    for (unsigned char c : value)
        if (!std::isspace(c)) normalized.push_back(static_cast<char>(std::tolower(c)));
    return normalized;
}

std::string joinErrors(const std::vector<std::string>& errors) {
    std::string joined;
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i) joined += ", ";
        joined += errors[i];
    }
    return joined;
}

DiagnosticFinding finding(const char* fault, const char* severity, double score,
                          std::string evidence, const char* ruleId,
                          const std::string& standardProfile, bool assessed = true) {
    DiagnosticFinding out;
    out.fault = fault;
    out.severity = severity;
    out.ruleMatchScore = score;
    out.evidence = std::move(evidence);
    out.assessed = assessed;
    out.ruleId = ruleId;
    out.standardProfile = standardProfile;
    return out;
}

DiagnosticFinding unassessed(const char* fault, std::string reason,
                             const std::string& standardProfile, const char* ruleId) {
    return finding(fault, "unassessed", 0, "not assessed: " + std::move(reason),
                   ruleId, standardProfile, false);
}

bool isVelocityMmPerSecond(const std::string& unit) {
    const std::string normalized = normalizedUnit(unit);
    return normalized == "mm/s" || normalized == "mm/srms";
}

bool isSupportedVibrationUnit(const std::string& quantity, const std::string& unit) {
    const std::string normalized = normalizedUnit(unit);
    if (quantity == "vibration_acceleration")
        return normalized == "g" || normalized == "m/s^2" || normalized == "m/s²"
            || normalized == "mm/s^2" || normalized == "mm/s²";
    if (quantity == "vibration_velocity")
        return normalized == "mm/s" || normalized == "mm/srms" || normalized == "m/s";
    if (quantity == "vibration_displacement")
        return normalized == "um" || normalized == "µm" || normalized == "μm"
            || normalized == "mm";
    return false;
}

bool bearingGeometryProvided(const BearingGeometry& bearing) {
    return bearing.rollingElements != 0 || bearing.ballDiaMm != 0 || bearing.pitchDiaMm != 0
        || bearing.contactAngleKnown;
}

bool bearingGeometryValid(const BearingGeometry& bearing) {
    return bearing.rollingElements >= 3
        && std::isfinite(bearing.ballDiaMm) && bearing.ballDiaMm > 0
        && std::isfinite(bearing.pitchDiaMm) && bearing.pitchDiaMm > bearing.ballDiaMm
        && bearing.contactAngleKnown && std::isfinite(bearing.contactAngleDeg)
        && bearing.contactAngleDeg >= 0 && bearing.contactAngleDeg <= 90;
}
}

ProfileValidation Model::validateMeasurementProfile(const MeasurementProfile& measurement) {
    ProfileValidation result;
    auto required = [&](const std::string& value, const char* name) {
        if (blank(value)) result.errors.push_back(name);
    };
    required(measurement.dataId, "measurement.dataId");
    if (measurement.channel < 0) result.errors.push_back("measurement.channel");
    required(measurement.measurementPoint, "measurement.measurementPoint");
    required(measurement.direction, "measurement.direction");
    required(measurement.physicalQuantity, "measurement.physicalQuantity");
    required(measurement.unit, "measurement.unit");
    if (!blank(measurement.physicalQuantity) && !blank(measurement.unit)
        && !isSupportedVibrationUnit(measurement.physicalQuantity, measurement.unit))
        result.errors.push_back("measurement.unit incompatible with physicalQuantity");
    if (!(std::isfinite(measurement.samplingRateHz) && measurement.samplingRateHz > 0))
        result.errors.push_back("measurement.samplingRateHz");
    required(measurement.standardProfile, "measurement.standardProfile");
    if (!blank(measurement.standardProfile)
        && measurement.standardProfile != kNoStandardProfile
        && measurement.standardProfile != kLegacyIso10816ClassProfile)
        result.errors.push_back(isKnownStandardProfile(measurement.standardProfile)
            ? "measurement.standardProfile catalog_only_requires_approval"
            : "measurement.standardProfile unsupported");
    return result;
}

ProfileValidation Model::validateDiagnosticProfile(const AssetProfile& asset,
                                                    const MeasurementProfile& measurement) {
    ProfileValidation result;
    auto required = [&](const std::string& value, const char* name) {
        if (blank(value)) result.errors.push_back(name);
    };
    required(asset.assetId, "asset.assetId");
    required(asset.machineType, "asset.machineType");
    if (!(std::isfinite(asset.rpm) && asset.rpm > 0)) result.errors.push_back("asset.rpm");
    required(asset.rpmSource, "asset.rpmSource");
    required(asset.operatingCondition, "asset.operatingCondition");

    ProfileValidation measurementResult = validateMeasurementProfile(measurement);
    result.errors.insert(result.errors.end(), measurementResult.errors.begin(),
                         measurementResult.errors.end());
    return result;
}

std::string Model::validateMeasurementBinding(const MeasurementProfile& requested,
                                              const MeasurementBinding& actual) {
    if (actual.dataId.empty() || actual.channel < 0 || actual.unit.empty()
        || !(std::isfinite(actual.samplingRateHz) && actual.samplingRateHz > 0))
        return "current_result_provenance_unavailable";
    if (actual.dataId != requested.dataId) return "measurement_data_mismatch";
    if (actual.channel != requested.channel) return "measurement_channel_mismatch";
    if (normalizedUnit(actual.unit) != normalizedUnit(requested.unit))
        return "measurement_unit_mismatch";
    const double tolerance = std::max(1e-6, std::fabs(actual.samplingRateHz) * 1e-6);
    if (std::fabs(actual.samplingRateHz - requested.samplingRateHz) > tolerance)
        return "measurement_sampling_rate_mismatch";
    return {};
}

std::string Model::validateFrequencyRange(double samplingRateHz, double maxFrequencyHz) {
    if (!(std::isfinite(samplingRateHz) && samplingRateHz > 0))
        return "invalid_sampling_rate";
    if (!(std::isfinite(maxFrequencyHz) && maxFrequencyHz >= 0))
        return "invalid_frequency_range";
    const double tolerance = std::max(1e-6, samplingRateHz * 1e-6);
    if (maxFrequencyHz > samplingRateHz / 2.0 + tolerance)
        return "band_exceeds_nyquist";
    return {};
}

std::vector<SpectralPeak> Model::findTopNPeaks(const std::vector<double>& x, const std::vector<double>& y, int n) {
    std::vector<SpectralPeak> peaks;
    const size_t sz = std::min(x.size(), y.size());
    for (size_t i = 1; i + 1 < sz; ++i) {
        if (y[i] > y[i-1] && y[i] >= y[i+1]) {          // local maximum
            SpectralPeak p;
            p.freqHz = x[i];
            p.value = y[i];
            p.prominence = y[i] - std::min(y[i-1], y[i+1]);
            peaks.push_back(p);
        }
    }
    std::sort(peaks.begin(), peaks.end(),
              [](const SpectralPeak& a, const SpectralPeak& b){ return a.value > b.value; });
    if (n > 0 && (int)peaks.size() > n) peaks.resize(n);
    return peaks;
}

std::vector<double> Model::harmonicAmplitudes(const std::vector<SpectralPeak>& peaks,
                                              double fundamentalHz, int maxOrder, double tolRatio) {
    std::vector<double> out(maxOrder > 0 ? maxOrder : 0, 0.0);
    if (fundamentalHz <= 0) return out;
    for (int k = 1; k <= maxOrder; ++k) {
        const double target = k * fundamentalHz;
        const double tol = tolRatio * target;
        double best = 0;
        for (const auto& p : peaks)
            if (std::fabs(p.freqHz - target) <= tol && p.value > best) best = p.value;
        out[k-1] = best;
    }
    return out;
}

BearingFreqs Model::bearingFrequencies(double rpm, const BearingGeometry& g) {
    BearingFreqs f;
    if (rpm <= 0 || g.rollingElements < 3 || g.ballDiaMm <= 0 || g.pitchDiaMm <= g.ballDiaMm
        || !g.contactAngleKnown || g.contactAngleDeg < 0 || g.contactAngleDeg > 90) return f;
    const double fr = rpm / 60.0;                                  // shaft Hz
    const double ratio = (g.ballDiaMm / g.pitchDiaMm) * std::cos(g.contactAngleDeg * 3.14159265358979 / 180.0);
    f.ftf  = (fr / 2.0) * (1.0 - ratio);
    f.bpfo = (g.rollingElements * fr / 2.0) * (1.0 - ratio);
    f.bpfi = (g.rollingElements * fr / 2.0) * (1.0 + ratio);
    if (g.ballDiaMm > 0)
        f.bsf = (g.pitchDiaMm * fr / (2.0 * g.ballDiaMm)) * (1.0 - ratio * ratio);
    return f;
}

std::string Model::legacyIso10816ClassZone(double v, int machineClass) {
    if (v <= 0 || machineClass < 1 || machineClass > 4) return "";
    // Versioned project compatibility table. It is evaluated only after the caller confirms
    // the legacy profile's applicability; these thresholds are never presented as ISO 20816.
    static const double ab[4] = {0.71, 1.12, 1.80, 2.80};
    static const double bc[4] = {1.80, 2.80, 4.50, 7.10};
    static const double cd[4] = {4.50, 7.10, 11.20, 18.00};
    const int i = machineClass - 1;
    if (v < ab[i]) return "A";
    if (v < bc[i]) return "B";
    if (v < cd[i]) return "C";
    return "D";
}

std::string Model::isoSeverityZone(double v, int machineClass) {
    return legacyIso10816ClassZone(v, machineClass);
}

static std::string fmt(const char* label, double freq, double val) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s @ %.2f Hz = %.4g", label, freq, val);
    return std::string(buf);
}

std::vector<DiagnosticFinding> Model::diagnoseVibration(const std::vector<SpectralPeak>& peaks,
                                                        const DiagnoseConfig& cfg) {
    std::vector<DiagnosticFinding> out;
    const ProfileValidation profile = validateDiagnosticProfile(cfg.asset, cfg.measurement);
    if (!profile.valid()) {
        out.push_back(unassessed("profile", "missing/invalid fields: " + joinErrors(profile.errors),
                                 cfg.measurement.standardProfile, "profile.required_fields"));
        return out;
    }

    const double tol = cfg.freqTolRatio;
    const double fr = cfg.asset.rpm / 60.0;
    const bool hasBearingGeometry = bearingGeometryProvided(cfg.asset.bearing);
    const bool validBearingGeometry = bearingGeometryValid(cfg.asset.bearing);
    const BearingFreqs bearing = validBearingGeometry
        ? bearingFrequencies(cfg.asset.rpm, cfg.asset.bearing) : BearingFreqs{};
    const std::string& standardProfile = cfg.measurement.standardProfile;

    if (!validBearingGeometry) {
        out.push_back(unassessed("bearing",
            hasBearingGeometry ? "bearing geometry is incomplete or physically invalid"
                               : "bearing geometry was not provided",
            standardProfile, "bearing.geometry.required"));
    }

    // --- Harmonic-based rotor faults (need shaft speed) ---
    if (fr > 0 && !peaks.empty()) {
        std::vector<double> h = harmonicAmplitudes(peaks, fr, 5, tol);
        const double h1 = h[0], h2 = h[1], h3 = h[2];
        const double maxHigher = std::max({h2, h3, h[3], h[4]});

        if (h1 > 0 && h1 >= 3.0 * maxHigher)
            out.push_back(finding("imbalance", "warn", 0.8, "1x " + fmt("dominant", fr, h1),
                                  "rotor.imbalance.1x_dominant", standardProfile));

        int nHarm = 0;
        for (double v : h) if (h1 > 0 && v >= 0.3 * h1) ++nHarm;
        // A broad 1x harmonic train is the more specific looseness signature. Do not
        // double-label the same synthetic evidence as misalignment solely because 2x is high.
        if (h1 > 0 && h2 >= 0.4 * h1 && nHarm < 3)
            out.push_back(finding("misalignment", "warn", 0.7, "2x " + fmt("elevated", 2*fr, h2),
                                  "rotor.misalignment.2x_elevated", standardProfile));

        if (nHarm >= 3)
            out.push_back(finding("looseness", "warn", 0.6,
                std::to_string(nHarm) + " harmonics of 1x >= 30% of 1x (looseness signature)",
                "rotor.looseness.harmonic_series", standardProfile));
    }

    // --- Bearing faults (need geometry-derived characteristic freqs) ---
    // A single coincident line is not enough: require a characteristic line plus
    // a harmonic or physically meaningful sideband before emitting a fault label.
    if (bearing.bpfo > 0) {
        struct BF { const char* fault; const char* label; double f; };
        const BF bfs[4] = {
            {"bearing_outer", "BPFO", bearing.bpfo},
            {"bearing_inner", "BPFI", bearing.bpfi},
            {"bearing_ball",  "BSF",  bearing.bsf},
            {"bearing_cage",  "FTF",  bearing.ftf},
        };
        auto nearPeak = [&](double target) -> const SpectralPeak* {
            if (!(target > 0)) return nullptr;
            for (const auto& peak : peaks)
                if (std::fabs(peak.freqHz - target) <= tol * target) return &peak;
            return nullptr;
        };
        for (const auto& bf : bfs) {
            if (bf.f <= 0) continue;
            const SpectralPeak* primary = nearPeak(bf.f);
            if (!primary) continue;
            const SpectralPeak* corroborating = nearPeak(2.0 * bf.f);
            if (!corroborating && std::string(bf.fault) == "bearing_inner") {
                corroborating = nearPeak(bf.f + fr);
                if (!corroborating && bf.f > fr) corroborating = nearPeak(bf.f - fr);
            }
            if (!corroborating && std::string(bf.fault) == "bearing_ball") {
                corroborating = nearPeak(bf.f + bearing.ftf);
                if (!corroborating && bf.f > bearing.ftf) corroborating = nearPeak(bf.f - bearing.ftf);
            }
            if (!corroborating) continue;
            out.push_back(finding(bf.fault, "warn", 0.65,
                std::string("corroborated ") + fmt(bf.label, primary->freqHz, primary->value)
                    + "; support " + fmt("line", corroborating->freqHz, corroborating->value),
                "bearing.characteristic_frequency_corroborated", standardProfile));
        }
    }

    // Resonance is deliberately not inferred from one spectrum. It requires
    // modal/speed-sweep or known-natural-frequency evidence not present here.

    // --- Explicit legacy severity profile (never generalized to ISO 20816) ---
    if (standardProfile == kLegacyIso10816ClassProfile) {
        if (!cfg.standardApplicabilityConfirmed || blank(cfg.standardApplicabilityBasis)) {
            out.push_back(unassessed("severity", "standard applicability confirmation and basis are required",
                                     standardProfile, "severity.legacy_profile.applicability"));
        } else if (!standardExecutionEvidenceMatches(cfg.standardExecutionEvidence, standardProfile,
                       diagnosticStandardScopeJson(cfg), kDiagnosticRuleVersion)) {
            out.push_back(unassessed("severity", "authorized source, clauses and context-bound domain approval are required; "
                                     + cfg.standardExecutionEvidence.error,
                                     standardProfile, "severity.legacy_profile.authorized_evidence"));
        } else if (!isVelocityMmPerSecond(cfg.measurement.unit)) {
            out.push_back(unassessed("severity", "measurement unit must be mm/s RMS for this profile",
                                     standardProfile, "severity.legacy_profile.unit"));
        } else if (blank(cfg.velocityRmsSource) || blank(cfg.velocityRmsDataRef)) {
            out.push_back(unassessed("severity", "velocity RMS source and data reference are required",
                                     standardProfile, "severity.legacy_profile.provenance"));
        } else if (cfg.severityVelocityRmsMmS <= 0
                   || cfg.legacyIso10816MachineClass < 1
                   || cfg.legacyIso10816MachineClass > 4) {
            out.push_back(unassessed("severity", "velocity RMS and legacy machine class 1..4 are required",
                                     standardProfile, "severity.legacy_profile.inputs"));
        } else {
            const std::string zone = legacyIso10816ClassZone(cfg.severityVelocityRmsMmS,
                                                              cfg.legacyIso10816MachineClass);
        const char* sev = (zone == "D") ? "alarm" : (zone == "C") ? "warn" : "info";
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s; class %d; %.3g mm/s RMS -> zone %s",
                          kLegacyIso10816ClassProfile, cfg.legacyIso10816MachineClass,
                          cfg.severityVelocityRmsMmS, zone.c_str());
            out.push_back(finding("severity", sev, 1.0, std::string(buf)
                                  + "; source=" + cfg.velocityRmsSource
                                  + "; data_ref=" + cfg.velocityRmsDataRef
                                  + "; applicability_basis=" + cfg.standardApplicabilityBasis
                                  + "; source_sha256=" + cfg.standardExecutionEvidence.sourceSha256
                                  + "; approval_sha256=" + cfg.standardExecutionEvidence.approvalSha256,
                                  "severity.legacy_iso10816_class_zone", standardProfile));
        }
    } else if (cfg.severityVelocityRmsMmS > 0 || cfg.legacyIso10816MachineClass > 0) {
        out.push_back(unassessed("severity", "standardProfile is none; severity thresholds were not applied",
                                 standardProfile, "severity.standard_profile.required"));
    }
    return out;
}

std::string Model::diagnosticReferenceJson() {
    return std::string("{\"algorithm_version\":\"") + kDiagnosticAlgorithmVersion
        + "\",\"rule_version\":\"" + kDiagnosticRuleVersion
        + "\",\"result_contract_version\":\"" + kDiagnosticResultContractVersion
        + "\",\"score_semantics\":\"rule_match_score is a deterministic rule strength, not a probability\""
        + R"JSON(,"knowledge_governance":{"claim_policy":"deterministic rules emit reviewable diagnostic hypotheses, not machine-condition certification","validation_status":"automated_regression_only","domain_owner_approval_status":"pending","pending_evidence":["authorized real-machine cases","legacy standard applicability, source and data-reference approval"]},
"rule_contracts":[{"rule_id":"rotor.imbalance.1x_dominant","evidence_requirement":"explicit rpm plus dominant 1x peak"},{"rule_id":"rotor.misalignment.2x_elevated","evidence_requirement":"explicit rpm plus elevated 2x without a broad harmonic train"},{"rule_id":"rotor.looseness.harmonic_series","evidence_requirement":"explicit rpm plus at least three 1x harmonics"},{"rule_id":"bearing.characteristic_frequency_corroborated","evidence_requirement":"domain-supplied bearing geometry plus characteristic line and harmonic or sideband corroboration"},{"rule_id":"severity.legacy_iso10816_class_zone","evidence_requirement":"domain-approved applicability and basis plus velocity RMS source and data reference"}])JSON"
        + R"JSON(,"faultSignatures":[
{"fault":"imbalance","signature":"1x 转频主导，2x/3x 很小","action":"做动平衡"},
{"fault":"misalignment","signature":"2x 明显，常伴轴向高","action":"校准对中"},
{"fault":"looseness","signature":"1x 及多次谐波(2x,3x,4x…)抬升 + 底噪升高","action":"紧固/检查配合"},
{"fault":"bearing_outer","signature":"包络谱在 BPFO 及其谐波出峰","action":"更换轴承(外圈)"},
{"fault":"bearing_inner","signature":"BPFI 及其 ±1x 边带","action":"更换轴承(内圈)"},
{"fault":"bearing_ball","signature":"BSF 及其 ±FTF 边带","action":"更换轴承(滚动体)"},
{"fault":"bearing_cage","signature":"FTF 低频","action":"更换轴承(保持架)"},
{"fault":"resonance","signature":"需模态/升降速或已知固有频率证据；单个非同步峰不足以自动判定","action":"先做模态或升降速验证"}
],"bearingFormulas":{"fr":"rpm/60","ratio":"(Bd/Pd)cos(angle)","BPFO":"(Nb/2)fr(1-ratio)","BPFI":"(Nb/2)fr(1+ratio)","BSF":"(Pd/2Bd)fr(1-ratio^2)","FTF":"(fr/2)(1-ratio)"},
"standardProfiles":[{"id":")JSON" + kLegacyIso10816ClassProfile
        + R"JSON(","basis":"legacy ISO 10816-1 Class I-IV compatibility table","requirements":"domain-confirmed applicability, velocity RMS in mm/s, class 1..4"},{"id":")JSON"
        + kNoStandardProfile
        + R"JSON(","basis":"no standard severity evaluation requested"}],
"note":"asset and measurement profiles, rpm source, bearing geometry, unit, sampling rate and applicability are user/domain supplied; missing fields are not inferred"})JSON";
}

std::string Model::diagnosticStandardScopeJson(const DiagnoseConfig& cfg) {
    return AgentJson::dump(QJsonObject{
        {"operation", "diagnostic_legacy_severity"},
        {"asset_id", QString::fromStdString(cfg.asset.assetId)},
        {"machine_type", QString::fromStdString(cfg.asset.machineType)},
        {"rated_power_kw", cfg.asset.ratedPowerKw},
        {"applicability_info", QString::fromStdString(cfg.asset.applicabilityInfo)},
        {"rpm", cfg.asset.rpm},
        {"operating_condition", QString::fromStdString(cfg.asset.operatingCondition)},
        {"measurement_point", QString::fromStdString(cfg.measurement.measurementPoint)},
        {"direction", QString::fromStdString(cfg.measurement.direction)},
        {"physical_quantity", QString::fromStdString(cfg.measurement.physicalQuantity)},
        {"unit", QString::fromStdString(cfg.measurement.unit)},
        {"sampling_rate_hz", cfg.measurement.samplingRateHz},
        {"legacy_machine_class", cfg.legacyIso10816MachineClass},
        {"applicability_basis", QString::fromStdString(cfg.standardApplicabilityBasis)}
    });
}

DiagnoseConfig Model::parseDiagnoseArgs(const std::string& argsJson) {
    DiagnoseConfig cfg;
    QJsonObject o = AgentJson::parseObject(argsJson);

    const QJsonObject asset = o.value("asset").toObject();
    const QJsonObject assetValues = asset.isEmpty() ? o : asset;
    cfg.asset.assetId = assetValues.value("assetId").toString().toStdString();
    cfg.asset.machineType = assetValues.value("machineType").toString().toStdString();
    cfg.asset.ratedPowerKw = assetValues.value("ratedPowerKw").toDouble(0);
    cfg.asset.applicabilityInfo = assetValues.value("applicabilityInfo").toString().toStdString();
    cfg.asset.rpm = assetValues.value("rpm").toDouble(0);
    cfg.asset.rpmSource = assetValues.value("rpmSource").toString().toStdString();
    cfg.asset.operatingCondition = assetValues.value("operatingCondition").toString().toStdString();

    const QJsonObject nestedBearing = assetValues.value("bearing").toObject();
    const QJsonObject bearing = nestedBearing.isEmpty() ? assetValues : nestedBearing;
    cfg.asset.bearing.rollingElements = bearing.value("rollingElements").toInt(0);
    cfg.asset.bearing.ballDiaMm = bearing.value("ballDiaMm").toDouble(0);
    cfg.asset.bearing.pitchDiaMm = bearing.value("pitchDiaMm").toDouble(0);
    cfg.asset.bearing.contactAngleDeg = bearing.value("contactAngleDeg").toDouble(0);
    cfg.asset.bearing.contactAngleKnown = bearing.contains("contactAngleDeg")
                                         && bearing.value("contactAngleDeg").isDouble();

    const QJsonObject measurement = o.value("measurement").toObject();
    const QJsonObject measurementValues = measurement.isEmpty() ? o : measurement;
    cfg.measurement.dataId = measurementValues.value("dataId").toString().toStdString();
    cfg.measurement.channel = measurementValues.contains("channel")
                            ? measurementValues.value("channel").toInt(-1) : -1;
    cfg.measurement.measurementPoint = measurementValues.value("measurementPoint").toString().toStdString();
    cfg.measurement.direction = measurementValues.value("direction").toString().toStdString();
    cfg.measurement.physicalQuantity = measurementValues.value("physicalQuantity").toString().toStdString();
    cfg.measurement.unit = measurementValues.value("unit").toString().toStdString();
    cfg.measurement.samplingRateHz = measurementValues.value("samplingRateHz").toDouble(0);
    cfg.measurement.standardProfile = measurementValues.value("standardProfile").toString().toStdString();

    const QJsonObject standard = o.value("standard").toObject();
    if (cfg.measurement.standardProfile.empty())
        cfg.measurement.standardProfile = standard.value("profile").toString().toStdString();
    cfg.standardApplicabilityConfirmed = standard.contains("applicabilityConfirmed")
        ? standard.value("applicabilityConfirmed").toBool(false)
        : o.value("standardApplicabilityConfirmed").toBool(false);
    cfg.standardApplicabilityBasis = standard.value("applicabilityBasis").toString().toStdString();
    cfg.severityVelocityRmsMmS = standard.contains("velocityRmsMmS")
        ? standard.value("velocityRmsMmS").toDouble(0)
        : o.value("isoVelocityRmsMmS").toDouble(0);
    cfg.velocityRmsSource = standard.value("velocityRmsSource").toString().toStdString();
    cfg.velocityRmsDataRef = standard.value("velocityRmsDataRef").toString().toStdString();
    cfg.legacyIso10816MachineClass = standard.contains("legacyMachineClass")
        ? standard.value("legacyMachineClass").toInt(0)
        : o.value("isoMachineClass").toInt(0);
    if (o.contains("freqTolRatio")) cfg.freqTolRatio = o.value("freqTolRatio").toDouble(0);
    return cfg;
}

FaultAdvice Model::faultAdvice(const std::string& fault, const std::string& severity) {
    // Grounded rotating-machinery knowledge (root causes + recommended actions) per fault label.
    // Labels match diagnoseVibration's output exactly.
    if (fault == "imbalance")
        return { {"转子质量分布不均/偏心", "叶轮积垢或叶片缺损", "联轴器或配重件缺失"},
                 {"按经领域负责人批准的平衡等级与作业规程做动平衡", "检查转子是否积垢、结垢或部件缺失", "复查安装配重与键块"} };
    if (fault == "misalignment")
        return { {"联轴器对中不良(角度/平行偏差)", "轴弯曲", "地脚沉降或基础变形"},
                 {"用激光对中仪校准联轴器", "检查轴是否弯曲、联轴器磨损", "复查地脚螺栓与基础水平"} };
    if (fault == "looseness")
        return { {"地脚螺栓或轴承座松动", "轴承配合过盈不足/间隙过大", "结构裂纹或垫铁松动"},
                 {"紧固地脚螺栓与轴承座", "检查轴承配合间隙与结构裂纹", "复测基础刚度与垫铁贴合"} };
    if (fault == "bearing_outer")
        return { {"轴承外圈滚道剥落/点蚀", "润滑不良或污染", "外圈配合过紧/装配损伤"},
                 {"安排更换轴承(外圈损伤)", "改善润滑与密封、控制污染", "缩短该轴承巡检周期、跟踪包络谱趋势"} };
    if (fault == "bearing_inner")
        return { {"轴承内圈滚道剥落/点蚀", "预紧或装配不当", "润滑失效"},
                 {"安排更换轴承(内圈损伤)", "核查装配与预紧、监测轴承温度", "跟踪 BPFI 及其边带趋势"} };
    if (fault == "bearing_ball")
        return { {"滚动体表面损伤/剥落", "润滑不良或混入异物"},
                 {"安排更换轴承(滚动体损伤)", "检查润滑质量与异物污染", "复测振动与包络谱趋势"} };
    if (fault == "bearing_cage")
        return { {"保持架磨损、变形或断裂", "润滑不足或转速工况异常"},
                 {"安排更换轴承(保持架损伤)", "评估润滑与实际转速工况"} };
    if (fault == "bearing")
        return { {"轴承几何缺失或无效，轴承特征频率子域未评估"},
                 {"由设备负责人补齐并确认滚动体数量、滚动体直径、节圆直径和接触角"} };
    if (fault == "resonance")
        return { {"激励频率接近结构/转子固有频率", "系统刚度或阻尼不足"},
                 {"避开共振转速区间运行", "增加阻尼或调整结构刚度", "做模态测试确认固有频率后再调整"} };
    if (fault == "profile")
        return { {"资产或测量档案不完整，诊断规则未执行"},
                 {"由设备负责人补齐并确认资产 ID、转速来源、轴承几何、测点、方向、单位、采样率、工况和标准配置"} };
    if (fault == "severity") {
        if (severity == "unassessed")
            return { {"标准配置适用性、单位或阈值输入不足，未做严重度评估"},
                     {"由领域负责人确认标准配置适用性和输入后再评估"} };
        // Zone-gated legacy compatibility profile: info (A/B) | warn (C) | alarm (D).
        if (severity == "info")
            return { {"整体振动烈度落在已确认适用的 legacy ISO 10816-1 Class I-IV 兼容配置 A/B 区"},
                     {"维持常规监测周期", "如烈度后续上升再复查"} };
        if (severity == "warn")
            return { {"整体振动烈度落入已确认适用的 legacy ISO 10816-1 Class I-IV 兼容配置 C 区"},
                     {"提高监测频次", "结合谐波/包络定位主导故障源并计划检修"} };
        // alarm (zone D) or unspecified -> require an owner-controlled safety decision.
        return { {"整体振动烈度落入已确认适用的 legacy ISO 10816-1 Class I-IV 兼容配置 D 区"},
                 {"由设备负责人按安全规程评估是否停机检修", "结合谐波/包络定位主导故障源", "排查并消除主要振动源后复测"} };
    }
    return {};   // unknown label -> no fabricated advice
}

ToolResult Model::buildDiagnoseResult(const std::vector<DiagnosticFinding>& findings) {
    ToolResult r;
    QJsonArray arr;
    int assessedCount = 0;
    int unassessedCount = 0;
    std::string standardProfile;
    for (const auto& f : findings) {
        QJsonObject o;
        o["fault"] = QString::fromStdString(f.fault);
        o["severity"] = QString::fromStdString(f.severity);
        o["assessed"] = f.assessed;
        o["rule_match_score"] = f.ruleMatchScore;
        o["rule_id"] = QString::fromStdString(f.ruleId);
        o["rule_version"] = QString::fromStdString(f.ruleVersion);
        o["algorithm_version"] = QString::fromStdString(f.algorithmVersion);
        o["standard_profile"] = QString::fromStdString(f.standardProfile);
        o["evidence"] = QString::fromStdString(f.evidence);
        // Fold in grounded root-cause + recommendation knowledge so the assistant's advice
        // is tied to the engineering reference rather than free-generated per finding.
        FaultAdvice adv = faultAdvice(f.fault, f.severity);
        QJsonArray rc; for (const auto& s : adv.rootCauses)      rc.append(QString::fromStdString(s));
        QJsonArray re; for (const auto& s : adv.recommendations) re.append(QString::fromStdString(s));
        o["root_causes"] = rc;
        o["recommendations"] = re;
        arr.append(o);
        if (f.assessed) ++assessedCount;
        else ++unassessedCount;
        if (standardProfile.empty() && !f.standardProfile.empty()) standardProfile = f.standardProfile;
    }
    QJsonObject d;
    d["assessment_status"] = findings.empty() ? QString("no_finding")
                             : assessedCount > 0 && unassessedCount > 0 ? QString("partially_assessed")
                             : assessedCount > 0 ? QString("assessed") : QString("unassessed");
    d["assessed_count"] = assessedCount;
    d["unassessed_count"] = unassessedCount;
    d["algorithm_version"] = QString::fromUtf8(kDiagnosticAlgorithmVersion);
    d["rule_version"] = QString::fromUtf8(kDiagnosticRuleVersion);
    d["standard_profile"] = QString::fromStdString(standardProfile);
    d["findings"] = arr;
    r.dataJson = AgentJson::dump(d);
    if (findings.empty()) {
        r.summary = "规则已执行，未发现达到阈值的故障特征。";
    } else if (assessedCount == 0) {
        r.ok = false;
        r.error = "diagnostic_unassessed";
        r.summary = "诊断未评估：" + findings.front().evidence;
    } else {
        std::string s = "诊断发现 " + std::to_string(assessedCount) + " 项：";
        for (const auto& f : findings) if (f.assessed) s += f.fault + "(" + f.severity + ") ";
        if (unassessedCount > 0)
            s += "；另有 " + std::to_string(unassessedCount) + " 个子域未评估";
        r.summary = s;
    }
    return r;
}

static QJsonObject scopeEntry(const char* status, const std::string& reason) {
    QJsonObject entry;
    entry["status"] = QString::fromUtf8(status);
    entry["reason"] = QString::fromStdString(reason);
    return entry;
}

static bool hasFinding(const std::vector<DiagnosticFinding>& findings,
                       const char* fault, bool assessed) {
    for (const DiagnosticFinding& finding : findings)
        if (finding.fault == fault && finding.assessed == assessed) return true;
    return false;
}

static QJsonObject diagnosticAssessmentScope(const std::vector<DiagnosticFinding>& findings,
                                             const DiagnoseConfig& config) {
    const ProfileValidation profile = validateDiagnosticProfile(config.asset, config.measurement);
    const bool profileValid = profile.valid();
    const bool bearingValid = profileValid && bearingGeometryValid(config.asset.bearing);
    QJsonObject scope;
    scope["profile"] = profileValid
        ? scopeEntry("assessed", "explicit asset and measurement profile validated")
        : scopeEntry("not_assessed", "missing/invalid fields: " + joinErrors(profile.errors));
    scope["rotor_order"] = profileValid
        ? scopeEntry("assessed", "explicit rpm and rpm source validated")
        : scopeEntry("not_assessed", "valid profile with explicit rpm is required");
    scope["bearing_frequency"] = bearingValid
        ? scopeEntry("assessed", "domain-supplied bearing geometry validated")
        : scopeEntry("not_assessed", profileValid
            ? "bearing geometry was missing or physically invalid"
            : "valid profile is required before bearing-frequency assessment");
    scope["resonance"] = scopeEntry("requires_additional_test",
        "modal test, speed sweep or approved natural-frequency evidence is required");
    if (config.measurement.standardProfile == kNoStandardProfile) {
        scope["severity"] = scopeEntry("not_requested", "standard profile is none");
    } else if (hasFinding(findings, "severity", true)) {
        scope["severity"] = scopeEntry("assessed",
            "configured legacy compatibility profile requirements were satisfied");
    } else {
        scope["severity"] = scopeEntry("not_assessed",
            "configured standard profile requirements were not satisfied");
    }
    return scope;
}

static QJsonObject diagnosticInputProvenance(const DiagnoseConfig& config,
                                             int spectralPeakCount,
                                             double maxFrequencyHz) {
    QJsonObject bearing;
    bearing["rolling_elements"] = config.asset.bearing.rollingElements;
    bearing["ball_diameter_mm"] = config.asset.bearing.ballDiaMm;
    bearing["pitch_diameter_mm"] = config.asset.bearing.pitchDiaMm;
    bearing["contact_angle_deg"] = config.asset.bearing.contactAngleDeg;
    bearing["contact_angle_known"] = config.asset.bearing.contactAngleKnown;

    QJsonObject asset;
    asset["asset_id"] = QString::fromStdString(config.asset.assetId);
    asset["machine_type"] = QString::fromStdString(config.asset.machineType);
    asset["rated_power_kw"] = config.asset.ratedPowerKw;
    asset["applicability_info"] = QString::fromStdString(config.asset.applicabilityInfo);
    asset["rpm"] = config.asset.rpm;
    asset["rpm_source"] = QString::fromStdString(config.asset.rpmSource);
    asset["operating_condition"] = QString::fromStdString(config.asset.operatingCondition);
    asset["bearing_geometry"] = bearing;

    QJsonObject measurement;
    measurement["data_id"] = QString::fromStdString(config.measurement.dataId);
    measurement["channel"] = config.measurement.channel;
    measurement["measurement_point"] = QString::fromStdString(config.measurement.measurementPoint);
    measurement["direction"] = QString::fromStdString(config.measurement.direction);
    measurement["physical_quantity"] = QString::fromStdString(config.measurement.physicalQuantity);
    measurement["unit"] = QString::fromStdString(config.measurement.unit);
    measurement["sampling_rate_hz"] = config.measurement.samplingRateHz;
    measurement["standard_profile"] = QString::fromStdString(config.measurement.standardProfile);

    QJsonObject standard;
    standard["applicability_confirmed"] = config.standardApplicabilityConfirmed;
    standard["applicability_basis"] = QString::fromStdString(config.standardApplicabilityBasis);
    standard["velocity_rms_mm_s"] = config.severityVelocityRmsMmS;
    standard["legacy_machine_class"] = config.legacyIso10816MachineClass;
    standard["velocity_rms_source"] = QString::fromStdString(config.velocityRmsSource);
    standard["velocity_rms_data_ref"] = QString::fromStdString(config.velocityRmsDataRef);
    standard["execution_scope"] = AgentJson::parseObject(diagnosticStandardScopeJson(config));
    standard["execution_evidence"] = AgentJson::parseObject(
        standardExecutionEvidenceJson(config.standardExecutionEvidence));


    QJsonObject features;
    features["kind"] = "spectral_peaks";
    features["peak_count"] = spectralPeakCount;
    features["max_frequency_hz"] = maxFrequencyHz;
    features["raw_values_included"] = false;

    QJsonObject provenance;
    provenance["asset"] = asset;
    provenance["measurement"] = measurement;
    provenance["standard"] = standard;
    provenance["features"] = features;
    return provenance;
}

ToolResult Model::buildDiagnoseResult(const std::vector<DiagnosticFinding>& findings,
                                      const DiagnoseConfig& config, int spectralPeakCount,
                                      double maxFrequencyHz) {
    ToolResult result = buildDiagnoseResult(findings);
    QJsonObject data = AgentJson::parseObject(result.dataJson);
    data["result_contract_version"] = QString::fromUtf8(kDiagnosticResultContractVersion);
    data["input_provenance"] = diagnosticInputProvenance(
        config, spectralPeakCount, maxFrequencyHz);
    data["assessment_scope"] = diagnosticAssessmentScope(findings, config);
    result.dataJson = AgentJson::dump(data);
    return result;
}

ToolResult Model::diagnoseSpectrum(const std::vector<SpectralPeak>& peaks,
                                  const DiagnoseConfig& cfg) {
    if (peaks.empty()) {
        ToolResult r;
        r.ok = false;
        r.error = "no_peaks";
        r.summary = "当前频谱没有可用于诊断的峰值特征；未作健康或故障判断。";
        return r;
    }
    const ProfileValidation profile = validateDiagnosticProfile(cfg.asset, cfg.measurement);
    for (const std::string& profileError : profile.errors) {
        if (profileError.find("measurement.unit") != std::string::npos) {
            ToolResult r;
            r.ok = false;
            r.error = "invalid_unit";
            r.summary = "测量物理量与单位不兼容；诊断已停止。";
            return r;
        }
    }
    if (!std::isfinite(cfg.freqTolRatio) || cfg.freqTolRatio < 0.001 || cfg.freqTolRatio > 0.25) {
        ToolResult r;
        r.ok = false;
        r.error = "invalid_frequency_tolerance";
        r.summary = "频率匹配容差必须在 0.001 到 0.25 之间。";
        return r;
    }
    double maxFrequency = 0;
    for (const SpectralPeak& peak : peaks) {
        if (!std::isfinite(peak.freqHz) || peak.freqHz < 0
            || !std::isfinite(peak.value) || !std::isfinite(peak.prominence)) {
            ToolResult r;
            r.ok = false;
            r.error = "invalid_spectrum_features";
            r.summary = "频谱特征包含无效数值；诊断已停止。";
            return r;
        }
        maxFrequency = std::max(maxFrequency, peak.freqHz);
    }
    const std::string rangeError = validateFrequencyRange(cfg.measurement.samplingRateHz, maxFrequency);
    if (!rangeError.empty()) {
        ToolResult r;
        r.ok = false;
        r.error = rangeError;
        r.summary = rangeError == "band_exceeds_nyquist"
            ? "频谱特征超出测量采样率的 Nyquist 上限；诊断已停止。"
            : "测量采样率或频率范围无效；诊断已停止。";
        return r;
    }
    return buildDiagnoseResult(diagnoseVibration(peaks, cfg), cfg,
                               static_cast<int>(peaks.size()), maxFrequency);
}

ToolResult Model::buildPeaksResult(const std::vector<SpectralPeak>& peaks, const std::string& label) {
    ToolResult r;
    QJsonArray arr;
    for (const auto& p : peaks) {
        QJsonObject o;
        o["freq_hz"] = p.freqHz;
        o["value"] = p.value;
        o["prominence"] = p.prominence;
        arr.append(o);
    }
    QJsonObject d;
    d["label"] = QString::fromStdString(label);
    d["peaks"] = arr;
    r.dataJson = AgentJson::dump(d);
    if (peaks.empty()) {
        r.ok = false;
        r.error = "no_peaks";
        r.summary = label + " 未提取到峰值（无原始波形/采样率，或频带参数不当）。";
    } else {
        r.summary = label + " 主峰 " + std::to_string((int)(peaks.front().freqHz + 0.5))
                  + " Hz，共 " + std::to_string(peaks.size()) + " 峰。";
    }
    return r;
}
