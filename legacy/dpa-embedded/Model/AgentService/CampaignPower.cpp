#include "CampaignPower.h"

#include "ExperimentCampaign.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <vector>

namespace Model {
namespace {

constexpr int kMaxRequestBytes = 64 * 1024;
constexpr int kDefaultSimulations = 200;
constexpr int kMinSimulations = 50;
constexpr int kMaxSimulations = 2000;
// Planning reads p-values against alpha; a coarse inner permutation loop adds
// Monte Carlo noise exactly at that decision boundary, so default higher than
// the analysis layer's exploratory default.
constexpr int kDefaultPermutations = 999;
constexpr int kMinPermutations = 99;
constexpr int kMaxPermutations = 1999;
constexpr double kDefaultAlpha = 0.05;
constexpr double kMaxAbsValue = 1e12;

bool integerValue(const QJsonValue& value, long long minimum, long long maximum) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (number != static_cast<double>(static_cast<long long>(number))) return false;
    const long long integer = static_cast<long long>(number);
    return integer >= minimum && integer <= maximum;
}

int unbiasedDraw(std::mt19937_64& rng, int bound) {
    const unsigned long long bound64 = static_cast<unsigned long long>(bound);
    const unsigned long long limit =
        (std::numeric_limits<unsigned long long>::max() / bound64) * bound64;
    unsigned long long draw = rng();
    while (draw >= limit) draw = rng();
    return static_cast<int>(draw % bound64);
}

double uniform01(std::mt19937_64& rng) {
    return static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0);
}

double standardNormal(std::mt19937_64& rng) {
    double first = uniform01(rng);
    while (first <= 0.0) first = uniform01(rng);
    const double second = uniform01(rng);
    return std::sqrt(-2.0 * std::log(first)) * std::cos(6.283185307179586 * second);
}

double betweenGroupSumOfSquares(const std::vector<double>& values,
                                const std::vector<int>& assignment, int groupCount) {
    std::vector<double> sums(static_cast<size_t>(groupCount), 0.0);
    std::vector<int> counts(static_cast<size_t>(groupCount), 0);
    for (size_t i = 0; i < values.size(); ++i) {
        sums[static_cast<size_t>(assignment[i])] += values[i];
        ++counts[static_cast<size_t>(assignment[i])];
    }
    double total = 0.0;
    int usedGroups = 0;
    for (int group = 0; group < groupCount; ++group) {
        if (counts[static_cast<size_t>(group)] == 0) continue;
        total += sums[static_cast<size_t>(group)];
        ++usedGroups;
    }
    if (usedGroups < 2 || values.empty()) return 0.0;
    const double grand = total / static_cast<double>(values.size());
    double statistic = 0.0;
    for (int group = 0; group < groupCount; ++group) {
        const int count = counts[static_cast<size_t>(group)];
        if (count == 0) continue;
        const double mean = sums[static_cast<size_t>(group)] / static_cast<double>(count);
        statistic += static_cast<double>(count) * (mean - grand) * (mean - grand);
    }
    return statistic;
}

// Exact Fisher randomization test p-value for one synthetic experiment. The
// rng is consumed after the noise draws so each simulation owns a single
// deterministic stream.
double randomizedPValue(const std::vector<double>& values,
                        const std::vector<int>& assignment, int groupCount,
                        int permutations, std::mt19937_64& rng) {
    double observedStatistic = betweenGroupSumOfSquares(values, assignment, groupCount);
    if (observedStatistic <= 0.0) return 1.0;
    std::vector<double> permuted(values);
    int exceedCount = 0;
    for (int trial = 0; trial < permutations; ++trial) {
        for (int i = static_cast<int>(permuted.size()) - 1; i > 0; --i) {
            const int j = unbiasedDraw(rng, i + 1);
            std::swap(permuted[static_cast<size_t>(i)], permuted[static_cast<size_t>(j)]);
        }
        if (betweenGroupSumOfSquares(permuted, assignment, groupCount)
            >= observedStatistic * (1.0 - 1e-9))
            ++exceedCount;
    }
    return (static_cast<double>(exceedCount) + 1.0)
        / (static_cast<double>(permutations) + 1.0);
}

} // namespace

