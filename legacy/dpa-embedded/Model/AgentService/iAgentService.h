#pragma once
#include "HeaderDefine.h"
#include <QString>
#include <vector>
namespace Model {
    class iAgentService : public M {
        Q_OBJECT
    public:
        virtual ~iAgentService() = default;
        // Returns the tool catalog as a JSON string (placeholder for now).
        virtual std::string listTools() = 0;
        // Calls a tool by name with JSON args; returns a ToolResult JSON string (placeholder for now).
        virtual std::string callTool(const std::string& name, const std::string& argsJson) = 0;
        virtual std::string listMcpTools() = 0;
        virtual std::string callMcpTool(const std::string& name, const std::string& argsJson) = 0;
        // Runs one user turn through the LLM orchestrator; returns the final reply JSON/text.
        virtual std::string sendMessage(const std::string& userText) = 0;
        // Async variant: returns immediately; emits replyReady(reply) on the main thread when done.
        virtual void sendMessageAsync(const std::string& userText) = 0;
        virtual void sendMessageWithAttachmentsAsync(const std::string& userText, const std::vector<std::string>& attachmentPaths) = 0;
        virtual void cancelCurrentTurn() = 0;
        virtual bool startNewSession() = 0;
        // Session recovery is local-only and never exposes tool arguments or secrets.
        virtual std::string listSessions() = 0;
        virtual std::string resumeSession(const std::string& sessionId) = 0;
        virtual std::string runtimeInfo() = 0;
        // Local deterministic companion monitor. Defaults keep lightweight test
        // doubles source-compatible; AgentServiceImpl provides the real behavior.
        virtual void setCompanionEnabled(bool enabled) { (void)enabled; }
        virtual bool companionEnabled() const { return false; }
        virtual std::string companionStatus() { return R"({"enabled":false,"state":"off"})"; }
        virtual std::string companionConfig() { return R"({"enabled":false})"; }
        virtual std::string configureCompanion(const std::string& configJson) {
            (void)configJson; return R"({"ok":false,"error":"unsupported"})";
        }
        // UI-originated operator feedback (audit write, no confirmation card).
        // Source-compatible default keeps lightweight test doubles working.
        virtual std::string recordOperatorFeedback(const std::string& argsJson) {
            (void)argsJson; return R"({"ok":false,"error":"unsupported"})";
        }
        // Called from the UI/main thread to approve or reject a pending terminal action.
        virtual void confirmPendingAction(const std::string& confirmationId, bool approve) = 0;
        // Fail closed by default: only implementations that can prove a tool is read-only
        // may return false. AgentServiceImpl delegates this decision to its ToolRegistry.
        virtual bool isMcpToolWrite(const std::string& mcpName) { (void)mcpName; return true; }
        // Appended to preserve the existing interface vtable layout for deployed plugins.
        virtual std::string acknowledgeCompanionAlarm(const std::string& argsJson) {
            (void)argsJson; return R"({"ok":false,"error":"unsupported"})";
        }
    signals:
        void replyReady(QString reply);
        // Emitted on the main thread when a terminal tool requires user confirmation.
        void confirmationRequired(QString confirmationId, QString toolName, QString argsSummary);
        // Emitted for model-call metadata and tool steps; payloads exclude model/prompt content.
        void agentStep(QString kind, QString name, QString detail);
        void companionStatusChanged(QString statusJson);
        void companionEvent(QString eventJson);
    };
}
