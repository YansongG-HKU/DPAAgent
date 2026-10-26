#pragma once
#include "iLLMProvider.h"
#include "ClaudeWire.h"
#include <atomic>
#include <QMutex>
class QNetworkReply;
namespace Model {
    class ClaudeProvider : public iLLMProvider {
    public:
        explicit ClaudeProvider(LlmConfig cfg) : _cfg(std::move(cfg)) {}
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
