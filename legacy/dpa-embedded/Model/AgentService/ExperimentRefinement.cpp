#include "ExperimentRefinement.h"

#include "CampaignAnalysis.h"
#include "CampaignPower.h"
#include "ExperimentCampaign.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace Model {
namespace {

constexpr int kMaxRequestBytes = 256 * 1024;
constexpr double kDefaultThreshold = 2.0;
constexpr int kMaxProposedRuns = 64;

struct FactorPlan {
    QString name;
    CampaignKnobKind kind;
    bool important = false;
    bool promoted = false;
    double effectRange = 0.0;
    double snr = 0.0;
    int bestLevelIndex = -1;
    QJsonValue bestLevelValue;
    double bestMean = 0.0;
    int bestCount = 0;
    std::vector<QJsonValue> originalLevels;
    std::vector<double> originalMeans;
    std::vector<int> originalCounts;
    std::vector<QJsonValue> refinedLevels;
};

std::string composeHypothesis(const std::string& sourceId,
                              const QStringList& refined,
                              const QStringList& fixed) {
    QString text = QStringLiteral("Refinement of %1: refine %2; fix %3 at best.")
        .arg(QString::fromStdString(sourceId),
             refined.isEmpty() ? QStringLiteral("none") : refined.join(QStringLiteral(",")),
             fixed.isEmpty() ? QStringLiteral("none") : fixed.join(QStringLiteral(",")));
    if (text.size() > 512) text = text.left(512);
    return text.toStdString();
}

} // namespace

