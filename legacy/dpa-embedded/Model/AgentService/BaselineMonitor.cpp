#include "BaselineMonitor.h"
#include "AgentJson.h"
#include "Diagnostics.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <utility>
using namespace Model;

BaselineArgs Model::parseBaselineArgs(const std::string& argsJson) {
    BaselineArgs a;
    QJsonObject o = AgentJson::parseObject(argsJson);
    a.assetId = o.value("assetId").toString().trimmed().toStdString();
    a.measurementPoint = o.value("measurementPoint").toString().trimmed().toStdString();
    a.direction = o.value("direction").toString().trimmed().toStdString();
    a.operatingCondition = o.value("operatingCondition").toString().trimmed().toStdString();
    a.standardProfile = o.value("standardProfile").toString("none").toStdString();
    a.dataId = o.value("dataId").toString().trimmed().toStdString();
    a.channel = o.contains("channel") ? o.value("channel").toInt(-1) : -1;
    a.physicalQuantity = o.value("physicalQuantity").toString().trimmed().toStdString();
    a.unit = o.value("unit").toString().toStdString();
    a.samplingRateHz = o.value("samplingRateHz").toDouble(0);
    const QJsonObject standard = o.value("standard").toObject();
    a.standardApplicabilityConfirmed = standard.value("applicabilityConfirmed").toBool(false);
    a.standardApplicabilityBasis = standard.value("applicabilityBasis").toString().trimmed().toStdString();
    a.severityVelocityRmsMmS = standard.value("velocityRmsMmS").toDouble(0);
    a.severitySource = standard.value("velocityRmsSource").toString().trimmed().toStdString();
    a.severityDataRef = standard.value("velocityRmsDataRef").toString().trimmed().toStdString();
    a.legacyIso10816MachineClass = standard.value("legacyMachineClass").toInt(0);
    if (o.contains("topN")) a.topN = o.value("topN").toInt(8);
    return a;
}

std::string Model::baselineSeverityZone(const BaselineArgs& a) {
    if (a.standardProfile != kLegacyIso10816ClassProfile
        || !a.standardApplicabilityConfirmed
        || a.standardApplicabilityBasis.empty()
        || !standardExecutionEvidenceMatches(a.standardExecutionEvidence, a.standardProfile,
               baselineStandardScopeJson(a), kDiagnosticRuleVersion)
        || a.severityVelocityRmsMmS <= 0
        || a.severitySource.empty() || a.severityDataRef.empty()
        || a.legacyIso10816MachineClass < 1 || a.legacyIso10816MachineClass > 4)
        return {};
    QString unit = QString::fromStdString(a.unit).toLower();
    unit.remove(' ');
    if (unit != "mm/s" && unit != "mm/srms") return {};
    return legacyIso10816ClassZone(a.severityVelocityRmsMmS,
                                   a.legacyIso10816MachineClass);
}

std::string Model::baselineStandardScopeJson(const BaselineArgs& a) {
    return AgentJson::dump(QJsonObject{
        {"operation", "baseline_legacy_severity"},
        {"asset_id", QString::fromStdString(a.assetId)},
        {"measurement_point", QString::fromStdString(a.measurementPoint)},
        {"direction", QString::fromStdString(a.direction)},
        {"operating_condition", QString::fromStdString(a.operatingCondition)},
        {"physical_quantity", QString::fromStdString(a.physicalQuantity)},
        {"unit", QString::fromStdString(a.unit)}, {"sampling_rate_hz", a.samplingRateHz},
        {"legacy_machine_class", a.legacyIso10816MachineClass},
        {"applicability_basis", QString::fromStdString(a.standardApplicabilityBasis)}
    });
}

std::string Model::baselineConfigFingerprint(const BaselineProvenance& p) {
    QJsonObject o;
    o["feature_definition_id"] = QString::fromUtf8(kBaselineFeatureDefinitionId);
    o["source_unit"] = QString::fromStdString(p.sourceUnit).trimmed().toLower();
    o["source_physical_type"] = QString::fromStdString(p.sourcePhysicalType).trimmed().toLower();
    o["sampling_rate_hz"] = p.samplingRateHz;
    o["process_type"] = QString::fromStdString(p.processType);
    o["result_function_type"] = QString::fromStdString(p.resultFunctionType);
    o["result_data_form"] = p.resultDataForm;
    o["axis_type_x"] = p.axisTypeX;
    o["frame_size"] = p.frameSize;
    o["average_num"] = p.averageNum;
    o["overlap_index"] = p.overlapIndex;
    o["overlap_value"] = p.overlapValue;
    o["window_type"] = p.windowType;
    o["result_unit"] = QString::fromStdString(p.resultUnit).trimmed().toLower();
    return QCryptographicHash::hash(QByteArray::fromStdString(AgentJson::dump(o)),
                                    QCryptographicHash::Sha256).toHex().toStdString();
}

