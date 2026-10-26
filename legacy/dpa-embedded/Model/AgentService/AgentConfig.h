#pragma once
#include "ClaudeWire.h"   // LlmConfig
#include <vector>
namespace Model {
    enum class ApiKeySource { None, Environment, DpapiCurrentUser, LegacyPlaintext };

    struct LlmRouteConfig {
        std::string id = "primary";
        LlmConfig config;
        ApiKeySource apiKeySource = ApiKeySource::None;
        bool legacyPlaintextApiKey = false;
        std::string apiKeyError;
        std::string apiKeyEnvironment;
        bool enabled = true;
        std::string disabledReason;
        bool pricingConfigured = false;
        std::string pricingError;
        long long inputCostMicrosPerMillionTokens = 0;
        long long outputCostMicrosPerMillionTokens = 0;
    };

    struct LlmRoutingPolicy {
        int failureThreshold = 2;
        long long cooldownMs = 60000;
    };

    struct LlmConfigLoadResult {
        LlmConfig config;
        std::string configPath;       // selected/overridden path; safe to display
        ApiKeySource apiKeySource = ApiKeySource::None;
        bool legacyPlaintextApiKey = false;
        std::string apiKeyError;      // non-secret stable code; never contains the key
        // routes[0] is the backward-compatible primary route. Fallbacks are
        // opt-in and cannot receive diagnostic data without explicit consent.
        std::vector<LlmRouteConfig> routes;
        LlmRoutingPolicy routing;
        LlmTurnBudget budget;
        std::vector<std::string> configErrors;
    };

    // pathOverride is used by tests/tools; empty keeps the executable-dir then
    // per-user AppData lookup. DPA_LLM_API_KEY is always the highest priority.
    LlmConfigLoadResult loadLlmConfigDetailed(const std::string& pathOverride = {});

    // Credentialed endpoints are allowed over plaintext HTTP only when the
    // destination is a literal loopback address or localhost.
    std::string validateCredentialedEndpoint(const LlmConfig& config);

    // Compatibility wrapper used by AgentServiceImpl.
    LlmConfig loadLlmConfig();
}
