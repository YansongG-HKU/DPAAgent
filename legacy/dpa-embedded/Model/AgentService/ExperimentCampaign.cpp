#include "ExperimentCampaign.h"

#include "AgentJson.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

#include <limits>
#include <cmath>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include <QVector>

namespace Model {
namespace {

constexpr int kMaxContractBytes = 64 * 1024;
constexpr int kMaxFactors = 6;
constexpr int kMinLevels = 2;
constexpr int kMaxLevels = 8;
constexpr int kMaxReplicates = 5;
constexpr int kMaxTotalRuns = 64;

const QStringList kOverridableKeys{
    "duration_ms", "analysis_type", "frame_size", "average_num", "overlap_index"};
const QStringList kAnalysisTypes{"FFT", "PSD", "DFT", "RMSPRO", "AVGPRO", "PEAKPRO"};
const QJsonArray kFrameSizes{128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
const QStringList kTemplateBools{"auto_connect", "save", "stop_on_warning",
                                 "standard_plan_required", "generate_report", "create_evidence"};

bool exactKeys(const QJsonObject& object, const QStringList& keys) {
    if (object.size() != keys.size()) return false;
    for (const QString& key : keys)
        if (!object.contains(key)) return false;
    return true;
}

bool boundedToken(const QJsonValue& value, int maximum) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > maximum) return false;
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
            || (code >= '0' && code <= '9') || code == '-' || code == '_') continue;
        return false;
    }
    return true;
}

bool boundedText(const QJsonValue& value, int minimum, int maximum) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    return text.size() >= minimum && text.size() <= maximum;
}

bool boundedUnit(const QJsonValue& value) {
    if (!boundedText(value, 1, 32)) return false;
    const QString text = value.toString();
    if (text.trimmed() != text) return false;
    for (const QChar character : text)
        if (character.isSpace() || character.unicode() < 0x20) return false;
    return true;
}

bool sha256Value(const QJsonValue& value) {
    if (!value.isString() || value.toString().size() != 64) return false;
    for (const QChar character : value.toString()) {
        const ushort code = character.unicode();
        if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f'))) return false;
    }
    return true;
}

bool integerValue(const QJsonValue& value, long long minimum, long long maximum) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (number != static_cast<double>(static_cast<long long>(number))) return false;
    const long long integer = static_cast<long long>(number);
    return integer >= minimum && integer <= maximum;
}

bool levelValueIsValid(const QString& key, const QJsonValue& level) {
    if (key == QStringLiteral("duration_ms")) return integerValue(level, 1000, 600000);
    if (key == QStringLiteral("frame_size")) {
        if (!integerValue(level, 128, 32768)) return false;
        for (const QJsonValue& frame : kFrameSizes)
            if (frame.toDouble() == level.toDouble()) return true;
        return false;
    }
    if (key == QStringLiteral("average_num")) return integerValue(level, 1, 99);
    if (key == QStringLiteral("overlap_index")) return integerValue(level, 0, 5);
    if (key == QStringLiteral("analysis_type"))
        return level.isString() && kAnalysisTypes.contains(level.toString());
    return false;
}

bool templateValueIsValid(const QString& key, const QJsonValue& value) {
    if (kTemplateBools.contains(key)) return value.isBool();
    if (key == QStringLiteral("diagnostic")) return value.isObject();
    return levelValueIsValid(key, value);
}

CampaignValidation fail(const std::string& error) {
    CampaignValidation result;
    result.error = error;
    return result;
}

int unbiasedDraw(std::mt19937_64& rng, int bound) {
    const unsigned long long bound64 = static_cast<unsigned long long>(bound);
    const unsigned long long limit =
        (std::numeric_limits<unsigned long long>::max() / bound64) * bound64;
    unsigned long long value = rng();
    while (value >= limit) value = rng();
    return static_cast<int>(value % bound64);
}

} // namespace

CampaignKnobKind campaignKnobKind(const std::string& planKey) {
    const QString key = QString::fromStdString(planKey);
    if (key == QStringLiteral("frame_size")) return CampaignKnobKind::OrderedEnum;
    if (key == QStringLiteral("analysis_type")) return CampaignKnobKind::Categorical;
    return CampaignKnobKind::LinearNumeric;
}

double campaignKnobMinimum(const std::string& planKey) {
    const QString key = QString::fromStdString(planKey);
    if (key == QStringLiteral("duration_ms")) return 1000.0;
    if (key == QStringLiteral("average_num")) return 1.0;
    if (key == QStringLiteral("overlap_index")) return 0.0;
    return 0.0;
}

double campaignKnobMaximum(const std::string& planKey) {
    const QString key = QString::fromStdString(planKey);
    if (key == QStringLiteral("duration_ms")) return 600000.0;
    if (key == QStringLiteral("average_num")) return 99.0;
    if (key == QStringLiteral("overlap_index")) return 5.0;
    return 0.0;
}

std::vector<double> campaignKnobLadder(const std::string& planKey) {
    std::vector<double> ladder;
    if (QString::fromStdString(planKey) != QStringLiteral("frame_size"))
        return ladder;
    for (const QJsonValue& frame : kFrameSizes)
        ladder.push_back(frame.toDouble());
    return ladder;
}

std::vector<std::string> campaignKnobCategoricalLevels(
    const std::string& planKey) {
    std::vector<std::string> levels;
    if (QString::fromStdString(planKey) != QStringLiteral("analysis_type"))
        return levels;
    for (const QString& analysis : kAnalysisTypes)
        levels.push_back(analysis.toStdString());
    return levels;
}

double campaignResponseUtility(const ExperimentCampaign& campaign, double value) {
    if (campaign.responseDirection == "minimize") return -value;
    if (campaign.responseDirection == "target" && campaign.responseTargetPresent)
        return -std::fabs(value - campaign.responseTarget);
    return value;
}