std::string Model::baselineKey(const BaselineArgs& a, const std::string& configFingerprint) {
    if (a.assetId.empty() || a.measurementPoint.empty() || a.direction.empty()
        || a.operatingCondition.empty() || a.dataId.empty() || a.channel < 0
        || a.physicalQuantity.empty() || a.unit.empty() || !(a.samplingRateHz > 0)
        || configFingerprint.empty())
        return {};
    std::string normalizedUnit;
    for (unsigned char c : a.unit)
        if (!std::isspace(c)) normalizedUnit.push_back(static_cast<char>(std::tolower(c)));
    return a.assetId + "\x1f" + a.measurementPoint + "\x1f" + a.direction + "\x1f"
        + a.operatingCondition + "\x1f" + a.physicalQuantity + "\x1f" + normalizedUnit + "\x1f"
        + kBaselineFeatureDefinitionId + "\x1f" + configFingerprint + "\x1f"
        + a.standardProfile + "\x1f" + std::to_string(a.legacyIso10816MachineClass);
}

static QJsonObject provenanceToJson(const BaselineProvenance& p) {
    return QJsonObject{
        {"source_data_id", QString::fromStdString(p.sourceDataId)},
        {"source_channel", p.sourceChannel},
        {"source_unit", QString::fromStdString(p.sourceUnit)},
        {"source_physical_type", QString::fromStdString(p.sourcePhysicalType)},
        {"sampling_rate_hz", p.samplingRateHz},
        {"process_type", QString::fromStdString(p.processType)},
        {"result_function_type", QString::fromStdString(p.resultFunctionType)},
        {"result_data_form", p.resultDataForm},
        {"axis_type_x", p.axisTypeX},
        {"frame_size", p.frameSize},
        {"average_num", p.averageNum},
        {"overlap_index", p.overlapIndex},
        {"overlap_value", p.overlapValue},
        {"window_type", p.windowType},
        {"result_unit", QString::fromStdString(p.resultUnit)},
        {"config_fingerprint", QString::fromStdString(p.configFingerprint)}
    };
}

static BaselineProvenance provenanceFromJson(const QJsonObject& o) {
    BaselineProvenance p;
    p.sourceDataId = o.value("source_data_id").toString().toStdString();
    p.sourceChannel = o.value("source_channel").toInt(-1);
    p.sourceUnit = o.value("source_unit").toString().toStdString();
    p.sourcePhysicalType = o.value("source_physical_type").toString().toStdString();
    p.samplingRateHz = o.value("sampling_rate_hz").toDouble();
    p.processType = o.value("process_type").toString().toStdString();
    p.resultFunctionType = o.value("result_function_type").toString().toStdString();
    p.resultDataForm = o.value("result_data_form").toInt();
    p.axisTypeX = o.value("axis_type_x").toInt();
    p.frameSize = o.value("frame_size").toInt();
    p.averageNum = o.value("average_num").toInt();
    p.overlapIndex = o.value("overlap_index").toInt(-1);
    p.overlapValue = o.value("overlap_value").toDouble();
    p.windowType = o.value("window_type").toInt(-1);
    p.resultUnit = o.value("result_unit").toString().toStdString();
    p.configFingerprint = o.value("config_fingerprint").toString().toStdString();
    return p;
}

