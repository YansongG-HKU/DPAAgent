#include "MeasurementContract.h"

#include "AgentJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>

namespace Model {
namespace {

constexpr double kMaxJsonInteger = 9007199254740991.0;
constexpr int kMaxJsonBytes = 128 * 1024;

bool exactKeys(const QJsonObject& object, std::initializer_list<const char*> keys) {
    if (object.size() != static_cast<int>(keys.size())) return false;
    for (const char* key : keys)
        if (!object.contains(QString::fromUtf8(key))) return false;
    return true;
}

bool boundedText(const QJsonValue& value, int maximum, bool required = true) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.size() > maximum || (required && text.isEmpty())) return false;
    return true;
}

bool token(const QJsonValue& value, int maximum) {
    if (!boundedText(value, maximum)) return false;
    for (const QChar c : value.toString()) {
        const ushort code = c.unicode();
        if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
            || (code >= '0' && code <= '9') || code == '-' || code == '_' || code == '.')
            continue;
        return false;
    }
    return true;
}

bool hash256(const QJsonValue& value) {
    if (!value.isString() || value.toString().size() != 64) return false;
    for (const QChar c : value.toString()) {
        const ushort code = c.unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f')))
            return false;
    }
    return true;
}

bool integer(const QJsonValue& value, double minimum, double maximum) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number
        && number >= minimum && number <= maximum && number <= kMaxJsonInteger;
}

bool number(const QJsonValue& value, double minimum, double maximum) {
    if (!value.isDouble()) return false;
    const double valueNumber = value.toDouble();
    return std::isfinite(valueNumber) && valueNumber >= minimum && valueNumber <= maximum;
}

std::optional<QJsonObject> parseObject(const std::string& json) {
    if (json.empty() || json.size() > kMaxJsonBytes) return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(json), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return std::nullopt;
    return document.object();
}

ContractValidation fail(const char* error) { return {false, error}; }
ContractValidation ok() { return {true, {}}; }

QJsonObject measurementObject(const CanonicalMeasurement& m) {
    return QJsonObject{
        {"schema", "dpa.canonical.measurement"}, {"version", m.version},
        {"measurement_id", QString::fromStdString(m.measurementId)},
        {"source_data_id", QString::fromStdString(m.sourceDataId)},
        {"asset_id", QString::fromStdString(m.assetId)},
        {"test_id", QString::fromStdString(m.testId)}, {"channel_id", m.channelId},
        {"measurement_point", QString::fromStdString(m.measurementPoint)},
        {"direction", QString::fromStdString(m.direction)},
        {"physical_quantity", QString::fromStdString(m.physicalQuantity)},
        {"unit", QString::fromStdString(m.unit)}, {"sampling_rate_hz", m.samplingRateHz},
        {"start_time_ms", static_cast<double>(m.startTimeMs)},
        {"end_time_ms", static_cast<double>(m.endTimeMs)},
        {"sample_count", static_cast<double>(m.sampleCount)},
        {"operating_condition", QString::fromStdString(m.operatingCondition)},
        {"context_fingerprint", QString::fromStdString(m.contextFingerprint)},
        {"source_sha256", QString::fromStdString(m.sourceSha256)},
        {"calibration_profile", QString::fromStdString(m.calibrationProfile)}
    };
}

QJsonObject qualityObject(const QualityReport& q) {
    QJsonArray codes;
    for (const auto& codeValue : q.codes) codes.append(QString::fromStdString(codeValue));
    return QJsonObject{
        {"schema", "dpa.quality.report"}, {"version", q.version},
        {"status", QString::fromStdString(q.status)},
        {"finite_samples", static_cast<double>(q.finiteSamples)},
        {"missing_samples", static_cast<double>(q.missingSamples)},
        {"clipped_samples", static_cast<double>(q.clippedSamples)},
        {"monotonic_time", q.monotonicTime}, {"nyquist_valid", q.nyquistValid},
        {"calibration_valid", q.calibrationValid}, {"codes", codes}
    };
}

QJsonObject evidenceObject(const EvidenceRef& e) {
    QJsonArray refs;
    for (const auto& ref : e.artifactRefs) refs.append(QString::fromStdString(ref));
    return QJsonObject{
        {"schema", "dpa.evidence.ref"}, {"version", e.version},
        {"evidence_id", QString::fromStdString(e.evidenceId)},
        {"kind", QString::fromStdString(e.kind)},
        {"measurement_id", QString::fromStdString(e.measurementId)},
        {"source_sha256", QString::fromStdString(e.sourceSha256)},
        {"algorithm_version", QString::fromStdString(e.algorithmVersion)},
        {"rule_version", QString::fromStdString(e.ruleVersion)},
        {"created_at_ms", static_cast<double>(e.createdAtMs)},
        {"summary_sha256", QString::fromStdString(e.summarySha256)},
        {"artifact_refs", refs}
    };
}

} // namespace

std::string canonicalMeasurementJson(const CanonicalMeasurement& measurement) {
    return AgentJson::dump(measurementObject(measurement));
}

std::string qualityReportJson(const QualityReport& quality) {
    return AgentJson::dump(qualityObject(quality));
}

std::string evidenceRefJson(const EvidenceRef& evidence) {
    return AgentJson::dump(evidenceObject(evidence));
}

