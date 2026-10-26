#include "StatsSidecar.h"

#include "AgentJson.h"
#include "ExperimentCampaign.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryFile>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace Model {

namespace {

constexpr int kMaxRequestBytes = 256 * 1024;
constexpr int kMaxOutputBytes = 256 * 1024;
constexpr int kStartTimeoutMs = 5000;
// Contract: one process per call, fail-closed at 10 seconds.
constexpr int kFinishTimeoutMs = 10000;

QString statsSidecarScriptPath() {
    const QString override = qEnvironmentVariable("DPA_STATS_SIDECAR_SCRIPT");
    if (!override.isEmpty()) return override;
    QDir repoRoot(QCoreApplication::applicationDirPath());
    repoRoot.cdUp();
    repoRoot.cdUp();
    return repoRoot.filePath("scripts/stats-sidecar/stats_sidecar.py");
}

QString pythonExecutable() {
    const QString override = qEnvironmentVariable("DPA_PYTHON");
    if (!override.isEmpty()) return override;
    return QStandardPaths::findExecutable(QStringLiteral("python"));
}

StatsSuggestionOutput failSuggestion(const std::string& error) {
    StatsSuggestionOutput output;
    output.error = error;
    return output;
}

} // namespace
std::string campaignKnobDomainJson(const std::string& planKey) {
    const CampaignKnobKind kind = campaignKnobKind(planKey);
    QJsonObject domain;
    if (kind == CampaignKnobKind::OrderedEnum) {
        domain["kind"] = QStringLiteral("ordered_enum");
        QJsonArray ladder;
        for (const double value : campaignKnobLadder(planKey))
            ladder.append(value);
        domain["ladder"] = ladder;
    } else if (kind == CampaignKnobKind::Categorical) {
        const std::vector<std::string> levels =
            campaignKnobCategoricalLevels(planKey);
        if (levels.empty()) return std::string();
        domain["kind"] = QStringLiteral("categorical");
        QJsonArray levelArray;
        for (const std::string& level : levels)
            levelArray.append(QString::fromStdString(level));
        domain["levels"] = levelArray;
    } else {
        const double minimum = campaignKnobMinimum(planKey);
        const double maximum = campaignKnobMaximum(planKey);
        if (maximum <= minimum) return std::string();
        domain["kind"] = QStringLiteral("linear_numeric");
        domain["min"] = minimum;
        domain["max"] = maximum;
    }
    return AgentJson::dump(domain);
}

bool campaignSuggestionWithinDomain(const std::string& planKey,
                                    const QJsonValue& value) {
    if (value.isNull() || value.isUndefined()) return false;
    const CampaignKnobKind kind = campaignKnobKind(planKey);
    if (kind == CampaignKnobKind::Categorical) {
        if (!value.isString()) return false;
        const std::vector<std::string> levels =
            campaignKnobCategoricalLevels(planKey);
        const std::string text = value.toString().toStdString();
        return std::find(levels.begin(), levels.end(), text) != levels.end();
    }
    if (kind == CampaignKnobKind::OrderedEnum) {
        if (!value.isDouble() || !qIsFinite(value.toDouble())) return false;
        for (const double entry : campaignKnobLadder(planKey))
            if (entry == value.toDouble()) return true;
        return false;
    }
    if (!value.isDouble() || !qIsFinite(value.toDouble())) return false;
    const double number = value.toDouble();
    return number >= campaignKnobMinimum(planKey)
        && number <= campaignKnobMaximum(planKey);
}

