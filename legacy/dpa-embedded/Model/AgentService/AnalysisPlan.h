#pragma once

#include "ToolResult.h"

#include <functional>
#include <string>
#include <vector>

namespace Model {

class ToolRegistry;

struct AnalysisPlanStep {
    std::string id;
    std::string tool;
    std::string argsJson;
    std::vector<std::string> dependsOn;
    bool requiresConfirmation = false;
};

struct AnalysisPlan {
    int version = 1;
    std::string planId;
    std::string contextFingerprint;
    std::string toolCatalogSha256;
    std::vector<AnalysisPlanStep> steps;
    std::string canonicalJson;
    std::string planSha256;
};

struct AnalysisPlanValidation {
    bool ok = false;
    std::string error;
    int stepIndex = -1;
    AnalysisPlan plan;
};

struct AnalysisPlanExecution {
    bool ok = false;
    std::string error;
    int completedSteps = 0;
    std::vector<ToolResult> results;
};

// A plan is model-produced data, never an authorization decision. Validation
// binds it to the current context and exact tool catalog before execution.
AnalysisPlanValidation parseAndValidateAnalysisPlan(
    const std::string& json,
    const ToolRegistry& registry,
    const std::string& expectedContextFingerprint,
    bool allowWrites = false);

// Executes only an already validated plan. The caller supplies the same
// application-owned executor used by normal tool calls, so confirmation,
// context and thread gates cannot be bypassed by the plan layer.
AnalysisPlanExecution executeAnalysisPlan(
    const AnalysisPlan& plan,
    std::function<ToolResult(const std::string&, const std::string&)> executor);

} // namespace Model