std::string Model::serializeBaseline(const MachineBaseline& b) {
    QJsonObject o;
    o["schema_version"] = 2;
    o["asset_id"] = QString::fromStdString(b.assetId);
    o["measurement_point"] = QString::fromStdString(b.measurementPoint);
    o["direction"] = QString::fromStdString(b.direction);
    o["operating_condition"] = QString::fromStdString(b.operatingCondition);
    o["physical_quantity"] = QString::fromStdString(b.physicalQuantity);
    o["unit"] = QString::fromStdString(b.unit);
    o["feature_definition_id"] = QString::fromStdString(b.featureDefinitionId);
    o["config_fingerprint"] = QString::fromStdString(b.configFingerprint);
    o["standard_profile"] = QString::fromStdString(b.standardProfile);
    o["legacy_machine_class"] = b.legacyMachineClass;
    QJsonArray peaks;
    for (double value : b.peakFreqsHz) peaks.append(value);
    o["peak_freqs_hz"] = peaks;
    o["iso_zone"] = QString::fromStdString(b.isoZone);
    o["standard_execution_evidence"] = AgentJson::parseObject(b.standardEvidenceJson);
    o["captured_ms"] = double(b.capturedMs);
    o["valid"] = b.valid;
    o["provenance"] = provenanceToJson(b.provenance);
    QJsonArray history;
    for (const BaselineObservation& observation : b.history) {
        QJsonObject item;
        item["captured_ms"] = double(observation.capturedMs);
        item["iso_zone"] = QString::fromStdString(observation.isoZone);
        QJsonArray itemPeaks;
        for (double value : observation.peakFreqsHz) itemPeaks.append(value);
        item["peak_freqs_hz"] = itemPeaks;
        item["provenance"] = provenanceToJson(observation.provenance);
        item["standard_profile"] = QString::fromStdString(observation.standardProfile);
        item["legacy_machine_class"] = observation.legacyMachineClass;
        item["standard_applicability_basis"] = QString::fromStdString(observation.standardApplicabilityBasis);
        item["severity_source"] = QString::fromStdString(observation.severitySource);
        item["severity_data_ref"] = QString::fromStdString(observation.severityDataRef);
        item["standard_execution_evidence"] = AgentJson::parseObject(observation.standardEvidenceJson);
        history.append(item);
    }
    o["history"] = history;
    return AgentJson::dump(o);
}

MachineBaseline Model::parseBaseline(const std::string& json) {
    MachineBaseline b;
    QJsonObject o = AgentJson::parseObject(json);
    if (o.value("schema_version").toInt() != 2) return b;
    b.assetId = o.value("asset_id").toString().toStdString();
    b.measurementPoint = o.value("measurement_point").toString().toStdString();
    b.direction = o.value("direction").toString().toStdString();
    b.operatingCondition = o.value("operating_condition").toString().toStdString();
    b.physicalQuantity = o.value("physical_quantity").toString().toStdString();
    b.unit = o.value("unit").toString().toStdString();
    b.featureDefinitionId = o.value("feature_definition_id").toString().toStdString();
    b.configFingerprint = o.value("config_fingerprint").toString().toStdString();
    b.standardProfile = o.value("standard_profile").toString().toStdString();
    b.legacyMachineClass = o.value("legacy_machine_class").toInt();
    for (const QJsonValue& value : o.value("peak_freqs_hz").toArray()) b.peakFreqsHz.push_back(value.toDouble());
    b.isoZone = o.value("iso_zone").toString().toStdString();
    b.standardEvidenceJson = AgentJson::dump(o.value("standard_execution_evidence").toObject());
    b.capturedMs = static_cast<long long>(o.value("captured_ms").toDouble());
    b.provenance = provenanceFromJson(o.value("provenance").toObject());
    b.valid = o.value("valid").toBool(false) && !b.assetId.empty()
        && !b.measurementPoint.empty() && !b.direction.empty() && !b.operatingCondition.empty()
        && !b.physicalQuantity.empty() && !b.unit.empty()
        && b.featureDefinitionId == kBaselineFeatureDefinitionId
        && !b.configFingerprint.empty()
        && b.configFingerprint == b.provenance.configFingerprint
        && b.configFingerprint == baselineConfigFingerprint(b.provenance);
    for (const QJsonValue& value : o.value("history").toArray()) {
        const QJsonObject item = value.toObject();
        BaselineObservation observation;
        observation.capturedMs = static_cast<long long>(item.value("captured_ms").toDouble());
        observation.isoZone = item.value("iso_zone").toString().toStdString();
        for (const QJsonValue& peak : item.value("peak_freqs_hz").toArray())
            observation.peakFreqsHz.push_back(peak.toDouble());
        observation.provenance = provenanceFromJson(item.value("provenance").toObject());
        observation.standardProfile = item.value("standard_profile").toString().toStdString();
        observation.legacyMachineClass = item.value("legacy_machine_class").toInt();
        observation.standardApplicabilityBasis = item.value("standard_applicability_basis").toString().toStdString();
        observation.severitySource = item.value("severity_source").toString().toStdString();
        observation.severityDataRef = item.value("severity_data_ref").toString().toStdString();
        observation.standardEvidenceJson = AgentJson::dump(item.value("standard_execution_evidence").toObject());
        if (observation.provenance.configFingerprint != b.configFingerprint
            || observation.standardProfile != b.standardProfile
            || observation.legacyMachineClass != b.legacyMachineClass) {
            b.valid = false;
            b.history.clear();
            break;
        }
        b.history.push_back(std::move(observation));
    }
    return b;
}