CampaignValidation parseAndValidateExperimentCampaign(const std::string& json) {
    if (json.empty() || json.size() > kMaxContractBytes)
        return fail("campaign_size_invalid");

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(json), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail("campaign_json_invalid");

    const QJsonObject root = document.object();
    static const QStringList kRootKeys{"schema", "version", "objective_sha256",
                                       "campaign_id", "hypothesis",
                                       "factors", "replicates", "randomization",
                                       "run_template", "policy", "response"};
    if (root.size() > kRootKeys.size()
        || !root.contains(QStringLiteral("schema"))
        || !root.contains(QStringLiteral("version"))
        || !root.contains(QStringLiteral("campaign_id"))
        || !root.contains(QStringLiteral("hypothesis"))
        || !root.contains(QStringLiteral("factors"))
        || !root.contains(QStringLiteral("replicates"))
        || !root.contains(QStringLiteral("randomization"))
        || !root.contains(QStringLiteral("run_template"))
        || !root.contains(QStringLiteral("policy")))
        return fail("campaign_schema_invalid");
    for (auto it = root.begin(); it != root.end(); ++it)
        if (!kRootKeys.contains(it.key())) return fail("campaign_schema_invalid");
    if (root.contains(QStringLiteral("objective_sha256"))
        && !sha256Value(root.value("objective_sha256")))
        return fail("objective_sha256_invalid");
    if (root.contains(QStringLiteral("response"))) {
        const QJsonObject response = root.value("response").toObject();
        static const QStringList kResponseKeys{
            "metric", "unit", "direction", "target", "tolerance"};
        static const QStringList kMetrics{"rms", "peak", "peak_freq", "crest_factor"};
        for (auto it = response.begin(); it != response.end(); ++it)
            if (!kResponseKeys.contains(it.key())) return fail("response_spec_invalid");
        if (!boundedToken(response.value("metric"), 32)
            || !kMetrics.contains(response.value("metric").toString())
            || (response.contains(QStringLiteral("unit"))
                && !boundedUnit(response.value("unit"))))
            return fail("response_spec_invalid");
        const QString metric = response.value("metric").toString();
        const QString unit = response.value("unit").toString();
        if ((!unit.isEmpty() && metric == QStringLiteral("peak_freq")
             && unit != QStringLiteral("Hz"))
            || (!unit.isEmpty() && metric == QStringLiteral("crest_factor")
                && unit != QStringLiteral("1")))
            return fail("response_unit_invalid");
        if (response.contains(QStringLiteral("direction"))) {
            const QString direction = response.value("direction").toString();
            if (direction != QStringLiteral("maximize")
                && direction != QStringLiteral("minimize")
                && direction != QStringLiteral("target"))
                return fail("response_direction_invalid");
            const bool target = direction == QStringLiteral("target");
            if (target != response.contains(QStringLiteral("target"))
                || target != response.contains(QStringLiteral("tolerance")))
                return fail("response_target_invalid");
            if (target && (!response.value("target").isDouble()
                           || !response.value("tolerance").isDouble()
                           || !std::isfinite(response.value("target").toDouble())
                           || !std::isfinite(response.value("tolerance").toDouble())
                           || std::fabs(response.value("target").toDouble()) > 1e12
                           || response.value("tolerance").toDouble() <= 0
                           || response.value("tolerance").toDouble() > 1e12))
                return fail("response_target_invalid");
        } else if (response.contains(QStringLiteral("target"))
                   || response.contains(QStringLiteral("tolerance"))) {
            return fail("response_direction_invalid");
        }
    }
    if (root.value("schema").toString() != QStringLiteral("dpa.experiment.campaign")
        || !root.value("version").isDouble() || root.value("version").toInt() != 1
        || !boundedToken(root.value("campaign_id"), 128)
        || !boundedText(root.value("hypothesis"), 8, 512)
        || !root.value("factors").isArray()
        || !integerValue(root.value("replicates"), 1, kMaxReplicates))
        return fail("campaign_schema_invalid");

    const QJsonArray factors = root.value("factors").toArray();
    if (factors.isEmpty() || factors.size() > kMaxFactors)
        return fail("factor_count_invalid");

    ExperimentCampaign campaign;
    campaign.objectiveSha256 =
        root.value("objective_sha256").toString().toStdString();
    campaign.campaignId = root.value("campaign_id").toString().toStdString();
    campaign.hypothesis = root.value("hypothesis").toString().toStdString();

    QVector<QVector<QJsonValue>> factorLevels;
    std::set<std::string> factorNames;
    long long totalDesigns = 1;
    for (int index = 0; index < factors.size(); ++index) {
        const QJsonValue entry = factors.at(index);
        if (!entry.isObject()) return fail("factor_schema_invalid");
        const QJsonObject factor = entry.toObject();
        if (!exactKeys(factor, {"name", "levels"})
            || !boundedToken(factor.value("name"), 64)
            || !factor.value("levels").isArray())
            return fail("factor_schema_invalid");
        const QString name = factor.value("name").toString();
        if (!kOverridableKeys.contains(name)) return fail("factor_key_not_overridable");
        if (!factorNames.insert(name.toStdString()).second)
            return fail("duplicate_factor_name");
        const QJsonArray levels = factor.value("levels").toArray();
        if (levels.size() < kMinLevels || levels.size() > kMaxLevels)
            return fail("level_count_invalid");
        const int kind = levels.first().type();
        QVector<QJsonValue> parsed;
        std::set<std::string> distinct;
        for (const QJsonValue& level : levels) {
            if (level.type() != kind || !levelValueIsValid(name, level))
                return fail("level_value_invalid");
            const QByteArray encoded = QJsonDocument(
                QJsonArray{level}).toJson(QJsonDocument::Compact);
            if (!distinct.insert(encoded.toStdString()).second)
                return fail("duplicate_level_value");
            parsed.append(level);
        }
        totalDesigns *= parsed.size();
        if (totalDesigns > kMaxTotalRuns) return fail("design_too_large");
        CampaignFactor stored;
        stored.name = name.toStdString();
        for (const QJsonValue& level : parsed)
            stored.levelsJson.push_back(
                QJsonDocument(QJsonArray{level}).toJson(QJsonDocument::Compact).toStdString());
        campaign.factors.push_back(std::move(stored));
        factorLevels.append(parsed);
    }

    if (root.contains(QStringLiteral("response"))) {
        const QJsonObject response = root.value("response").toObject();
        campaign.responseMetric =
            response.value("metric").toString().toStdString();
        campaign.responseUnit = response.value("unit").toString().toStdString();
        campaign.responseDirection =
            response.value("direction").toString().toStdString();
        campaign.responseTargetPresent =
            campaign.responseDirection == "target";
        if (campaign.responseTargetPresent) {
            campaign.responseTarget = response.value("target").toDouble();
            campaign.responseTolerance = response.value("tolerance").toDouble();
        }
    }

    const QJsonObject randomization = root.value("randomization").toObject();
    if (!exactKeys(randomization, {"enabled", "seed"})
        || !randomization.value("enabled").isBool()
        || !integerValue(randomization.value("seed"), 0, 9007199254740991LL))
        return fail("randomization_schema_invalid");
    campaign.randomize = randomization.value("enabled").toBool();
    campaign.seed = static_cast<unsigned long long>(
        randomization.value("seed").toDouble());

    const QJsonObject policy = root.value("policy").toObject();
    static const QStringList kPolicyKeys{"on_run_failure", "max_runs",
                                         "max_failed_runs", "interim_after"};
    if (policy.isEmpty() || policy.size() > 4
        || !policy.contains(QStringLiteral("on_run_failure"))
        || !policy.contains(QStringLiteral("max_runs")))
        return fail("policy_schema_invalid");
    for (auto it = policy.begin(); it != policy.end(); ++it)
        if (!kPolicyKeys.contains(it.key())) return fail("policy_schema_invalid");
    if (!policy.value("on_run_failure").isString()
        || !integerValue(policy.value("max_runs"), 1, kMaxTotalRuns))
        return fail("policy_schema_invalid");
    const QString onFailure = policy.value("on_run_failure").toString();
    if (onFailure != QStringLiteral("abort") && onFailure != QStringLiteral("continue"))
        return fail("policy_on_run_failure_invalid");
    campaign.onFailure = onFailure.toStdString();
    campaign.maxRuns = policy.value("max_runs").toInt();
    if (policy.contains(QStringLiteral("max_failed_runs"))) {
        // 0 means the cap is disabled, matching the canonical form emitted for
        // contracts that omit the key.
        if (!integerValue(policy.value("max_failed_runs"), 0, kMaxTotalRuns))
            return fail("policy_max_failed_runs_invalid");
        campaign.maxFailedRuns =
            policy.value("max_failed_runs").toInt();
    }
    if (policy.contains(QStringLiteral("interim_after"))) {
        if (!integerValue(policy.value("interim_after"), 0, kMaxTotalRuns))
            return fail("policy_interim_after_invalid");
        campaign.interimAfter =
            policy.value("interim_after").toInt();
    }

    const QJsonObject templateObject = root.value("run_template").toObject();
    if (templateObject.isEmpty()) return fail("run_template_invalid");
    QStringList allowedKeys{"duration_ms", "analysis_type", "frame_size", "average_num",
                            "overlap_index", "diagnostic"};
    allowedKeys += kTemplateBools;
    for (auto it = templateObject.begin(); it != templateObject.end(); ++it) {
        if (!allowedKeys.contains(it.key())) return fail("template_key_not_allowed");
        if (!templateValueIsValid(it.key(), it.value()))
            return fail("template_value_invalid");
    }
    if (templateObject.contains(QStringLiteral("create_evidence"))
        && !templateObject.value(QStringLiteral("create_evidence")).toBool(true))
        return fail("evidence_binding_required");
    auto coveredByFactor = [&](const char* key) {
        return factorNames.count(key) > 0
            || templateObject.contains(QLatin1String(key));
    };
    if (!coveredByFactor("duration_ms") || !coveredByFactor("analysis_type"))
        return fail("required_plan_key_uncovered");
    if (campaign.responseMetric == "peak_freq") {
        bool compatible = false;
        for (const CampaignFactor& factor : campaign.factors) {
            if (factor.name != "analysis_type") continue;
            compatible = std::all_of(
                factor.levelsJson.begin(), factor.levelsJson.end(),
                [](const std::string& levelJson) {
                    const QString value = QJsonDocument::fromJson(
                        QByteArray::fromStdString(levelJson)).array().first().toString();
                    return value == QStringLiteral("FFT")
                        || value == QStringLiteral("PSD")
                        || value == QStringLiteral("DFT");
                });
            break;
        }
        if (!compatible && !factorNames.count("analysis_type")) {
            const QString value = templateObject.value("analysis_type").toString();
            compatible = value == QStringLiteral("FFT")
                || value == QStringLiteral("PSD")
                || value == QStringLiteral("DFT");
        }
        if (!compatible) return fail("response_analysis_incompatible");
    }

    const int replicates = root.value("replicates").toInt();
    campaign.replicates = replicates;
    const long long totalRuns = totalDesigns * replicates;
    if (totalRuns > policy.value("max_runs").toDouble())
        return fail("max_runs_below_design");
    if (campaign.interimAfter > 0 && campaign.interimAfter >= totalRuns)
        return fail("policy_interim_after_invalid");

    // Canonical form and hash are computed from the validated fields only, so
    // the campaign identity is independent of input key order or formatting.
    // Factor[0] varies slowest: design d takes level
    // (d / stride[f]) % levelCount[f] with the stride of all later factors.
    std::vector<int> levelCounts;
    std::vector<long long> strides;
    long long suffix = totalDesigns;
    for (const CampaignFactor& factor : campaign.factors) {
        const int count = static_cast<int>(factor.levelsJson.size());
        levelCounts.push_back(count);
        suffix /= count;
        strides.push_back(suffix);
    }
    QJsonArray normalizedFactors;
    QVector<QJsonObject> designLevelObjects(static_cast<int>(totalDesigns));
    for (int index = 0; index < campaign.factors.size(); ++index) {
        const CampaignFactor& factor = campaign.factors.at(static_cast<size_t>(index));
        QJsonArray levels;
        for (int level = 0; level < static_cast<int>(factor.levelsJson.size()); ++level) {
            const QJsonValue value = QJsonDocument::fromJson(
                QByteArray::fromStdString(
                    factor.levelsJson[static_cast<size_t>(level)])).array().first();
            levels.append(value);
            for (int design = 0; design < static_cast<int>(totalDesigns); ++design)
                if ((design / strides[static_cast<size_t>(index)])
                        % levelCounts[static_cast<size_t>(index)] == level)
                    designLevelObjects[design].insert(
                        QString::fromStdString(factor.name), value);
        }
        normalizedFactors.append(QJsonObject{
            {"name", QString::fromStdString(factor.name)}, {"levels", levels}});
    }
    QJsonObject canonical{
        {"schema", "dpa.experiment.campaign"},
        {"version", 1},
        {"campaign_id", QString::fromStdString(campaign.campaignId)},
        {"hypothesis", QString::fromStdString(campaign.hypothesis)},
        {"factors", normalizedFactors},
        {"replicates", replicates},
        {"randomization", QJsonObject{{"enabled", campaign.randomize},
                                       {"seed", randomization.value("seed").toDouble()}}},
        {"run_template", templateObject},
        {"policy", QJsonObject{{"on_run_failure", onFailure},
                                {"max_runs", policy.value("max_runs").toDouble()},
                                {"max_failed_runs", static_cast<double>(campaign.maxFailedRuns)},
                                {"interim_after", static_cast<double>(campaign.interimAfter)}}}
    };
    if (!campaign.objectiveSha256.empty())
        canonical.insert(QStringLiteral("objective_sha256"),
                         QString::fromStdString(campaign.objectiveSha256));
    if (!campaign.responseMetric.empty()) {
        QJsonObject response{{"metric",
                              QString::fromStdString(campaign.responseMetric)}};
        if (!campaign.responseUnit.empty())
            response.insert(QStringLiteral("unit"),
                            QString::fromStdString(campaign.responseUnit));
        if (!campaign.responseDirection.empty())
            response.insert(QStringLiteral("direction"),
                            QString::fromStdString(campaign.responseDirection));
        if (campaign.responseTargetPresent) {
            response.insert(QStringLiteral("target"), campaign.responseTarget);
            response.insert(QStringLiteral("tolerance"), campaign.responseTolerance);
        }
        canonical.insert(QStringLiteral("response"), response);
    }
    const QByteArray canonicalBytes = QJsonDocument(canonical).toJson(QJsonDocument::Compact);
    campaign.canonicalJson = canonicalBytes.toStdString();
    campaign.campaignSha256 = QCryptographicHash::hash(canonicalBytes, QCryptographicHash::Sha256)
        .toHex().toStdString();
    campaign.templateJson = QJsonDocument(templateObject).toJson(QJsonDocument::Compact).toStdString();

    for (int design = 0; design < static_cast<int>(totalDesigns); ++design)
        campaign.designLevelsJson.push_back(
            QJsonDocument(designLevelObjects[design]).toJson(QJsonDocument::Compact).toStdString());

    // Replicate-major blocks: each replicate is a contiguous block over the
    // full factorial set, so a blocked analysis remains possible after the fact.
    std::vector<int> order;
    order.reserve(static_cast<size_t>(totalRuns));
    for (int replicate = 0; replicate < replicates; ++replicate)
        for (int design = 0; design < static_cast<int>(totalDesigns); ++design)
            order.push_back(design);
    if (campaign.randomize) {
        std::mt19937_64 rng(campaign.seed);
        for (size_t i = order.size() - 1; i > 0; --i) {
            const size_t j = static_cast<size_t>(unbiasedDraw(rng, static_cast<int>(i + 1)));
            std::swap(order[i], order[j]);
        }
    }
    for (const int design : order) {
        QJsonObject plan = templateObject;
        const QJsonObject levels = designLevelObjects[design];
        for (auto it = levels.begin(); it != levels.end(); ++it)
            plan.insert(it.key(), it.value());
        campaign.runPlanJsons.push_back(
            QJsonDocument(plan).toJson(QJsonDocument::Compact).toStdString());
        campaign.designIndices.push_back(design);
    }

    CampaignValidation result;
    result.ok = true;
    result.campaign = std::move(campaign);
    return result;
}