StatsSidecarOutput runStatsSidecarRequest(const std::string& requestJson) {
    StatsSidecarOutput output;
    if (requestJson.empty() || requestJson.size() > kMaxRequestBytes) {
        output.error = "stats_sidecar_request_invalid";
        return output;
    }
    const QString script = statsSidecarScriptPath();
    if (!QFileInfo::exists(script)) {
        output.error = "stats_sidecar_script_missing";
        return output;
    }
    const QString python = pythonExecutable();
    if (python.isEmpty()) {
        output.error = "stats_sidecar_runtime_missing";
        return output;
    }

    QTemporaryFile requestFile;
    if (!requestFile.open()) {
        output.error = "stats_sidecar_request_write_failed";
        return output;
    }
    requestFile.write(QByteArray::fromStdString(requestJson));
    requestFile.flush();
    requestFile.close();

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    // Bound worker allocation before Python imports BLAS/Torch. This child-only
    // limit avoids exhausting Windows commit memory during a one-shot request.
    for (const QString& name : {QStringLiteral("OPENBLAS_NUM_THREADS"),
                               QStringLiteral("OMP_NUM_THREADS"),
                               QStringLiteral("MKL_NUM_THREADS")})
        environment.insert(name, QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    process.setProcessEnvironment(environment);
    process.setProgram(python);
    process.setArguments({script, requestFile.fileName()});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(kStartTimeoutMs)
        || !process.waitForFinished(kFinishTimeoutMs)) {
        process.kill();
        output.error = "stats_sidecar_timeout";
        return output;
    }
    const QByteArray rawOutput = process.readAllStandardOutput();
    if (rawOutput.isEmpty() || rawOutput.size() > kMaxOutputBytes) {
        output.error = "stats_sidecar_output_too_large";
        return output;
    }
    const QJsonObject result =
        AgentJson::parseObject(std::string(rawOutput.constData(),
                                           static_cast<size_t>(rawOutput.size())));
    if (result.isEmpty()
        || process.exitCode() != 0
        || result.value("ok").toBool() != true) {
        output.error = result.value("error").toString("stats_sidecar_failed")
                           .toStdString();
        return output;
    }
    output.resultJson = std::string(rawOutput.constData(),
                                    static_cast<size_t>(rawOutput.size()));
    output.ok = true;
    return output;
}