ToolResult Model::buildDeviationResult(const DeviationReport& d) {
    ToolResult r;
    r.ok = true;
    QJsonObject o;
    o["has_baseline"] = d.hasBaseline;
    o["iso_worsened"] = d.isoWorsened;
    o["severity"] = QString::fromStdString(d.severity);
    QJsonArray np; for (double f : d.newPeaksHz) np.append(f);
    o["new_peaks_hz"] = np;
    r.dataJson = AgentJson::dump(o);
    r.summary = d.summary;
    return r;
}

static bool recordedStandardEvidence(const std::string& json) {
    const QJsonObject receipt = AgentJson::parseObject(json);
    return receipt.value("ok").toBool(false)
        && receipt.value("standard_profile").toString() == kLegacyIso10816ClassProfile
        && receipt.value("source_sha256").toString().size() == 64
        && receipt.value("approval_sha256").toString().size() == 64;
}

ToolResult Model::buildBaselineTrendResult(const MachineBaseline& baseline) {
    ToolResult r;
    if (!baseline.valid) {
        r.ok = false;
        r.error = "baseline_not_found";
        r.summary = "未找到该资产/测点/方向/工况的基线。";
        return r;
    }
    std::vector<BaselineObservation> observations = baseline.history;
    // Baselines written before history was introduced still represent one grounded point.
    if (observations.empty()) {
        BaselineObservation observation;
        observation.capturedMs = baseline.capturedMs;
        observation.isoZone = baseline.isoZone;
        observation.standardEvidenceJson = baseline.standardEvidenceJson;
        observation.peakFreqsHz = baseline.peakFreqsHz;
        observation.provenance = baseline.provenance;
        observation.standardProfile = baseline.standardProfile;
        observation.legacyMachineClass = baseline.legacyMachineClass;
        observations.push_back(std::move(observation));
    }
    QJsonArray points;
    for (const BaselineObservation& observation : observations) {
        QJsonObject point;
        point["captured_ms"] = double(observation.capturedMs);
        point["iso_zone"] = QString::fromStdString(observation.isoZone);
        point["source_data_id"] = QString::fromStdString(observation.provenance.sourceDataId);
        point["source_channel"] = observation.provenance.sourceChannel;
        point["config_fingerprint"] = QString::fromStdString(observation.provenance.configFingerprint);
        point["standard_profile"] = QString::fromStdString(observation.standardProfile);
        point["legacy_machine_class"] = observation.legacyMachineClass;
        point["standard_applicability_basis"] = QString::fromStdString(observation.standardApplicabilityBasis);
        point["severity_source"] = QString::fromStdString(observation.severitySource);
        point["severity_data_ref"] = QString::fromStdString(observation.severityDataRef);
        point["standard_execution_evidence"] = AgentJson::parseObject(observation.standardEvidenceJson);
        point["legacy_severity_assessed"] = recordedStandardEvidence(observation.standardEvidenceJson);
        points.append(point);
    }
    QJsonObject data;
    data["asset_id"] = QString::fromStdString(baseline.assetId);
    data["measurement_point"] = QString::fromStdString(baseline.measurementPoint);
    data["direction"] = QString::fromStdString(baseline.direction);
    data["operating_condition"] = QString::fromStdString(baseline.operatingCondition);
    data["physical_quantity"] = QString::fromStdString(baseline.physicalQuantity);
    data["unit"] = QString::fromStdString(baseline.unit);
    data["feature_definition_id"] = QString::fromStdString(baseline.featureDefinitionId);
    data["config_fingerprint"] = QString::fromStdString(baseline.configFingerprint);
    data["standard_profile"] = QString::fromStdString(baseline.standardProfile);
    data["observations"] = points;
    QJsonObject trend{
        {"method", "first_confirmed_to_latest_confirmed/1.0"},
        {"evaluated", false},
        {"evaluation_status", "insufficient_history"},
        {"peak_tolerance_ratio", 0.05},
        {"rms_evaluated", false}
    };
    if (observations.size() >= 2) {
        bool chronological = true;
        for (size_t i = 1; i < observations.size(); ++i) {
            if (observations[i].capturedMs < observations[i - 1].capturedMs) {
                chronological = false;
                break;
            }
        }
        trend["reference_captured_ms"] = double(observations.front().capturedMs);
        trend["latest_captured_ms"] = double(observations.back().capturedMs);
        if (chronological) {
            MachineBaseline reference = baseline;
            reference.peakFreqsHz = observations.front().peakFreqsHz;
            reference.isoZone = observations.front().isoZone;
            reference.standardEvidenceJson = observations.front().standardEvidenceJson;
            const auto& latest = observations.back();
            const std::string latestZone = latest.standardProfile == kLegacyIso10816ClassProfile
                && !recordedStandardEvidence(latest.standardEvidenceJson) ? std::string() : latest.isoZone;
            const DeviationReport deviation = compareToBaseline(
                reference, latest.peakFreqsHz, latestZone, 0.05);
            QJsonArray newPeaks;
            for (double frequency : deviation.newPeaksHz) newPeaks.append(frequency);
            trend["evaluated"] = true;
            trend["evaluation_status"] = "evaluated";
            trend["severity"] = QString::fromStdString(deviation.severity);
            trend["iso_worsened"] = deviation.isoWorsened;
            trend["new_peaks_hz"] = newPeaks;
            trend["summary"] = QString::fromStdString(deviation.summary);
        } else {
            trend["evaluation_status"] = "invalid_chronology";
        }
    }
    data["trend"] = trend;
    r.dataJson = AgentJson::dump(data);
    if (trend.value("evaluated").toBool()) {
        r.summary = "已基于 " + std::to_string(observations.size())
            + " 次已确认基线评估首末趋势。";
    } else if (trend.value("evaluation_status").toString() == "invalid_chronology") {
        r.summary = "基线历史时间顺序无效，未生成趋势结论。";
    } else {
        r.summary = "已读取 " + std::to_string(observations.size())
            + " 次已确认基线；至少需要 2 次才能评估趋势。";
    }
    return r;
}

