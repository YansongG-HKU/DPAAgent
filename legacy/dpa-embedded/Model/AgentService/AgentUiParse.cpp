#include "AgentUiParse.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
using namespace Model;

// Uses Qt's own JSON (not AgentJson) so this unit compiles standalone into the View plugin too.
std::vector<UiFinding> Model::parseDiagnoseFindings(const std::string& toolResultJson) {
    std::vector<UiFinding> out;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(toolResultJson));
    if (!doc.isObject()) return out;
    QJsonObject root = doc.object();
    // Findings live at data.findings (ToolResult wraps dataJson under "data"); tolerate root.findings.
    QJsonArray arr = root.value("data").toObject().value("findings").toArray();
    if (arr.isEmpty()) arr = root.value("findings").toArray();
    for (const auto& v : arr) {
        QJsonObject o = v.toObject();
        if (!o.contains("fault")) continue;
        UiFinding f;
        f.fault = o.value("fault").toString().toStdString();
        f.severity = o.value("severity").toString().toStdString();
        f.evidence = o.value("evidence").toString().toStdString();
        f.ruleMatchScore = o.value("rule_match_score").toDouble(0);
        f.assessed = o.value("assessed").toBool(true);
        for (const auto& rc : o.value("root_causes").toArray())      f.rootCauses.push_back(rc.toString().toStdString());
        for (const auto& re : o.value("recommendations").toArray())  f.recommendations.push_back(re.toString().toStdString());
        out.push_back(f);
    }
    return out;
}

std::string Model::buildDiagnosticFeedbackJson(const std::string& feedbackType,
                                               const std::string& fault,
                                               const std::string& diagnosticSeverity,
                                               const std::string& ruleId,
                                               const std::vector<std::string>& evidenceRefs) {
    const QString diagnosticSeverityText = QString::fromStdString(diagnosticSeverity);
    const QString feedbackSeverity = diagnosticSeverityText == QStringLiteral("alarm")
        ? QStringLiteral("critical")
        : diagnosticSeverityText == QStringLiteral("warn")
            ? QStringLiteral("concern") : QStringLiteral("info");
    QJsonObject payload{
        {"feedbackType", QString::fromStdString(feedbackType)},
        {"severity", feedbackSeverity},
        {"summary", QStringLiteral("诊断反馈：fault=%1；diagnostic_severity=%2；rule_id=%3")
            .arg(QString::fromStdString(fault), diagnosticSeverityText,
                 QString::fromStdString(ruleId).isEmpty() ? QStringLiteral("not_provided")
                                                          : QString::fromStdString(ruleId))}
    };
    if (!ruleId.empty()) payload["correlationId"] = QString::fromStdString(ruleId).left(160);
    QJsonArray refs;
    for (const std::string& ref : evidenceRefs) {
        if (!ref.empty()) refs.append(QString::fromStdString(ref).left(160));
        if (refs.size() == 20) break;
    }
    if (!refs.isEmpty()) payload["evidenceRefs"] = refs;
    return QJsonDocument(payload).toJson(QJsonDocument::Compact).toStdString();
}

bool Model::parseCampaignRunRows(const std::string& campaignResultJson,
                                 UiCampaignSnapshot& out) {
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(campaignResultJson));
    if (!doc.isObject()) return false;
    QJsonObject root = doc.object();
    QJsonObject payload = root.value("data").toObject();
    if (payload.isEmpty()) payload = root;
    if (payload.value("schema").toString()
        != QStringLiteral("dpa.experiment.campaign.result/v1"))
        return false;
    if (!payload.value("runs").isArray()) return false;
    out.status = payload.value("status").toString().toStdString();
    out.stopReason = payload.value("stop_reason").toString().toStdString();
    out.plannedRuns = payload.value("planned_runs").toInt(0);
    out.runs.clear();
    for (const auto& value : payload.value("runs").toArray()) {
        const QJsonObject run = value.toObject();
        if (run.isEmpty()) continue;
        UiCampaignRunRow row;
        row.runIndex = run.value("run_index").toInt(-1);
        row.designIndex = run.value("design_index").toInt(-1);
        row.ok = run.value("ok").toBool(false);
        row.errorCode = run.value("error_code").toString().toStdString();
        row.runId = run.value("run_id").toString().toStdString();
        row.evidenceId = run.value("evidence_artifact_id").toString().toStdString();
        row.levelsText = QJsonDocument(run.value("levels").toObject())
            .toJson(QJsonDocument::Compact).toStdString();
        out.runs.push_back(row);
    }
    return true;
}

