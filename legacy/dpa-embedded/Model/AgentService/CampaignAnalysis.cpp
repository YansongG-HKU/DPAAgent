#include "CampaignAnalysis.h"

#include "ExperimentCampaign.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <cmath>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace Model {
namespace {

constexpr int kMaxRequestBytes = 256 * 1024;
constexpr int kDefaultPermutations = 999;
constexpr int kMinPermutations = 199;
constexpr int kMaxPermutations = 4999;
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

// Marginal and cell summaries for one factor pair, shared by the observed
// grid and every Freedman-Lane permuted replica.
struct Grid {
    std::vector<double> aMeans;
    std::vector<double> bMeans;
    std::vector<double> cellMeans;
    std::vector<int> cellCounts;
    double grand = 0.0;
    double interactionSS = 0.0;
};

// Between-group sum of squares over a fixed assignment structure. Used both on
// the observed values and on permuted copies for the randomization test.
double betweenGroupSumOfSquares(const std::vector<double>& values,
                                const std::vector<int>& assignment, int groupCount) {    std::vector<double> sums(static_cast<size_t>(groupCount), 0.0);
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

QJsonValue levelScalar(const std::string& levelJson) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(levelJson))
        .array().first();
}

EvidenceBoundObservations bindObservationsToEvidenceImpl(
    const std::string& campaignResultJson,
    const std::string& expectedCampaignSha256,
    const std::string& observationsJson,
    double minCoverage) {
    EvidenceBoundObservations output;
    constexpr double kMaxAbs = 1e12;
    const auto fail = [&output](const std::string& error) {
        output.error = error;
        return output;
    };
    if (campaignResultJson.empty() || observationsJson.empty()
        || campaignResultJson.size() > kMaxRequestBytes
        || observationsJson.size() > kMaxRequestBytes)
        return fail("evidence_binding_size_invalid");
    if (!std::isfinite(minCoverage) || minCoverage < 0.0 || minCoverage > 1.0)
        return fail("coverage_threshold_invalid");

    // The artifact must be a campaign result of THIS exact campaign.
    const QJsonDocument artifactDoc = QJsonDocument::fromJson(
        QByteArray::fromStdString(campaignResultJson));
    if (!artifactDoc.isObject()) return fail("campaign_artifact_json_invalid");
    QJsonObject artifactRoot = artifactDoc.object();
    if (artifactRoot.value("data").isObject())
        artifactRoot = artifactRoot.value("data").toObject();
    if (artifactRoot.value("schema").toString()
        != QStringLiteral("dpa.experiment.campaign.result/v1"))
        return fail("campaign_artifact_schema_invalid");
    if (artifactRoot.value("status").toString() != QStringLiteral("completed"))
        return fail("campaign_artifact_not_completed");
    if (!artifactRoot.value("campaign_sha256").isString()
        || artifactRoot.value("campaign_sha256").toString().toStdString()
            != expectedCampaignSha256)
        return fail("campaign_artifact_mismatch");
    if (!artifactRoot.value("runs").isArray())
        return fail("campaign_artifact_runs_invalid");

    // Verified set: runs that completed with a bound evidence artifact.
    std::map<int, bool> evidenceOk;  // run_index -> has evidence id
    int recordedRuns = 0;
    const int plannedRuns = artifactRoot.value("planned_runs").toInt(-1);
    if (plannedRuns <= 0) return fail("campaign_artifact_runs_invalid");
    for (const QJsonValue& value : artifactRoot.value("runs").toArray()) {
        const QJsonObject run = value.toObject();
        if (run.isEmpty()) continue;
        const int runIndex = run.value("run_index").toInt(-1);
        if (runIndex < 0 || runIndex >= plannedRuns
            || !run.value("ok").isBool()
            || !evidenceOk.emplace(runIndex, false).second)
            return fail("campaign_artifact_runs_invalid");
        ++recordedRuns;
        if (run.value("ok").toBool(false)
            && !run.value("evidence_artifact_id").toString().isEmpty()) {
            evidenceOk[runIndex] = true;
        }
    }

    const QJsonDocument observationsDoc = QJsonDocument::fromJson(
        QByteArray::fromStdString(observationsJson));
    if (!observationsDoc.isArray()) return fail("observations_invalid");
    QJsonArray bound;
    std::set<int> seen;
    for (const QJsonValue& value : observationsDoc.array()) {
        const QJsonObject observation = value.toObject();
        if (observation.size() != 2 || !observation.value("run_index").isDouble()
            || !observation.value("value").isDouble())
            return fail("observation_value_invalid");
        const int runIndex = observation.value("run_index").toInt(-1);
        const double number = observation.value("value").toDouble();
        if (runIndex < 0 || !qIsFinite(number) || std::fabs(number) > kMaxAbsValue
            || !seen.insert(runIndex).second)
            return fail("observation_value_invalid");
        const auto recorded = evidenceOk.find(runIndex);
        if (recorded == evidenceOk.end())
            return fail("run_not_in_campaign:" + std::to_string(runIndex));
        if (!recorded->second)
            return fail("run_without_evidence:" + std::to_string(runIndex));
        bound.append(QJsonObject{{"run_index", runIndex}, {"value", number}});
    }
    if (recordedRuns == 0 || bound.isEmpty()) return fail("observations_empty");
    const double coverage =
        static_cast<double>(bound.size()) / static_cast<double>(recordedRuns);
    if (coverage < minCoverage)
        return fail("coverage_below_threshold");

    output.observationsJson =
        QJsonDocument(bound).toJson(QJsonDocument::Compact).toStdString();
    output.verifiedRuns = static_cast<int>(bound.size());
    output.ok = true;
    return output;
}

} // namespace

