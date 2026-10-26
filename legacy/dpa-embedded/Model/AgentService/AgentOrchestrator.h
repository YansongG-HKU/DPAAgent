#pragma once
#include <string>
#include <functional>
#include "iLLMProvider.h"
#include "ToolRegistry.h"
namespace Model {
    inline constexpr int kAgentPolicyVersion = 3;
    // Versioned policy sent through the provider's real system channel.
    std::string buildAgentSystemPrompt(const std::string& situationJson);

    class AgentOrchestrator {
    public:
        AgentOrchestrator(iLLMProvider* provider, const ToolRegistry* registry, int maxIters = 8)
            : _provider(provider), _registry(registry), _maxIters(maxIters) {}
        // Optional progress callback (UI uses it; tests may leave null).
        void setNoteCallback(std::function<void(const std::string&)> cb) { _onNote = std::move(cb); }
        // Optional tool executor — when set, used instead of _registry->call().
        // Allows marshaling tool calls to the main thread from a worker thread.
        void setToolExecutor(std::function<ToolResult(const std::string&, const std::string&)> fn) { _toolExec = std::move(fn); }
        // Always-on system context (situation header), prepended as the first message each run.
        void setSystemContext(std::string ctx) { _systemContext = std::move(ctx); }
        // Recent text turns from the current session, inserted before the new user turn.
        void setHistory(std::vector<LlmMessage> history) { _history = std::move(history); }
        // Structured callback for completed model calls plus tool start/results.
        void setStepCallback(std::function<void(const std::string&, const std::string&, const std::string&)> cb) { _onStep = std::move(cb); }
        void setCancelCallback(std::function<bool()> cb) { _isCancelled = std::move(cb); }
        void setBudget(LlmTurnBudget budget) { _budget = budget; }
        // In plan-first mode the model may only dispatch the deterministic plan
        // executor. Direct tool calls are returned to the model as a bounded,
        // non-executing policy error so the application remains the authority.
        void setPlanFirst(bool enabled) { _planFirst = enabled; }
        bool planFirst() const { return _planFirst; }
        // Run one user turn: loop chat<->tool calls until a Text reply or maxIters.
        std::string run(const std::string& userText);
        std::string runParts(const std::vector<LlmPart>& userParts);
    private:
        iLLMProvider* _provider;
        const ToolRegistry* _registry;
        int _maxIters;
        std::function<void(const std::string&)> _onNote;
        std::function<void(const std::string&, const std::string&, const std::string&)> _onStep;
        std::function<ToolResult(const std::string&, const std::string&)> _toolExec;
        std::string _systemContext;
        std::vector<LlmMessage> _history;
        std::function<bool()> _isCancelled;
        LlmTurnBudget _budget;
        bool _planFirst = false;
    };
}