bool Model::parseCheckpointHint(const std::string& campaignResultJson,
                                UiCheckpointHint& out) {
    out = UiCheckpointHint{};
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(campaignResultJson));
    if (!doc.isObject()) return false;
    QJsonObject root = doc.object();
    QJsonObject payload = root.value("data").toObject();
    if (payload.isEmpty()) payload = root;
    if (payload.value("schema").toString()
        != QStringLiteral("dpa.experiment.campaign.result/v1"))
        return false;
    out.awaiting = payload.value("awaiting_interim").toBool(false);
    out.checkpointArtifactId =
        payload.value("checkpoint_artifact_id").toString().toStdString();
    out.stopReason = payload.value("stop_reason").toString().toStdString();
    out.campaignSha256 = payload.value("campaign_sha256").toString().toStdString();
    out.plannedRuns = payload.value("planned_runs").toInt(0);
    for (const auto& value : payload.value("runs").toArray())
        if (value.toObject().value("ok").toBool(false)) ++out.completedRuns;
    return true;
}

bool Model::parseRefinementProposal(const std::string& toolResultJson,
                                    UiRefinementProposal& out) {
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(toolResultJson));
    if (!doc.isObject()) return false;
    QJsonObject root = doc.object();
    QJsonObject payload = root.value("data").toObject();
    if (payload.isEmpty()) payload = root;
    if (payload.value("schema").toString()
        != QStringLiteral("dpa.experiment.campaign.refinement/v1"))
        return false;
    out.sourceCampaignId =
        payload.value("source_campaign_id").toString().toStdString();
    out.proposedCampaignId =
        payload.value("proposed_campaign_id").toString().toStdString();
    out.proposedSha256 =
        payload.value("proposed_contract_sha256").toString().toStdString();
    out.refinementArtifactId =
        payload.value("refinement_artifact_id").toString().toStdString();
    out.plannedRuns = payload.value("planned_runs").toInt(0);
    out.replicates = payload.value("replicates").toInt(0);
    const QJsonObject power = payload.value("power").toObject();
    out.hasPower = !power.isEmpty();
    if (out.hasPower) {
        out.power = power.value("estimated_power").toDouble(-1.0);
        out.powerCalibrated = power.value("calibrated").toBool(false);
    }
    out.refinedFactors.clear();
    out.fixedFactors.clear();
    for (const auto& value : payload.value("factor_assessment").toArray()) {
        const QJsonObject factor = value.toObject();
        const QString action = factor.value("action").toString();
        const QString name = factor.value("name").toString();
        if (action == QStringLiteral("refined") || action == QStringLiteral("promoted"))
            out.refinedFactors.push_back(name.toStdString());
        else if (action == QStringLiteral("fixed_at_best"))
            out.fixedFactors.push_back(name.toStdString());
    }
    return true;
}

bool Model::parseKnowledgeRows(const std::string& listToolResultJson,
                               std::vector<UiKnowledgeRow>& rows) {
    rows.clear();
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(listToolResultJson));
    if (!doc.isObject()) return false;
    QJsonObject root = doc.object();
    QJsonObject payload = root.value("data").toObject();
    if (payload.isEmpty()) payload = root;
    if (payload.value("schema").toString()
        != QStringLiteral("dpa.experiment.knowledge.list/v1"))
        return false;
    if (!payload.value("entries").isArray()) return false;
    // Entries arrive newest-last in the file; the list tool already reverses
    // them, so render order equals array order.
    for (const auto& value : payload.value("entries").toArray()) {
        const QJsonObject entry = value.toObject();
        UiKnowledgeRow row;
        row.recordedAtMs = static_cast<long long>(
            entry.value("recorded_at_ms").toDouble());
        row.campaignId = entry.value("campaign_id").toString().toStdString();
        row.residualStd = entry.value("residual_std").toDouble();
        row.residualEstimable = entry.value("residual_estimable").toBool(false);
        double bestRange = -1.0;
        for (const auto& factorValue : entry.value("factors").toArray()) {
            const QJsonObject factor = factorValue.toObject();
            const bool testable = factor.value("testable").toBool(false);
            const double range = factor.value("effect_range").toDouble(0);
            if (testable && range > bestRange) {
                bestRange = range;
                row.dominantFactor = factor.value("name").toString().toStdString();
                row.effectRange = range;
                row.permP = factor.value("perm_p").toDouble(-1.0);
                row.testable = true;
            }
        }
        row.bestLevels = QJsonDocument(entry.value("best_cell").toObject()
            .value("levels").toObject()).toJson(QJsonDocument::Compact).toStdString();
        rows.push_back(row);
    }
    return true;
}
