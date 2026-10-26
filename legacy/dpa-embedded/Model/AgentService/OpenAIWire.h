#pragma once
#include <string>
#include <vector>
#include "iLLMProvider.h"
#include "ClaudeWire.h"   // reuse LlmConfig
namespace Model {
    // Build an OpenAI /v1/chat/completions request body.
    std::string buildOpenAIRequestBody(const std::vector<LlmMessage>& messages,
                                       const std::string& toolsJson, const LlmConfig& cfg);
    // Build an OpenAI /v1/responses request body for endpoints that expect /responses.
    std::string buildOpenAIResponsesRequestBody(const std::vector<LlmMessage>& messages,
                                               const std::string& toolsJson, const LlmConfig& cfg);
    // Parse an OpenAI chat-completions response into an LlmReply.
    LlmReply parseOpenAIReply(const std::string& responseJson);
    // Normalize an OpenAI-compatible base URL to the actual request endpoint.
    std::string resolveOpenAICompatEndpoint(const std::string& baseUrl);
}
