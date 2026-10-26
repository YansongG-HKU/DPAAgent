#include "AnalysisPlan.h"

#include "AgentJson.h"
#include "ToolRegistry.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <set>

namespace Model {
namespace {

constexpr int kMaxPlanBytes = 256 * 1024;
constexpr int kMaxSteps = 32;
constexpr int kMaxStepBytes = 64 * 1024;

bool exactKeys(const QJsonObject& object, const QStringList& keys) {
    if (object.size() != keys.size()) return false;
    for (const QString& key : keys)
        if (!object.contains(key)) return false;
    return true;
}

bool boundedToken(const QJsonValue& value, int maximum, bool allowDot = false) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    if (text.isEmpty() || text.size() > maximum) return false;
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if ((code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z')
            || (code >= '0' && code <= '9') || code == '-' || code == '_'
            || (allowDot && code == '.')) continue;
        return false;
    }
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

AnalysisPlanValidation fail(const std::string& error, int stepIndex = -1) {
    AnalysisPlanValidation result;
    result.error = error;
    result.stepIndex = stepIndex;
    return result;
}

} // namespace

AnalysisPlanValidation parseAndValidateAnalysisPlan(
    const std::string& json,
    const ToolRegistry& registry,
    const std::string& expectedContextFingerprint,
    bool allowWrites) {
    if (json.empty() || json.size() > kMaxPlanBytes)
        return fail("plan_size_invalid");

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromStdString(json), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail("plan_json_invalid");

    const QJsonObject root = document.object();
    if (!exactKeys(root, {"schema", "version", "plan_id", "context_fingerprint",
                          "tool_catalog_sha256", "steps"}))
        return fail("plan_schema_invalid");
    if (root.value("schema").toString() != QStringLiteral("dpa.analysis.plan")
        || !root.value("version").isDouble() || root.value("version").toInt() != 1
        || !boundedToken(root.value("plan_id"), 128)
        || !sha256Value(root.value("context_fingerprint"))
        || !sha256Value(root.value("tool_catalog_sha256"))
        || !root.value("steps").isArray())
        return fail("plan_schema_invalid");

    const std::string contextFingerprint =
        root.value("context_fingerprint").toString().toStdString();
    if (!expectedContextFingerprint.empty() && contextFingerprint != expectedContextFingerprint)
        return fail("context_fingerprint_mismatch");
    const std::string catalogSha256 =
        root.value("tool_catalog_sha256").toString().toStdString();
    if (catalogSha256 != registry.approvalCatalogSha256())
        return fail("tool_catalog_changed");

    const QJsonArray steps = root.value("steps").toArray();
    if (steps.isEmpty() || steps.size() > kMaxSteps)
        return fail("step_count_invalid");

    AnalysisPlan plan;
    plan.planId = root.value("plan_id").toString().toStdString();
    plan.contextFingerprint = contextFingerprint;
    plan.toolCatalogSha256 = catalogSha256;
    std::set<std::string> ids;
    for (int index = 0; index < steps.size(); ++index) {
        const QJsonValue value = steps.at(index);
        if (!value.isObject()) return fail("step_schema_invalid", index);
        const QJsonObject step = value.toObject();
        if (!exactKeys(step, {"id", "tool", "args", "depends_on", "requires_confirmation"})
            || !boundedToken(step.value("id"), 64)
            || !boundedToken(step.value("tool"), 128, true)
            || !step.value("args").isObject()
            || !step.value("depends_on").isArray()
            || !step.value("requires_confirmation").isBool())
            return fail("step_schema_invalid", index);

        const std::string id = step.value("id").toString().toStdString();
        if (!ids.insert(id).second) return fail("duplicate_step_id", index);
        const std::string tool = step.value("tool").toString().toStdString();
        if (tool == "execute_analysis_plan") return fail("nested_plan_not_allowed", index);
        if (!registry.has(tool)) return fail("unknown_tool", index);
        const bool write = registry.isWrite(tool);
        if (write && (!allowWrites || !step.value("requires_confirmation").toBool()))
            return fail("write_step_requires_confirmation", index);
        if (!write && step.value("requires_confirmation").toBool())
            return fail("read_step_confirmation_invalid", index);

        const QByteArray args = QJsonDocument(step.value("args").toObject())
            .toJson(QJsonDocument::Compact);
        if (args.size() > kMaxStepBytes) return fail("step_args_too_large", index);

        AnalysisPlanStep parsed;
        parsed.id = id;
        parsed.tool = tool;
        parsed.argsJson = args.toStdString();
        parsed.requiresConfirmation = step.value("requires_confirmation").toBool();
        const QJsonArray dependencies = step.value("depends_on").toArray();
        if (dependencies.size() > kMaxSteps) return fail("dependency_count_invalid", index);
        std::set<std::string> dependencyIds;
        for (const QJsonValue& dependency : dependencies) {
            if (!boundedToken(dependency, 64)) return fail("dependency_invalid", index);
            const std::string dependencyId = dependency.toString().toStdString();
            if (!dependencyIds.insert(dependencyId).second || !ids.count(dependencyId))
                return fail("dependency_order_invalid", index);
            parsed.dependsOn.push_back(dependencyId);
        }
        plan.steps.push_back(std::move(parsed));
    }

    // Use a fixed serializer so the plan hash is stable for the same plan.
    QJsonArray normalizedSteps;
    for (const AnalysisPlanStep& step : plan.steps) {
        QJsonArray dependencies;
        for (const auto& dependency : step.dependsOn)
            dependencies.append(QString::fromStdString(dependency));
        normalizedSteps.append(QJsonObject{
            {"id", QString::fromStdString(step.id)},
            {"tool", QString::fromStdString(step.tool)},
            {"args", AgentJson::parseObject(step.argsJson)},
            {"depends_on", dependencies},
            {"requires_confirmation", step.requiresConfirmation}
        });
    }
    const QJsonObject normalized{
        {"schema", "dpa.analysis.plan"},
        {"version", 1},
        {"plan_id", QString::fromStdString(plan.planId)},
        {"context_fingerprint", QString::fromStdString(plan.contextFingerprint)},
        {"tool_catalog_sha256", QString::fromStdString(plan.toolCatalogSha256)},
        {"steps", normalizedSteps}
    };
    const QByteArray canonical = QJsonDocument(normalized).toJson(QJsonDocument::Compact);
    plan.canonicalJson = canonical.toStdString();
    plan.planSha256 = QCryptographicHash::hash(canonical, QCryptographicHash::Sha256)
        .toHex().toStdString();

    AnalysisPlanValidation result;
    result.ok = true;
    result.plan = std::move(plan);
    return result;
}

AnalysisPlanExecution executeAnalysisPlan(
    const AnalysisPlan& plan,
    std::function<ToolResult(const std::string&, const std::string&)> executor) {
    AnalysisPlanExecution result;
    if (plan.steps.empty()) {
        result.error = "empty_plan";
        return result;
    }
    if (!executor) {
        result.error = "executor_unavailable";
        return result;
    }
    std::set<std::string> completed;
    for (const AnalysisPlanStep& step : plan.steps) {
        for (const std::string& dependency : step.dependsOn) {
            if (!completed.count(dependency)) {
                result.error = "dependency_not_completed:" + step.id;
                return result;
            }
        }
        ToolResult stepResult = executor(step.tool, step.argsJson);
        result.results.push_back(stepResult);
        if (!stepResult.ok) {
            result.error = "step_failed:" + step.id;
            return result;
        }
        completed.insert(step.id);
        ++result.completedSteps;
    }
    result.ok = true;
    return result;
}

} // namespace Model