CampaignAnalysisOutput analyzeExperimentCampaign(const std::string& requestJson) {
    CampaignAnalysisOutput output;
    if (requestJson.empty() || requestJson.size() > kMaxRequestBytes) {
        output.error = "campaign_analysis_size_invalid";
        return output;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(requestJson), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        output.error = "campaign_analysis_json_invalid";
        return output;
    }
    const QJsonObject root = document.object();
    const QStringList knownKeys{"contract", "observations", "seed", "permutations"};
    for (auto it = root.begin(); it != root.end(); ++it)
        if (!knownKeys.contains(it.key())) {
            output.error = "campaign_analysis_schema_invalid";
            return output;
        }
    if (!root.contains(QStringLiteral("contract"))
        || !root.contains(QStringLiteral("observations"))
        || !root.contains(QStringLiteral("seed"))) {
        output.error = "campaign_analysis_schema_invalid";
        return output;
    }

    // The embedded contract is validated by the campaign layer itself, so the
    // analysis can never drift from a contract the executor would accept.
    const QByteArray contractBytes = QJsonDocument(
        root.value("contract").toObject()).toJson(QJsonDocument::Compact);
    const CampaignValidation contract = parseAndValidateExperimentCampaign(
        std::string(contractBytes.constData(), static_cast<size_t>(contractBytes.size())));
    if (!contract.ok) {
        output.error = "campaign_contract_invalid:" + contract.error;
        return output;
    }
    const ExperimentCampaign& campaign = contract.campaign;

    if (!integerValue(root.value("seed"), 0, 9007199254740991LL)) {
        output.error = "seed_invalid";
        return output;
    }
    int permutations = kDefaultPermutations;
    if (root.contains(QStringLiteral("permutations"))) {
        if (!integerValue(root.value("permutations"), kMinPermutations, kMaxPermutations)) {
            output.error = "permutations_invalid";
            return output;
        }
        permutations = static_cast<int>(root.value("permutations").toDouble());
    }

    const QJsonArray observations = root.value("observations").toArray();
    const int plannedRuns = static_cast<int>(campaign.runPlanJsons.size());
    if (observations.isEmpty() || static_cast<int>(observations.size()) > plannedRuns) {
        output.error = "observations_count_invalid";
        return output;
    }
    std::vector<double> values;
    std::vector<int> runIndices;
    std::set<int> seenRuns;
    for (const QJsonValue& entry : observations) {
        if (!entry.isObject()) {
            output.error = "observation_schema_invalid";
            return output;
        }
        const QJsonObject observation = entry.toObject();
        if (observation.size() != 2
            || !integerValue(observation.value("run_index"), 0, plannedRuns - 1)
            || !observation.value("value").isDouble()
            || !qIsFinite(observation.value("value").toDouble())
            || qAbs(observation.value("value").toDouble()) > kMaxAbsValue) {
            output.error = "observation_value_invalid";
            return output;
        }
        const int runIndex = observation.value("run_index").toInt();
        if (!seenRuns.insert(runIndex).second) {
            output.error = "duplicate_run_index";
            return output;
        }
        runIndices.push_back(runIndex);
        values.push_back(observation.value("value").toDouble());
    }
    const int analyzedRuns = static_cast<int>(values.size());

    // Mixed-radix strides: stride[f] = product of level counts after f.
    std::vector<int> levelCounts;
    std::vector<long long> strides;
    long long totalDesigns = 1;
    for (const CampaignFactor& factor : campaign.factors)
        totalDesigns *= static_cast<long long>(factor.levelsJson.size());
    long long suffix = totalDesigns;
    for (const CampaignFactor& factor : campaign.factors) {
        const int count = static_cast<int>(factor.levelsJson.size());
        levelCounts.push_back(count);
        suffix /= count;
        strides.push_back(suffix);
    }

    // Per-factor level assignment for every analyzed observation.
    std::vector<std::vector<int>> assignments(campaign.factors.size());
    for (size_t f = 0; f < campaign.factors.size(); ++f)
        assignments[f].resize(static_cast<size_t>(analyzedRuns));
    for (int i = 0; i < analyzedRuns; ++i) {
        const int design =
            campaign.designIndices[static_cast<size_t>(runIndices[static_cast<size_t>(i)])];
        for (size_t f = 0; f < campaign.factors.size(); ++f) {
            assignments[f][static_cast<size_t>(i)] = static_cast<int>(
                (design / strides[f]) % static_cast<long long>(levelCounts[f]));
        }
    }

    double valueSum = 0.0;
    for (const double value : values) valueSum += value;
    const double grandMean = valueSum / static_cast<double>(analyzedRuns);

    // Fisher randomization test: the treatment structure stays fixed while the
    // observed values are reshuffled across exchangeable runs. Swapping labels
    // together would preserve pairing, so only the values move.
    const auto randomizedPValue = [&](size_t factorIndex) {
        const std::vector<int>& assignment = assignments[factorIndex];
        const double observed = betweenGroupSumOfSquares(
            values, assignment, levelCounts[factorIndex]);
        if (observed <= 0.0) return 1.0;
        std::mt19937_64 rng(static_cast<unsigned long long>(
            root.value("seed").toDouble()));
        std::vector<double> permuted(values);
        int exceedCount = 0;
        for (int trial = 0; trial < permutations; ++trial) {
            for (int i = analyzedRuns - 1; i > 0; --i) {
                const int j = unbiasedDraw(rng, i + 1);
                std::swap(permuted[static_cast<size_t>(i)],
                          permuted[static_cast<size_t>(j)]);
            }
            if (betweenGroupSumOfSquares(permuted, assignment,
                    levelCounts[factorIndex]) >= observed * (1.0 - 1e-9))
                ++exceedCount;
        }
        return (static_cast<double>(exceedCount) + 1.0)
            / (static_cast<double>(permutations) + 1.0);
    };

    QJsonArray factorArray;
    for (size_t f = 0; f < campaign.factors.size(); ++f) {
        const CampaignFactor& factor = campaign.factors[f];
        const std::vector<int>& assignment = assignments[f];
        const int groups = levelCounts[f];
        std::vector<double> sums(static_cast<size_t>(groups), 0.0);
        std::vector<int> counts(static_cast<size_t>(groups), 0);
        for (int i = 0; i < analyzedRuns; ++i) {
            const size_t level = static_cast<size_t>(assignment[static_cast<size_t>(i)]);
            sums[level] += values[static_cast<size_t>(i)];
            ++counts[level];
        }
        int nonEmptyLevels = 0;
        double lowest = std::numeric_limits<double>::max();
        double highest = std::numeric_limits<double>::lowest();
        QJsonArray levelArray;
        for (int level = 0; level < groups; ++level) {
            const size_t slot = static_cast<size_t>(level);
            const double mean = counts[slot] == 0
                ? 0.0 : sums[slot] / static_cast<double>(counts[slot]);
            if (counts[slot] > 0) {
                ++nonEmptyLevels;
                lowest = std::min(lowest, mean);
                highest = std::max(highest, mean);
            }
            levelArray.append(QJsonObject{
                {"value", levelScalar(factor.levelsJson[slot])},
                {"mean", mean},
                {"count", counts[slot]}});
        }
        const bool testable = nonEmptyLevels >= 2;
        factorArray.append(QJsonObject{
            {"name", QString::fromStdString(factor.name)},
            {"effect_range", testable ? highest - lowest : 0.0},
            {"perm_p", testable ? randomizedPValue(f) : -1.0},
            {"testable", testable},
            {"levels", levelArray}});
    }

    // Two-factor interactions via Freedman–Lane residual permutation
    // (Anderson & ter Braak 2003): fit the additive model, permute its
    // residuals, add them back to the fitted values, and recompute the
    // classical interaction sum of squares. Shuffling raw values instead would
    // test the joint null of all effects rather than the interaction itself.
    const auto interactionPValue = [&](size_t fa, size_t fb) {
        const std::vector<int>& assignmentA = assignments[fa];
        const std::vector<int>& assignmentB = assignments[fb];
        const int groupsA = levelCounts[fa];
        const int groupsB = levelCounts[fb];
        const int pairCells = groupsA * groupsB;
        const auto makeGrid = [&](const std::vector<double>& ys) {
            std::vector<double> aSums(static_cast<size_t>(groupsA), 0.0);
            std::vector<int> aCounts(static_cast<size_t>(groupsA), 0);
            std::vector<double> bSums(static_cast<size_t>(groupsB), 0.0);
            std::vector<int> bCounts(static_cast<size_t>(groupsB), 0);
            std::vector<double> cellSums(static_cast<size_t>(pairCells), 0.0);
            std::vector<int> cellCounts(static_cast<size_t>(pairCells), 0);
            double total = 0.0;
            for (int i = 0; i < analyzedRuns; ++i) {
                const size_t slot = static_cast<size_t>(i);
                const int la = assignmentA[slot];
                const int lb = assignmentB[slot];
                const double v = ys[slot];
                total += v;
                aSums[static_cast<size_t>(la)] += v;
                ++aCounts[static_cast<size_t>(la)];
                bSums[static_cast<size_t>(lb)] += v;
                ++bCounts[static_cast<size_t>(lb)];
                const int cell = la * groupsB + lb;
                cellSums[static_cast<size_t>(cell)] += v;
                ++cellCounts[static_cast<size_t>(cell)];
            }
            for (int cell = 0; cell < pairCells; ++cell)
                if (cellCounts[static_cast<size_t>(cell)] == 0)
                    return std::make_pair(Grid{}, -1.0);
            Grid grid;
            grid.aMeans.assign(static_cast<size_t>(groupsA), 0.0);
            grid.bMeans.assign(static_cast<size_t>(groupsB), 0.0);
            grid.cellMeans.assign(static_cast<size_t>(pairCells), 0.0);
            grid.cellCounts = cellCounts;
            grid.grand = total / static_cast<double>(analyzedRuns);
            for (int a = 0; a < groupsA; ++a)
                grid.aMeans[static_cast<size_t>(a)] =
                    aSums[static_cast<size_t>(a)]
                    / static_cast<double>(aCounts[static_cast<size_t>(a)]);
            for (int b = 0; b < groupsB; ++b)
                grid.bMeans[static_cast<size_t>(b)] =
                    bSums[static_cast<size_t>(b)]
                    / static_cast<double>(bCounts[static_cast<size_t>(b)]);
            for (int cell = 0; cell < pairCells; ++cell)
                grid.cellMeans[static_cast<size_t>(cell)] =
                    cellSums[static_cast<size_t>(cell)]
                    / static_cast<double>(cellCounts[static_cast<size_t>(cell)]);
            double statistic = 0.0;
            for (int a = 0; a < groupsA; ++a)
                for (int b = 0; b < groupsB; ++b) {
                    const size_t cell = static_cast<size_t>(a * groupsB + b);
                    const double deviation = grid.cellMeans[cell]
                        - grid.aMeans[static_cast<size_t>(a)]
                        - grid.bMeans[static_cast<size_t>(b)] + grid.grand;
                    statistic += static_cast<double>(grid.cellCounts[cell])
                        * deviation * deviation;
                }
            grid.interactionSS = statistic;
            return std::make_pair(grid, statistic);
        };

        const auto observedPair = makeGrid(values);
        if (observedPair.second < 0.0) return -1.0;
        const Grid& observedGrid = observedPair.first;
        if (observedPair.second <= 0.0) return 1.0;

        std::vector<double> fitted(static_cast<size_t>(analyzedRuns), 0.0);
        std::vector<double> residuals(static_cast<size_t>(analyzedRuns), 0.0);
        for (int i = 0; i < analyzedRuns; ++i) {
            const size_t slot = static_cast<size_t>(i);
            fitted[slot] = observedGrid.aMeans[static_cast<size_t>(assignmentA[slot])]
                + observedGrid.bMeans[static_cast<size_t>(assignmentB[slot])]
                - observedGrid.grand;
            residuals[slot] = values[slot] - fitted[slot];
        }
        std::mt19937_64 rng(static_cast<unsigned long long>(
            root.value("seed").toDouble()));
        int exceedCount = 0;
        for (int trial = 0; trial < permutations; ++trial) {
            std::vector<double> permuted(residuals);
            for (int i = analyzedRuns - 1; i > 0; --i) {
                const int j = unbiasedDraw(rng, i + 1);
                std::swap(permuted[static_cast<size_t>(i)],
                          permuted[static_cast<size_t>(j)]);
            }
            std::vector<double> synthetic(static_cast<size_t>(analyzedRuns), 0.0);
            for (int i = 0; i < analyzedRuns; ++i)
                synthetic[static_cast<size_t>(i)] =
                    fitted[static_cast<size_t>(i)] + permuted[static_cast<size_t>(i)];
            if (makeGrid(synthetic).second >= observedPair.second * (1.0 - 1e-9))
                ++exceedCount;
        }
        return (static_cast<double>(exceedCount) + 1.0)
            / (static_cast<double>(permutations) + 1.0);
    };

    QJsonArray interactionArray;
    for (size_t fa = 0; fa < campaign.factors.size(); ++fa)
        for (size_t fb = fa + 1; fb < campaign.factors.size(); ++fb) {
            const double pValue = interactionPValue(fa, fb);
            const bool testable = pValue >= 0.0;
            interactionArray.append(QJsonObject{
                {"factors", QJsonArray{
                    QString::fromStdString(campaign.factors[fa].name),
                    QString::fromStdString(campaign.factors[fb].name)}},
                {"perm_p", testable ? pValue : -1.0},
                {"testable", testable},
                {"method", QStringLiteral("freedman_lane")}});
        }

    // Pure-error estimate from replicated cells of the factorial grid.
    std::vector<long long> cellOfRun(static_cast<size_t>(analyzedRuns), 0);
    const long long cellTotal = totalDesigns;
    std::vector<double> cellSums(static_cast<size_t>(cellTotal), 0.0);
    std::vector<int> cellCounts(static_cast<size_t>(cellTotal), 0);
    for (int i = 0; i < analyzedRuns; ++i) {
        const int design =
            campaign.designIndices[static_cast<size_t>(runIndices[static_cast<size_t>(i)])];
        long long cell = 0;
        for (size_t f = 0; f < campaign.factors.size(); ++f)
            cell = cell * levelCounts[f] + assignments[f][static_cast<size_t>(i)];
        cellOfRun[static_cast<size_t>(i)] = cell;
        cellSums[static_cast<size_t>(cell)] += values[static_cast<size_t>(i)];
        ++cellCounts[static_cast<size_t>(cell)];
    }
    long long usedCells = 0;
    double residualSquareSum = 0.0;
    QJsonArray cellArray;
    std::vector<double> cellMeanValues(static_cast<size_t>(cellTotal), 0.0);
    int bestCellIndex = -1;
    int worstCellIndex = -1;
    for (long long cell = 0; cell < cellTotal; ++cell) {
        const int count = cellCounts[static_cast<size_t>(cell)];
        const double mean = count == 0 ? 0.0
            : cellSums[static_cast<size_t>(cell)] / static_cast<double>(count);
        cellMeanValues[static_cast<size_t>(cell)] = mean;
        if (count > 0) {
            ++usedCells;
            for (int i = 0; i < analyzedRuns; ++i) {
                if (cellOfRun[static_cast<size_t>(i)] != cell) continue;
                const double deviation = values[static_cast<size_t>(i)] - mean;
                residualSquareSum += deviation * deviation;
            }
            if (bestCellIndex < 0
                || campaignResponseUtility(campaign, mean)
                    > campaignResponseUtility(campaign,
                        cellMeanValues[static_cast<size_t>(bestCellIndex)]))
                bestCellIndex = static_cast<int>(cell);
            if (worstCellIndex < 0
                || campaignResponseUtility(campaign, mean)
                    < campaignResponseUtility(campaign,
                        cellMeanValues[static_cast<size_t>(worstCellIndex)]))
                worstCellIndex = static_cast<int>(cell);
        }
        QJsonObject levelObject;
        long long remaining = cell;
        for (size_t f = campaign.factors.size(); f > 0; --f) {
            const size_t factorIndex = f - 1;
            const int level = static_cast<int>(remaining % levelCounts[factorIndex]);
            remaining /= levelCounts[factorIndex];
            levelObject.insert(QString::fromStdString(
                campaign.factors[factorIndex].name),
                levelScalar(campaign.factors[factorIndex]
                    .levelsJson[static_cast<size_t>(level)]));
        }
        cellArray.append(QJsonObject{
            {"index", static_cast<double>(cell)},
            {"levels", levelObject},
            {"mean", mean},
            {"count", count}});
    }
    const long long errorDegreesOfFreedom = analyzedRuns - usedCells;
    const bool residualEstimable = usedCells >= 1 && errorDegreesOfFreedom >= 1;
    const double residualStd = residualEstimable
        ? std::sqrt(residualSquareSum / static_cast<double>(errorDegreesOfFreedom))
        : 0.0;

    const QJsonObject result{
        {"schema", "dpa.experiment.campaign.analysis/v1"},
        {"campaign_id", QString::fromStdString(campaign.campaignId)},
        {"campaign_sha256", QString::fromStdString(campaign.campaignSha256)},
        {"analyzed_runs", analyzedRuns},
        {"planned_runs", plannedRuns},
        {"coverage_ratio", static_cast<double>(analyzedRuns)
            / static_cast<double>(plannedRuns)},
        {"seed", root.value("seed").toDouble()},
        {"permutations", permutations},
        {"grand_mean", grandMean},
        {"residual_std", residualStd},
        {"residual_estimable", residualEstimable},
        {"response_direction", campaign.responseDirection.empty()
            ? QStringLiteral("legacy_maximize")
            : QString::fromStdString(campaign.responseDirection)},
        {"best_cell_index", bestCellIndex},
        {"worst_cell_index", worstCellIndex},
        {"factors", factorArray},
        {"interactions", interactionArray},
        {"cells", cellArray}};
    output.resultJson = QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();
    output.ok = true;
    return output;
}

