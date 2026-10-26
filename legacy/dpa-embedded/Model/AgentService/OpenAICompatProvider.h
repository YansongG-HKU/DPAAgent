#pragma once
#include "iLLMProvider.h"
#include "ClaudeWire.h"   // LlmConfig
#include <atomic>
#include <QMutex>
class QNetworkReply;
namespace Model {
    // HTTP provider for OpenAI-compatible /v1/chat/completions endpoints
    // (local Ollama/vLLM/DGX, etc.). Synchronous request via QNetworkAccessManager.
    class OpenAICompatProvider : public iLLMProvider {
    public:
        explicit OpenAICompatProvider(LlmConfig cfg) : _cfg(std::move(cfg)) {}
        LlmReply chat(const std::vector<LlmMessage>& messages, const std::string& toolsJson) override;
        void cancel() override;
        void resetCancellation() override { _cancelled.store(false); }
    private:
        LlmConfig _cfg;
        std::atomic_bool _cancelled{false};
        QMutex _replyMutex;
        QNetworkReply* _activeReply = nullptr;
    };
}