CampaignPowerOutput estimateCampaignPower(const std::string& requestJson) {
    CampaignPowerOutput output;
    if (requestJson.empty() || requestJson.size() > kMaxRequestBytes) {
        output.error = "campaign_power_size_invalid";
        return output;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(requestJson), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        output.error = "campaign_power_json_invalid";
        return output;
    }
    const QJsonObject root = document.object();
    const QStringList knownKeys{"contract", "factor", "effect_range", "noise_std",
                                "seed", "alpha", "simulations", "permutations"};
    for (auto it = root.begin(); it != root.end(); ++it)
        if (!knownKeys.contains(it.key())) {
            output.error = "campaign_power_schema_invalid";
            return output;
        }
    if (!root.contains(QStringLiteral("contract"))
        || !root.contains(QStringLiteral("factor"))
        || !root.contains(QStringLiteral("effect_range"))
        || !root.contains(QStringLiteral("noise_std"))
        || !root.contains(QStringLiteral("seed"))) {
        output.error = "campaign_power_schema_invalid";
        return output;
    }

    const QByteArray contractBytes = QJsonDocument(
        root.value("contract").toObject()).toJson(QJsonDocument::Compact);
    const CampaignValidation contract = parseAndValidateExperimentCampaign(
        std::string(contractBytes.constData(), static_cast<size_t>(contractBytes.size())));
    if (!contract.ok) {
        output.error = "campaign_contract_invalid:" + contract.error;
        return output;
    }
    const ExperimentCampaign& campaign = contract.campaign;

    if (!root.value("factor").isString()) {
        output.error = "factor_invalid";
        return output;
    }
    const QString factorName = root.value("factor").toString();
    size_t factorIndex = campaign.factors.size();
    for (size_t f = 0; f < campaign.factors.size(); ++f)
        if (QString::fromStdString(campaign.factors[f].name) == factorName)
            factorIndex = f;
    if (factorIndex == campaign.factors.size()) {
        output.error = "factor_unknown";
        return output;
    }

    if (!root.value("effect_range").isDouble()
        || !qIsFinite(root.value("effect_range").toDouble())
        || root.value("effect_range").toDouble() < 0.0
        || root.value("effect_range").toDouble() > kMaxAbsValue
        || !root.value("noise_std").isDouble()
        || !qIsFinite(root.value("noise_std").toDouble())
        || root.value("noise_std").toDouble() < 0.0
        || root.value("noise_std").toDouble() > kMaxAbsValue) {
        output.error = "magnitude_invalid";
        return output;
    }
    const double effectRange = root.value("effect_range").toDouble();
    const double noiseStd = root.value("noise_std").toDouble();
    if (effectRange <= 0.0 && noiseStd <= 0.0) {
        output.error = "empty_question";
        return output;
    }

    if (!integerValue(root.value("seed"), 0, 9007199254740991LL)) {
        output.error = "seed_invalid";
        return output;
    }
    double alpha = kDefaultAlpha;
    if (root.contains(QStringLiteral("alpha"))) {
        if (!root.value("alpha").isDouble()
            || !qIsFinite(root.value("alpha").toDouble())
            || root.value("alpha").toDouble() <= 0.0
            || root.value("alpha").toDouble() > 0.5) {
            output.error = "alpha_invalid";
            return output;
        }
        alpha = root.value("alpha").toDouble();
    }
    int simulations = kDefaultSimulations;
    if (root.contains(QStringLiteral("simulations"))) {
        if (!integerValue(root.value("simulations"), kMinSimulations, kMaxSimulations)) {
            output.error = "simulations_invalid";
            return output;
        }
        simulations = static_cast<int>(root.value("simulations").toDouble());
    }
    int permutations = kDefaultPermutations;
    if (root.contains(QStringLiteral("permutations"))) {
        if (!integerValue(root.value("permutations"), kMinPermutations, kMaxPermutations)) {
            output.error = "permutations_invalid";
            return output;
        }
        permutations = static_cast<int>(root.value("permutations").toDouble());
    }

    // Fixed assignment of the planned execution order to the target factor's
    // levels, plus the assumed additive level effects spread across the range.
    const CampaignFactor& factor = campaign.factors[factorIndex];
    const int groups = static_cast<int>(factor.levelsJson.size());
    const int plannedRuns = static_cast<int>(campaign.runPlanJsons.size());
    long long totalDesigns = 1;
    for (const CampaignFactor& entry : campaign.factors)
        totalDesigns *= static_cast<long long>(entry.levelsJson.size());
    std::vector<long long> strides(campaign.factors.size(), 0);
    long long suffix = totalDesigns;
    for (size_t f = 0; f < campaign.factors.size(); ++f) {
        suffix /= static_cast<long long>(campaign.factors[f].levelsJson.size());
        strides[f] = suffix;
    }
    std::vector<int> targetAssignment(static_cast<size_t>(plannedRuns), 0);
    for (int runIndex = 0; runIndex < plannedRuns; ++runIndex) {
        const int design = campaign.designIndices[static_cast<size_t>(runIndex)];
        targetAssignment[static_cast<size_t>(runIndex)] = static_cast<int>(
            (design / strides[factorIndex]) % static_cast<long long>(groups));
    }
    std::vector<double> levelEffects(static_cast<size_t>(groups), 0.0);
    for (int level = 0; level < groups; ++level)
        levelEffects[static_cast<size_t>(level)] = groups == 1 ? 0.0
            : -effectRange / 2.0
                + effectRange * static_cast<double>(level)
                    / static_cast<double>(groups - 1);

    const auto runTrials = [&](unsigned long long streamSeed, bool withEffect) {
        int detected = 0;
        for (int simulation = 0; simulation < simulations; ++simulation) {
            std::mt19937_64 rng(streamSeed
                + static_cast<unsigned long long>(simulation) * 92821ULL);
            std::vector<double> values(static_cast<size_t>(plannedRuns), 0.0);
            for (int runIndex = 0; runIndex < plannedRuns; ++runIndex) {
                const double effect = withEffect
                    ? levelEffects[static_cast<size_t>(
                        targetAssignment[static_cast<size_t>(runIndex)])]
                    : 0.0;
                values[static_cast<size_t>(runIndex)] = effect
                    + (noiseStd > 0.0 ? noiseStd * standardNormal(rng) : 0.0);
            }
            if (randomizedPValue(values, targetAssignment, groups,
                    permutations, rng) <= alpha + 1e-12)
                ++detected;
        }
        return detected;
    };

    const int effectDetected = runTrials(
        static_cast<unsigned long long>(root.value("seed").toDouble()) * 2ULL + 1ULL,
        true);
    const int nullDetected = runTrials(
        static_cast<unsigned long long>(root.value("seed").toDouble()) * 2ULL,
        false);

    const double estimatedPower =
        static_cast<double>(effectDetected) / static_cast<double>(simulations);
    const double nullRate =
        static_cast<double>(nullDetected) / static_cast<double>(simulations);
    // The null pass is a live calibration check: under H0 the exact test
    // rejects at rate alpha up to Monte Carlo error (~3 sigma tolerance).
    const double mcSigma = std::sqrt(alpha * (1.0 - alpha)
        / static_cast<double>(simulations));
    const bool calibrated = qAbs(nullRate - alpha) <= 3.0 * mcSigma + 1e-12;

    const QJsonObject result{
        {"schema", "dpa.experiment.campaign.power/v1"},
        {"campaign_id", QString::fromStdString(campaign.campaignId)},
        {"campaign_sha256", QString::fromStdString(campaign.campaignSha256)},
        {"factor", factorName},
        {"levels", static_cast<double>(groups)},
        {"effect_range", effectRange},
        {"noise_std", noiseStd},
        {"alpha", alpha},
        {"planned_runs", static_cast<double>(plannedRuns)},
        {"replicates", static_cast<double>(campaign.replicates)},
        {"simulations", static_cast<double>(simulations)},
        {"permutations_per_test", static_cast<double>(permutations)},
        {"seed", root.value("seed").toDouble()},
        {"estimated_power", estimatedPower},
        {"null_false_positive_rate", nullRate},
        {"calibrated", calibrated}};
    output.resultJson = QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
    output.ok = true;
    return output;
}

} // namespace Model