int Model::isoZoneDelta(const std::string& base, const std::string& cur) {
    auto rank = [](const std::string& z) -> int {
        if (z == "A") return 0; if (z == "B") return 1;
        if (z == "C") return 2; if (z == "D") return 3;
        return -1;   // unknown / not provided
    };
    const int b = rank(base), c = rank(cur);
    if (b < 0 || c < 0) return 0;
    return c - b;    // >0 = worse
}

DeviationReport Model::compareToBaseline(const MachineBaseline& base,
                                         const std::vector<double>& currentPeaksHz,
                                         const std::string& currentIsoZone, double peakTolRatio) {
    DeviationReport d;
    if (!base.valid) {
        d.hasBaseline = false;
        d.severity = "info";
        d.summary = "无该机器基线；可将当前正常状态存为基线以便日后对比。";
        return d;
    }
    d.hasBaseline = true;

    // New peaks: current peaks with no baseline peak within tolerance.
    for (double f : currentPeaksHz) {
        bool matched = false;
        for (double bf : base.peakFreqsHz)
            if (bf > 0 && std::fabs(f - bf) <= peakTolRatio * bf) { matched = true; break; }
        if (!matched) d.newPeaksHz.push_back(f);
    }

    // Old stored labels remain visible as history but cannot prove a standard
    // comparison when no authorized-evidence receipt was captured with them.
    const std::string baseZone = base.standardProfile == kLegacyIso10816ClassProfile
        && !recordedStandardEvidence(base.standardEvidenceJson) ? std::string() : base.isoZone;
    const int zoneDelta = isoZoneDelta(baseZone, currentIsoZone);
    d.isoWorsened = zoneDelta > 0;

    std::string sev = "info";
    if (!d.newPeaksHz.empty() || zoneDelta >= 1) sev = "warn";
    if (currentIsoZone == "D" || zoneDelta >= 2) sev = "alarm";
    d.severity = sev;

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "对比基线：新增峰 %d 个，legacy 严重度 %s→%s%s；未进行跨频谱幅值 RMS 比较。",
        (int)d.newPeaksHz.size(),
        baseZone.empty() ? "-" : baseZone.c_str(),
        currentIsoZone.empty() ? "-" : currentIsoZone.c_str(),
        d.isoWorsened ? "(恶化)" : "");
    d.summary = buf;
    return d;
}
