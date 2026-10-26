#pragma once
#include <string>
#include <map>
#include <vector>
#include "tools/ResultSummaryTool.h"   // ResultSummary
namespace Model {
    // Proactive-advice configuration. Thresholds come from user/config, NOT from the
    // result (results carry no limits). Disabled by default — proactive mode is opt-in.
    struct AdvisoryConfig {
        bool enabled = false;                          // proactive mode master switch
        double peakValueMax = 0;                        // 0 = disabled
        std::map<std::string,double> maxByMetric;       // stat name -> max allowed
        long long minIntervalMs = 60000;                // throttle: min gap between fires per metric
    };
    // Throttle memory carried across analyses.
    struct AdvisoryState {
        long long lastFiredMs = 0;
        std::string lastKey;
    };
    struct AdvisoryDecision {
        bool fire = false;
        std::string metric;
        double value = 0, limit = 0;
        std::string message;
    };
    // Pure core (no Qt/services): given a fresh result summary + config + throttle state,
    // decide whether to proactively advise. Mutates st when it fires.
    AdvisoryDecision evaluateAdvisory(const ResultSummary& s, const AdvisoryConfig& cfg,
                                      long long nowMs, AdvisoryState& st);

    // Deterministic real-time monitoring core. Hardware/user settings supply all
    // limits; the language model never participates in an alarm decision.
    struct CompanionConfig {
        bool enabled = false;
        int sustainedSamples = 3;
        long long cooldownMs = 60000;
        long long escalationMs = 30 * 60 * 1000;
        double hysteresisRatio = 0.02;
    };
    struct CompanionChannelSample {
        int channelId = -1;
        std::string name;
        std::string unit;
        bool hasValue = false;
        double value = 0;
        double minimum = 0;
        double maximum = 0;
        bool limitEnabled = false;
        double lowLimit = 0;
        double highLimit = 0;
        bool stale = false;
        bool qualityInvalid = false;
    };
    struct CompanionConditionState {
        int consecutive = 0;
        bool active = false;
        long long lastFiredMs = 0;
        long long activatedMs = 0;
        long long acknowledgedMs = 0;
        int occurrenceCount = 0;
        bool acknowledged = false;
        bool escalated = false;
        std::string alarmId;
    };
    struct CompanionChannelState {
        CompanionConditionState high;
        CompanionConditionState low;
        CompanionConditionState stale;
        CompanionConditionState invalid;
    };
    struct CompanionState {
        std::map<int, CompanionChannelState> channels;
    };
    struct CompanionDecision {
        std::string kind;       // limit_high|limit_low|data_stale|*_recovered
        std::string severity;   // alarm|warning|info
        int channelId = -1;
        std::string channelName;
        std::string unit;
        double value = 0;
        double limit = 0;
        bool recovery = false;
        std::string alarmId;
        std::string lifecycle;  // active|repeat|escalated|recovered
        int occurrenceCount = 0;
        bool acknowledged = false;
    };
    std::vector<CompanionDecision> evaluateCompanionChannels(
        const std::vector<CompanionChannelSample>& samples,
        const CompanionConfig& cfg, long long nowMs, CompanionState& state);
    bool acknowledgeCompanionAlarm(CompanionState& state,
                                   const std::string& alarmId, long long nowMs);

    // Read-only health projection for continuous monitoring.  This is deliberately
    // a small deterministic contract so the UI, MCP bridge and future exporters
    // can share the same state machine without duplicating alarm logic.
    struct MonitoringHealthInput {
        bool enabled = false;
        bool hasDaqTest = false;
        bool connected = false;
        bool acquiring = false;
        bool saving = false;
        int monitoredChannels = 0;
        int activeMonitoredChannels = 0;
        int activeAlerts = 0;
        int invalidChannels = 0;
        bool dataFresh = false;
        long long dataAgeMs = 0;
        long long staleAfterMs = 5000;
    };
    struct MonitoringHealth {
        std::string state;    // off|unavailable|waiting|healthy|degraded|stale|alarm
        std::string severity; // info|warning|critical
        bool live = false;
        bool ready = false;
        std::vector<std::string> reasons;
    };
    MonitoringHealth evaluateMonitoringHealth(const MonitoringHealthInput& input);

    // Deterministic experiment-readiness snapshot. Optional monitoring and
    // storage gaps remain warnings rather than invented hardware blockers.
    struct ExperimentPreflightInput {
        bool hasProject = false;
        bool hasDaqTest = false;
        bool connected = false;
        bool acquiring = false;
        bool saving = false;
        bool companionEnabled = false;
        int selectedChannels = 0;
        int monitoredChannels = 0;
        int validLimitChannels = 0;
        int activeMonitoredChannels = 0;
        int dataSources = 0;
    };
    struct ExperimentHealthNode {
        std::string id;
        std::string state; // ok|active|waiting|warning|blocked
    };
    struct ExperimentPreflight {
        ExperimentPreflightInput snapshot;
        std::string overall; // blocked|ready|degraded|monitoring|monitoring_with_warnings
        bool readyToAcquire = false;
        bool readyToMonitor = false;
        std::vector<std::string> blockers;
        std::vector<std::string> warnings;
        std::vector<ExperimentHealthNode> nodes;
    };
    ExperimentPreflight evaluateExperimentPreflight(const ExperimentPreflightInput& input);

    struct CompanionDeviceDecision {
        bool valid = false;
        std::string severity;
        std::string kind;
        std::string title;
        std::string message;
        bool explain = false;
    };
    CompanionDeviceDecision describeCompanionDeviceEvent(
        const std::string& index, const std::string& deviceName,
        const std::string& detail, bool connected);
}