ExperimentObjectiveValidation parseAndValidateExperimentObjective(
    const std::string& json,
    const std::string& expectedContextFingerprint,
    const std::string& expectedToolCatalogSha256) {
    const auto reject = [](const std::string& error) {
        ExperimentObjectiveValidation result;
        result.error = error;
        return result;
    };
    if (json.empty() || json.size() > kMaxContractBytes)
        return reject("objective_size_invalid");

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(json), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return reject("objective_json_invalid");
    const QJsonObject root = document.object();
    if (!exactKeys(root, {"schema", "version", "objective_id",
                          "context_fingerprint", "tool_catalog_sha256",
                          "hypothesis", "response", "constraints",
                          "allowed_factors", "budget", "stop_conditions",
                          "evidence_requirements"}))
        return reject("objective_schema_invalid");
    if (root.value("schema").toString() != QStringLiteral("dpa.experiment.objective")
        || !integerValue(root.value("version"), 1, 1)
        || !boundedToken(root.value("objective_id"), 96)
        || !sha256Value(root.value("context_fingerprint"))
        || !sha256Value(root.value("tool_catalog_sha256"))
        || !boundedText(root.value("hypothesis"), 8, 512)
        || !root.value("response").isObject()
        || !root.value("constraints").isObject()
        || !root.value("allowed_factors").isArray()
        || !root.value("budget").isObject()
        || !root.value("stop_conditions").isObject()
        || !root.value("evidence_requirements").isObject())
        return reject("objective_schema_invalid");
    const std::string context =
        root.value("context_fingerprint").toString().toStdString();
    const std::string catalog =
        root.value("tool_catalog_sha256").toString().toStdString();
    if (!expectedContextFingerprint.empty() && context != expectedContextFingerprint)
        return reject("objective_context_mismatch");
    if (!expectedToolCatalogSha256.empty() && catalog != expectedToolCatalogSha256)
        return reject("objective_tool_catalog_mismatch");

    const QJsonObject response = root.value("response").toObject();
    const QString direction = response.value("direction").toString();
    const bool targeted = direction == QStringLiteral("target");
    if (!(targeted
              ? exactKeys(response, {"metric", "unit", "direction", "target",
                                     "tolerance"})
              : exactKeys(response, {"metric", "unit", "direction"})))
        return reject("objective_response_invalid");

    const QJsonObject constraints = root.value("constraints").toObject();
    if (!exactKeys(constraints, {"fixed_plan", "randomize", "min_replicates"})
        || !constraints.value("fixed_plan").isObject()
        || !constraints.value("randomize").isBool()
        || !integerValue(constraints.value("min_replicates"), 1,
                         kMaxReplicates))
        return reject("objective_constraints_invalid");
    const QJsonObject fixedPlan = constraints.value("fixed_plan").toObject();
    static const QStringList kObjectiveFixedKeys{
        "duration_ms", "analysis_type", "frame_size", "average_num",
        "overlap_index", "auto_connect", "standard_plan_required",
        "generate_report"};
    for (auto it = fixedPlan.begin(); it != fixedPlan.end(); ++it)
        if (!kObjectiveFixedKeys.contains(it.key())
            || !templateValueIsValid(it.key(), it.value()))
            return reject("objective_fixed_plan_invalid");

    const QJsonObject budget = root.value("budget").toObject();
    if (!exactKeys(budget, {"max_runs", "max_run_duration_ms",
                            "max_total_duration_ms"})
        || !integerValue(budget.value("max_runs"), 2, kMaxTotalRuns)
        || !integerValue(budget.value("max_run_duration_ms"), 1000, 600000)
        || !integerValue(budget.value("max_total_duration_ms"), 1000,
                         600000LL * kMaxTotalRuns))
        return reject("objective_budget_invalid");
    const int maxRuns = budget.value("max_runs").toInt();

    const QJsonObject stops = root.value("stop_conditions").toObject();
    if (!exactKeys(stops, {"stop_on_warning", "on_run_failure",
                           "max_failed_runs", "interim_after"})
        || !stops.value("stop_on_warning").isBool()
        || !stops.value("stop_on_warning").toBool()
        || !stops.value("on_run_failure").isString()
        || !integerValue(stops.value("max_failed_runs"), 0, maxRuns)
        || !integerValue(stops.value("interim_after"), 0, maxRuns - 1))
        return reject("objective_stop_conditions_invalid");
    const QString onFailure = stops.value("on_run_failure").toString();
    const int maxFailedRuns = stops.value("max_failed_runs").toInt();
    if ((onFailure == QStringLiteral("abort") && maxFailedRuns != 0)
        || (onFailure == QStringLiteral("continue") && maxFailedRuns < 1)
        || (onFailure != QStringLiteral("abort")
            && onFailure != QStringLiteral("continue")))
        return reject("objective_stop_conditions_invalid");

    const QJsonObject evidence = root.value("evidence_requirements").toObject();
    static const QStringList kEvidenceKeys{
        "save_raw_data", "create_per_run_evidence", "verify_current_catalog",
        "auto_analyze", "auto_refine"};
    if (!exactKeys(evidence, kEvidenceKeys))
        return reject("objective_evidence_requirements_invalid");
    for (const QString& key : kEvidenceKeys)
        if (!evidence.value(key).isBool() || !evidence.value(key).toBool())
            return reject("objective_evidence_requirements_invalid");

    const QJsonArray factors = root.value("allowed_factors").toArray();
    for (const QJsonValue& value : factors) {
        if (!value.isObject()) continue;
        const QString name = value.toObject().value("name").toString();
        if (!name.isEmpty() && fixedPlan.contains(name))
            return reject("objective_factor_constraint_conflict");
    }

    QJsonObject runTemplate = fixedPlan;
    runTemplate.insert(QStringLiteral("save"), true);
    runTemplate.insert(QStringLiteral("stop_on_warning"), true);
    runTemplate.insert(QStringLiteral("create_evidence"), true);
    const QJsonObject probe{
        {"schema", "dpa.experiment.campaign"},
        {"version", 1},
        {"campaign_id", root.value("objective_id")},
        {"hypothesis", root.value("hypothesis")},
        {"factors", factors},
        {"replicates", constraints.value("min_replicates")},
        {"randomization", QJsonObject{{"enabled", constraints.value("randomize")},
                                       {"seed", 0}}},
        {"run_template", runTemplate},
        {"policy", QJsonObject{{"on_run_failure", onFailure},
                                {"max_runs", maxRuns},
                                {"max_failed_runs", maxFailedRuns},
                                {"interim_after", stops.value("interim_after")}}},
        {"response", response}};
    const CampaignValidation campaignGate = parseAndValidateExperimentCampaign(
        QJsonDocument(probe).toJson(QJsonDocument::Compact).toStdString());
    if (!campaignGate.ok)
        return reject("objective_design_invalid:" + campaignGate.error);

    long long minimumDurationMs = 0;
    const int maxRunDurationMs = budget.value("max_run_duration_ms").toInt();
    for (const std::string& planJson : campaignGate.campaign.runPlanJsons) {
        const int duration = AgentJson::parseObject(planJson)
                                 .value("duration_ms").toInt();
        if (duration > maxRunDurationMs)
            return reject("objective_run_duration_exceeds_budget");
        minimumDurationMs += duration;
    }
    const long long maxTotalDurationMs =
        static_cast<long long>(budget.value("max_total_duration_ms").toDouble());
    if (minimumDurationMs > maxTotalDurationMs)
        return reject("objective_total_duration_exceeds_budget");

    ExperimentObjective objective;
    objective.objectiveId = root.value("objective_id").toString().toStdString();
    objective.contextFingerprint = context;
    objective.toolCatalogSha256 = catalog;
    objective.hypothesis = root.value("hypothesis").toString().toStdString();
    objective.minimumCampaign = campaignGate.campaign;
    objective.fixedPlanJson =
        QJsonDocument(fixedPlan).toJson(QJsonDocument::Compact).toStdString();
    objective.maxRuns = maxRuns;
    objective.maxRunDurationMs = maxRunDurationMs;
    objective.maxTotalDurationMs = maxTotalDurationMs;
    objective.randomize = constraints.value("randomize").toBool();
    objective.minReplicates = constraints.value("min_replicates").toInt();

    const QJsonObject canonicalCampaign =
        AgentJson::parseObject(campaignGate.campaign.canonicalJson);
    const QJsonObject normalized{
        {"schema", "dpa.experiment.objective"},
        {"version", 1},
        {"objective_id", QString::fromStdString(objective.objectiveId)},
        {"context_fingerprint", QString::fromStdString(context)},
        {"tool_catalog_sha256", QString::fromStdString(catalog)},
        {"hypothesis", QString::fromStdString(objective.hypothesis)},
        {"response", canonicalCampaign.value("response")},
        {"constraints", QJsonObject{{"fixed_plan", fixedPlan},
                                     {"randomize", objective.randomize},
                                     {"min_replicates", objective.minReplicates}}},
        {"allowed_factors", canonicalCampaign.value("factors")},
        {"budget", QJsonObject{{"max_runs", objective.maxRuns},
                                {"max_run_duration_ms", objective.maxRunDurationMs},
                                {"max_total_duration_ms",
                                 static_cast<double>(objective.maxTotalDurationMs)}}},
        {"stop_conditions", QJsonObject{{"stop_on_warning", true},
                                         {"on_run_failure", onFailure},
                                         {"max_failed_runs", maxFailedRuns},
                                         {"interim_after", stops.value("interim_after")}}},
        {"evidence_requirements", evidence}};
    const QByteArray canonical =
        QJsonDocument(normalized).toJson(QJsonDocument::Compact);
    objective.canonicalJson = canonical.toStdString();
    objective.objectiveSha256 = QCryptographicHash::hash(
        canonical, QCryptographicHash::Sha256).toHex().toStdString();

    ExperimentObjectiveValidation result;
    result.ok = true;
    result.objective = std::move(objective);
    return result;
}