StatsSuggestionOutput buildExperimentRefinementSuggestion(
    const std::string& requestJson) {
    StatsSuggestionOutput output;
    const QJsonObject input = AgentJson::parseObject(requestJson);
    if (input.isEmpty())
        return failSuggestion("stats_suggestion_request_invalid");
    static const QStringList knownKeys{"contract", "observations", "seed",
                                       "candidate_budget"};
    for (auto it = input.begin(); it != input.end(); ++it)
        if (!knownKeys.contains(it.key()))
            return failSuggestion("stats_suggestion_schema_invalid");
    if (!input.contains(QStringLiteral("contract"))
        || !input.contains(QStringLiteral("observations")))
        return failSuggestion("stats_suggestion_schema_invalid");

    const QByteArray contractBytes = QJsonDocument(
        input.value("contract").toObject()).toJson(QJsonDocument::Compact);
    const CampaignValidation gate = parseAndValidateExperimentCampaign(
        std::string(contractBytes.constData(), static_cast<size_t>(contractBytes.size())));
    if (!gate.ok)
        return failSuggestion("campaign_contract_invalid:" + gate.error);
    const ExperimentCampaign& campaign = gate.campaign;

    if (!input.value("observations").isArray())
        return failSuggestion("observations_invalid");
    const QJsonArray observations = input.value("observations").toArray();

    int seed = 0;
    if (input.contains(QStringLiteral("seed"))) {
        if (!input.value("seed").isDouble()
            || input.value("seed").toDouble() < 0.0
            || input.value("seed").toDouble() != std::floor(input.value("seed").toDouble()))
            return failSuggestion("seed_invalid");
        seed = static_cast<int>(
            std::fmod(input.value("seed").toDouble(), 2147483648.0));
    }
    int candidateBudget = 256;
    if (input.contains(QStringLiteral("candidate_budget"))) {
        if (!input.value("candidate_budget").isDouble())
            return failSuggestion("candidate_budget_invalid");
        candidateBudget = input.value("candidate_budget").toInt(256);
        candidateBudget = std::max(16, std::min(1024, candidateBudget));
    }

    // Expanded design rows aligned by run index through the randomized order,
    // exactly like the analysis layer consumes them.
    const int plannedRuns = static_cast<int>(campaign.runPlanJsons.size());
    QJsonArray designRows;
    QJsonArray pairedObservations;
    QSet<int> observedRuns;
    for (const QJsonValue& entry : observations) {
        const QJsonObject observation = entry.toObject();
        if (observation.isEmpty()
            || !observation.value("run_index").isDouble()
            || !observation.value("value").isDouble())
            return failSuggestion("observation_invalid");
        const int runIndex = observation.value("run_index").toInt(-1);
        if (runIndex < 0 || runIndex >= plannedRuns
            || observation.value("run_index").toDouble() != runIndex)
            return failSuggestion("run_index_out_of_range");
        if (observedRuns.contains(runIndex)) return failSuggestion("duplicate_run_index");
        observedRuns.insert(runIndex);
        designRows.append(AgentJson::parseObject(campaign.designLevelsJson[
            static_cast<size_t>(campaign.designIndices[static_cast<size_t>(runIndex)])]));
        QJsonObject paired = observation;
        paired.insert(QStringLiteral("run_index"), designRows.size() - 1);
        pairedObservations.append(paired);
    }

    QJsonObject domains;
    for (const CampaignFactor& factor : campaign.factors) {
        const std::string domain = campaignKnobDomainJson(factor.name);
        if (domain.empty()) return failSuggestion("knob_domain_unknown:" + factor.name);
        domains.insert(QString::fromStdString(factor.name),
                       AgentJson::parseObject(domain));
    }

    QStringList warnings;
    QString surrogate;
    QJsonObject suggestion;
    bool usedSidecar = false;
    const QString sidecarDirection = campaign.responseDirection.empty()
        ? QStringLiteral("maximize")
        : QString::fromStdString(campaign.responseDirection);
    const QString objectiveDirection = campaign.responseDirection.empty()
        ? QStringLiteral("legacy_maximize")
        : sidecarDirection;
    if (!designRows.isEmpty()) {
        QJsonObject request{
            {"method", QStringLiteral("bo_next")},
            {"observations", pairedObservations},
            {"design", designRows},
            {"domains", domains},
            {"direction", sidecarDirection},
            {"seed", seed},
            {"candidate_budget", candidateBudget}};
        if (campaign.responseTargetPresent)
            request.insert(QStringLiteral("target"), campaign.responseTarget);
        const StatsSidecarOutput sidecar =
            runStatsSidecarRequest(AgentJson::dump(request));
        if (sidecar.ok) {
            const QJsonObject response =
                AgentJson::parseObject(sidecar.resultJson);
            const QJsonObject levels = response.value("next_levels").toObject();
            bool domainValid = !levels.isEmpty() && levels.size() == domains.size()
                && response.value("direction").toString() == sidecarDirection;
            if (levels.size() != domains.size())
                warnings.append(QStringLiteral("stats_sidecar_levels_incomplete"));
            if (response.value("direction").toString() != sidecarDirection)
                warnings.append(QStringLiteral("stats_sidecar_direction_mismatch"));
            for (auto it = levels.begin(); it != levels.end(); ++it) {
                const std::string key = it.key().toStdString();
                const bool knownKnob = std::any_of(
                    campaign.factors.begin(), campaign.factors.end(),
                    [&](const CampaignFactor& factor) {
                        return factor.name == key; });
                if (!knownKnob
                    || !campaignSuggestionWithinDomain(key, it.value())) {
                    warnings.append(QStringLiteral(
                        "stats_sidecar_domain_violation:") + it.key());
                    domainValid = false;
                    break;
                }
            }
            if (domainValid) {
                usedSidecar = true;
                surrogate = response.value("surrogate").toString();
                suggestion = levels;
            }
        } else {
            warnings.append(QStringLiteral("stats_sidecar_unavailable:")
                            + QString::fromStdString(sidecar.error));
        }
    } else {
        warnings.append(QStringLiteral("no_observations_deterministic_grid"));
    }

    if (!usedSidecar) {
        // Deterministic grid fallback: domain midpoint per knob, mirroring the
        // sidecar's own degenerate-input behaviour.
        surrogate = QStringLiteral("builtin_grid");
        for (const CampaignFactor& factor : campaign.factors) {
            const QString key = QString::fromStdString(factor.name);
            const QJsonObject domain = AgentJson::parseObject(
                campaignKnobDomainJson(factor.name));
            const QString kind = domain.value("kind").toString();
            if (kind == QStringLiteral("linear_numeric")) {
                const double middle = (domain.value("min").toDouble()
                                       + domain.value("max").toDouble()) / 2.0;
                suggestion.insert(key, middle);
            } else if (kind == QStringLiteral("ordered_enum")) {
                const QJsonArray ladder = domain.value("ladder").toArray();
                suggestion.insert(key, ladder.at(ladder.size() / 2));
            } else {
                const QJsonArray levelList = domain.value("levels").toArray();
                suggestion.insert(key, levelList.at(levelList.size() / 2));
            }
        }
    }

    QJsonObject resultPayload{
        {"schema", QStringLiteral("dpa.stats.sidecar.suggestion/v1")},
        {"source_campaign_id", QString::fromStdString(campaign.campaignId)},
         {"source_campaign_sha256",
          QString::fromStdString(campaign.campaignSha256)},
        {"objective_direction", objectiveDirection},
        {"next_levels", suggestion},
        {"surrogate", surrogate},
        {"source", usedSidecar ? QStringLiteral("stats_sidecar")
                               : QStringLiteral("builtin")},
        {"paired_observations", designRows.size()},
        {"warnings", QJsonArray::fromStringList(warnings)}};
    output.ok = true;
    output.resultJson =
        QJsonDocument(resultPayload).toJson(QJsonDocument::Compact).toStdString();
    return output;
}

} // namespace Model