ContractValidation validateCanonicalMeasurementJson(const std::string& json) {
    const auto parsed = parseObject(json);
    if (!parsed) return fail("measurement_json_invalid");
    const QJsonObject& o = *parsed;
    if (!exactKeys(o, {"schema", "version", "measurement_id", "source_data_id", "asset_id",
                       "test_id", "channel_id", "measurement_point", "direction",
                       "physical_quantity", "unit", "sampling_rate_hz", "start_time_ms",
                       "end_time_ms", "sample_count", "operating_condition",
                       "context_fingerprint", "source_sha256", "calibration_profile"}))
        return fail("measurement_schema_invalid");
    if (o.value("schema").toString() != "dpa.canonical.measurement"
        || !integer(o.value("version"), 1, 1) || !token(o.value("measurement_id"), 128)
        || !token(o.value("source_data_id"), 256) || !token(o.value("asset_id"), 128)
        || !token(o.value("test_id"), 128) || !integer(o.value("channel_id"), 0, 1000000)
        || !boundedText(o.value("measurement_point"), 128)
        || !boundedText(o.value("direction"), 64)
        || !boundedText(o.value("physical_quantity"), 64)
        || !boundedText(o.value("unit"), 32)
        || !number(o.value("sampling_rate_hz"), 0.000001, 1000000000)
        || !integer(o.value("start_time_ms"), 0, kMaxJsonInteger)
        || !integer(o.value("end_time_ms"), 0, kMaxJsonInteger)
        || !integer(o.value("sample_count"), 1, kMaxJsonInteger)
        || !boundedText(o.value("operating_condition"), 512)
        || !hash256(o.value("context_fingerprint")) || !hash256(o.value("source_sha256"))
        || !boundedText(o.value("calibration_profile"), 128, false))
        return fail("measurement_value_invalid");
    if (o.value("end_time_ms").toDouble() < o.value("start_time_ms").toDouble())
        return fail("measurement_time_range_invalid");
    return ok();
}

ContractValidation validateQualityReportJson(const std::string& json, long long sampleCount) {
    const auto parsed = parseObject(json);
    if (!parsed) return fail("quality_json_invalid");
    const QJsonObject& o = *parsed;
    if (!exactKeys(o, {"schema", "version", "status", "finite_samples", "missing_samples",
                       "clipped_samples", "monotonic_time", "nyquist_valid",
                       "calibration_valid", "codes"})) return fail("quality_schema_invalid");
    const QStringList statuses = {"pass", "warn", "fail", "unknown"};
    if (o.value("schema").toString() != "dpa.quality.report"
        || !integer(o.value("version"), 1, 1) || !o.value("status").isString()
        || !statuses.contains(o.value("status").toString())
        || !integer(o.value("finite_samples"), 0, kMaxJsonInteger)
        || !integer(o.value("missing_samples"), 0, kMaxJsonInteger)
        || !integer(o.value("clipped_samples"), 0, kMaxJsonInteger)
        || !o.value("monotonic_time").isBool() || !o.value("nyquist_valid").isBool()
        || !o.value("calibration_valid").isBool() || !o.value("codes").isArray())
        return fail("quality_value_invalid");
    const long long finite = static_cast<long long>(o.value("finite_samples").toDouble());
    const long long missing = static_cast<long long>(o.value("missing_samples").toDouble());
    const long long clipped = static_cast<long long>(o.value("clipped_samples").toDouble());
    if (sampleCount >= 0 && finite + missing > sampleCount) return fail("quality_sample_count_invalid");
    if (o.value("codes").toArray().size() > 64) return fail("quality_codes_too_many");
    for (const auto& codeValue : o.value("codes").toArray())
        if (!token(codeValue, 64)) return fail("quality_code_invalid");
    (void)clipped;
    return ok();
}

ContractValidation validateEvidenceRefJson(const std::string& json,
                                           const std::string& expectedMeasurementId) {
    const auto parsed = parseObject(json);
    if (!parsed) return fail("evidence_ref_json_invalid");
    const QJsonObject& o = *parsed;
    if (!exactKeys(o, {"schema", "version", "evidence_id", "kind", "measurement_id",
                       "source_sha256", "algorithm_version", "rule_version", "created_at_ms",
                       "summary_sha256", "artifact_refs"})) return fail("evidence_ref_schema_invalid");
    if (o.value("schema").toString() != "dpa.evidence.ref"
        || !integer(o.value("version"), 1, 1) || !token(o.value("evidence_id"), 128)
        || !token(o.value("kind"), 64) || !token(o.value("measurement_id"), 128)
        || !hash256(o.value("source_sha256")) || !token(o.value("algorithm_version"), 128)
        || !token(o.value("rule_version"), 128) || !integer(o.value("created_at_ms"), 0, kMaxJsonInteger)
        || !hash256(o.value("summary_sha256")) || !o.value("artifact_refs").isArray())
        return fail("evidence_ref_value_invalid");
    if (!expectedMeasurementId.empty()
        && o.value("measurement_id").toString().toStdString() != expectedMeasurementId)
        return fail("evidence_ref_measurement_mismatch");
    if (o.value("artifact_refs").toArray().size() > 64) return fail("evidence_ref_too_many_artifacts");
    for (const auto& ref : o.value("artifact_refs").toArray())
        if (!token(ref, 256)) return fail("evidence_ref_artifact_invalid");
    return ok();
}

ContractValidation validateMeasurementBundleJson(const std::string& measurementJson,
                                                 const std::string& qualityJson,
                                                 const std::string& evidenceJson) {
    const ContractValidation measurement = validateCanonicalMeasurementJson(measurementJson);
    if (!measurement.ok) return measurement;
    const auto parsed = parseObject(measurementJson);
    const long long sampleCount = static_cast<long long>(parsed->value("sample_count").toDouble());
    const ContractValidation quality = validateQualityReportJson(qualityJson, sampleCount);
    if (!quality.ok) return quality;
    const std::string measurementId = parsed->value("measurement_id").toString().toStdString();
    return validateEvidenceRefJson(evidenceJson, measurementId);
}

} // namespace Model