ObjectiveCampaignValidation validateCampaignAgainstObjective(
    const ExperimentObjective& objective,
    const ExperimentCampaign& campaign) {
    const auto reject = [](const std::string& error) {
        ObjectiveCampaignValidation result;
        result.error = error;
        return result;
    };
    if (campaign.objectiveSha256 != objective.objectiveSha256)
        return reject("campaign_objective_mismatch");
    const ExperimentCampaign& allowed = objective.minimumCampaign;
    if (campaign.responseMetric != allowed.responseMetric
        || campaign.responseUnit != allowed.responseUnit
        || campaign.responseDirection != allowed.responseDirection
        || campaign.responseTargetPresent != allowed.responseTargetPresent
        || (campaign.responseTargetPresent
            && (campaign.responseTarget != allowed.responseTarget
                || campaign.responseTolerance != allowed.responseTolerance)))
        return reject("campaign_response_mismatch");
    if (campaign.randomize != objective.randomize
        || campaign.replicates < objective.minReplicates)
        return reject("campaign_design_constraints_mismatch");
    if (campaign.onFailure != allowed.onFailure
        || campaign.maxFailedRuns != allowed.maxFailedRuns
        || campaign.interimAfter != allowed.interimAfter)
        return reject("campaign_stop_conditions_mismatch");

    const auto allowedFactor = [&](const std::string& name)
            -> const CampaignFactor* {
        for (const CampaignFactor& factor : allowed.factors)
            if (factor.name == name) return &factor;
        return nullptr;
    };
    for (const CampaignFactor& factor : campaign.factors) {
        const CampaignFactor* permitted = allowedFactor(factor.name);
        if (!permitted) return reject("campaign_factor_not_allowed");
        for (const std::string& level : factor.levelsJson)
            if (std::find(permitted->levelsJson.begin(),
                          permitted->levelsJson.end(), level)
                == permitted->levelsJson.end())
                return reject("campaign_factor_level_not_allowed");
    }

    const QJsonObject fixed = AgentJson::parseObject(objective.fixedPlanJson);
    const QJsonObject templateObject = AgentJson::parseObject(campaign.templateJson);
    for (auto it = fixed.begin(); it != fixed.end(); ++it)
        if (!templateObject.contains(it.key())
            || templateObject.value(it.key()) != it.value())
            return reject("campaign_fixed_plan_mismatch");
    if (!templateObject.value("stop_on_warning").toBool(false)
        || !templateObject.value("save").toBool(true)
        || !templateObject.value("create_evidence").toBool(true))
        return reject("campaign_evidence_or_stop_policy_weakened");
    for (auto it = templateObject.begin(); it != templateObject.end(); ++it) {
        if (fixed.contains(it.key())
            || it.key() == QStringLiteral("save")
            || it.key() == QStringLiteral("stop_on_warning")
            || it.key() == QStringLiteral("create_evidence"))
            continue;
        const std::string name = it.key().toStdString();
        const CampaignFactor* permitted = allowedFactor(name);
        if (!permitted) return reject("campaign_template_key_not_allowed");
        const bool varied = std::any_of(
            campaign.factors.begin(), campaign.factors.end(),
            [&](const CampaignFactor& factor) { return factor.name == name; });
        if (varied) return reject("campaign_factor_template_conflict");
        const std::string encoded = QJsonDocument(QJsonArray{it.value()})
                                        .toJson(QJsonDocument::Compact)
                                        .toStdString();
        if (std::find(permitted->levelsJson.begin(), permitted->levelsJson.end(),
                      encoded) == permitted->levelsJson.end())
            return reject("campaign_template_level_not_allowed");
    }

    if (campaign.maxRuns > objective.maxRuns
        || campaign.runPlanJsons.size() > static_cast<size_t>(objective.maxRuns))
        return reject("campaign_run_budget_exceeded");
    long long totalDurationMs = 0;
    for (const std::string& planJson : campaign.runPlanJsons) {
        const int duration = AgentJson::parseObject(planJson)
                                 .value("duration_ms").toInt();
        if (duration > objective.maxRunDurationMs)
            return reject("campaign_run_duration_exceeded");
        totalDurationMs += duration;
        if (totalDurationMs > objective.maxTotalDurationMs)
            return reject("campaign_total_duration_exceeded");
    }

    ObjectiveCampaignValidation result;
    result.ok = true;
    result.totalDurationMs = totalDurationMs;
    return result;
}