CampaignRefinementOutput refineExperimentCampaign(const std::string& requestJson) {
    CampaignRefinementOutput output;
    if (requestJson.empty() || requestJson.size() > kMaxRequestBytes) {
        output.error = "campaign_refinement_size_invalid";
        return output;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(requestJson), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        output.error = "campaign_refinement_json_invalid";
        return output;
    }
    const QJsonObject root = document.object();
    const QStringList knownKeys{"contract", "observations", "seed", "threshold"};
    for (auto it = root.begin(); it != root.end(); ++it)
        if (!knownKeys.contains(it.key())) {
            output.error = "campaign_refinement_schema_invalid";
            return output;
        }
    if (!root.contains(QStringLiteral("contract"))
        || !root.contains(QStringLiteral("observations"))
        || !root.contains(QStringLiteral("seed"))) {
        output.error = "campaign_refinement_schema_invalid";
        return output;
    }
    double threshold = kDefaultThreshold;
    if (root.contains(QStringLiteral("threshold"))) {
        if (!root.value("threshold").isDouble()
            || !qIsFinite(root.value("threshold").toDouble())
            || root.value("threshold").toDouble() <= 0.0
            || root.value("threshold").toDouble() > 100.0) {
            output.error = "threshold_invalid";
            return output;
        }
        threshold = root.value("threshold").toDouble();
    }

    // Findings come from the deterministic analysis layer, never re-derived.
    const QJsonObject analysisRequest{
        {"contract", root.value("contract")},
        {"observations", root.value("observations")},
        {"seed", root.value("seed")}};
    const QByteArray analysisBytes =
        QJsonDocument(analysisRequest).toJson(QJsonDocument::Compact);
    const CampaignAnalysisOutput analysis = analyzeExperimentCampaign(
        std::string(analysisBytes.constData(), static_cast<size_t>(analysisBytes.size())));
    if (!analysis.ok) {
        output.error = "refine_analysis_failed:" + analysis.error;
        return output;
    }
    const QJsonObject analysisResult = QJsonDocument::fromJson(
        QByteArray::fromStdString(analysis.resultJson)).object();

    const QByteArray contractBytes = QJsonDocument(
        root.value("contract").toObject()).toJson(QJsonDocument::Compact);
    const CampaignValidation contract = parseAndValidateExperimentCampaign(
        std::string(contractBytes.constData(), static_cast<size_t>(contractBytes.size())));
    if (!contract.ok) {
        output.error = "campaign_contract_invalid:" + contract.error;
        return output;
    }
    const ExperimentCampaign& source = contract.campaign;

    const double residualStd = analysisResult.value("residual_std").toDouble();
    const bool residualEstimable = analysisResult.value("residual_estimable").toBool();
    const QJsonArray analysisFactors = analysisResult.value("factors").toArray();

    std::vector<FactorPlan> plans(source.factors.size());
    for (size_t f = 0; f < source.factors.size(); ++f) {
        FactorPlan& plan = plans[f];
        const CampaignFactor& factor = source.factors[f];
        plan.name = QString::fromStdString(factor.name);
        plan.kind = campaignKnobKind(factor.name);
        const QJsonObject analysisFactor =
            analysisFactors.at(static_cast<int>(f)).toObject();
        plan.effectRange = analysisFactor.value("effect_range").toDouble();
        const bool testable = analysisFactor.value("testable").toBool();
        const QJsonArray levels = analysisFactor.value("levels").toArray();
        double bestMean = 0.0;
        double bestUtility = std::numeric_limits<double>::lowest();
        for (int level = 0; level < levels.size(); ++level) {
            const QJsonObject entry = levels.at(level).toObject();
            plan.originalLevels.push_back(entry.value("value"));
            plan.originalMeans.push_back(entry.value("mean").toDouble());
            plan.originalCounts.push_back(entry.value("count").toInt());
        }
        for (int level = 0; level < levels.size(); ++level) {
            if (plan.originalCounts[static_cast<size_t>(level)] == 0) continue;
            const double utility = campaignResponseUtility(
                source, plan.originalMeans[static_cast<size_t>(level)]);
            if (plan.bestLevelIndex < 0 || utility > bestUtility) {
                bestMean = plan.originalMeans[static_cast<size_t>(level)];
                bestUtility = utility;
                plan.bestLevelIndex = level;
            }
        }
        if (plan.bestLevelIndex < 0) {
            output.error = "factor_without_observations:" + factor.name;
            return output;
        }
        plan.bestLevelValue = plan.originalLevels[static_cast<size_t>(plan.bestLevelIndex)];
        plan.bestMean = bestMean;
        plan.bestCount = plan.originalCounts[static_cast<size_t>(plan.bestLevelIndex)];
        plan.snr = residualStd > 0.0 ? plan.effectRange / residualStd
                                     : (plan.effectRange > 0.0 ? 1e18 : 0.0);
        plan.important = testable && plan.snr >= threshold;
    }

    QStringList warnings;
    if (std::none_of(plans.begin(), plans.end(),
                     [](const FactorPlan& plan) { return plan.important; })) {
        size_t dominant = 0;
        for (size_t f = 1; f < plans.size(); ++f)
            if (plans[f].effectRange > plans[dominant].effectRange) dominant = f;
        if (plans[dominant].effectRange <= 0.0) {
            output.error = "no_signal_to_refine";
            return output;
        }
        plans[dominant].important = true;
        plans[dominant].promoted = true;
        warnings.append(QStringLiteral(
            "no_factor_above_threshold_promoted_dominant"));
    }

    // Refined levels per important factor; unimportant factors are baked into
    // the template at their best observed value.
    QJsonObject proposedTemplate = root.value("contract").toObject()
        .value("run_template").toObject();
    QJsonArray proposedFactors;
    QStringList refinedNames;
    QStringList fixedNames;
    std::size_t totalDesigns = 1;
    for (FactorPlan& plan : plans) {
        if (!plan.important) {
            proposedTemplate.insert(plan.name, plan.bestLevelValue);
            fixedNames.append(plan.name);
            continue;
        }
        refinedNames.append(plan.name);
        std::vector<QJsonValue> chosen;
        if (plan.kind == CampaignKnobKind::Categorical) {
            std::vector<int> order;
            for (int level = 0; level < static_cast<int>(plan.originalLevels.size());
                 ++level)
                if (plan.originalCounts[static_cast<size_t>(level)] > 0)
                    order.push_back(level);
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                const double aUtility = campaignResponseUtility(
                    source, plan.originalMeans[static_cast<size_t>(a)]);
                const double bUtility = campaignResponseUtility(
                    source, plan.originalMeans[static_cast<size_t>(b)]);
                if (aUtility != bUtility) return aUtility > bUtility;
                return a < b;
            });
            for (const int level : order) {
                chosen.push_back(plan.originalLevels[static_cast<size_t>(level)]);
                if (chosen.size() >= 2) break;
            }
        } else if (plan.kind == CampaignKnobKind::OrderedEnum) {
            const std::vector<double> ladder =
                campaignKnobLadder(plan.name.toStdString());
            const double bestValue = plan.bestLevelValue.toDouble();
            int position = 0;
            double distance = std::numeric_limits<double>::max();
            for (int i = 0; i < static_cast<int>(ladder.size()); ++i) {
                const double gap = std::fabs(ladder[static_cast<size_t>(i)] - bestValue);
                if (gap < distance) { distance = gap; position = i; }
            }
            std::set<double> distinct;
            for (const int offset : {-1, 0, 1}) {
                const int index = position + offset;
                if (index < 0 || index >= static_cast<int>(ladder.size())) continue;
                if (distinct.insert(ladder[static_cast<size_t>(index)]).second)
                    chosen.push_back(QJsonValue(ladder[static_cast<size_t>(index)]));
            }
        } else {
            const std::string key = plan.name.toStdString();
            std::vector<std::pair<double, int>> sortedLevels;
            for (int level = 0; level < static_cast<int>(plan.originalLevels.size());
                 ++level)
                sortedLevels.emplace_back(
                    plan.originalLevels[static_cast<size_t>(level)].toDouble(), level);
            std::sort(sortedLevels.begin(), sortedLevels.end());
            int sortedBest = 0;
            for (int i = 0; i < static_cast<int>(sortedLevels.size()); ++i)
                if (sortedLevels[static_cast<size_t>(i)].second == plan.bestLevelIndex)
                    sortedBest = i;
            const double lower = sortedLevels[
                static_cast<size_t>(std::max(0, sortedBest - 1))].first;
            const double center = sortedLevels[static_cast<size_t>(sortedBest)].first;
            const double upper = sortedLevels[static_cast<size_t>(
                std::min(static_cast<int>(sortedLevels.size()) - 1, sortedBest + 1))].first;
            std::set<long long> distinct;
            const auto consider = [&](double raw) {
                const double clamped = std::min(std::max(raw,
                    campaignKnobMinimum(key)), campaignKnobMaximum(key));
                distinct.insert(std::llround(clamped));
            };
            consider((lower + center) / 2.0);
            consider(center);
            consider((center + upper) / 2.0);
            for (const long long value : distinct)
                chosen.push_back(QJsonValue(static_cast<double>(value)));
        }
        plan.refinedLevels = chosen;
        QJsonArray levelArray;
        for (const QJsonValue& value : plan.refinedLevels)
            levelArray.append(value);
        proposedFactors.append(QJsonObject{
            {"name", plan.name}, {"levels", levelArray}});
        totalDesigns *= plan.refinedLevels.size();
    }

    int replicates = std::min(5, source.replicates + 1);
    while (totalDesigns * static_cast<std::size_t>(replicates) > kMaxProposedRuns) {
        bool trimmed = false;
        for (FactorPlan& plan : plans) {
            if (!plan.important || plan.refinedLevels.size() <= 2) continue;
            plan.refinedLevels.erase(plan.refinedLevels.begin() + 2,
                                     plan.refinedLevels.end());
            trimmed = true;
        }
        if (!trimmed) {
            output.error = "proposal_too_large";
            return output;
        }
        totalDesigns = 1;
        for (const FactorPlan& plan : plans)
            if (plan.important) totalDesigns *= plan.refinedLevels.size();
    }

    // Deterministic derived seed so the refinement design is reproducible from
    // its parent without colliding with the parent's run order.
    const unsigned long long derivedSeed = [&]() {
        unsigned long long seed = 0;
        for (const QChar character :
             QString::fromStdString(source.campaignSha256).left(13))
            seed = seed * 16ULL + static_cast<unsigned long long>(character.digitValue());
        return seed % 9007199254740991ULL;
    }();

    QString id = QString::fromStdString(source.campaignId);
    if (id.size() > 120) id = id.left(120);
    id += QStringLiteral("-r2");

    size_t strongest = 0;
    for (size_t f = 1; f < plans.size(); ++f)
        if (plans[f].important && !plans[strongest].important)
            strongest = f;
        else if (plans[f].important && plans[strongest].important
                 && plans[f].effectRange > plans[strongest].effectRange)
            strongest = f;

    const auto buildProposal = [&](int candidateReplicates) {
        QJsonObject proposal{
            {"schema", "dpa.experiment.campaign"},
            {"version", 1},
            {"campaign_id", id},
            {"hypothesis", QString::fromStdString(composeHypothesis(
                source.campaignId, refinedNames, fixedNames))},
            {"factors", proposedFactors},
            {"replicates", candidateReplicates},
            {"randomization", QJsonObject{{"enabled", true},
                                           {"seed", static_cast<double>(derivedSeed)}}},
            {"run_template", proposedTemplate},
            {"policy", QJsonObject{{"on_run_failure", QStringLiteral("abort")},
                                    {"max_runs", static_cast<double>(kMaxProposedRuns)},
                                    {"max_failed_runs", 2.0}}}
        };
        const QJsonObject response = root.value("contract").toObject()
                                         .value("response").toObject();
        if (!response.isEmpty()) proposal["response"] = response;
        const QJsonValue objectiveSha = root.value("contract").toObject()
                                             .value("objective_sha256");
        if (objectiveSha.isString()) proposal["objective_sha256"] = objectiveSha;
        return proposal;
    };

    // Small designs have a discrete p-value floor above alpha (mirrored
    // permutations carry identical statistics), so a proposal must be large
    // enough for its own inference to be calibratable. Replicates climb until
    // the power layer confirms calibration or the envelope cap is reached.
    const bool plannable = residualEstimable && residualStd > 0.0;
    QJsonObject proposedContract;
    CampaignValidation proposalCheck;
    QJsonObject powerReport;
    QString powerSkippedReason;
    bool calibrated = false;
    bool powerAttempted = false;
    for (int candidate = replicates; candidate <= 5; ++candidate) {
        if (totalDesigns * static_cast<std::size_t>(candidate) > kMaxProposedRuns)
            break;
        proposedContract = buildProposal(candidate);
        const QByteArray proposedBytes =
            QJsonDocument(proposedContract).toJson(QJsonDocument::Compact);
        proposalCheck = parseAndValidateExperimentCampaign(
            std::string(proposedBytes.constData(), static_cast<size_t>(proposedBytes.size())));
        if (!proposalCheck.ok) {
            output.error = "proposal_self_validation_failed:" + proposalCheck.error;
            return output;
        }
        if (!plannable) break;
        powerAttempted = true;
        const QJsonObject powerRequest{
            {"contract", QJsonDocument::fromJson(QByteArray::fromStdString(
                proposalCheck.campaign.canonicalJson)).object()},
            {"factor", plans[strongest].name},
            {"effect_range", plans[strongest].effectRange},
            {"noise_std", residualStd},
            {"seed", static_cast<double>(derivedSeed)}};
        const QByteArray powerBytes =
            QJsonDocument(powerRequest).toJson(QJsonDocument::Compact);
        const CampaignPowerOutput power = estimateCampaignPower(
            std::string(powerBytes.constData(), static_cast<size_t>(powerBytes.size())));
        if (!power.ok) {
            powerSkippedReason = QString::fromStdString(power.error);
            break;
        }
        powerReport = QJsonDocument::fromJson(
            QByteArray::fromStdString(power.resultJson)).object();
        if (powerReport.value("calibrated").toBool()) {
            calibrated = true;
            break;
        }
    }
    if (powerAttempted && !calibrated)
        warnings.append(QStringLiteral("proposal_below_calibration_floor"));
    if (!plannable)
        powerSkippedReason = QStringLiteral("zero_noise_no_planning_baseline");
    if (!proposalCheck.ok || proposedContract.isEmpty()) {
        output.error = "proposal_too_large";
        return output;
    }

    QJsonArray assessmentArray;
    for (const FactorPlan& plan : plans) {
        assessmentArray.append(QJsonObject{
            {"name", plan.name},
            {"kind", plan.kind == CampaignKnobKind::Categorical
                ? QStringLiteral("categorical")
                : (plan.kind == CampaignKnobKind::OrderedEnum
                    ? QStringLiteral("ordered_enum") : QStringLiteral("linear_numeric"))},
            {"action", plan.important
                ? (plan.promoted ? QStringLiteral("promoted")
                                 : QStringLiteral("refined"))
                : QStringLiteral("fixed_at_best")},
            {"snr", plan.snr > 1e17 ? 1e18 : plan.snr},
            {"effect_range", plan.effectRange},
            {"best_level", QJsonObject{{"value", plan.bestLevelValue},
                                        {"mean", plan.bestMean},
                                        {"count", plan.bestCount}}}});
    }

    QJsonObject result;
    result.insert(QStringLiteral("schema"),
                  QStringLiteral("dpa.experiment.campaign.refinement/v1"));
    result.insert(QStringLiteral("source_campaign_id"),
                  QString::fromStdString(source.campaignId));
    result.insert(QStringLiteral("source_campaign_sha256"),
                  QString::fromStdString(source.campaignSha256));
    result.insert(QStringLiteral("threshold"), threshold);
    result.insert(QStringLiteral("residual_std"), residualStd);
    result.insert(QStringLiteral("residual_estimable"), residualEstimable);
    result.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(warnings));
    result.insert(QStringLiteral("factor_assessment"), assessmentArray);
    result.insert(QStringLiteral("proposed_campaign_id"), id);
    result.insert(QStringLiteral("proposed_contract_sha256"),
                  QString::fromStdString(proposalCheck.campaign.campaignSha256));
    result.insert(QStringLiteral("proposed_contract"), proposedContract);
    result.insert(QStringLiteral("planned_runs"),
                  static_cast<double>(proposalCheck.campaign.runPlanJsons.size()));
    result.insert(QStringLiteral("replicates"),
                  static_cast<double>(proposalCheck.campaign.replicates));
    if (!powerReport.isEmpty())
        result.insert(QStringLiteral("power"), powerReport);
    result.insert(QStringLiteral("power_skipped_reason"), powerSkippedReason);
    output.resultJson = QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
    output.ok = true;
    return output;
}

} // namespace Model
