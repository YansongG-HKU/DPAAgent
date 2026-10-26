#pragma once
#include <string>
#include <vector>
#include <optional>
#include "iLLMProvider.h"
namespace Model {
    struct LlmConfig {
        // provider selector: "openai_compat" (default; local/DGX OpenAI-compatible
        // endpoint) or "claude" (Anthropic Messages API via ClaudeProvider).
        std::string provider = "openai_compat";
        std::string apiKey;
        std::string model = "claude-opus-4-8";
        std::string baseUrl = "https://api.anthropic.com/v1/messages";
        int maxTokens = 2048;
        int timeoutMs = 60000;    // QNetworkRequest 传输超时（0=不超时）
        // Opt-in vLLM/Qwen Chat Completions extension; unset preserves server defaults.
        std::optional<bool> enableThinking;
    };
    // Build the Anthropic Messages request body JSON from our messages + tool catalog + config.
    std::string buildClaudeRequestBody(const std::vector<LlmMessage>& messages,
                                       const std::string& toolsJson, const LlmConfig& cfg);
    // Parse an Anthropic Messages response body JSON into an LlmReply.
    LlmReply parseClaudeReply(const std::string& responseJson);
}