EvidenceBoundObservations bindObservationsToEvidence(
    const std::string& campaignResultJson,
    const std::string& expectedCampaignSha256,
    const std::string& observationsJson,
    double minCoverage) {
    return bindObservationsToEvidenceImpl(
        campaignResultJson, expectedCampaignSha256, observationsJson, minCoverage);
}

EvidenceBoundObservations extractObservationsFromCampaignResult(
    const std::string& campaignResultJson,
    const std::string& expectedCampaignSha256,
    const std::string& expectedResponseMetric,
    double minCoverage,
    const std::string& expectedResponseUnit) {
    EvidenceBoundObservations failed;
    auto fail = [&failed](const char* error) {
        failed.error = error;
        return failed;
    };
    if (campaignResultJson.empty() || campaignResultJson.size() > kMaxRequestBytes
        || expectedResponseMetric.empty())
        return fail("response_extraction_input_invalid");
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(campaignResultJson));
    if (!document.isObject()) return fail("campaign_artifact_json_invalid");
    QJsonObject root = document.object();
    if (root.value("data").isObject()) root = root.value("data").toObject();
    const QString recordedDigest = root.take("payload_sha256").toString();
    const QString actualDigest = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(root).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
    if (recordedDigest.size() != 64 || recordedDigest != actualDigest)
        return fail("campaign_artifact_integrity_mismatch");
    if (!root.value("runs").isArray()) return fail("campaign_artifact_runs_invalid");

    QJsonArray observations;
    int channelId = -1;
    QString unit;
    for (const QJsonValue& value : root.value("runs").toArray()) {
        const QJsonObject run = value.toObject();
        if (!run.value("ok").toBool(false)) continue;
        const QJsonObject response = run.value("response").toObject();
        const QString digest = response.value("evidence_payload_sha256").toString();
        bool digestValid = digest.size() == 64;
        for (const QChar character : digest)
            digestValid = digestValid
                && ((character >= '0' && character <= '9')
                    || (character >= 'a' && character <= 'f'));
        if (response.size() != 7
            || response.value("metric").toString().toStdString() != expectedResponseMetric
            || !response.value("value").isDouble()
            || !qIsFinite(response.value("value").toDouble())
            || qAbs(response.value("value").toDouble()) > kMaxAbsValue
            || response.value("channel_id").toInt(-1) < 0
            || response.value("sample_count").toInt(0) < 1
            || response.value("unit").toString().isEmpty()
            || !digestValid)
            return fail("response_record_invalid");
        const int currentChannel = response.value("channel_id").toInt();
        const QString currentUnit = response.value("unit").toString();
        if (!expectedResponseUnit.empty()
            && currentUnit.toStdString() != expectedResponseUnit)
            return fail("response_unit_mismatch");
        if (channelId < 0) {
            channelId = currentChannel;
            unit = currentUnit;
        } else if (channelId != currentChannel || unit != currentUnit) {
            return fail("response_provenance_mismatch");
        }
        const QString source = response.value("source").toString();
        if ((expectedResponseMetric == "peak_freq"
             && (source != "analysis_dominant_peak" || currentUnit != "Hz"))
            || (expectedResponseMetric != "peak_freq"
                && source != "monitoring_window_mean")
            || (expectedResponseMetric == "crest_factor" && currentUnit != "1"))
            return fail("response_provenance_invalid");
        observations.append(QJsonObject{
            {"run_index", run.value("run_index")},
            {"value", response.value("value")}});
    }
    return bindObservationsToEvidence(
        campaignResultJson, expectedCampaignSha256,
        QJsonDocument(observations).toJson(QJsonDocument::Compact).toStdString(),
        minCoverage);
}

} // namespace Model