std::string serializeCampaignState(const CampaignState& state) {
    QJsonArray records;
    for (const CampaignRunRecord& record : state.records) {
        QJsonObject item{
            {"run_index", record.runIndex},
            {"design_index", record.designIndex},
            {"ok", record.ok},
            {"error_code", QString::fromStdString(record.errorCode)},
            {"run_id", QString::fromStdString(record.runId)},
            {"evidence_artifact_id", QString::fromStdString(record.evidenceArtifactId)}};
        item["response"] = record.responsePresent ? QJsonValue(QJsonObject{
            {"value", record.responseValue},
            {"channel_id", record.responseChannelId},
            {"sample_count", record.responseSampleCount},
            {"unit", QString::fromStdString(record.responseUnit)},
            {"source", QString::fromStdString(record.responseSource)},
            {"evidence_payload_sha256",
             QString::fromStdString(record.responseEvidenceSha256)}})
            : QJsonValue(QJsonValue::Null);
        records.append(item);
    }
    const QJsonObject object{
        {"schema", "dpa.experiment.campaign.state"},
        {"campaign_sha256", QString::fromStdString(state.campaignSha256)},
        {"status", QString::fromStdString(state.status)},
        {"stop_reason", QString::fromStdString(state.stopReason)},
        {"next_run_index", state.nextRunIndex},
        {"interim_taken", state.interimTaken},
        {"records", records}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

CampaignStateValidation parseCampaignState(const std::string& json) {
    CampaignStateValidation result;
    const auto failState = [&result](const char* error) {
        result.error = error;
        return result;
    };
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(json), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = "state_json_invalid";
        return result;
    }
    const QJsonObject object = document.object();
    const QStringList requiredKeys{"schema", "campaign_sha256", "status",
                                   "stop_reason", "next_run_index",
                                   "interim_taken", "records"};
    if (object.size() != requiredKeys.size())
        return failState("state_schema_invalid");
    for (const QString& key : requiredKeys)
        if (!object.contains(key)) return failState("state_schema_invalid");
    if (object.value("schema").toString() != QStringLiteral("dpa.experiment.campaign.state")
        || !sha256Value(object.value("campaign_sha256")))
        return failState("state_schema_invalid");
    const QString status = object.value("status").toString();
    if (status != QStringLiteral("running") && status != QStringLiteral("completed")
        && status != QStringLiteral("aborted")
        && status != QStringLiteral("paused_interim"))
        return failState("state_status_invalid");
    CampaignState state;
    state.campaignSha256 = object.value("campaign_sha256").toString().toStdString();
    state.status = status.toStdString();
    state.stopReason = object.value("stop_reason").toString().toStdString();
    state.interimTaken = object.value("interim_taken").toBool(false);
    if (!integerValue(object.value("next_run_index"), 0, kMaxTotalRuns))
        return failState("state_cursor_invalid");
    state.nextRunIndex = static_cast<int>(object.value("next_run_index").toDouble());
    const QJsonArray records = object.value("records").toArray();
    if (records.size() != state.nextRunIndex)
        return failState("state_cursor_mismatch");
    for (int index = 0; index < records.size(); ++index) {
        const QJsonValue entry = records.at(index);
        if (!entry.isObject()) {
            result.error = "state_record_invalid";
            return result;
        }
        const QJsonObject record = entry.toObject();
        const bool currentRecord = exactKeys(record, {"run_index", "design_index", "ok",
            "error_code", "run_id", "evidence_artifact_id", "response"});
        const bool legacyRecord = exactKeys(record, {"run_index", "design_index", "ok",
            "error_code", "run_id", "evidence_artifact_id"});
        if ((!currentRecord && !legacyRecord)
            || !integerValue(record.value("run_index"), 0, kMaxTotalRuns)
            || !integerValue(record.value("design_index"), -1, kMaxTotalRuns)
            || !record.value("ok").isBool()) {
            result.error = "state_record_invalid";
            return result;
        }
        if (record.value("run_index").toInt() != index) {
            result.error = "state_record_order_invalid";
            return result;
        }
        CampaignRunRecord parsed;
        parsed.runIndex = record.value("run_index").toInt();
        parsed.designIndex = record.value("design_index").toInt();
        parsed.ok = record.value("ok").toBool();
        parsed.errorCode = record.value("error_code").toString().toStdString();
        parsed.runId = record.value("run_id").toString().toStdString();
        parsed.evidenceArtifactId = record.value("evidence_artifact_id").toString().toStdString();
        if (currentRecord && record.value("response").isObject()) {
            const QJsonObject response = record.value("response").toObject();
            if (!exactKeys(response, {"value", "channel_id", "sample_count", "unit",
                                      "source", "evidence_payload_sha256"})
                || !response.value("value").isDouble()
                || !std::isfinite(response.value("value").toDouble())
                || !integerValue(response.value("channel_id"), 0, 1000000)
                || !integerValue(response.value("sample_count"), 1,
                                 std::numeric_limits<int>::max())
                || !boundedText(response.value("unit"), 1, 64)
                || !boundedToken(response.value("source"), 64)
                || !sha256Value(response.value("evidence_payload_sha256")))
                return failState("state_record_response_invalid");
            parsed.responsePresent = true;
            parsed.responseValue = response.value("value").toDouble();
            parsed.responseChannelId = response.value("channel_id").toInt();
            parsed.responseSampleCount = response.value("sample_count").toInt();
            parsed.responseUnit = response.value("unit").toString().toStdString();
            parsed.responseSource = response.value("source").toString().toStdString();
            parsed.responseEvidenceSha256 =
                response.value("evidence_payload_sha256").toString().toStdString();
        } else if (currentRecord && !record.value("response").isNull()) {
            return failState("state_record_response_invalid");
        }
        if (parsed.ok && parsed.evidenceArtifactId.empty()) {
            result.error = "state_record_evidence_missing";
            return result;
        }
        state.records.push_back(std::move(parsed));
    }
    result.ok = true;
    result.state = std::move(state);
    return result;
}

CampaignExecution executeExperimentCampaign(
    const ExperimentCampaign& campaign,
    const std::string& resumeStateJson,
    std::function<CampaignRunOutcome(int runIndex, int designIndex,
                                     const std::string& planJson)> executor,
    std::function<bool()> cancelProbe) {
    CampaignExecution execution;
    if (!executor || !cancelProbe) {
        execution.error = "executor_unavailable";
        return execution;
    }
    CampaignState state;
    state.campaignSha256 = campaign.campaignSha256;
    if (!resumeStateJson.empty()) {
        const CampaignStateValidation resumed = parseCampaignState(resumeStateJson);
        if (!resumed.ok) {
            execution.error = resumed.error;
            return execution;
        }
        state = resumed.state;
        if (state.campaignSha256 != campaign.campaignSha256) {
            execution.error = "state_campaign_mismatch";
            return execution;
        }
        if (!campaign.responseMetric.empty()) {
            for (const CampaignRunRecord& record : state.records) {
                if (record.ok && !record.responsePresent) {
                    execution.error = "state_response_missing";
                    return execution;
                }
            }
        }
        // Only a completed campaign is terminal. Aborted or paused campaigns
        // are exactly what checkpoint recovery exists to continue.
        if (state.status == "completed") {
            execution.ok = true;
            execution.state = state;
            return execution;
        }
        state.status = "running";
    }

    const int totalRuns = static_cast<int>(campaign.runPlanJsons.size());
    int failedCount = 0;
    for (const CampaignRunRecord& record : state.records)
        if (!record.ok) ++failedCount;
    bool stoppedEarly = false;
    while (state.nextRunIndex < totalRuns) {
        if (cancelProbe()) {
            state.status = "aborted";
            state.stopReason = "cancelled";
            stoppedEarly = true;
            break;
        }
        const int runIndex = state.nextRunIndex;
        const int designIndex = campaign.designIndices[static_cast<size_t>(runIndex)];
        CampaignRunOutcome outcome = executor(
            runIndex, designIndex, campaign.runPlanJsons[static_cast<size_t>(runIndex)]);
        CampaignRunRecord record;
        record.runIndex = runIndex;
        record.designIndex = designIndex;
        record.ok = outcome.ok;
        record.errorCode = outcome.errorCode;
        record.runId = outcome.runId;
        record.evidenceArtifactId = outcome.evidenceArtifactId;
        record.responsePresent = outcome.responsePresent;
        record.responseValue = outcome.responseValue;
        record.responseChannelId = outcome.responseChannelId;
        record.responseSampleCount = outcome.responseSampleCount;
        record.responseUnit = outcome.responseUnit;
        record.responseSource = outcome.responseSource;
        record.responseEvidenceSha256 = outcome.responseEvidenceSha256;
        if (record.ok && record.runId.empty()) {
            record.ok = false;
            record.errorCode = "run_id_missing";
        } else if (record.ok && record.evidenceArtifactId.empty()) {
            record.ok = false;
            record.errorCode = "evidence_missing";
        } else if (record.ok && !campaign.responseMetric.empty()
                   && (!record.responsePresent
                       || !std::isfinite(record.responseValue)
                       || record.responseChannelId < 0
                       || record.responseSampleCount < 1
                       || record.responseUnit.empty()
                       || record.responseSource.empty()
                       || record.responseEvidenceSha256.size() != 64)) {
            record.ok = false;
            record.errorCode = "response_missing";
        }
        state.records.push_back(record);
        ++state.nextRunIndex;
        if (!record.ok) {
            ++failedCount;
            if (record.errorCode == "cancelled" || record.errorCode == "run_id_missing"
                || record.errorCode == "evidence_missing" || record.errorCode == "response_missing"
                || campaign.onFailure == "abort") {
                state.status = "aborted";
                state.stopReason = record.errorCode;
                stoppedEarly = true;
                break;
            }
            if (campaign.maxFailedRuns > 0 && failedCount >= campaign.maxFailedRuns) {
                state.status = "aborted";
                state.stopReason = "max_failed_runs_reached";
                stoppedEarly = true;
                break;
            }
        }
        // Planned operational oversight gate: pause once for a human
        // continue/abort decision. This is monitoring, not an interim
        // statistical test, so no alpha correction is implied.
        if (!state.interimTaken && campaign.interimAfter > 0
            && state.nextRunIndex >= campaign.interimAfter) {
            state.interimTaken = true;
            state.status = "paused_interim";
            state.stopReason = "interim_checkpoint";
            execution.awaitingInterim = true;
            stoppedEarly = true;
            break;
        }
    }
    if (!stoppedEarly && state.nextRunIndex >= totalRuns) state.status = "completed";

    execution.ok = state.status == "completed";
    execution.state = std::move(state);
    return execution;
}

} // namespace Model
