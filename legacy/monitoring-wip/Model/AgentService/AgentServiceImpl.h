#pragma once
#include "iAgentService.h"
#include "ToolRegistry.h"
#include "AgentOrchestrator.h"
#include "ClaudeProvider.h"
#include "SessionRecord.h"
#include "AdvisoryTrigger.h"
#include "BaselineMonitor.h"
#include "DiagnosticRun.h"
#include "AlertEngine.h"
#include "Utils/StreamingStatistics.h"
#include "Utils/TimeWindowAggregator.h"
#include "Utils/RingBuffer.h"
#include <map>
#include <atomic>
#include <QFuture>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QWaitCondition>
class QTimer;
class TstAgentServiceAsyncOps;
namespace Model {
    class McpBridgeService;
    struct ExperimentCampaign;
    struct CampaignExecution;
    class AgentServiceImpl : public iAgentService {
        Q_OBJECT
    public:
        AgentServiceImpl();
        ~AgentServiceImpl() override;
        void initBindings();   // wires current-analysis-test binding; see note
        std::string listTools() override;
        std::string callTool(const std::string& name, const std::string& argsJson) override;
        std::string listMcpTools() override;
        std::string callMcpTool(const std::string& name, const std::string& argsJson) override;
        bool isMcpToolWrite(const std::string& mcpName) override;
        std::string sendMessage(const std::string& userText) override;
        void sendMessageAsync(const std::string& userText) override;
        void sendMessageWithAttachmentsAsync(const std::string& userText, const std::vector<std::string>& attachmentPaths) override;
        void cancelCurrentTurn() override;
        bool startNewSession() override;
        std::string listSessions() override;
        std::string resumeSession(const std::string& sessionId) override;
        std::string runtimeInfo() override { return _runtimeInfoJson; }
        void setCompanionEnabled(bool enabled) override;
        bool companionEnabled() const override { return _companionConfig.enabled; }
        std::string companionStatus() override;
        std::string companionConfig() override;
        std::string configureCompanion(const std::string& configJson) override;
        std::string acknowledgeCompanionAlarm(const std::string& argsJson) override;
        std::string recordOperatorFeedback(const std::string& argsJson) override;
        // Read-only aggregation of operator-feedback records held in this session
        // (counts by type/severity plus the latest 10 bounded summaries).
        ToolResult aggregateOperatorFeedback(const std::string& args);
        // Called from the UI/main thread to approve or reject a pending terminal action.
        Q_INVOKABLE void confirmPendingAction(const std::string& confirmationId, bool approve) override;
        virtual void onPropertyChanged(const std::string& Index, utils::Property* prop);
    public:
        M* _projectService = NULL;
        M* _currentProject = NULL;
        M* _dpaTest        = NULL;   // current "分析试验" iTest*, bound in Task5
    private:
        friend class ::TstAgentServiceAsyncOps;
        enum class ToolOrigin { PublicApi, AgentSync, AgentInteractive, Autonomous, Mcp };
        static const char* toolOriginName(ToolOrigin origin);
        ToolResult executeTool(const std::string& name, const std::string& argsJson,
                               ToolOrigin origin, bool mcpName = false);
        void recordToolAudit(const std::string& correlationId, const std::string& name,
                             const std::string& argsJson, ToolOrigin origin, bool write,
                             const std::string& confirmation, const std::string& confirmationId,
                             const std::string& contextFingerprint,
                             const std::string& toolCatalogSha256,
                             qint64 startedMs,
                             qint64 durationMs, const ToolResult& result);
        bool persistBaseline(const std::string& key, const MachineBaseline& baseline);
        MachineBaseline restoreBaseline(const std::string& key) const;
        void beginDiagnosticTool(const std::string& name);
        void finishDiagnosticTool(const std::string& name, const ToolResult& result);
        void bindPendingAnalysisProcess(M* process, const std::string& runId);
        void clearPendingAnalysisProcess();
        void onAnalysisProcessAborted(M* process, const std::string& runId);
        void publishDiagnosticRun();
        bool persistSession();
        void appendSessionTurn(const std::string& role, const std::string& content);
        void appendCompanionEvent(const std::string& eventJson);
        void onAnalysisFinished(M* process);   // correlated completion + companion advisory
        std::string currentAnalysisContextFingerprint() const;
        std::string currentStandardPlanJson() const;
        void invalidatePendingAnalysis(const std::string& error);
        void invalidatePendingConfirmation();
        void loadCompanionConfig();
        void saveCompanionConfig() const;
        struct TrendPoint;
        void refreshCompanionRuntime(bool rebuildStatistics = false);
        void restartCompanionStatistics();
        void stopCompanionStatistics();
        void pollCompanion();
        void publishCompanionStatus();
        bool appendMonitoringHistory(int channelId, const TrendPoint& point);
        bool flushMonitoringHistory(long long throughMs, bool includePartial);
        bool persistMonitoringHistory() const;
        void restoreMonitoringHistory();
        ExperimentPreflight experimentPreflight() const;
        void publishCompanionDecision(const CompanionDecision& decision);
        void publishCompanionEvent(const std::string& severity, const std::string& kind,
                                   const std::string& title, const std::string& message,
                                   bool autoExplain, int channelId = -1,
                                   double value = 0, double limit = 0,
                                   const std::string& unit = "",
                                   const std::string& alarmId = "",
                                   const std::string& lifecycle = "",
                                   int occurrenceCount = 0,
                                   bool acknowledged = false);
        void handleCompanionDeviceEvent(const std::string& index, utils::Property* prop);
        ToolResult runAutonomousExperiment(const QJsonObject& plan);
        ToolResult runAutonomousExperimentCampaign(const QJsonObject& contract);
        ToolResult analyzeAutonomousExperimentCampaign(const std::string& args);
        ToolResult estimateAutonomousExperimentCampaignPower(const std::string& args);
        ToolResult refineAutonomousExperimentCampaign(const std::string& args);
        ToolResult suggestExperimentRefinementNext(const std::string& args);
        ToolResult computeSequentialBoundary(const std::string& args);
        ToolResult sweepCampaignPowerCurve(const std::string& args);
        ToolResult resumeAutonomousExperimentCampaign(const std::string& args);
        ToolResult listExperimentKnowledge(const std::string& args);
        ToolResult recordFeedbackCore(const std::string& args);
        ToolResult getCompanionTrend(const std::string& args);
        ToolResult getCompanionHistory(const std::string& args);
        ToolResult finishCampaignResult(const ExperimentCampaign& campaign,
                                        const CampaignExecution& execution);
        ToolResult autonomousExperimentStatus();
        bool waitForAutonomousAnalysis(long long timeoutMs);
        void beginTurnContext();
        void refreshTurnContext();
        void clearTurnContext();
        bool turnContextMatches() const;
        std::vector<LlmPart> buildUserParts(const std::string& userText, const std::vector<std::string>& attachmentPaths) const;
        ToolRegistry _registry;
        McpBridgeService* _mcpBridge = nullptr;
        iLLMProvider* _provider = nullptr;
        LlmTurnBudget _llmBudget;
        SessionRecord _session;
        std::string _runtimeInfoJson;
        DiagnosticRun _diagnosticRun;
        QMutex _confirmMutex;
        QWaitCondition _confirmCond;
        int _confirmDecision = 0;   // 0 pending, 1 approved, -1 rejected, -2 expired
        std::string _pendingConfirmationId;
        std::string _pendingConfirmationTool;
        std::string _pendingConfirmationArgsSha256;
        qint64 _pendingConfirmationExpiresAtMs = 0;
        mutable QMutex _turnContextMutex;
        std::string _turnContextFingerprint;
        QPointer<M> _pendingAnalysisProcess;
        std::string _pendingAnalysisRunId;
        std::string _analysisContextFingerprint;
        // Last explicit standards plan for this exact experiment context. It is
        // cleared on context/session changes and copied into evidence only after
        // deterministic re-validation.
        std::string _currentStandardPlanJson;
        bool _analysisCancelRequested = false;
        QMetaObject::Connection _pendingAnalysisConnection;
        AdvisoryConfig _advisoryConfig;   // proactive mode OFF by default
        AdvisoryState _advisoryState;
        M* _daqTest = nullptr;
        M* _daqWorkSpace = nullptr;
        M* _daqChannelSettings = nullptr;
        bool _daqRunning = false;
        bool _daqSaving = false;
        bool _daqConnected = false;
        struct CompanionStatTask {
            QPointer<M> data;
            std::vector<int> channelIds;
            std::map<int, long long> lastSequence;
            std::map<int, long long> lastAdvancedMs;
        };
        CompanionConfig _companionConfig;
        CompanionState _companionState;
        std::vector<CompanionStatTask> _companionStatTasks;
        QTimer* _companionTimer = nullptr;
        int _companionSampleIntervalMs = 1000;
        long long _companionStaleAfterMs = 5000;
        long long _companionSummaryIntervalMs = 300000;
        bool _companionAutoExplain = true;
        bool _companionAutoSummary = true;
        long long _companionRunStartedMs = 0;
        long long _companionLastSummaryMs = 0;
        long long _companionLastPollMs = 0;
        long long _companionLastDataMs = 0;
        long long _companionLastEventMs = 0;
        long long _companionLastSampleMs = 0;
        long long _companionFirstStartMs = 0;
        long long _companionPollCount = 0;
        long long _companionSampleCount = 0;
        long long _companionInvalidWindowCount = 0;
        long long _companionMissedWindowCount = 0;
        int _companionEventCount = 0;
        std::map<std::string, long long> _companionDeviceEventLastMs;
        std::string _lastCompanionStatusJson;
        std::map<std::string, MachineBaseline> _baselines;   // SQLite-backed hot cache by asset/point/direction/condition
        struct TrendPoint {
            long long observedMs = 0;
            long long sourceTimestampUs = 0;
            long long sequence = 0;
            long long sampleCount = 0;
            bool sourceAxisValid = false;
            double sourceStartX = 0;
            double sourceEndX = 0;
            double value = 0;
            double average = 0;
            double rms = 0;
            double stddev = 0;
            double minimum = 0;
            double maximum = 0;
            double peak = 0;
            double peakPeak = 0;
            double crestFactor = 0;
            double shapeFactor = 0;
            double impulseFactor = 0;
            double kurtosis = 0;
        };
        struct ChannelTrend {
            std::vector<TrendPoint> points;
            long long lastMs = 0;
        };
        static constexpr size_t kTrendCap = 240;
        std::map<int, ChannelTrend> _channelTrends;
        struct MonitoringHistoryPoint {
            long long minuteMs = 0;
            long long count = 0;
            double average = 0;
            double rms = 0;
            double minimum = 0;
            double maximum = 0;
            double peak = 0;
            double kurtosis = 0;
        };
        struct MonitoringBucket {
            long long minuteMs = 0;
            long long count = 0;
            double sumAverage = 0;
            double sumRms = 0;
            double minimum = 0;
            double maximum = 0;
            double peak = 0;
            double sumKurtosis = 0;
        };
        static constexpr size_t kHistoryCapPerChannel = 1440;
        static constexpr size_t kHistoryCapTotal = 20000;
        std::map<int, MonitoringBucket> _monitoringBuckets;
        std::map<int, std::vector<MonitoringHistoryPoint>> _monitoringHistory;
        struct FeedbackEvent {
            std::string id;
            qint64 timestampMs = 0;
            std::string sessionId;
            std::string correlationId;
            std::vector<std::string> evidenceRefs;
            std::string feedbackType;
            std::string severity;
            std::string summary;
            std::string operatorNote;
            std::string contextFingerprint;
            std::string toolCatalogSha256;
        };
        std::vector<FeedbackEvent> _feedbackEvents;
        std::atomic_bool _cancelRequested{false};
        std::atomic_bool _turnRunning{false};
        bool _planFirstEnabled = true;
        bool _autonomousExecutionActive = false;
        std::string _autonomousRunId;
        std::string _autonomousState = "idle";
        std::string _autonomousStopReason;
        qint64 _autonomousStartedMs = 0;
        qint64 _autonomousFinishedMs = 0;
        qint64 _autonomousDurationMs = 0;
        std::string _autonomousPlanSha256;
        QFuture<void> _turnFuture;
        AlertEngine _alertEngine;
        std::map<int, utils::StreamingStatistics> _channelStreamStats;
        std::map<int, utils::DefaultTimeWindowAggregator> _channelTimeWindows;
        std::map<int, utils::RingBuffer<double, 1024>> _channelSampleBuffers;
    };
}
