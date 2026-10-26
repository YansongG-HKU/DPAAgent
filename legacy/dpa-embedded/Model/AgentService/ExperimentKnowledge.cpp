#include "ExperimentKnowledge.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace Model {
namespace {

constexpr int kMaxEntryBytes = 64 * 1024;

bool integerValue(const QJsonValue& value, long long minimum, long long maximum) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (number != static_cast<double>(static_cast<long long>(number))) return false;
    const long long integer = static_cast<long long>(number);
    return integer >= minimum && integer <= maximum;
}

} // namespace

KnowledgeBuildOutput buildKnowledgeEntry(const std::string& analysisResultJson,
                                         const std::string& sourceArtifactId,
                                         long long recordedAtMs) {
    KnowledgeBuildOutput output;
    if (analysisResultJson.empty() || analysisResultJson.size() > kMaxEntryBytes) {
        output.error = "knowledge_source_size_invalid";
        return output;
    }
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(analysisResultJson));
    if (!document.isObject()) {
        output.error = "knowledge_source_json_invalid";
        return output;
    }
    const QJsonObject analysis = document.object();
    if (analysis.value("schema").toString()
        != QStringLiteral("dpa.experiment.campaign.analysis/v1")) {
        output.error = "knowledge_source_schema_invalid";
        return output;
    }
    if (!analysis.value("campaign_id").isString()
        || !analysis.value("campaign_sha256").isString()
        || !analysis.value("factors").isArray()
        || !analysis.value("cells").isArray()
        || !integerValue(analysis.value("best_cell_index"), -1, 1 << 30)) {
        output.error = "knowledge_source_fields_invalid";
        return output;
    }
    int bestIndex = static_cast<int>(analysis.value("best_cell_index").toDouble());
    const QJsonArray cells = analysis.value("cells").toArray();
    if (bestIndex < 0 || bestIndex >= cells.size()) {
        output.error = "knowledge_best_cell_invalid";
        return output;
    }

    ExperimentKnowledgeEntry entry;
    entry.recordedAtMs = recordedAtMs;
    entry.campaignId = analysis.value("campaign_id").toString().toStdString();
    entry.campaignSha256 = analysis.value("campaign_sha256").toString().toStdString();
    entry.residualStd = analysis.value("residual_std").toDouble();
    entry.residualEstimable = analysis.value("residual_estimable").toBool();

    // Keep only the decision-relevant projection of each factor.
    QJsonArray factorsProjection;
    for (const QJsonValue& value : analysis.value("factors").toArray()) {
        const QJsonObject factor = value.toObject();
        factorsProjection.append(QJsonObject{
            {"name", factor.value("name")},
            {"effect_range", factor.value("effect_range").toDouble()},
            {"perm_p", factor.value("perm_p").toDouble()},
            {"testable", factor.value("testable").toBool()}});
    }
    entry.factorsJson = QJsonDocument(factorsProjection)
        .toJson(QJsonDocument::Compact).toStdString();

    const QJsonObject bestCell = cells.at(bestIndex).toObject();
    const QJsonObject bestCellProjection{
        {"levels", bestCell.value("levels")},
        {"mean", bestCell.value("mean").toDouble()},
        {"count", bestCell.value("count").toInt()}};
    entry.bestCellJson = QJsonDocument(bestCellProjection)
        .toJson(QJsonDocument::Compact).toStdString();
    entry.sourceArtifactId = sourceArtifactId;

    const QJsonObject canonical{
        {"schema", "dpa.experiment.knowledge.entry/v1"},
        {"recorded_at_ms", static_cast<double>(entry.recordedAtMs)},
        {"campaign_id", QString::fromStdString(entry.campaignId)},
        {"campaign_sha256", QString::fromStdString(entry.campaignSha256)},
        {"residual_std", entry.residualStd},
        {"residual_estimable", entry.residualEstimable},
        {"factors", factorsProjection},
        {"best_cell", bestCellProjection},
        {"source_artifact_id", QString::fromStdString(entry.sourceArtifactId)}};
    entry.canonicalJson = QJsonDocument(canonical).toJson(QJsonDocument::Compact).toStdString();

    output.entryJson = entry.canonicalJson;
    output.ok = true;
    return output;
}

KnowledgeEntryParse parseKnowledgeEntry(const std::string& json) {
    KnowledgeEntryParse result;
    const auto fail = [&result](const char* error) {
        result.error = error;
        return result;
    };
    if (json.empty() || json.size() > kMaxEntryBytes)
        return fail("knowledge_entry_size_invalid");
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(json));
    if (!document.isObject())
        return fail("knowledge_entry_json_invalid");
    const QJsonObject object = document.object();
    const QStringList requiredKeys{"schema", "recorded_at_ms", "campaign_id",
                                   "campaign_sha256", "residual_std",
                                   "residual_estimable", "factors", "best_cell",
                                   "source_artifact_id"};
    if (object.size() != requiredKeys.size())
        return fail("knowledge_entry_schema_invalid");
    for (const QString& key : requiredKeys)
        if (!object.contains(key)) return fail("knowledge_entry_schema_invalid");
    if (object.value("schema").toString()
        != QStringLiteral("dpa.experiment.knowledge.entry/v1")
        || !integerValue(object.value("recorded_at_ms"), 0, 1LL << 62)
        || !object.value("campaign_id").isString()
        || !object.value("campaign_sha256").isString()
        || !object.value("residual_std").isDouble()
        || !object.value("residual_estimable").isBool()
        || !object.value("factors").isArray()
        || !object.value("best_cell").isObject()
        || !object.value("source_artifact_id").isString())
        return fail("knowledge_entry_schema_invalid");

    result.entry.recordedAtMs = static_cast<long long>(
        object.value("recorded_at_ms").toDouble());
    result.entry.campaignId = object.value("campaign_id").toString().toStdString();
    result.entry.campaignSha256 = object.value("campaign_sha256").toString().toStdString();
    result.entry.residualStd = object.value("residual_std").toDouble();
    result.entry.residualEstimable = object.value("residual_estimable").toBool();
    result.entry.factorsJson = QJsonDocument(object.value("factors").toArray())
        .toJson(QJsonDocument::Compact).toStdString();
    result.entry.bestCellJson = QJsonDocument(object.value("best_cell").toObject())
        .toJson(QJsonDocument::Compact).toStdString();
    result.entry.sourceArtifactId =
        object.value("source_artifact_id").toString().toStdString();
    result.entry.canonicalJson =
        QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
    result.ok = true;
    return result;
}

} // namespace Model
